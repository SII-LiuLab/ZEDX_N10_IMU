"""Environment for the N10-validated official SDK, without system installation."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sdk_environment():
    env = os.environ.copy()
    sdk = Path(env.get('ZED_DIR', ROOT/'evidence/zed-sdk-5.4/extracted')).resolve()
    # Optional Ubuntu libturbojpeg dependency extracted beside the SDK.
    deps = sdk.parent/'deps/usr/lib/aarch64-linux-gnu'
    libraries = [str(sdk/'lib')]
    if deps.is_dir():
        libraries.append(str(deps))
    if env.get('LD_LIBRARY_PATH'):
        libraries.append(env['LD_LIBRARY_PATH'])
    env['ZED_DIR'] = str(sdk)
    env['LD_LIBRARY_PATH'] = ':'.join(libraries)
    env.setdefault('ZED_SDK_VIDEO_CAPTURE_VERSION', '4')
    return env
