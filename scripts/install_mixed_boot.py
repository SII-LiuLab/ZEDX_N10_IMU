#!/usr/bin/env python3
"""Install a separate mixed-camera boot entry; retain the original entry and DTB."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=['check', 'install', 'rollback'])
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
config = Path('/boot/extlinux/extlinux.conf')
backup = root / 'evidence/preflight/extlinux.before-mixed-sdk.conf'
text = config.read_text()
if args.action == 'rollback':
    if os.geteuid() != 0:
        parser.error('Run with sudo')
    dtb_backup = backup.with_suffix('.dtb')
    if dtb_backup.exists():
        shutil.copyfile(dtb_backup, '/boot/dtb/n10-cam3-mixed-sdk.dtb')
    shutil.copyfile(backup, config)
    os.sync()
    print('Original boot configuration restored. Reboot normally to activate it.')
    raise SystemExit
board = Path('/sys/firmware/devicetree/base/rb,dtbbuildversion').read_bytes()
if b'B25EJ21_007' not in board or os.uname().release != '6.8.12-rt-tegra':
    parser.error('Requires the verified N10')
dtb = root / 'build/n10-cam3-mixed.dtb'
expected = {
    '/bus@0/i2c@810c680000/max9296_b@49': ('compatible', 'rb,max9296'),
    '/bus@0/i2c@810c680000/imx185_e@1f': ('status', 'disabled'),
    '/bus@0/i2c@810c680000/imx185_h@22': ('status', 'disabled'),
    '/bus@0/i2c@810c680000/imx185_f@20': ('compatible', 'sensing,shw5g_30fps'),
    '/bus@0/i2c@810c680000/imx185_g@21': ('compatible', 'sensing,shw5g_30fps'),
    '/bus@0/i2c@810c680000/zedx_left@28/mode0': ('vc_id', '0'),
    '/bus@0/i2c@810c680000/zedx_right@20/mode0': ('vc_id', '3'),
    '/n10-cam3-mixed': ('compatible', 'n10,cam3-mixed-zedx'),
    '/tegra-camera-platform/modules/module0': ('badge', 'zedx_ar0234'),
    '/tegra-camera-platform/modules/module1': ('badge', 'zedx_ar0234'),
    '/tegra-camera-platform/modules/module0/drivernode0': ('devname', 'zedx 7-0020'),
    '/tegra-camera-platform/modules/module1/drivernode0': ('devname', 'zedx 7-0028'),
}
for node, (prop, value) in expected.items():
    actual = subprocess.check_output(['fdtget', str(dtb), node, prop], text=True).strip()
    if actual != value:
        parser.error(f'DTB mismatch: {node}/{prop}: {actual}')
blocks = re.split(r'(?m)(?=^LABEL )', text)
primary = next((b for b in blocks if b.startswith('LABEL primary\n')), None)
if not primary:
    parser.error('No primary recovery entry')
entry = re.sub(r'^LABEL primary', 'LABEL mixed_sdk', primary)
entry = re.sub(r'(?m)^\s*MENU LABEL .*$', '      MENU LABEL N10 SHW V6 V7 + ZED V8', entry)
entry = re.sub(r'(?m)^\s*FDT .*$', '      FDT /boot/dtb/n10-cam3-mixed-sdk.dtb', entry)
entry = re.sub(r'(?m)^(\s*APPEND .*)$', r'\1 modprobe.blacklist=sl_max96712,sl_max9295,sl_zedx,bmi088', entry)
blocks = [b for b in blocks if not b.startswith('LABEL mixed_sdk\n')]
proposed = re.sub(r'(?m)^DEFAULT .*$', 'DEFAULT mixed_sdk', ''.join(blocks))
proposed = proposed.rstrip() + '\n\n' + entry.rstrip() + '\n'
print(proposed)
print('DTB SHA256:', hashlib.sha256(dtb.read_bytes()).hexdigest())
if args.action == 'install':
    if os.geteuid() != 0:
        parser.error('Run with sudo')
    if subprocess.run(['fuser', *map(str, Path('/dev').glob('video*'))], capture_output=True).returncode == 0:
        parser.error('Camera is in use')
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.exists():
        with backup.open('x') as f:
            f.write(text)
    target = Path('/boot/dtb/n10-cam3-mixed-sdk.dtb')
    dtb_backup = backup.with_suffix('.dtb')
    if target.exists() and not dtb_backup.exists():
        shutil.copyfile(target, dtb_backup)
    shutil.copyfile(dtb, target)
    staged = config.with_suffix('.mixed.tmp')
    staged.write_text(proposed)
    os.replace(staged, config)
    os.sync()
    print('Installed. Reboot normally to activate; original primary entry remains available.')
