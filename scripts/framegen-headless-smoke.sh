#!/bin/sh
set -eu

if ! command -v vkcube >/dev/null 2>&1; then
	echo "vkcube is required" >&2
	exit 1
fi

gamescope_bin=${GAMESCOPE_BIN:-build/src/gamescope}
control=${FRAMEGEN_CONTROL_COMMAND:-}
gamescope_dir=$(dirname "$gamescope_bin")

# An uninstalled build resolves its helper through PATH just like an installed
# Gamescope does. This keeps the smoke test usable directly from a Meson tree.
PATH=$gamescope_dir:$PATH
export PATH

"$gamescope_bin" --backend headless -W 1280 -H 720 -r 120 -- vkcube &
gamescope_pid=$!
trap 'kill "$gamescope_pid" 2>/dev/null || true' EXIT INT TERM

sleep 2
if [ -n "$control" ]; then
	sh -c "$control"
else
	echo "Set GAMESCOPE_FRAME_GENERATION_ENABLED=1 with the active X11/Wayland controller." >&2
fi

sleep 5
kill -0 "$gamescope_pid"
echo "headless frame-generation smoke session stayed alive"
