# Lessons learned

What getting Anarchy Reigns running taught us about recompiling Xbox 360 games: the bugs that were hard to find, why
they happened, and how they were found. Most of it is about the console, its kernel and its GPU, and applies to any
title; the Anarchy Reigns specifics are at the end. `git log` has the details of each fix.

## Recompilation (XenonRecomp)

- Jump tables: some titles switch on unsigned 32 bit values; the fork's analyzer handles `.u32` switches. Missing
  jump table entries show up as functions that return early or crash in odd places.
- `mftb` (the time base) runs at 50 MHz on the console; `PPC_QUERY_TIMEBASE` is implemented by the runtime
  (`ArQueryTimeBase`) and must agree with `KeQueryPerformanceFrequency` (50000000).
- The physical memory view at 0xE0000000 maps physical memory 4 KB higher than the views at 0xA0000000 and
  0xC0000000. Host views must be 64 KB aligned, so every access at or above 0xE0000000 is moved up by 4 KB
  (`PPCHostOffset` in `ARRecompLib/ar_memory.h`); everything must go through `PPC_MEMORY_ADDRESS`.
- Guest functions can be replaced from the runtime: `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX); PPC_FUNC(sub_XXXXXXXX) { ...
  __imp__sub_XXXXXXXX(ctx, base); }` (see `gpu/debug_hooks.cpp`, `kernel/timing_hooks.cpp`). `ctx.lr` holds the guest
  return address, which tells you the caller.
- To find what a guest function does, read it in `ARRecompLib/ppc/*.cpp` (`grep -l "PPC_FUNC_IMPL(__imp__sub_X)"`);
  to find the function containing a return address, search for `ctx.lr = 0x<address>;`.
- Host stacks of guest threads need to be large (64 MB): recompiled calls nest on the host stack.

## Kernel and system (HLE)

- Thread waits: never wake every waiting thread on every signal. A single condition variable with `notify_all` kept
  ~30 guest threads re-checking their waits and caused bursts of 5 fps (5-10 cores busy). Waiters register under the
  objects they wait on (`kernel/objects.cpp`).
- Guest thread priorities run from -15 to 15. Map low priorities close to normal (Sofdec's movie decoder lowers
  itself to -16 and then decodes far below real time) and keep the host GPU thread above the title's high priority
  streaming threads (CRI file and decompression threads ask for priority 2).
- Async file I/O must return `STATUS_PENDING` and complete later, or saves don't load.
- `EVENT_WRITE_ZPD` (occlusion queries) must write sample count reports, or D3D's `GetData` spins forever.
- The XAudio callback receives the address of a word holding the context; `VdInitializeScalerCommandBuffer` takes 11
  arguments; interrupts go to a target CPU (PCR + 0x10C holds the current processor).
- Message boxes (XShowMessageBoxUI) can simply answer with button 0.
- o1heap needs `O1HEAP_CLZ=__builtin_clzll` on Windows.

## GPU: command processor

- The title's D3D library builds PM4 packets; the command processor reads them from the ring buffer and indirect
  buffers. Vertex fetch constants are allocated from vf95 down.
- Opcodes worth knowing: `EVENT_WRITE_EXT` is 0x5A with 8192 extents; `LOAD_ALU_CONSTANT` (0x2F) copies constants
  from memory when the packet executes (Anarchy Reigns loads ~2700 matrices a frame that way).
- Predicated tiling: honour the bin mask/select and the type 3 packet predicate bit.
- Frame pacing comes from the emulated vertical blank interrupt; the title's D3D counts vblanks to decide when a
  swap may happen. Scaling the vblank rate scales the frame rate cap, but not the game's speed by itself (see the
  time step below).

## GPU: EDRAM, render targets and resolves

