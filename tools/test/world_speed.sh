#!/bin/bash
# Usage: world_speed.sh <name> [VAR=value ...] [game arguments such as --fps 120]
# Continues the campaign from the save in run/user and runs back and forth, printing how fast the world moves per real
# second (speed.py, from the far shadow cascade's light matrix, which follows the camera). The peak of the histogram
# should match a run at --fps 30; a quarter of it means characters are in slow motion at 120 fps. Unlike game_speed.sh
# it steers by real time through AR_REMOTE, so it doesn't depend on the menus' frame timing.
source "$(dirname "$0")/common.sh"
name=$1
shift
split_args "$@"
cd "$RUN" || exit 1
{
    printf '%s\n' 15000 A 3000 A 3000 A 5000 A 12000
    for i in 1 2 3 4 5 6; do printf '%s\n' "L0 4000" "L180 4000"; done
} > "remote_speed_$name.txt"
env AR_REMOTE="remote_speed_$name.txt" AR_DUMP_ONLY=1 AR_SHADOW_LOG=1 AR_DUMP_FRAME=0 AR_DUMP_FRAMES=100000 "${ENV_ARGS[@]}" \
    timeout 95 "$EXE" --game "$ISO" "${GAME_ARGS[@]}" 2>&1 | grep -E ': L [0-9]+ |Crash' > "speed_$name.log" # The whole log is hundreds of MB.
python "$TOOLS/speed.py" "speed_$name.log"
rm -f "remote_speed_$name.txt"
