"""Own the remote live session and SSH tunnel for its entire lifetime."""
import json
import queue
import shlex
import signal
import socket
import subprocess
import threading
import time
from urllib.request import build_opener, ProxyHandler
import webbrowser


def run(args, remote_root):
    local_http = build_opener(ProxyHandler({}))
    stop, ready = threading.Event(), queue.Queue()
    remote = tunnel = None
    previous = {}
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, lambda *_: stop.set())
    options = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'AddressFamily=any', '-o', 'ConnectTimeout=8',
               '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2']
    command = ['sudo', '-n', 'python3', '-u', remote_root+'/scripts/capture_three_n10.py',
               '--live', '--watch-stdin', '--cameras', *args.cameras,
               '--preview-fps', str(args.preview_fps), '--preview-width', str(args.preview_width),
               '--duration', str(args.duration)]
    try:
        remote = subprocess.Popen([*options, args.host, shlex.join(command)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  bufsize=1, start_new_session=True)
        def output():
            for line in remote.stdout:
                if line.startswith('N10_LIVE_READY '):
                    ready.put(json.loads(line.removeprefix('N10_LIVE_READY ')))
                else:
                    print(line.rstrip(), flush=True)
        def heartbeat():
            while not stop.is_set():
                try:
                    remote.stdin.write('heartbeat\n')
                    remote.stdin.flush()
                except (OSError, ValueError):
                    return
                stop.wait(3)
        reader = threading.Thread(target=output, daemon=True)
        reader.start()
        beat = threading.Thread(target=heartbeat, daemon=True)
        beat.start()
        deadline = time.monotonic()+30
        info = None
        while not stop.is_set() and remote.poll() is None and time.monotonic() < deadline:
            try:
                info = ready.get(timeout=.2)
                break
            except queue.Empty:
                pass
        if info is None:
            if stop.is_set():
                return 0
            raise RuntimeError('Remote preview did not start; check the camera/SSH error above')
        port = args.port
        if not port:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
        tunnel = subprocess.Popen([*options, '-N', '-o', 'ExitOnForwardFailure=yes',
                                   '-L', f'127.0.0.1:{port}:127.0.0.1:{info["port"]}', args.host],
                                  stdin=subprocess.DEVNULL, start_new_session=True)
        url = f'http://127.0.0.1:{port}/'
        deadline = time.monotonic()+15
        connected = False
        while not stop.is_set() and time.monotonic() < deadline:
            if tunnel.poll() is not None or remote.poll() is not None:
                raise RuntimeError('SSH preview connection exited')
            try:
                with local_http.open(url+'api/status', timeout=1) as response:
                    connected = response.status == 200
                if connected:
                    break
            except OSError:
                stop.wait(.2)
        if not connected:
            if stop.is_set():
                return 0
            raise RuntimeError('Could not connect to the forwarded preview port')
        print(f'Live preview: {url}\nSelected cameras: {", ".join(args.cameras)}\nCtrl+C stops this live session.', flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        while not stop.wait(.2):
            if remote.poll() is not None:
                return remote.returncode
            if tunnel.poll() is not None:
                raise RuntimeError('SSH tunnel disconnected')
        return 0
    finally:
        stop.set()
        if remote is not None:
            # EOF asks the remote owner to stop its workers and remove tmpfs data.
            if remote.stdin:
                try:
                    remote.stdin.close()
                except (OSError, ValueError):
                    pass
            try:
                remote.wait(timeout=40)
            except subprocess.TimeoutExpired:
                remote.terminate()
                try:
                    remote.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    remote.kill()
                    remote.wait()
            reader.join(timeout=2)
        if tunnel is not None:
            if tunnel.poll() is None:
                tunnel.terminate()
            try:
                tunnel.wait(timeout=5)
            except subprocess.TimeoutExpired:
                tunnel.kill()
                tunnel.wait()
        for signum, handler in previous.items():
            signal.signal(signum, handler)
