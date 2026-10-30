# Shared setup for the test runners (sourced). Runs happen in run/ like a normal launch.
#   AR_ISO     disc image (default: the developer's copy)
#   AR_EXE     executable (default: out/build/ARRecomp/ARRecomp.exe)
# Git Bash started from PowerShell or cmd puts Windows first on PATH, where timeout.exe is a different program.
export PATH="/usr/bin:$PATH"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$REPO/run"
EXE="${AR_EXE:-$REPO/out/build/ARRecomp/ARRecomp.exe}"
ISO="${AR_ISO:-C:/Downloads/Repos/Anarchy Reigns/Anarchy Reigns.iso}"
TOOLS="$REPO/tools/test"

# From the title screen into Beer Street gameplay with the save in run/user (gameplay starts around frame 2400).
TO_GAMEPLAY="A,A,A,WAIT,A,WAIT,WAIT,A,WAIT,A,WAIT,A,WAIT,START,WAIT,A,WAIT,WAIT"

# Dumps are 1-4 MB each: a run replaces the previous run's instead of piling up (they once filled the disk).
clear_dumps()
{
    mkdir -p "$RUN/screenshots"
    find "$RUN/screenshots" -maxdepth 1 -name '*.bmp' -delete
}

# Splits the arguments after the name: VAR=value words go to ENV_ARGS, everything else to GAME_ARGS
# (e.g. "fight.sh run1 AR_SINGLE_THREAD=1 --fps 60"). Works the same from bash, cmd and PowerShell.
split_args()
{
    ENV_ARGS=()
    GAME_ARGS=()
    for arg in "$@"; do
        if [[ "$arg" == *=* && "$arg" != -* ]]; then ENV_ARGS+=("$arg"); else GAME_ARGS+=("$arg"); fi
    done
}
