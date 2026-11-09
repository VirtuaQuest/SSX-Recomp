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
texture detail, sharpening and vsync. A controller works as on the console.

### Keyboard and mouse

Keyboard, mouse and controller all work at the same time, as one controller: use whichever you like at any moment, with
nothing to switch. The default layout follows SSX's Standard controls:

| Keys | Controller | On the mountain | In menus |
|---|---|---|---|
| W A S D | Left stick | Steer and lean; in the air, spin and flip | Move |
| Space (Enter) | A | Jump (hold to crouch, let go to jump); in the air, back grab | Select |
| Mouse movement (I J K L) | Right stick | Flick up to jump, flick to grab, circle to tweak | |
| Left mouse button (Left Shift) | RT | Boost (hold); in the air, tweak | Next page |
| Right mouse button (C) | LT | Grind | Previous page |
| R | LB | Rewind (hold) | Previous tab |
| Left Ctrl | RB | Open the wingsuit (hold) | Next tab |
| Q / E / F | X / B / Y | Tricks; in the air, left / right / front grabs | E or Backspace: back |
| Escape | Start | Pause | |
| Arrow keys | D-pad | | Move |
| Tab, Z, V (middle mouse) | Back, stick clicks | | |

Click into the game's window to let the mouse drive the trick stick (in borderless and fullscreen it does from the
start); Alt gives the cursor back. Every key can be changed: "Edit keys" in the launcher opens `user\key_bindings.ini`,
which lists what each controller input does. Mouse sensitivity and inversion are in the launcher.

### Online

No Xbox Live or EA account is needed. The profile is a local one, and saves go to `run\user\content` on your PC (back
that folder up to keep your progress). EA's servers for SSX are gone, so the game runs as an offline game: World Tour,
Explore and RiderNet's badges and settings all work, and Global Events and SSX Rivals (which were server content) say
that they need the servers.

## Sharing

The programs you build contain the game's code, and their icon comes from the game. Don't share them. Everything made
from the game is in `ARRecompLib\ppc`, `ARRecompLib\private`, `out` and `run`, which git ignores.

More: [docs/LESSONS.md](docs/LESSONS.md) and [docs/DEBUGGING.md](docs/DEBUGGING.md).
