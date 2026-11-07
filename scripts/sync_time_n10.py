#!/usr/bin/env python3
"""Set N10 time from this PC over the existing SSH link and verify the offset."""
import argparse
import json
import select
import shlex
import subprocess
import time

REMOTE = r'''
import fcntl, glob, json, subprocess, sys, time
lock = open('/home/nvidia/ZEDX_N10_IMU/.capture.lock', 'r')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
devices = glob.glob('/dev/video*')
if devices:
    busy = subprocess.run(['fuser', *devices], capture_output=True)
    if busy.returncode != 1:
        raise RuntimeError('Stop camera consumers before changing system time')
print('ready', flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request['action'] == 'get':
        print(time.time_ns(), flush=True)
    elif request['action'] == 'set':
        time.clock_settime_ns(time.CLOCK_REALTIME, int(request['ns']))
        print('set', flush=True)
    elif request['action'] == 'rtc':
        subprocess.run(['timedatectl', 'set-timezone', 'Asia/Shanghai'], check=True)
        subprocess.run(['hwclock', '--systohc', '--utc'], check=True)
        print('rtc-written', flush=True)
        break
'''


def sync(host='thor'):
    command = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host,
               shlex.join(['sudo', '-n', 'python3', '-u', '-c', REMOTE])]
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, bufsize=1) as child:
        def read():
            if not select.select([child.stdout], [], [], 15)[0]:
                child.kill()
                raise TimeoutError('N10 time synchronization timed out')
            line = child.stdout.readline().strip()
            if not line:
                raise RuntimeError('N10 time synchronization failed')
            return line

        def request(data):
            child.stdin.write(json.dumps(data) + '\n')
            child.stdin.flush()
            return read()

        def measure():
            start = time.time_ns()
            remote = int(request({'action': 'get'}))
            end = time.time_ns()
            return (end - start, remote - (start + end) / 2)

        if read() != 'ready':
            raise RuntimeError('Invalid synchronization handshake')
        before = min(measure() for _ in range(5))
        request({'action': 'set', 'ns': time.time_ns() + before[0] // 2})
        after = min(measure() for _ in range(5))
        if abs(after[1]) > 50_000_000:
            raise RuntimeError(f'N10 clock verification failed: offset {after[1]/1e6:.3f} ms')
        if request({'action': 'rtc'}) != 'rtc-written':
            raise RuntimeError('Hardware clock update failed')
        child.stdin.close()
        if child.wait(timeout=10):
            raise RuntimeError('Remote clock synchronization failed')
    return {'source': 'PC system clock over SSH', 'timezone': 'Asia/Shanghai',
            'offset_before_ms': before[1] / 1e6, 'offset_after_ms': after[1] / 1e6,
            'round_trip_ms': after[0] / 1e6, 'rtc_written': True,
            'pc_unix_ns': time.time_ns()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='thor')
    args = parser.parse_args()
    print(json.dumps(sync(args.host), indent=2))
