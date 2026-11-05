# Anarchy Reigns Recompiled

A native PC port of Anarchy Reigns (Xbox 360) by static recompilation. This project contains no game code or art. You build the game on your own PC from your own copy.

## Building

You need:

- Your own Xbox 360 copy of the game: a disc image (`.iso`) or a folder of its files. DLC packages are optional.
- [Visual Studio](https://visualstudio.microsoft.com/) 2022 or its Build Tools, with "Desktop development with C++" and a Windows SDK. That includes CMake and Ninja.
- [LLVM](https://llvm.org/) (`clang-cl`), or "C++ Clang tools for Windows" in the Visual Studio installer.
- The XenonRecomp fork, in a folder named `XenonRecomp` next to this one, with its submodules (`git clone --recurse-submodules`). This has been tested with the `xenon-doax2` branch.
- About 5 GB of free disk space.

Then run `Setup.bat`. Choose your game and, if you have it, your DLC folder, and press **Build**. Setup shows what it found and how far along it is. On a 10 core PC the whole build took about 6 minutes.

When it's finished, press **Open launcher**, or start `out\build\ARRecomp\ARLauncher.exe`, to choose your settings and play.

## What Setup does

All of this stays on your PC, and nothing is downloaded:

1. Takes the game's executable from your copy, and checks it's the version this project was made for.
2. Unpacks your DLC into `run\user\content`, checking every block against the package's own hashes.
3. Recompiles the game's code into `ARRecompLib\ppc`.
4. Compiles the game and the launcher into `out\build\ARRecomp`.
5. Runs the game for a moment, unseen and silent, to cut the window and program icon from its logo.

## Sharing

The programs you build contain the game's code, and their icon comes from the game. Don't share them. Everything made from the game is in `ARRecompLib\ppc`, `ARRecompLib\private`, `out` and `run`, which git ignores.

More: [docs/LESSONS.md](docs/LESSONS.md) and [docs/DEBUGGING.md](docs/DEBUGGING.md).
