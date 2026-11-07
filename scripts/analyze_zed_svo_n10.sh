#!/usr/bin/env bash
# Check official depth offline from one SVO without opening a physical camera.
set -euo pipefail
[[ $(uname -m) = aarch64 && $# = 2 ]] || {
    echo 'Usage on Thor: analyze_zed_svo_n10.sh INPUT.svo2 NEW_OUTPUT_DIR' >&2; exit 1;
}
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ZED_DIR:-$root/evidence/zed-sdk-5.4/extracted}
deps="$(dirname "$sdk")/deps/usr/lib/aarch64-linux-gnu"
[[ -f "$1" && ! -e "$2" ]] || { echo 'Input missing or output already exists' >&2; exit 1; }
binary=$(mktemp /tmp/n10-svo-check.XXXXXX)
trap 'rm -f "$binary"' EXIT
g++ -std=c++17 -O2 "$root/source/analyze_zed_svo_n10.cpp" \
    -isystem "$sdk/include" -isystem /usr/local/cuda/include -L"$sdk/lib" \
    -Wl,-rpath,"$sdk/lib:$deps" -Wl,-rpath-link,"$deps" -lsl_zed -o "$binary"
env ZED_DIR="$sdk" LD_LIBRARY_PATH="$sdk/lib:$deps${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$binary" "$1" "$2"
python3 "$root/scripts/session_integrity.py" seal "$2"
