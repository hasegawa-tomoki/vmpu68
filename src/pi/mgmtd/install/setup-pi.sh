#!/bin/bash
# SPDX-License-Identifier: MIT
# vmpu68 Pi setup — run ON the Raspberry Pi from the repo root:
#   sudo bash pi/mgmtd/install/setup-pi.sh
set -e
cd "$(dirname "$0")/../../.."
REPO=$(pwd)
echo "repo: $REPO"

apt-get update -qq
apt-get install -y -qq gcc make python3

make -C "$REPO/pi/mgmtd" clean || true
make -C "$REPO/pi/core" clean || true
make -C "$REPO/pi/mgmtd" web_ui.h
make -C "$REPO/pi/mgmtd"
make -C "$REPO/pi/pi"

id -u vmpu68 >/dev/null 2>&1 || useradd -r -M -G gpio vmpu68
mkdir -p /opt/vmpu68
install -m 755 "$REPO/pi/mgmtd/mgmtd" /opt/vmpu68/
install -m 755 "$REPO/pi/pi/flash68" "$REPO/pi/pi/buscheck" /opt/vmpu68/
chown -R vmpu68:vmpu68 /opt/vmpu68

install -m 644 "$REPO/pi/mgmtd/install/vmpu68-mgmtd.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now vmpu68-mgmtd

sleep 1
systemctl --no-pager -l status vmpu68-mgmtd | head -8
echo
echo "OK — web UI:  http://$(hostname).local/  (or http://<this-pi-ip>/)"
