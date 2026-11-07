#!/usr/bin/env bash
# Build the parallel SHW RGB and official ZED RGB-D capture workers.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ZED_DIR:-$root/evidence/zed-sdk-5.4/extracted}
deps="$(dirname "$sdk")/deps/usr/lib/aarch64-linux-gnu"
[[ $(uname -m) = aarch64 ]] || { echo 'Build on Thor' >&2; exit 1; }
[[ -f "$sdk/include/sl/Camera.hpp" && -f "$sdk/lib/libsl_zed.so" ]] || {
    echo 'Set ZED_DIR to the official L4T38.2 SDK directory' >&2; exit 1;
}
mkdir -p "$root/build"
# Vendor OpenCV's pkg-config points at /usr/local on some N10 images.
opencv_include=$(pkg-config --variable=includedir opencv4)
[[ -f "$opencv_include/opencv2/core.hpp" ]] || opencv_include=/usr/include/opencv4
[[ -f "$opencv_include/opencv2/core.hpp" ]] || { echo 'OpenCV C++ headers required for live preview' >&2; exit 1; }
g++ -std=c++17 -O2 -Wall -Wextra "$root/source/capture_shw_n10.cpp" \
    -I/usr/src/jetson_multimedia_api/argus/include \
    -L/usr/lib/aarch64-linux-gnu/nvidia -lnvargus_socketclient \
    -o "$root/build/capture_shw_n10"
# Keep OpenCV/libjpeg out of the recording executable: the vendor Argus JPEG
# writer uses a different JPEG ABI. Both modes compile the same capture source.
g++ -std=c++17 -O2 -Wall -Wextra -pthread -DN10_ENABLE_PREVIEW "$root/source/capture_shw_n10.cpp" \
    -I/usr/src/jetson_multimedia_api/argus/include \
    -I/usr/src/jetson_multimedia_api/include \
    -I"$opencv_include" -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
    -L/usr/lib/aarch64-linux-gnu/nvidia -lnvargus_socketclient -lnvbufsurface \
    -o "$root/build/capture_shw_live_n10"
g++ -std=c++17 -O2 -Wall -Wextra -pthread "$root/source/capture_zed_n10.cpp" \
    -isystem "$sdk/include" -isystem /usr/local/cuda/include -L"$sdk/lib" \
    -Wl,-rpath,"$sdk/lib:$deps" -Wl,-rpath-link,"$deps" -lsl_zed -o "$root/build/capture_zed_n10"
