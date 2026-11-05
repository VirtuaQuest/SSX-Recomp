# Anarchy Reigns Recompiled

Native PC port of Anarchy Reigns (Xbox 360) by static recompilation with a XenonRecomp fork, based on the
UnleashedRecomp runtime. Read `docs/LESSONS.md` before changing the kernel or GPU code (what was learned and why
things are the way they are) and `docs/DEBUGGING.md` for the switches and test scripts.

## Layout

- `ARRecompLib/` — recompiler config (`config/`), generated code (`ppc/`, not in git), the title's executable
  (`private/default.xex`, not in git).
- `ARRecomp/` — runtime: `kernel/` (HLE kernel and XAM, imports in `kernel/imports/`), `cpu/` (guest threads), `gpu/`
  (PM4 command processor, microcode to HLSL translator, D3D12 renderer, submission thread), `os/` (crash handler,
  profiler).
- `thirdparty/xenia` — BSD subset of Xenia (microcode parser). `tools/launcher` (ARLauncher), `tools/shader_tool`
  (ARShaderTool: disassemble or translate dumped shaders), `tools/test` (test runners).
- Reference code: Xenia (`src/xenia/gpu`) and XenosRecomp at `C:\Downloads\Repos\_tools\ref`, UnleashedRecomp at
  `C:\Downloads\Repos\UnleashedRecomp`.

## Build and run

- The build uses the XenonRecomp fork checked out next to this folder (`../XenonRecomp`, branch `main` of the same
  GitHub repository; `XENONRECOMP_ROOT` overrides the path).
- `tools\build.bat [ARRecomp|ARLauncher|ARShaderTool]` — Ninja, clang-cl (portable LLVM 19 at
  `C:\Downloads\Repos\_tools\llvm`), VS 2022 Build Tools, RelWithDebInfo, into `out\build`. DXC comes from the
  Windows SDK and is copied next to the exe.
- Run from `run\`: `..\out\build\ARRecomp\ARRecomp.exe --game "C:\Downloads\Repos\Anarchy Reigns\Anarchy Reigns.iso"`,
  or `out\build\ARRecomp\ARLauncher.exe` (settings in `launcher.ini` next to it, with paths relative to it; the defaults in `tools/launcher/launcher.ini` are
  copied there only if it has none).
- If the exe is locked because the game is running, rename it (`ARRecomp.running.exe`) instead of closing the game.

## Working rules

- Never commit game data or anything generated from it (`.gitignore` covers `ppc/`, `private/`, `run/`, `out/`).
- Benchmark only when nothing else is busy (other games and emulators often run in the background); check CPU use
  first and say when results are skewed.
- Keep dumps limited (`AR_DUMP_ONLY`, `AR_DUMP_FRAME`) and clean them up; they are megabytes each.
- New behaviour that could break the game gets an `AR_NO_...` switch to turn it off, and diagnostics stay opt-in.
- Verify visual fixes with dumps or screenshots and performance with `tools/test/fight.sh`, and game speed with
  `tools/test/game_speed.sh` when touching timing.
- Commit messages explain the cause, the fix and the measured result.