- The 360 renders into 10 MB of EDRAM and copies (resolves) results to textures in memory. The host keeps a texture
  per EDRAM base/pitch/format/depth, so render targets that share EDRAM must share contents explicitly:
  - Overlapping targets of the same format copy the rows written since the other was last used.
  - Games reinterpret depth as colour and colour as depth through the same EDRAM (clears, copying depth words as
    colour to restore a depth buffer later). Depth tiles have their two 40-sample halves swapped relative to colour
    tiles (Xenia's `xenos.h`); missing that swap gave interlaced blocks. Colour into depth only for float depth
    targets: the unorm ones share EDRAM with colour for combined clears.
  - Track written rectangles per target, or copies drag stale data over.
- MSAA targets are kept with a host sample per texel. The EDRAM sample order for 4x is
  `u = 4*(x>>1) | 2*sampleX | (x&1)` (same for v); convert layouts when a target is used at a different sample count
  (games clear a 1x surface through a 4x alias of the same memory because it covers 4 samples per pixel).
- Resolves are address aware: games render in strips and point each strip's resolve at its rows of one texture.
- Resolves to the same address with different sizes happen every frame; pool released textures instead of creating
  new ones (creating and destroying resources was 14% of the GPU thread).
- One fetch can cover several resolves: Anarchy Reigns resolves its three 1024x1024 shadow cascades one after
  another and samples them as one 1024x3072 texture. Without assembling them, shadows existed only in a block
  around the player.
- Depth resolves hold raw depth words; textures of format 24_8 / 24_8_FLOAT fetched from them are decoded in the
  shader.
- Packed mips apply to linear textures too.

## GPU: shaders (microcode to HLSL)

- Pixel parameter generation (PsParamGen): the pixel position, with the sign bit of x set for back faces (also for
  x = 0) and the magnitude unchanged. Using -x-1 for back faces was off by one for shaders that use |x|, which put
  micro tears (thin lines at columns where x % 40 == 39) in every frame.
- `vtx_w0_fmt` (PA_CL_VTE_CNTL) clear means the shader outputs 1/W.
- Float and depth textures ignore the sign flags; front buffer fetch swizzle decides the red/blue swap at present.
- DXC (SM6, HLSL 2021) needs `select()`/`or()` for vector conditions; FXC can't sample inside the state machine loop.
  Generated float literals must look like `1.000000f` (`0f` is invalid).
- `getWeights2D` (bilinear weights) is needed for shadow PCF; point sprites need a geometry shader.
- Point sampled fetches of scaled textures must snap to texel centres (`floor(coord * size) + 0.5`).

## Performance

The host renderer is CPU bound: about 5000 draws a frame, each needing state translation, constants, vertex data and
texture lookups. What mattered, roughly in order:

1. The title waiting on the GPU. Occlusion queries aren't measured, so a completed query always returns the same
   count; answering pending queries with it at once removed ~60% idle time from the title's render thread.
2. Per-draw overhead: `getenv` on hot paths (cache it, `AR_ENV`), creating resources per frame (pool them), converting
   index buffers when the host can use them as they are (16 bit strips with reset 0xFFFF), clock reads for profiling
   (only with `AR_PROFILE`), allocations (reuse buffers), single dependency chain hashes (use independent lanes).
3. Avoid re-uploading unchanged data: cross-frame vertex cache and sampled content hashes for large ranges (full hash
   every 30 frames). Be careful: stale data shows up as flicker, so don't skip checks to save time.
4. Record D3D12 commands on the GPU thread but replay, submit and present on a second thread (`gpu/deferred_list.*`).
   Render target and depth views are read when commands are replayed, so free their descriptors only after the frame.
5. Thread priorities: the GPU command processor runs at time critical priority and sleeps when idle.
6. Compiler flags: RelWithDebInfo defaults to /Ob1 (only explicitly inline functions are inlined); /Ob2 made heavy
   scenes 10-20% faster. Name pipeline library entries by state key and shader bytecode, or entries from older
   builds collide with new ones and those pipelines get recompiled every run.

7. Where the time really goes at high frame rates: the title's render thread spent ~40% spinning in D3D's block on
   fence (frame pacing waits for the GPU to finish the previous frame), so its own work was ~6 ms a frame; the GPU
   (RTX 3080 Ti, AR_GPU_TIMING) ~5-8 ms; the bottleneck is the GPU command processor thread (~10 ms in heavy fights).
   Constant and register blocks without side effects are copied in one piece, and shader loads compare the
   microcode at the same address with the last load instead of hashing it.

Profile instead of guessing: `AR_HOST_PROFILE=command` (the GPU thread) and `AR_THREAD_PROFILE=1` (guest threads)
show where time goes; idle time in the GPU thread means the title is the bottleneck.

## Frame rate above 30 fps

- Titles locked to 30 fps often still measure frame time but clamp the step. Anarchy Reigns measures each frame in
  30 Hz frames and uses the measurement only between 1.01 and 2, otherwise a whole frame: above 30 fps the game ran
  fast. Lowering the minimum makes the step follow the frame time. Find such code by logging which functions read
  the time base or call QueryPerformanceCounter once per frame (`AR_LOG_TIMEBASE`, `AR_LOG_QPC`).
- Verify speed by measurement, not by eye: compare how far the world moves per real second at 30 and 60 fps
  (`tools/test/game_speed.sh`).

## Debugging methods that worked

- Trace one frame (`AR_TRACE_FRAME`), dump every resolve, and look at the same pixel region in each to find the first
  stage where an artifact appears; then list the affected coordinates and their patterns (x mod 40/80 pointed at
  EDRAM tiles).
- Bisect with switches: most features have an `AR_NO_...` toggle; `AR_SKIP_PS`/`AR_SKIP_VS` remove draws by shader.
- For timing dependent bugs, keep logging light (`AR_DUMP_ONLY`, `AR_SHADOW_LOG`): heavy tracing slows the game and
  makes them disappear.
- Watch out for the machine: other games and emulators running in the background slow everything uniformly. Check
  CPU use before benchmarking.
- Dumps are big: always limit them (`AR_DUMP_ONLY`, `AR_DUMP_FRAME/FRAMES`) and clear old ones. They filled a disk once.

## Anarchy Reigns specifics

- Disc image: XGD3. Saves live in `run/user/content/00000001/SystemData`.
- Timing: `sub_82BD7EC8` sets the game's time step (float at 0x830778C8, in 30 Hz frames) from constants at
  0x820A4130 (+16 default 1, +68 maximum 2, +716 ms scale 0.03, +2176 minimum 1.01). `kernel/timing_hooks.cpp` lowers
  the minimum to 0.2 when the frame rate setting is above 30.
- D3D's `IDirect3DQuery9::GetData` is `sub_8223F950` (device fence progress at device + 11036, completed value at
  *(device + 11024)); the title's QueryPerformanceCounter is `sub_8245B458`.
