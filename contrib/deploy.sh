#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Development loop (AGENTS.md, standing permission for milestone 1): copy this tree to the radio,
# build and test it there, install it to /usr and restart our services.
#   LINHT_HOST=root@10.17.17.17 LINHT_SSH_OPTS="-o UserKnownHostsFile=..." contrib/deploy.sh
set -eu
HOST=${LINHT_HOST:-root@10.17.17.17}
SSH="ssh -o BatchMode=yes ${LINHT_SSH_OPTS:-}"
$SSH "$HOST" mkdir -p /root/linht-dev/linht-launcher
rsync -a --delete --exclude '/build*' --exclude /.git --exclude /.claude -e "$SSH" \
	"$(dirname "$0")/../" "$HOST:/root/linht-dev/linht-launcher/"
$SSH "$HOST" 'set -e
cd /root/linht-dev/linht-launcher
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
# the radio clock is not set yet, so make warns about clock skew; keep the exit status of the build
cmake --build build -j2 >build.log 2>&1 || { grep -v -e "Clock skew" -e "in the future" build.log; exit 1; }
grep -v -e "Clock skew" -e "in the future" build.log || true
ctest --test-dir build --output-on-failure
cmake --install build >/dev/null
systemd-sysusers
# dbus-daemon drops policy rules for users that did not exist when it read the file
busctl call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
udevadm control --reload
udevadm trigger --action=change --subsystem-match=drm && udevadm settle # /dev/dri/linht-lcd
systemctl daemon-reload
systemctl enable --quiet linht-settingsd linht-shelld
systemctl restart linht-settingsd linht-shelld # restarting shelld also restarts the foreground app'
