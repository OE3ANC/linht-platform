#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Runs the LinHT UI on a PC: the real linht-settingsd and a mock shell on a private session bus,
# apps in a 160x128 SDL window (zoom 4). Keys: arrows, Enter, Esc. Close the launcher window to quit.
#   nix develop -c cmake -S . -B build-sim -DLINHT_SIM=ON && nix develop -c cmake --build build-sim
#   nix develop -c sim/run.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${LINHT_SIM_BUILD:-$ROOT/build-sim}
STORE=${LINHT_SIM_STORE:-$(mktemp -d)} # settings persist only if you set LINHT_SIM_STORE
export LINHT_BUS=session
exec dbus-run-session --config-file="$ROOT/settingsd/tests/session.conf" -- sh -c '
	"$1/settingsd/linht-settingsd" --session --schemas "$2/schemas" --store "$3" --device-user "$(id -un)" &
	python3 "$2/sim/mock-shell.py" "$2/apps" "$1/apps"
	kill $! 2>/dev/null' sh "$BUILD" "$ROOT" "$STORE"
