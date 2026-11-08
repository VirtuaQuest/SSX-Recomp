# SSX Recompiled

A native PC port of SSX (Xbox 360, 2012) by static recompilation. This project contains no game code, art or audio. You
build the game on your own PC from your own copy.

## Building

You need:

- Your own Xbox 360 copy of the game: a disc image (`.iso`) or a folder of its files. DLC packages are optional.
- [Visual Studio](https://visualstudio.microsoft.com/) 2022 or its Build Tools, with "Desktop development with C++" and
  a Windows SDK. That includes CMake and Ninja.
- [LLVM](https://llvm.org/) (`clang-cl`), or "C++ Clang tools for Windows" in the Visual Studio installer.
- The XenonRecomp fork, in a folder named `XenonRecomp` next to this one, with its submodules
  (`git clone --recurse-submodules`), on the `xenon-ssx` branch.
- FFmpeg, which decodes the game's audio: a Windows x64 **LGPL shared** build with `include`, `lib` and `bin` folders
  (for example `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from BtbN's FFmpeg builds). Unzip it into
  `thirdparty\ffmpeg`, or anywhere next to this folder in a folder whose name starts with `ffmpeg`, or set `FFMPEG_ROOT`.
- About 5 GB of free disk space.

Then run `Setup.bat`. Choose your game and, if you have it, your DLC folder, and press **Build**. Setup shows what it
found, what's missing and how far along it is.

When it's finished, press **Open launcher**, or start `out\build\ARRecomp\ARLauncher.exe`, to choose your settings and
play.

## What Setup does

All of this stays on your PC, and nothing is downloaded:

1. Takes the game's executable from your copy and checks it's the version this project was made for.
2. Unpacks your DLC into `run\user\content`, checking every block against the package's own hashes.
3. Recompiles the game's code into `ARRecompLib\ppc`.
4. Compiles the game and the launcher into `out\build\ARRecomp`, and puts FFmpeg's DLLs next to them.
5. Takes the window and program icon from your game's executable.

## Playing

The launcher sets the resolution, windowed, borderless or fullscreen display, the frame rate (30 as on the console, or 60
at the same game speed; SSX holds itself to 60 at most), the internal render scale (up to 2160p), anisotropic filtering,
texture detail, sharpening and vsync. A controller works as on the console. Keyboard and mouse are merged into the first
controller (WASD the left stick, IJKL or the mouse the trick stick, Space A, arrows the d-pad, Escape Start).

Saves are in `run\user`.

## Sharing

The programs you build contain the game's code, and their icon comes from the game. Don't share them. Everything made
from the game is in `ARRecompLib\ppc`, `ARRecompLib\private`, `out` and `run`, which git ignores.

More: [docs/LESSONS.md](docs/LESSONS.md) and [docs/DEBUGGING.md](docs/DEBUGGING.md).
