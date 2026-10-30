#!/bin/bash
# Usage: trace_frame.sh <frame> ["ENV=1 ..."]
# Logs every draw and resolve of one gameplay frame (AR_TRACE_FRAME) and dumps its resolves to run/screenshots
# (resolve_<frame>_<index>_<address>.bmp), then writes a condensed summary to run/trace_<frame>_summary.txt.
# Frame ~2900 is Beer Street gameplay; the main menu image with Jack at the bar is around frame 1500.
source "$(dirname "$0")/common.sh"
frame=$1
cd "$RUN" || exit 1
clear_dumps
env $2 AR_TRACE_FRAME=$frame AR_SCREENSHOT_INTERVAL=100000 AR_AUTO_PRESS=6 AR_AUTO_SEQUENCE="$TO_GAMEPLAY" \
    timeout 140 "$EXE" --game "$ISO" --screenshots > "trace_$frame.log" 2>&1
python "$TOOLS/summ.py" "trace_$frame.log" > "trace_${frame}_summary.txt"
echo "$(grep -c 'draw prim' "trace_$frame.log") draws; summary in run/trace_${frame}_summary.txt"
