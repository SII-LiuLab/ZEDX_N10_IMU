#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $(uname -m) = aarch64 && $(uname -r) = 6.8.12-rt-tegra ]] || exit 1
oot=${N10_OOT_ROOT:-/home/nvidia/zedx-vendor-source/staged/code/source/kernel_src/nvidia-oot}
symvers=${N10_VERIFIED_SYMVERS:-/home/nvidia/abi_probe/verified.symvers}
mkdir -p "$root/build"
gcc -O2 -Wall -Wextra -shared -fPIC "$root/source/shw5g_init_compat_n10.c" -ldl -o "$root/build/shw5g_init_compat.so"
make -C "/lib/modules/$(uname -r)/build" M="$root/source/max96712_n10_mixed" \
    N10_OOT_INCLUDE="$oot/include" KBUILD_EXTRA_SYMBOLS="$symvers" -j8 modules
cp "$root/source/max96712_n10_mixed/n10_cam3_mixed.ko" "$root/build/"
grep sl_max9295 "${N10_SERIALIZER_SYMVERS:-/home/nvidia/zedx-n10-build/Module.symvers}" > "$root/build/serializer.symvers"
make -C "/lib/modules/$(uname -r)/build" M="$root/source/stereolabs/zedx" \
    N10_OOT_INCLUDE="$oot/include" \
    KBUILD_EXTRA_SYMBOLS="$symvers $root/source/max96712_n10_mixed/Module.symvers $root/build/serializer.symvers" -j8 modules
cp "$root/source/stereolabs/zedx/sl_zedx.ko" "$root/build/"
cd "$root/device-tree"
cpp -nostdinc -undef -x assembler-with-cpp -I . n10-cam3-mixed-overlay.dts > "$root/build/mixed.pp.dts"
dtc -@ -I dts -O dtb -o "$root/build/mixed.dtbo" "$root/build/mixed.pp.dts"
base=/boot/dtb/tegra264-p4071-0000+p3834-0008-nv.dtb
[[ $(sha256sum "$base" | cut -d' ' -f1) = 815546907068d5a83a749351f0ea34d9a0c5e7ac4065b59748c4081a8ec6f9eb ]] || { echo 'Unrecognized base DTB' >&2; exit 1; }
fdtoverlay -i "$base" -o "$root/build/n10-cam3-mixed.dtb" "$root/build/mixed.dtbo"
