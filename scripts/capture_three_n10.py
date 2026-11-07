#!/usr/bin/env python3
"""Independent SHW native-ISP RGB and official ZED SDK RGB-D capture on N10."""
import argparse
import csv
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import tempfile
from contextlib import ExitStack
from session_integrity import seal
from zed_sdk_runtime import sdk_environment

ROOT = Path(__file__).resolve().parent.parent
CAMERAS = ('shw_v6', 'shw_v7', 'zed')
SHW_INDICES = {'shw_v6': 2, 'shw_v7': 3}
WARMUP_FRAMES = 90


def capture_command(name, folder, args):
    if name == 'zed':
        return [str(ROOT/'build/capture_zed_n10'), str(folder), str(args.frames),
                str(WARMUP_FRAMES), str(args.save_every), args.depth_mode,
                str(args.depth_stabilization)]
    executable = 'capture_shw_live_n10' if args.live else 'capture_shw_n10'
    return [str(ROOT/'build'/executable), str(SHW_INDICES[name]), str(folder),
            str(args.frames), str(args.save_every)]


def summarize(folder):
    import cv2
    import numpy as np
    from stereo_geometry import check_pair
    complete = json.loads((folder/'complete.json').read_text())
    with (folder/'frames.csv').open() as f:
        rows = list(csv.DictReader(f))
    stamps = np.array([int(r['timestamp_ns']) for r in rows], dtype=np.int64)
    host = np.array([int(r['host_monotonic_ns']) for r in rows], dtype=np.int64)
    if len(rows) < 2 or complete['frames'] != len(rows) or (np.diff(stamps) <= 0).any():
        raise ValueError(f'{folder.name}: missing frames or nonmonotonic timestamps')
    samples = [r for r in rows if r['saved'] == '1']
    depth_stats = []
    for row in samples:
        i = row['index']
        path = folder/(i+'-left.png' if complete['backend'] == 'zed-sdk' else i+'-rgb.jpg')
        image = cv2.imread(str(path))
        if image is None or image.std() < 1:
            raise ValueError(f'Invalid image: {path}')
        if complete['backend'] == 'zed-sdk':
            depth = np.load(folder/(i+'-depth-m.npy'), allow_pickle=False)
            confidence = np.load(folder/(i+'-confidence.npy'), allow_pickle=False)
            valid = np.isfinite(depth) & (depth > 0)
            if depth.dtype != np.float32 or depth.shape != image.shape[:2] or not valid.any():
                raise ValueError('Invalid SDK depth')
            if confidence.shape != depth.shape or not np.isfinite(confidence[valid]).all():
                raise ValueError('Invalid SDK confidence')
            geometry = check_pair(image, cv2.imread(str(folder/(i+'-right.png'))), depth,
                                  json.loads((folder/'calibration.json').read_text()))
            if not geometry['passed']:
                raise ValueError(f'Stereo geometry failed: {geometry}')
            depth_stats.append({'index': int(i), 'stereo_geometry': geometry, 'valid_fraction': float(valid.mean()),
                                'median_m': float(np.median(depth[valid]))})
    return {'frames': len(rows), 'saved_frames': len(samples),
            'fps_from_sensor_timestamps': float((len(rows)-1)*1e9/(stamps[-1]-stamps[0])),
            'max_frame_gap_ms': float(np.diff(stamps).max()/1e6),
            'first_host_monotonic_ns': int(host[0]), 'last_host_monotonic_ns': int(host[-1]),
            'depth': depth_stats}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--live', action='store_true', help='Continuous preview without recording image files')
    parser.add_argument('--preview-fps', type=int, default=10)
    parser.add_argument('--preview-width', type=int, default=960)
    parser.add_argument('--preview-port', type=int, default=0)
    parser.add_argument('--watch-stdin', action='store_true', help='Stop after SSH input closes or heartbeat expires')
    parser.add_argument('--duration', type=float, default=0, help='Live seconds after warmup; 0 runs until stopped')
    parser.add_argument('--cameras', nargs='+', choices=CAMERAS, default=list(CAMERAS))
    parser.add_argument('--frames', type=int, default=600)
    parser.add_argument('--depth-mode', choices=('NEURAL', 'NEURAL_LIGHT'), default='NEURAL')
    parser.add_argument('--depth-stabilization', type=int, choices=range(101), default=30, metavar='0..100')
    parser.add_argument('--save-every', type=int, default=60,
                        help='Record every Nth frame; latest.jpg is atomically updated at this interval')
    args = parser.parse_args()
    if os.geteuid() or os.uname().machine != 'aarch64':
        parser.error('Run with sudo on Thor')
    if not args.live and (args.output is None or not args.output.is_absolute() or args.output.exists()):
        parser.error('Use a new absolute output directory')
    if args.live and args.output is not None:
        parser.error('Live output is temporary; omit the output directory')
    if (not 1 <= args.preview_fps <= 30 or not 320 <= args.preview_width <= 1920 or args.preview_width % 2
            or not 0 <= args.preview_port <= 65535 or not 0 <= args.duration <= 86400):
        parser.error('Invalid live rate, width, port or duration')
    if not 2 <= args.frames <= 10000 or args.save_every < 1 or len(set(args.cameras)) != len(args.cameras):
        parser.error('Invalid frame count, save interval or duplicate camera')
    env = sdk_environment()
    jobs, interrupted = [], False
    def stop(signum, frame):
        nonlocal interrupted
        interrupted = True
        for _, child in jobs:
            if child.poll() is None:
                child.terminate()
    with ExitStack() as stack:
        # Disjoint camera sessions share this lock; initialization is exclusive.
        for name, mode in [('.capture.lock', fcntl.LOCK_SH)]+[(f'.{c}.lock', fcntl.LOCK_EX) for c in sorted(args.cameras)]:
            lock = stack.enter_context((ROOT/name).open('a+'))
            try:
                fcntl.flock(lock, mode | fcntl.LOCK_NB)
            except BlockingIOError:
                parser.error(f'Camera or initialization busy: {name}')
        if args.live:
            from live_preview import run_live
            args.frames = 0
            args.output = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='n10-live-', dir='/dev/shm')))/'session'
            return run_live(args, env, capture_command)
        args.output.mkdir(parents=True)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        report = {'backend': 'shw-native-isp-and-zed-sdk', 'sdk_capture_version': env['ZED_SDK_VIDEO_CAPTURE_VERSION'],
                  'sdk_directory': env['ZED_DIR'],
                  'status': 'running', 'cameras': {},
                  'validation_scope': 'Capture, file payload and rectified stereo geometry only; depth accuracy is not certified',
                  'depth_accuracy_verified': False}
        try:
            for name in args.cameras:
                if interrupted:
                    break
                folder = args.output/name
                cmd = capture_command(name, folder, args)
                log = stack.enter_context((args.output/(name+'.log')).open('w'))
                child = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
                jobs.append((name, child))
                deadline = time.monotonic()+900
                while not (folder/'ready').exists():
                    if child.poll() is not None or interrupted:
                        raise RuntimeError(f'{name} stopped before first image; see log')
                    if time.monotonic() > deadline:
                        raise TimeoutError(f'{name} initialization timed out')
                    time.sleep(.1)
                print(f'{name} ready: {folder}/latest.jpg', flush=True)
            deadline = time.monotonic()+max(180, args.frames*3)
            while any(child.poll() is None for _, child in jobs):
                if time.monotonic() > deadline:
                    raise TimeoutError('Capture exceeded time limit')
                # Preserve other cameras when one fails; report each result.
                time.sleep(.2)
            for name, child in jobs:
                report['cameras'][name] = {'return_code': child.returncode}
                if child.returncode == 0:
                    report['cameras'][name].update(summarize(args.output/name))
            report['status'] = 'checked' if all(c['return_code'] == 0 for c in report['cameras'].values()) else 'failed'
            report['interrupted'] = interrupted
        except Exception as error:
            report.update(status='failed', error=str(error))
        finally:
            for name, child in jobs:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                report.setdefault('exit_codes', {})[name] = child.returncode
            (args.output/'run.json').write_text(json.dumps(report, indent=2)+'\n')
            seal(args.output)
        print(json.dumps(report, ensure_ascii=False), flush=True)
        return 0 if report['status'] == 'checked' else 1


if __name__ == '__main__':
    raise SystemExit(main())
