# Recomp Launcher

[Recomp Launcher](https://github.com/elripalda/RecompLauncher) installs recomps from their GitHub releases, sets them up
with the player's own copy of the game and starts them. SSX Recompiled can't ship a program (it would contain the
game's code), so what the launcher installs is this project's source, and its setup step builds the game on the
player's PC from their disc, the way Setup does.

## How it goes for a player

1. They install SSX Recompiled from the launcher's Discover page. The release zip is unpacked into the game's folder:
   the source, XenonRecomp and its libraries (`thirdparty/XenonRecomp`), and `recomplauncher.json` at the top.
2. They pick their SSX disc image (or a folder of its files). The launcher checks it's SSX (`audiobestream.big` on the
   disc), reads it through, and runs the setup step, `tools/recomplauncher/prepare.cmd --game <their copy>`, without
   a window, showing its progress lines under "Getting the extras":
   - `get_ffmpeg.ps1` gets FFmpeg 8.1 (LGPL, shared) from BtbN's GitHub releases into `thirdparty/ffmpeg` if there's
     none, checked against the release's SHA-256 list. It's the only download.
   - It builds `out/setup/ARPrepare.exe`: Setup (`tools/setup/main.cpp`) as a console program, which `--prepare` runs
     without its window.
   - ARPrepare checks the build tools and that the disc's `default.xex` is the one this project was made for, then
     does what Setup's Build button does: takes the executable, recompiles it, compiles the game and the launcher into
     `out/build/ARRecomp`, and writes the default `user/key_bindings.ini` there. It leaves the icon for the game's first
     start (taking it now would open the game's window). The log is `out/setup/setup.log`.
3. Play starts `out/build/ARRecomp/ARRecomp.exe --game <their copy>` with the settings they picked on the game's page as
   arguments (the same ones ARLauncher passes). The game runs from that folder, so its saves are in
   `out/build/ARRecomp/user/content/00000001`, which the launcher backs up. `AR_LOG_FILE` puts the log in
   `out/build/ARRecomp/logs/ssx.log`, which goes with a crash report.

The keyboard and mouse layout is on the game's page too: the launcher edits `user/key_bindings.ini` (the game reads the
launcher's key names, such as "Mouse Left" and "Return", as well as its own). In Big Picture mode
(`RECOMPLAUNCHER_MODE=bigpicture`) the game starts borderless unless a display mode was picked.

### Updates

An update unpacks the new source over the folder, and the build there is still of the old version. When the launcher
starts the game (`RECOMPLAUNCHER` is set), it compares the version it was built from with `VERSION` in the folder, and
says so if they differ: Yes plays the old build, No quits with exit code 75, which the launcher explains. Choosing the
disc again on the game's page runs the setup step, which builds the new version (only what changed is compiled
again). In Big Picture mode the old build just plays. Started any other way, the game doesn't check.
`AR_NO_VERSION_CHECK=1` turns the check off.

The log (`AR_LOG_FILE`, `logs/ssx.log`) keeps the three runs before it beside it, as `ssx-1.log` to `ssx-3.log`.

## What the PC needs

- Visual Studio 2022 Build Tools (or Visual Studio) with "Desktop development with C++", which brings the Windows SDK,
  CMake and Ninja, and "C++ Clang tools for Windows". prepare.cmd finds them with vswhere; ARPrepare also takes LLVM
  from `LLVM_DIR` or `Program Files\LLVM`.
- About 5 GB free, and 10 to 30 minutes for the first build.
- An internet connection for FFmpeg, unless an LGPL shared build is already in `thirdparty/ffmpeg`.

## Exit codes of the setup step

| Code | Meaning |
|---|---|
| 0 | Built. |
| 1 | Wrong use (no `--prepare`, an unknown option). |
| 2 | The build tools or FFmpeg aren't there (the last progress line says which). |
| 3 | The copy given can't be read, or has no `default.xex`. |
| 4 | Another version of SSX than the USA disc this project was made for (`--any-version` builds it anyway). |
| 5 | The build failed: see `out/setup/setup.log` (or `out/setup/prepare_build.log` if ARPrepare itself didn't build). |

## Making a release

1. Raise `VERSION` (the game's update check compares it) and commit.
2. `powershell -ExecutionPolicy Bypass -File tools\recomplauncher\make_release.ps1` writes
   `out\release\SSX-resled-v<version>-Windows-x64.zip` (and its `.sha256`). It holds only files from git: this project
   at HEAD and the submodules at the commits it names. It refuses game files and uncommitted changes.
3. Make a GitHub release on VirtuaQuest/SSX-resled tagged `v<version>` with the zip. The launcher picks it by
   "Windows" and "x64" in its name. A pre-release is a beta, which normal players don't get.

To try it the way a player gets it before publishing: in the launcher, Settings > Developers > Developer tools, then
"Add a build" with a folder the zip was unpacked into. Its Developer tab checks `recomplauncher.json`.

## Getting listed

Submit the repo at [recomplauncher.com/submit](https://recomplauncher.com/submit?platform=pc) as `VirtuaQuest/SSX-resled`,
and say that `recomplauncher.json` is in the release and covers all three levels (Easy Setup, settings, and the setup
step; no `dumps`, since the Redump values weren't checked). A free developer account at
[recompstore.com/hub](https://recompstore.com/hub) (sign in with GitHub) claims the page. Once it's listed, point the
README's badge at `https://recomplauncher.com/game/<its id>` (the id is in https://recomplauncher.com/pc-catalog-web.json,
by `repo`). Another SSX (2012) recomp is already listed (AmarilloNL/ssx-recomp), so say that this is a separate one. The catalog's own profile for the game
only needs what the file can't say:

```json
{
  "kind": "portable",
  "needs": ["build_tools", "clang"],
  "player_note": "The setup step builds the game on your PC from your own SSX disc: it needs Visual Studio 2022 Build Tools with Desktop development with C++ and C++ Clang tools for Windows, about 5 GB free and 10 to 30 minutes, once. FFmpeg (the audio decoder) is downloaded the first time."
}
```
