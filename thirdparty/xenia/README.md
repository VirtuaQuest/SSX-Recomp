# Xenia shader microcode subset

A subset of [xenia-canary](https://github.com/xenia-canary/xenia-canary) (BSD license, see `LICENSE`): the Xenos
register, microcode and tiled texture address definitions, and the shader microcode parser and disassembler. These
files are unchanged copies.

The rest replaces parts of Xenia's base library they use, so the subset builds on its own:

- `src/xenia/base/logging.h`, `src/xenia/base/filesystem.h`, `src/xenia/base/string.h`
- `src/xenia/gpu/gpu_flags.h` (configuration variables)
- `src/third_party/fmt/include/fmt/format.h` (forwards to the fmt library)
- `shim/memory_shim.cc` (byte swapping copies)
