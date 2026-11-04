#!/bin/bash
# Usage: shadow_flick.sh <name> [VAR=value ...] [game arguments]
# Continues the campaign from the save, runs back and forth, and counts far cascade flicker from a non-blocking copy of
# the shadow map (AR_SHADOW_READBACK, smflick.py). Compare several runs: counts vary a lot. Keep off the controller.
source "$(dirname "$0")/common.sh"
name=$1
shift
split_args "$@"
cd "$RUN" || exit 1
{
    printf '%s\n' 15000 A 3000 A 3000 A 5000 A 12000
    for i in 1 2 3 4 5 6; do printf '%s\n' "L0 4000" "L180 4000"; done
} > "remote_flick_$name.txt"
env AR_REMOTE="remote_flick_$name.txt" AR_SHADOW_READBACK=1 "${ENV_ARGS[@]}" timeout 95 "$EXE" --game "$ISO" "${GAME_ARGS[@]}" 2>&1 |
    grep -E ': SM |Crash' > "flick_$name.log"
rm -f "remote_flick_$name.txt"
python "$TOOLS/smflick.py" "flick_$name.log"
