#!/bin/bash

set -e

test "$DISPLAY" = ":0" || {
    echo 1>&2 "warning: not launching deadman xwrits because DISPLAY is not :0 (DISPLAY=$DISPLAY)"
    exit 1
}

activity_log_args=()

if xwrits --help 2>&1 | grep -q activity-log; then
    activity_log_args=(--activity-log "$HOME/.xwrits_activity")
fi

xwrits "${activity_log_args[@]}" typetime="${TYPETIME:-40}" breaktime="${BREAKTIME:-4}" +mouse  +breakclock +finger=german  +beep +clock after 5 multiply maxhands=6 after 5 flashtime=:0.4 after 1 maxhands=2000 &

pid=$!

sudo insmod "$(dirname "$(readlink -f "$0")")/deadman.ko" || true

echo "$pid" | sudo tee /proc/deadman

cat /proc/deadman
