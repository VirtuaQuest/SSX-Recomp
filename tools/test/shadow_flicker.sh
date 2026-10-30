#!/bin/bash
# Usage: shadow_flicker.sh <name> ["ENV=1 ..."]
# Walks around Beer Street dumping the far shadow cascade (resolve 0F801000) every frame from 2950 for up to 400
# frames, then scores frames that flip back and forth (altscore.py, period.py). Results vary from run to run (0-16
# events), so compare several runs. Add AR_SHADOW_LOG=1 to log every shadow map draw for drawsets.py/worldcmp.py.
source "$(dirname "$0")/common.sh"
name=$1
cd "$RUN" || exit 1
clear_dumps
env $2 AR_DUMP_ALPHA=1 AR_DUMP_ONLY=0F801000 AR_DUMP_FRAME=2950 AR_DUMP_FRAMES=400 AR_SCREENSHOT_INTERVAL=100000 \
    AR_AUTO_PRESS=6 AR_AUTO_SEQUENCE="$TO_GAMEPLAY,MOVE90,MOVE90,MOVE270,MOVE270,MOVE180,MOVE0" \
    timeout 170 "$EXE" --game "$ISO" --screenshots > "flicker_$name.log" 2>&1
python "$TOOLS/altscore.py" screenshots
python "$TOOLS/period.py" screenshots
