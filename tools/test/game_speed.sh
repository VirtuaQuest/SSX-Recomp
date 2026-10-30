#!/bin/bash
# Usage: game_speed.sh <name> ["--fps 60"]
# Measures how fast the world moves per real second while the player runs (from the far shadow cascade's light
# matrix, which follows the camera), to check the game runs at normal speed at a frame rate setting. Compare the
# peak of the histogram with a run at --fps 30 (about 9 when running straight in Beer Street).
source "$(dirname "$0")/common.sh"
name=$1
cd "$RUN" || exit 1
AR_DUMP_ONLY=1 AR_SHADOW_LOG=1 AR_DUMP_FRAME=0 AR_DUMP_FRAMES=100000 AR_AUTO_PRESS=6 \
    AR_AUTO_SEQUENCE="$TO_GAMEPLAY,MOVE0,MOVE0,MOVE0,MOVE0,MOVE180,MOVE180,MOVE180,MOVE180,MOVE0,MOVE0,MOVE0,MOVE0" \
    timeout 200 "$EXE" --game "$ISO" $2 > "speed_$name.log" 2>&1
python "$TOOLS/speed.py" "speed_$name.log"
