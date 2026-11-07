#!/usr/bin/env bash
# Manual initialization after booting the mixed DTB. No module installation.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $(id -u) = 0 && $(uname -r) = 6.8.12-rt-tegra ]] || { echo 'Requires sudo on the configured N10' >&2; exit 1; }
tr '\0' '\n' < /sys/firmware/devicetree/base/rb,dtbbuildversion | grep -q B25EJ21_007
tr '\0' '\n' < /sys/firmware/devicetree/base/n10-cam3-mixed/compatible | grep -qx n10,cam3-mixed-zedx
exec 9>"$root/.capture.lock"
flock -n 9 || { echo 'A unified capture/init is running' >&2; exit 1; }
if fuser /dev/video* /dev/spsc_bmi0 >/dev/null 2>&1; then
    echo 'Stop camera/IMU consumers first' >&2; exit 1
fi
[[ -x /usr/local/sbin/IMU_Daemon && -f /etc/systemd/system/n10-imu-sdk.service ]] || {
    echo 'Install the official IMU helper with install_sdk_imu_helper.sh first' >&2; exit 1;
}
systemctl stop n10-imu-sdk.service nvargus-daemon.service
platform="$root/vendor/shw5g"
[[ $(sha256sum "$platform/cfg/hb_j5dev.json" | cut -d' ' -f1) = d06293324d36cd73d75a5a076eb904eddfed069b8cc65727cd81a1c9a4e119c6 ]]
[[ $(sha256sum "$platform/lib/sensorlib/libyuv_cam.so" | cut -d' ' -f1) = 46e6effa11145da8d4fc085093d1027cdaa8b7c10c88c2306d0ef7d3f2c95719 ]]
if [[ -e /sys/class/bmi_spsc/spsc_bmi0/timer_control ]]; then echo 0 > /sys/class/bmi_spsc/spsc_bmi0/timer_control; fi
for module in bmi088 bmi_spsc sl_zedx sl_max9295 n10_cam3_mixed sl_max96712; do
    if [[ -d /sys/module/$module ]]; then rmmod "$module"; fi
done
# Vendor trigger setup required even when CAM-1 has no connected camera.
for node in /sys/class/video4linux/video*/name; do
    if grep -q 'vi-output, s56 ' "$node"; then
        v4l2-ctl -d "/dev/$(basename "$(dirname "$node")")" -c trig_pin=0x00020007,trig_mode=1
    fi
done
(
    cd "$platform"
    env LD_LIBRARY_PATH="$platform/lib:$platform/lib/sensorlib" \
        SHW5G_SENSOR_LIBRARY="$platform/lib/sensorlib/libyuv_cam.so" \
        LD_PRELOAD="$root/build/shw5g_init_compat.so" timeout 45s ./demo/init_demo 6
)
insmod "$root/build/n10_cam3_mixed.ko"
test -L /sys/bus/platform/devices/n10-cam3-mixed/driver
# insmod deliberately avoids auto-loading the conflicting single-ZED deserializer.
insmod "$(modinfo -n sl_max9295)"
test -L /sys/bus/i2c/devices/7-0062/driver
insmod "$root/build/sl_zedx.ko"
test -L /sys/bus/i2c/devices/7-0028/driver
test -L /sys/bus/i2c/devices/7-0020/driver
insmod "$root/driver/bmi_spsc.ko"
insmod "$root/driver/bmi088.ko"
test -L /sys/bus/i2c/devices/7-003b/driver
test -e /dev/spsc_bmi0
chmod 600 /dev/spsc_bmi0
systemctl start nvargus-daemon.service n10-imu-sdk.service
v4l2-ctl --list-devices
echo 'Mixed cameras initialized; run capture_three_n10.py for independent RGB / official RGB-D.'
