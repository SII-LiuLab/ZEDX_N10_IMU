#!/usr/bin/env python3
"""PC-side N10 capture and checksum-verified file return over SSH."""
import argparse
from datetime import datetime
from pathlib import Path
import re
import shlex
import subprocess
import sys
import uuid

from session_integrity import verify

ROOT = Path(__file__).resolve().parent.parent
REMOTE = '/home/nvidia/ZEDX_N10_IMU'


def ssh(host, command, check=True):
    return subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', host, shlex.join(command)], check=check)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='thor')
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('init', help='Initialize idle cameras on N10')
    record = commands.add_parser('record', help='Record and return camera data')
    record.add_argument('--name', default=datetime.now().strftime('%Y%m%d-%H%M%S-')+uuid.uuid4().hex[:6])
    record.add_argument('--frames', type=int, default=600)
    record.add_argument('--initialize', action='store_true')
    record.add_argument('--destination', type=Path, default=ROOT/'ouputs')
    record.add_argument('--save-every', type=int, default=60)
    record.add_argument('--cameras', nargs='+', choices=('shw_v6', 'shw_v7', 'zed'),
                        default=['shw_v6', 'shw_v7', 'zed'])
    live = commands.add_parser('live', help='Show selected cameras live in a local browser (no recording)')
    live.add_argument('--cameras', nargs='+', choices=('shw_v6', 'shw_v7', 'zed'),
                      default=['shw_v6', 'shw_v7', 'zed'])
    live.add_argument('--initialize', action='store_true')
    live.add_argument('--preview-fps', type=int, default=10, help='Preview update limit, 1..30')
    live.add_argument('--preview-width', type=int, default=960, help='Even preview width, 320..1920')
    live.add_argument('--port', type=int, default=8844, help='Local browser port; 0 selects a free port')
    live.add_argument('--no-browser', action='store_true')
    live.add_argument('--duration', type=float, default=0, help='Live seconds after warmup; 0 runs until stopped')
    fetch = commands.add_parser('fetch', help='Resume file return and verify checksums')
    fetch.add_argument('name')
    fetch.add_argument('--destination', type=Path, default=ROOT/'ouputs')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', args.host):
        parser.error('Use a configured SSH host alias')
    if args.action in ('record', 'fetch') and not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}', args.name):
        parser.error('Session name must be 1..100 letters, digits, underscore, dot or dash')
    if args.action == 'record' and not 2 <= args.frames <= 10000:
        parser.error('Frame count must be 2..10000')
    if args.action == 'record' and (args.save_every < 1 or len(set(args.cameras)) != len(args.cameras)):
        parser.error('Invalid save interval or duplicate camera')
    if args.action == 'live':
        if len(set(args.cameras)) != len(args.cameras):
            parser.error('Duplicate camera')
        if (not 1 <= args.preview_fps <= 30 or not 320 <= args.preview_width <= 1920 or args.preview_width % 2
                or not 0 <= args.port <= 65535 or not 0 <= args.duration <= 86400):
            parser.error('Invalid live rate, width, port or duration')
    return args


def capture_command(args, output):
    return ['sudo', '-n', 'python3', REMOTE+'/scripts/capture_three_n10.py', output,
            '--frames', str(args.frames), '--save-every', str(args.save_every),
            '--cameras', *args.cameras]


def main():
    args = parse_args()
    if args.action == 'init':
        ssh(args.host, ['sudo', '-n', 'bash', REMOTE+'/scripts/load_mixed_n10.sh'])
        return 0
    if args.action == 'live':
        if args.initialize:
            from sync_time_n10 import sync
            clock = sync(args.host)
            print(f'N10 clock synchronized: offset {clock["offset_after_ms"]:.3f} ms', flush=True)
            ssh(args.host, ['sudo', '-n', 'bash', REMOTE+'/scripts/load_mixed_n10.sh'])
        from live_remote import run
        try:
            return run(args, REMOTE)
        except (RuntimeError, OSError) as error:
            print(f'Live preview failed: {error}', file=sys.stderr)
            return 1
    remote = REMOTE+'/ouputs/'+args.name
    result = 0
    if args.action == 'record':
        if args.initialize:
            from sync_time_n10 import sync
            clock = sync(args.host)
            print(f'N10 clock synchronized: offset {clock["offset_after_ms"]:.3f} ms', flush=True)
            ssh(args.host, ['sudo', '-n', 'bash', REMOTE+'/scripts/load_mixed_n10.sh'])
        result = ssh(args.host, capture_command(args, remote), check=False).returncode
    target = args.destination.resolve()/args.name
    target.mkdir(parents=True, exist_ok=True)
    subprocess.run(['rsync', '-a', '--partial', '--checksum', '--protect-args',
                    '-e', 'ssh -o BatchMode=yes', args.host+':'+remote+'/', str(target)+'/'], check=True)
    print(f'Returned and SHA256 verified {verify(target)} files: {target}')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
