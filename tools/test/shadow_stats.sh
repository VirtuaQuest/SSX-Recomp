#!/bin/bash
# Usage: shadow_stats.sh <name> [VAR=value ...] [game arguments]
# Continues the campaign from the save and runs back and forth, logging per far cascade draw its samples and pipeline
# statistics (PX lines, AR_SHADOW_PIXELS, AR_SHADOW_STATS), then lists draws that wrote nothing for a frame (pxdrop.py).
# Only those lines are kept: the whole log is hundreds of MB. Keep off the controller: runs read it too.
source "$(dirname "$0")/common.sh"
name=$1
shift
split_args "$@"
cd "$RUN" || exit 1
{
    printf '%s\n' 15000 A 3000 A 3000 A 5000 A 12000
    for i in 1 2 3 4 5 6; do printf '%s\n' "L0 4000" "L180 4000"; done
} > "remote_stats_$name.txt"
env AR_REMOTE="remote_stats_$name.txt" AR_SHADOW_LOG=1 AR_SHADOW_PIXELS=3 AR_SHADOW_STATS=1 AR_DUMP_ONLY=1 AR_DUMP_FRAME=0 AR_DUMP_FRAMES=100000 \
    "${ENV_ARGS[@]}" timeout 95 "$EXE" --game "$ISO" "${GAME_ARGS[@]}" 2>&1 | grep -E ': PX [0-9]+ 0 |: SM |: (EV|SC|SR|FW) |Crash' > "stats_$name.log"
rm -f "remote_stats_$name.txt"
python "$TOOLS/pxdrop.py" "stats_$name.log"
