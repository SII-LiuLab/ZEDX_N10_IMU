"""Exercise real session ownership with small substitute camera processes."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.request import build_opener, ProxyHandler


WORKER = '''
import json,os,sys,time
from pathlib import Path
p=Path(sys.argv[1]);p.mkdir()
if sys.argv[2]=='failure' and p.name=='shw_v6':
 print('simulated camera failure',flush=True);raise SystemExit(1)
(p/'child.pid').write_text(str(os.getpid()))
header={'sequence':1,'timestamp_ns':1,'host_monotonic_ns':time.monotonic_ns(),'sizes':[3]}
(p/'preview.bin').write_bytes(json.dumps(header).encode()+b'\\nRGB')
time.sleep(100)
'''


class LiveLifecycleTests(unittest.TestCase):
    def check_session(self, method):
        scripts = Path(__file__).resolve().parents[1]/'scripts'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'session'
            runner = f'''
import os,sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,{str(scripts)!r})
from live_preview import run_live
a=SimpleNamespace(output=Path(sys.argv[1]),cameras=['shw_v6','shw_v7'] if sys.argv[2]=='failure' else ['shw_v7'],
                  watch_stdin=True,preview_fps=10,preview_width=960,preview_port=0,duration=0)
def command(name,folder,args):
 return [sys.executable,'-c',{WORKER!r},str(folder),sys.argv[2]]
raise SystemExit(run_live(a,os.environ.copy(),command))
'''
            process = subprocess.Popen([sys.executable, '-u', '-c', runner, str(root), method],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                line = process.stdout.readline()
                self.assertTrue(line.startswith('N10_LIVE_READY '), line)
                info = json.loads(line.removeprefix('N10_LIVE_READY '))
                deadline = time.monotonic()+5
                pid_file = root/'shw_v7'/'child.pid'
                while not pid_file.exists() and time.monotonic()<deadline:
                    time.sleep(.02)
                self.assertTrue(pid_file.exists())
                child_pid = int(pid_file.read_text())
                if method == 'failure':
                    with build_opener(ProxyHandler({})).open(f'http://127.0.0.1:{info["port"]}/api/status') as response:
                        cameras = json.load(response)['cameras']
                    self.assertEqual(cameras['shw_v6']['state'], 'failed')
                    self.assertEqual(cameras['shw_v7']['state'], 'live')
                if method == 'signal':
                    process.send_signal(signal.SIGTERM)
                elif method != 'heartbeat':
                    process.stdin.close()
                self.assertEqual(process.wait(timeout=20 if method == 'heartbeat' else 8),
                                 1 if method == 'failure' else 0, process.stdout.read())
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if not process.stdin.closed:
                    process.stdin.close()
                process.stdout.close()

    def test_ssh_input_eof_stops_owned_camera(self):
        self.check_session('eof')

    def test_terminal_signal_stops_owned_camera(self):
        self.check_session('signal')

    def test_one_failed_camera_preserves_other_camera(self):
        self.check_session('failure')

    def test_missing_heartbeat_stops_owned_camera(self):
        self.check_session('heartbeat')


if __name__ == '__main__':
    unittest.main()
