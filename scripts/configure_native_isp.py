#!/usr/bin/env python3
"""Install/check per-model ISP files for the verified N10; preserve rollback data."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parent.parent
SETTINGS = Path('/var/nvidia/nvcam/settings')
SERVICE = Path('/etc/systemd/system/nvargus-daemon.service.d/n10-mixed-isp.conf')
ENV = '[Service]\nEnvironment=NVCAMERA_NITO_PATH=CONFIG\nEnvironment=enableCamInfiniteTimeout=1\n'
FILES = {'imx185_topright_liimx185': 'shw5g_vb1940.isp',
         'imx185_bottomcenter_liimx185': 'shw5g_vb1940.isp',
         'zedx_ar0234': 'zedx_ar0234.isp'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'install', 'rollback'))
    parser.add_argument('--backup', type=Path, help='Backup directory created by install; required for rollback')
    args = parser.parse_args()
    if os.uname().release != '6.8.12-rt-tegra' or b'B25EJ21_007' not in Path('/proc/device-tree/rb,dtbbuildversion').read_bytes():
        parser.error('This configuration is restricted to the verified N10 BSP')
    provenance = json.loads((ROOT/'calibration/isp/provenance.json').read_text())
    for name in FILES.values():
        if digest(ROOT/'calibration/isp'/name) != provenance[name]['sha256']:
            parser.error('Factory ISP provenance hash mismatch')
    badges = {p.read_bytes().rstrip(b'\0').decode() for p in Path('/proc/device-tree/tegra-camera-platform/modules').glob('*/badge')}
    if not FILES.keys() <= badges:
        parser.error('Expected camera badges are absent')
    if args.action == 'check':
        assert not (SETTINGS/'camera_overrides.isp').exists(), 'Global ISP overrides mixed camera tuning'
        for badge, name in FILES.items():
            assert digest(SETTINGS/(badge+'.isp')) == provenance[name]['sha256'], badge
        assert SERVICE.read_text() == ENV, 'Argus environment differs from N10 manual'
        print('Per-model ISP files and service configuration verified')
        return
    if os.geteuid() != 0:
        parser.error('install/rollback requires sudo')
    with (ROOT/'.capture.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        busy = subprocess.run(['fuser', *map(str, Path('/dev').glob('video*'))], capture_output=True)
        if busy.returncode != 1:
            parser.error('A camera consumer is running, or occupancy check failed')
        targets = [SETTINGS/(badge+'.isp') for badge in FILES] + [SETTINGS/'camera_overrides.isp', SERVICE]
        if args.action == 'install':
            backup = args.backup or ROOT/'evidence'/('native-isp-backup-'+uuid.uuid4().hex[:12])
            backup.mkdir(parents=True, exist_ok=False)
            entries = []
            for i, path in enumerate(targets):
                entry = {'path': str(path), 'existed': path.exists(), 'backup': str(i)}
                if path.exists():
                    shutil.copy2(path, backup/str(i)); entry['sha256'] = digest(path)
                entries.append(entry)
            (backup/'manifest.json').write_text(json.dumps(entries, indent=2)+'\n')
            for badge, name in FILES.items():
                shutil.copyfile(ROOT/'calibration/isp'/name, SETTINGS/(badge+'.isp'))
                (SETTINGS/(badge+'.isp')).chmod(0o644)
            (SETTINGS/'camera_overrides.isp').unlink(missing_ok=True)
            SERVICE.parent.mkdir(parents=True, exist_ok=True)
            SERVICE.write_text(ENV); SERVICE.chmod(0o644)
            print(f'Rollback backup: {backup}')
        else:
            if args.backup is None:
                parser.error('--backup required for rollback')
            entries = json.loads((args.backup/'manifest.json').read_text())
            if {e['path'] for e in entries} != set(map(str, targets)):
                parser.error('Unexpected backup target list')
            for entry in entries:
                if entry['existed'] and digest(args.backup/entry['backup']) != entry['sha256']:
                    parser.error('Backup checksum mismatch')
            for entry in entries:
                path = Path(entry['path'])
                if entry['existed']:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(args.backup/entry['backup'], path)
                else:
                    path.unlink(missing_ok=True)
        subprocess.run(['systemctl', 'daemon-reload'], check=True)
        subprocess.run(['systemctl', 'restart', 'nvargus-daemon'], check=True)


if __name__ == '__main__':
    main()