- Effects pass depth is restored by drawing saved depth words as colour at EDRAM 0x5B7; the shadow map depth buffer is
  EDRAM base 128, pitch 1040, D24FS8, cleared by a 1x strip plus a 4x MSAA alias rectangle; the shadow mask is the
  alpha of resolve 0x0E75F000 (pixel shader 47A3CA).
- Testing: `AR_AUTO_SEQUENCE` drives the menus (see `tools/test/common.sh`); Training > Practice has no enemies.

## Open issues

- Distant shadows occasionally flicker (the launcher's Shadows Off setting avoids it). Cause found, fix not yet: the title itself issues some large shadow casters
  (whole buildings drawn with the instanced shaders 497B89DE/4631A8B4, e.g. vertex buffer 0x126320A0) into the far
  cascade on some frames and not on others; the renderer draws exactly what it gets (inputs, upload memory, D3D12
  state and GPU validation all check out, and the clears leave empty maps when casters are skipped). Next: hook the
  title's draw call for that mesh, log the guest call stack and find the culling decision (a job system race or time
  sliced culling behaving differently with recompiled timing are the main suspects; kernel waits don't time out).
  Lesson: compare draws per instance (mesh plus world matrix), not per mesh, or extra instances stay invisible.
- Short bursts (about 2 s) where the GPU thread is uniformly ~4x slower, mostly with other games running.
- 120 fps setting: correct speed; light scenes 110-120 fps, the heaviest fights 56-85 (the GPU command processor
  thread is saturated, and the title's render thread is close behind).
- Angular light patches around characters in tiled deferred lighting (pixel shader E9891C vs 57FBD2), possibly a
  translation bug; thin vertical ticks under ledge edges in the Foundry.
- Anti-aliasing is a placeholder in the launcher.
