#!/usr/bin/env bash
# Install only the official user-space IMU helper from an extracted driver package.
# Do not install another board's kernel, modules or device tree.
set -euo pipefail
[[ $(id -u) = 0 && $(uname -m) = aarch64 && $# = 1 ]] || {
    echo 'Usage on Thor: sudo bash install_sdk_imu_helper.sh /path/to/official/IMU_Daemon' >&2; exit 1;
}
root=$(cd "$(dirname "$0")/.." && pwd)
exec 9>"$root/.capture.lock"
flock -n 9 || { echo 'Stop camera capture first' >&2; exit 1; }
readelf -h "$1" | grep -q 'Machine:.*AArch64'
if fuser /dev/video* /dev/spsc_bmi0 >/dev/null 2>&1; then
    echo 'Stop camera/IMU consumers first' >&2; exit 1
fi
systemctl stop n10-imu-sdk.service 2>/dev/null || true
install -m 0755 "$1" /usr/local/sbin/IMU_Daemon
cat > /etc/systemd/system/n10-imu-sdk.service <<'EOF'
[Unit]
Description=Official ZED IMU helper for N10
After=nvargus-daemon.service
[Service]
Type=simple
ExecStart=/usr/local/sbin/IMU_Daemon
Restart=on-failure
RestartSec=2
EOF
systemctl daemon-reload
sha256sum /usr/local/sbin/IMU_Daemon
echo 'Helper installed. load_mixed_n10.sh starts it after the camera/IMU modules.'
