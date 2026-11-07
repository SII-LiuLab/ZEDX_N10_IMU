import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import build_opener, ProxyHandler

urlopen = build_opener(ProxyHandler({})).open

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from live_preview import PreviewServer, read_snapshot
from capture_remote_n10 import parse_args


class RunningCamera:
    def poll(self):
        return None


class PreviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root/'zed').mkdir()
        self.server = PreviewServer(self.root, ['zed'], {'zed': RunningCamera()}, {}, 10)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def publish(self, sequence=1, age=0):
        self.metadata = {'sequence': sequence, 'timestamp_ns': 123,
                         'host_monotonic_ns': time.monotonic_ns()-int(age*1e9),
                         'sizes': [3, 5]}
        path = self.root/'zed'/'preview-tmp.bin'
        path.write_bytes(json.dumps(self.metadata).encode()+b'\nRGBDEPTH')
        path.replace(self.root/'zed'/'preview.bin')

    def test_selection_rgb_depth_and_newest_frame(self):
        self.publish()
        with urlopen(self.url+'/api/status') as response:
            cameras = json.load(response)['cameras']
        self.assertEqual(list(cameras), ['zed'])
        self.assertEqual(cameras['zed']['views'], ['rgb', 'depth'])
        self.assertEqual(urlopen(self.url+'/frame/zed/rgb').read(), b'RGB')
        self.assertEqual(urlopen(self.url+'/frame/zed/depth').read(), b'DEPTH')
        with urlopen(self.url+'/frame/zed/rgbd') as response:
            self.assertEqual(response.read(), b'RGBDEPTH')
            self.assertEqual(response.headers['X-RGB-Bytes'], '3')
        self.assertEqual(urlopen(self.url+'/frame/zed/rgb?after=1').status, 204)
        self.publish(8)
        with urlopen(self.url+'/frame/zed/rgb?after=1') as response:
            self.assertEqual(response.headers['X-Sequence'], '8')
        with self.assertRaises(HTTPError) as failure:
            urlopen(self.url+'/frame/shw_v6/rgb')
        self.assertEqual(failure.exception.code, 404)

    def test_stale_and_exited_workers_do_not_serve_old_images(self):
        self.publish(age=3)
        with self.assertRaises(HTTPError) as failure:
            urlopen(self.url+'/frame/zed/rgb')
        self.assertEqual(failure.exception.code, 503)
        self.publish()
        self.server.jobs.clear()
        with self.assertRaises(HTTPError):
            urlopen(self.url+'/frame/zed/rgb')

    def test_incomplete_pair_rejected(self):
        self.publish()
        path = self.root/'zed'/'preview.bin'
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            read_snapshot(path.parent)

    def test_shw_has_no_depth_endpoint(self):
        self.server.cameras = ['shw_v6']
        with urlopen(self.url+'/api/status') as response:
            self.assertEqual(json.load(response)['cameras']['shw_v6']['views'], ['rgb'])
        with self.assertRaises(HTTPError) as failure:
            urlopen(self.url+'/frame/shw_v6/depth')
        self.assertEqual(failure.exception.code, 404)

    def test_bad_cli_selection_rejected_before_ssh(self):
        cli = Path(__file__).resolve().parents[1]/'scripts/capture_remote_n10.py'
        for args in (['--cameras', 'zed', 'zed'], ['--cameras', 'unknown'],
                     ['--preview-width', '961'], ['--preview-fps', '0'], ['--duration', 'nan']):
            result = subprocess.run([sys.executable, str(cli), 'live', *args], capture_output=True)
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_selected_cameras_and_live_options_accepted(self):
        with patch.object(sys, 'argv', ['capture_remote_n10.py', 'live', '--cameras', 'shw_v7', 'zed',
                                      '--preview-width', '640', '--preview-fps', '15', '--no-browser']):
            args = parse_args()
        self.assertEqual(args.cameras, ['shw_v7', 'zed'])
        self.assertEqual((args.preview_width, args.preview_fps), (640, 15))


if __name__ == '__main__':
    unittest.main()
