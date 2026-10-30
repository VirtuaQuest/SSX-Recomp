# Shared setup for the test runners (sourced). Runs happen in run/ like a normal launch.
#   AR_ISO     disc image (default: the developer's copy)
#   AR_EXE     executable (default: out/build/ARRecomp/ARRecomp.exe)
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
