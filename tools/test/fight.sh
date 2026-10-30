#!/bin/bash
# Usage: fight.sh <name> [VAR=value ...] [game arguments such as --fps 60]
# Fight benchmark: into gameplay from the save, then 200 s of attacks and movement. Prints the AR_PROFILE frame rate
# lines and the hitch count; the log is run/fight_<name>.log and screenshots (every 300 frames) are in run/screenshots.
# Check that nothing else is using the CPU first (other games skew the numbers).
source "$(dirname "$0")/common.sh"
name=$1
shift
split_args "$@"
cd "$RUN" || exit 1
clear_dumps
FIGHT="X,Y,X,X,Y,MOVE90,X,Y,X,X,Y,MOVE270,X,Y,X,X,Y,MOVE0,X,Y,X,X,Y"
env "${ENV_ARGS[@]}" AR_PROFILE=1 AR_SCREENSHOT_INTERVAL=300 AR_AUTO_PRESS=6 \
    AR_AUTO_SEQUENCE="$TO_GAMEPLAY,$FIGHT,$FIGHT,$FIGHT,$FIGHT,$FIGHT,$FIGHT" \
    timeout 200 "$EXE" --game "$ISO" --screenshots "${GAME_ARGS[@]}" > "fight_$name.log" 2>&1
echo "exit $? (124 = stopped by the timeout, as intended)"
grep -o "profile: [0-9.]* fps" "fight_$name.log" | tr '\n' ' '
echo
echo "hitches (frames over 100 ms): $(grep -c hitch "fight_$name.log")"
