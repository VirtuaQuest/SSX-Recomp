# Xenia shader microcode subset

Files under `src/` are copied unchanged from [xenia-canary](https://github.com/xenia-canary/xenia-canary)
(BSD license, see `LICENSE`): the Xenos register and microcode definitions and the shader microcode parser and
disassembler. `shim/` replaces the parts of Xenia's base library they use (logging, configuration variables,
file helpers and byte swapping) so they build on their own.
