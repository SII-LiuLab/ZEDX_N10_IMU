"""Latest-frame browser preview; standard library only, bound to loopback."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from urllib.parse import urlsplit, parse_qs


def read_snapshot(folder):
    # One open of one atomically replaced file keeps RGB and depth consistent.
    with (Path(folder)/'preview.bin').open('rb') as source:
        header = source.readline(4096)
        metadata = json.loads(header)
        sizes = metadata['sizes']
        if len(sizes) not in (1, 2) or any(not isinstance(n, int) or not 0 < n <= 8_000_000 for n in sizes):
            raise ValueError('Invalid preview payload size')
        images = [source.read(n) for n in sizes]
        if any(len(image) != size for image, size in zip(images, sizes)) or source.read(1):
            raise ValueError('Incomplete preview snapshot')
    metadata['age_ms'] = max(0, (time.monotonic_ns()-metadata['host_monotonic_ns'])/1e6)
    return metadata, images


class PreviewServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, root, cameras, jobs, errors, fps, port=0):
        self.root, self.cameras = Path(root), list(cameras)
        self.jobs, self.errors, self.fps = jobs, errors, fps
        super().__init__(('127.0.0.1', port), PreviewHandler)

    def status(self):
        result = {}
        for name in self.cameras:
            job = self.jobs.get(name)
            entry = {'state': 'warming', 'views': ['rgb', 'depth'] if name == 'zed' else ['rgb']}
            try:
                metadata, _ = read_snapshot(self.root/name)
                entry.update(metadata)
                entry['state'] = 'live' if metadata['age_ms'] < 2000 else 'stale'
            except (OSError, ValueError, KeyError):
                pass
            if name in self.errors or (job is not None and job.poll() is not None):
                entry['state'] = 'failed' if name in self.errors or job.returncode else 'stopped'
                entry['error'] = self.errors.get(name, 'Camera process exited')
            result[name] = entry
        return {'cameras': result, 'fps_limit': self.fps}


class PreviewHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def reply(self, code, data=b'', content_type='application/json', headers=None):
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        for key, value in (headers or {}).items():
            self.send_header(key, str(value))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def do_GET(self):
        try:
            self.route()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass

    def route(self):
        url = urlsplit(self.path)
        if url.path == '/':
            page = Path(__file__).with_name('live_preview.html').read_bytes()
            return self.reply(200, page, 'text/html; charset=utf-8')
        if url.path == '/api/status':
            return self.reply(200, json.dumps(self.server.status()).encode())
        parts = url.path.strip('/').split('/')
        if len(parts) != 3 or parts[0] != 'frame' or parts[1] not in self.server.cameras:
            return self.reply(404)
        name, view = parts[1:]
        allowed = ['rgb', 'depth'] if name == 'zed' else ['rgb']
        paired = name == 'zed' and view == 'rgbd'
        if view not in allowed and not paired:
            return self.reply(404)
        job = self.server.jobs.get(name)
        if job is None or job.poll() is not None:
            return self.reply(503)
        try:
            metadata, images = read_snapshot(self.server.root/name)
            if metadata['age_ms'] >= 2000:
                return self.reply(503)
            if parse_qs(url.query).get('after', [''])[0] == str(metadata['sequence']):
                return self.reply(204)
        except (OSError, ValueError, KeyError):
            return self.reply(503)
        headers = {'X-Sequence': metadata['sequence'], 'X-Frame-Age-Ms': round(metadata['age_ms'], 2),
                   'X-Sensor-Timestamp-Ns': metadata['timestamp_ns']}
        if paired:
            headers['X-RGB-Bytes'] = len(images[0])
            self.reply(200, b''.join(images), 'application/octet-stream', headers)
        else:
            self.reply(200, images[allowed.index(view)], 'image/jpeg', headers)


def run_live(args, env, capture_command):
    """Called while capture_three_n10 holds its existing camera/init locks."""
    root = args.output
    root.mkdir(parents=True)
    stop = threading.Event()
    jobs, errors, logs = {}, {}, []
    heartbeat = [time.monotonic()]
    def request_stop(*_):
        stop.set()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    if args.watch_stdin:
        def watch():
            for _ in sys.stdin:
                heartbeat[0] = time.monotonic()
            stop.set()
        threading.Thread(target=watch, daemon=True).start()
    def stopping():
        return stop.is_set() or (args.watch_stdin and time.monotonic()-heartbeat[0] > 15)
    env.update(N10_LIVE='1', N10_PREVIEW_FPS=str(args.preview_fps), N10_PREVIEW_WIDTH=str(args.preview_width))
    server = PreviewServer(root, args.cameras, jobs, errors, args.preview_fps, args.preview_port)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print('N10_LIVE_READY '+json.dumps({'port': server.server_port, 'cameras': args.cameras}), flush=True)
    try:
        for name in args.cameras:
            if stopping():
                break
            log = (root/(name+'.log')).open('w')
            logs.append(log)
            jobs[name] = subprocess.Popen(capture_command(name, root/name, args), env=env,
                                          stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic()+120
            while not (root/name/'preview.bin').exists() and not stopping():
                if jobs[name].poll() is not None:
                    errors[name] = (root/(name+'.log')).read_text(errors='replace')[-1500:]
                    break
                if time.monotonic() > deadline:
                    errors[name] = 'Camera warmup timed out'
                    jobs[name].terminate()
                    break
                stop.wait(.1)
            print(f'{name}: '+('failed: '+errors[name] if name in errors else
                             'stopping' if stopping() else 'ready'), flush=True)
        deadline = time.monotonic()+args.duration if args.duration else float('inf')
        while not stopping() and time.monotonic() < deadline:
            for name, child in jobs.items():
                if child.poll() is not None and name not in errors:
                    errors[name] = (root/(name+'.log')).read_text(errors='replace')[-1500:]
            if jobs and all(child.poll() is not None for child in jobs.values()):
                break
            stop.wait(.2)
        print('N10_LIVE_SUMMARY '+json.dumps(server.status()), flush=True)
    finally:
        for child in jobs.values():
            if child.poll() is None:
                child.terminate()
        for child in jobs.values():
            try:
                child.wait(timeout=25)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
        for log in logs:
            log.close()
    return 1 if errors else 0
