#!/bin/bash

set -e

if [[ ! "${DISPLAY:-}" =~ ^:[0-9]+(\.[0-9]+)?$ ]]; then
    echo 1>&2 "warning: not launching deadman xwrits because DISPLAY is not a local display (DISPLAY=${DISPLAY:-})"
    exit 1
fi

script_dir="$(dirname "$(readlink -f "$0")")"

activity_log_args=()

if xwrits --help 2>&1 | grep -q activity-log; then
    activity_log_args=(--activity-log "$HOME/.xwrits_activity")
fi

if [ -f "$script_dir/libxwrits-sendevent.so" ]; then
    LD_PRELOAD="$script_dir/libxwrits-sendevent.so${LD_PRELOAD:+:$LD_PRELOAD}" \
        xwrits "${activity_log_args[@]}" typetime="${TYPETIME:-40}" breaktime="${BREAKTIME:-4}" +mouse +breakclock +finger=german +beep +clock after 5 multiply maxhands=6 after 5 flashtime=:0.4 after 1 maxhands=2000 &
else
    xwrits "${activity_log_args[@]}" typetime="${TYPETIME:-40}" breaktime="${BREAKTIME:-4}" +mouse +breakclock +finger=german +beep +clock after 5 multiply maxhands=6 after 5 flashtime=:0.4 after 1 maxhands=2000 &
fi

xwrits_pid=$!

"$script_dir/wayland-xwrits-bridge" &
bridge_pid=$!

sudo insmod "$script_dir/deadman.ko" || true

echo "$xwrits_pid" | sudo tee /proc/deadman
echo "$bridge_pid" | sudo tee /proc/deadman

cat /proc/deadman
