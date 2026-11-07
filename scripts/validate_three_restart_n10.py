#!/usr/bin/env python3
"""Hardware acceptance: independently restart each camera while the others grab."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import time
from session_integrity import seal

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if not args.output.is_absolute() or args.output.exists():
        parser.error('Use a new absolute directory')
    args.output.mkdir(parents=True)
    jobs, logs, events, generations = {}, [], [], {}
    def rows(name):
        folder, _ = jobs[name]
        path = folder/name/'frames.csv'
        if not path.exists():
            return []
        return [r for r in csv.DictReader(path.read_text().splitlines()) if r.get('saved') in ('0','1')]
    def event(action, name):
        events.append({'action': action, 'camera': name, 'monotonic_ns': time.monotonic_ns()})
        print(action, name, flush=True)
    def start(name):
        n = generations.get(name, 0)+1
        generations[name] = n
        folder = args.output/f'{name}-{n}'
        log = (args.output/f'{name}-{n}.log').open('w')
        logs.append(log)
        event('start_requested', name)
        process = subprocess.Popen(['python3', str(ROOT/'scripts/capture_three_n10.py'), str(folder),
                                    '--cameras', name, '--frames', '10000', '--save-every', '120'],
                                   stdout=log, stderr=subprocess.STDOUT)
        jobs[name] = (folder, process)
        deadline = time.monotonic()+90
        while len(rows(name)) < 5:
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f'{name} failed to start; inspect {log.name}')
            time.sleep(.1)
        event('receiving', name)
    def stop(name):
        folder, process = jobs[name]
        event('stop_requested', name)
        process.terminate()
        process.wait(timeout=45)
        if process.returncode:
            raise RuntimeError(f'{name} failed during stop: {process.returncode}')
        event('stopped', name)
        del jobs[name]
    def observe(action):
        before = {n: len(rows(n)) for n in jobs}
        begin = time.monotonic_ns()
        action()
        time.sleep(2)
        result = {}
        for name, count in before.items():
            if name not in jobs:
                continue
            data = rows(name)
            if len(data)-count < 20 or jobs[name][1].poll() is not None:
                raise RuntimeError(f'{name} stopped delivering during another camera transition')
            recent = [int(r['timestamp_ns']) for r in data[max(0,count-1):]]
            gap = max((b-a for a,b in zip(recent,recent[1:])), default=0)/1e6
            if gap > 500:
                raise RuntimeError(f'{name} frame gap during transition: {gap:.1f} ms')
            result[name] = {'new_frames': len(data)-count, 'max_gap_ms': gap}
        events.append({'action': 'continuity_check', 'begin_ns': begin,
                       'end_ns': time.monotonic_ns(), 'cameras': result})
    report = {'passed': False, 'events': events}
    try:
        start('zed')
        observe(lambda: start('shw_v6'))
        observe(lambda: start('shw_v7'))
        for name in ('shw_v6', 'zed', 'shw_v7'):
            observe(lambda name=name: stop(name))
            observe(lambda name=name: start(name))
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        for name in list(jobs):
            try:
                stop(name)
            except Exception as error:
                report['passed'] = False
                report.setdefault('cleanup_errors', []).append(str(error))
                process = jobs[name][1]
                if process.poll() is None:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()
        (args.output/'independence.json').write_text(json.dumps(report, indent=2)+'\n')
        seal(args.output)
    print(json.dumps(report), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
