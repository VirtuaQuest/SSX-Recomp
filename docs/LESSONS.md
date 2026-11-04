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
- To find which code uses an input, watch who reads it: the stick values are floats at 0x832151D0 + 16 to + 28 (pad 0), and `find readwatch` on them named the class that copies them for the screen showing. From there the copy's offsets lead to the code that acts on them. Searching for the camera's values had found only UI text.
- To tell a shadow really changing from the camera moving, reproject: map each pixel to where its world point was on the previous frame, with the game's own matrices, and compare there. Filtering on "depth didn't change" hides whatever characters do, and counts on a downsampled mask hide fine patterns. Plot the rate per distance from the camera: planes at fixed distances then stand out by a factor of hundreds.
- A clean test needs a positive control. `AR_DEPTH_ONLY_PS=1` brings the old flicker back; that the measure saw it (10x as many shadows going lit for a frame, at 120 fps) is what made its "nothing else" mean something. And repeat before believing a change: one band read 2.1 before and 8.9 after, then 6.2 and 50 before, 6.1 and 5.8 after. A fight makes those numbers noisy.
- Compare at equal frame rates. Render scale 2 looked 14x worse until both ran at 30 fps: it had only been running at half the frame rate.
- A `shot` is taken before the overlay is drawn, so anything added by `kbm::GetOverlay` is missing from it (rows looked gone that weren't). `PrintWindow` with full content (flag 3) gets the window as shown, even behind other windows.
- To find what runs too fast at high frame rates, scan memory for values that change by exactly 1 per update at
  120 fps (`AR_SCAN_TIMERS=@<seconds>`): anything following the time step changes by 0.25-0.5 there, and the scan's
  own slow first frame weeds out the rest. `AR_SCAN_TIMERS_WATCH` then logs the code writing a sample of them. That
  led from a UI element's derived counter to its time, to the manager adding a fixed step.
- For what runs fast only in one scene, have someone press F9 in it (`AR_SCAN_KEY`) and compare with a scan in ordinary play
  (`timerdiff.py`). A value changing by the same amount through the scan's own slow updates ignores the time step. F10 then
  logs who writes those values. Check any fix with characters too: slow motion from one was first blamed on the debug tool
  because the watch really did slow the game at the time; `world_speed.sh` measures it without a tester.
- When a search by signature keeps catching other things, narrow it to the memory range where the real hits were: a hunt for
  a steady 1.5 per update first caught only sound threads.
- Pipeline statistics queries (`AR_SHADOW_STATS`) say where a draw got lost: vertices, shader runs, primitives through the
  clipper, pixel shader runs. The shadow flicker cost days because occlusion queries alone (zero samples) pointed at invalid
  vertex outputs, while the pixel shader was running and killing everything. Measure this first.
- Check which shader a draw really uses, per draw. Notes said the shadow pixel shader was a trivial one; that came from a
  different draw, so textures and pixel constants were never compared. A comparison of "all inputs" must include all of them.
- A diagnostic that waits for the GPU can hide what it looks for: dumping every frame slows the game to a few frames per
  second, and at that speed the flicker didn't show (why was not established). `AR_SHADOW_READBACK` copies without waiting (`smflick.py`).
- Read Xenia's comments on a register before guessing at its behaviour.
- For draws that come and go, measure them on the GPU instead of comparing images: an occlusion query around each
  draw (`AR_SHADOW_PIXELS`) gives the samples it wrote, and drawing it again without depth testing gives its
  rasterized area. That separates "not issued" from "failed the depth test" from "rasterized nothing".
- Segment per-frame comparisons by pass (here by cascade, using the resolves between them): totals over a whole frame
  hid a draw moving between cascades. And compare per instance (mesh plus world matrix), not per vertex buffer.
- To tie GPU-side draws to guest code, put NOP packets with an ID into the title's command buffer from a hook on its
  draw functions (`AR_MARK_DRAWS`): the D3D device's write pointer is at device + 48 (pre-increment), limit at + 56.
- Checking upload memory after the GPU is done is only meaningful before the space is reused: mid-frame flushes
  reuse the frame's slice, so check at the flush (`CheckOcclusionInputs`), not at the end of the frame. Reading
  upload heap memory from the CPU is extremely slow (write combined): hash at upload time instead.

## Anarchy Reigns specifics

- Disc image: XGD3. Saves live in `run/user/content/00000001/SystemData`.
- Timing: `sub_82BD7EC8` sets the game's time step (float at 0x830778C8, in 30 Hz frames) from constants at
  0x820A4130 (+16 default 1, +68 maximum 2, +716 ms scale 0.03, +2176 minimum 1.01). `kernel/timing_hooks.cpp` lowers
  the minimum to 0.2 when the frame rate setting is above 30.
  `sub_82BC6DE8` spreads it into per-category steps at 0x8312124C (read through `sub_82BC9B28(category)`; some
  categories are in 60 Hz units). A flag (bit 0x2000 at 0x83074D14) forces the default step of 1.
- The UI does not use the time step: its updates pass their elements a fixed 1/30 s per update (the constant at
  0x820A4130 + 192), so above 30 fps the HUD, radar pulses, dialogue and result screens ran 2-4x fast. The UI
  manager `sub_827BA3B0` (walks its elements, calling each one's update, vtable slot 3, with that step; an element's
  time is the float at +116), its twin `sub_82789708`, `sub_827B5BC8`/`827B6690`/`827B6E40` and `sub_827F7828`
  get mid-function hooks (`AR.toml`, `UiTimeStepHook*` in `kernel/timing_hooks.cpp`) that scale the step by the
  time step. In gameplay the first, second and last run every frame; the other three were not seen running (menus or
  screens not reached by the tests). Other code that keeps the 1/30 constant in a register and passes it on, not
  hooked yet: `sub_829F9750`, `sub_829F9DD0`, `sub_82BB0F48`, `sub_8281A2A8`. If something else still runs fast
  above 30 fps, look there first, then use `AR_SCAN_TIMERS`.
- High frame rates, what was fixed in November 2026 (all in `kernel/timing_hooks.cpp` and `AR.toml`, each with an off switch):
  - Two-character dialogues' line timing: 32 more loads of the 1/30 constant, all in the UI code range 0x82750000-0x82840000
    (`UiConstantHook`), plus `sub_827EAEE0`, which advances element time. The title has 303 such loads. Do not scale the
    constant itself: character code multiplies it by the time step, so characters ran at a quarter speed at 120 fps.
  - Dialogue characters' lips and idle motion: animation advances by its object's own step at +944. Most objects refresh it
    from `sub_82BC9B28(category)`; those that don't keep the 1 they were created with. `sub_82BAE108`, which all animation
    goes through, now gives those the time step. In-engine cutscenes keep their timing with this.
  - Vents and ramps: characters move toward a target, at most 1.5 units per update (0x820A4130 + 644). Normal movement stays
    below that and follows the time step, but launches hit it. `MotionCapHook` scales the limit where three movement routines
    load it (`sub_82A36F98`, `sub_82CD1F20`, `sub_82A5A270`), and the gradually approached limit falling goes through.
  - Menu input: `sub_826E91E0` counts updates a button is held, repeating at counts from globals (delay 10, period 3, at
    0x831AE360/0x831AE35C). The count now advances at 30 Hz. A 3 s hold: 27 repeats at 30 fps, 27 at 120 fps, 101 unfixed.
  - Known, not fixed: values around -180 to 180 changing by 10 per update during launches, written from `sub_826DEDE0` and
    `sub_82965678` (they look like angles; nobody has reported a visible problem).
- Flickering distant shadows (fixed November 2026): plain shadow casters draw in depth only mode (RB_MODECONTROL 5), which D3D
  selects for a null pixel shader while the previous shader stays loaded; the hardware doesn't run it (`EdramMode` in Xenia's
  xenos.h). The renderer did. That leftover was the cut-out casters' shader 4018513F (texture fetch, then kill), and the plain
  casters export no coordinates, so one texel of whichever texture the last cut-out caster bound decided all or nothing for
  the whole draw. Draw order changes per frame, so buildings dropped out for single frames. `AR_DEPTH_ONLY_PS` restores it.
  Large one frame changes of the far cascade: 26 and 19 per run before, 3, 0 and 0 after.
- Shadows popping as the camera moves (November 2026): after the flicker above was fixed, what was left came from two planes at fixed distances from the camera, in the shadow mask's shader (47A3CAD5..., resolved to 0x0E75F000 first, alpha; the second resolve there each frame is object motion). It stops shadowing at view depth c186.w = 100 all at once, and turning the camera sweeps that plane through the world: 358-458 flips per 10000 pixels per frame at 98-102 units against about 1 elsewhere that far out, whole towers at a time. `FadeShadowsAtTheirEnd` (`gpu/renderer.cpp`) fades over the last fifth instead: 0, 0, 0. And the bias per cascade (c185.x for the middle one, z for near and far) differs by a factor of 3.5, so surfaces change at the near/middle switch at 10 units: 10-21 there, 1.2-1.9 with them equal, with a clean dose response (9.2, 4.5, 0.22 for 0.00078, 0.0016, 0.00273). Off with `AR_NO_SHADOW_FADE`, `AR_NO_SHADOW_BIAS_MATCH`.
- Ruled out for that with measurements, so don't chase them again: the receiver's matrices lagging the casters' (4500 of 4500 the same frame), cascades moving by fractions of a texel (they snap to whole ones; the scale steps on about 7% of frames while moving, with no spike in changes), render scale (at equal frame rates 2x is no worse), polygon offset (the title turns it off for its casters; now implemented anyway, `AR_NO_POLY_OFFSET`). The light is fixed and the far cascade is fine.
- What's left is near the characters (92% of the flips, within 8 units), mostly their shadows really moving. Characters moving can't be told from flicker there by this measure.
- D3D's `IDirect3DQuery9::GetData` is `sub_8223F950` (device fence progress at device + 11036, completed value at
  *(device + 11024)); the title's QueryPerformanceCounter is `sub_8245B458`.
- Effects pass depth is restored by drawing saved depth words as colour at EDRAM 0x5B7; the shadow map depth buffer is
  EDRAM base 128, pitch 1040, D24FS8, cleared by a 1x strip plus a 4x MSAA alias rectangle; the shadow mask is the
  alpha of resolve 0x0E75F000 (pixel shader 47A3CA).
- Button prompts and the controller diagram are textures, not text: a 256 x 256 icon atlas (hash 6588FD33...) and a 512 x 512
  diagram atlas (86739066...), both BC3, white with the picture in alpha and black where empty in the icon atlas. Swapping
  them per device changes every prompt that uses them, with no game files touched. Dump textures with `AR_DUMP_TEXTURES=<frame>`.
- Adding rows to a menu without changing the title's menu code (`kernel/keyboard_mouse.cpp`, `OptionsSprite`/`OptionsText` in
  `gpu/draw_hooks.cpp`): sprite and text items are rebuilt each frame and their translate is applied when drawn, so a draw hook can move
  them, or call the draw again with another translate and scale to get another row that blends exactly like the title's (the highlight
  is additive, which an overlay can't copy). New text and icons go in an overlay using the title's own textures, found by hash at
  upload, so nothing of the game's art is stored. The title's cursor keeps waiting on a neighbouring row with its highlight hidden.
  There are two Options screens with different code (pause: `sub_827F2370`, seven rows; main menu: `sub_82E29220` with state 22, nine rows),
  so the selection is read from the highlights drawn rather than from either menu's object.
- When an overlay copies a value from a UI item, check nothing else matches. The Options names flickered because the fade level was read from the big title's text, and a faint animated copy of it passes over the same place.
- Windows that pop up during gameplay (something unlocked) leave the menu flags at 0x83214CF0 as they were, so "in a menu" also has to mean such a window is being drawn (`AR_TEST_IGNORE_MENU_FLAGS` tests that path on the pause menu).
- Fonts are cut down to the letters a screen needs: the title screen's distressed stencil has only W D P R B I H S A C E L K T. The soft stencil for big titles, in the 2048 x 1024 atlas, has them all, and with hard edges (a threshold in the overlay's shader) passes for the same lettering.
- Sounds: `sub_8297F6E0(0x820A5F58, id, 0)`. Menus: 544 move, 545 change value, 542 confirm, 543 back; call it from the menu's own update.
- Camera: the camera object is at 0x831C51C0: +5504 and +5508 invert vertical and horizontal, +6100 and +6104 normal and aiming speed, +5936
  and +5940 its angles. `sub_82E693B8` sets those from the saved settings each update and turns the camera inside the same call, so a value
  written after it returns is too late; `sub_82E5EFC8` (the speed lookup) is called right after the invert flags are set, which is the place
  to change them. The stick has a dead zone of 0.4 per axis. The speed table at 0x820ADEF8 is only 0.6 to 1.5.
- The Gallery's character viewer: `sub_82E3AC60` updates it by a state at +156; state 2 runs `sub_82E1C508`, the camera. It adds a fixed amount per update (zoom 0.4 at camera +628, tilt and turn 0.3 times the stick squared, moving 0.05 at +580, ten times that for the largest characters), so above 30 fps all of it ran 2-4 times too fast. Even at 30 fps a held stick crosses the whole zoom range in a third of a second. Turning goes through `sub_8270B3C8` on the character at 0x831C3110.
- Result screens are classes with an update in vtable slot 3 (f1 is the UI's step). Timers use it, but counting up doesn't: `sub_828042B8` (mission; `sub_827F0808`/`sub_827F08F0` bring each score half way to its total per update, state 3 step 1 raises the unlock bar by a thirtieth per update) and `sub_82803688` (state 2, a thirtieth per update). Each counts only on the updates 30 fps would have had. `sub_82802C08` and `sub_82803C40` only use timers.
- Verify settings by measuring what they should change. Writing a value and seeing it stick proved nothing here: reading the camera's angles with
  injected mouse movement showed the invert flags had no effect while the values looked right.
- The controller's Back button opens the mission list, not a map.
- Menu state: the word at 0x83214CF0 has bit 0x02000000 on the title screen and main menus and 0x00200000 in the pause menu and
  what it opens (0000C000 in gameplay). Menus are separate classes in 0x8275-0x8283 that copy the pad state into themselves and
  dispatch on a byte at +177; their text isn't plain text in memory. Row text ids aren't the cursor: 0x4090285C (0x2A + row in
  Options) is the description's text id.
- Testing: `AR_AUTO_SEQUENCE` drives the menus (see `tools/test/common.sh`); Training > Practice has no enemies.

## Graphics options (launcher, all off by default)

- Ultrawide: `sub_82BB8AB0` makes a camera's aspect from two screen size integers and passes it to `sub_82703DF8`, which builds the projection, so changing it there widens the view, and the frustum and every shader that reconstructs positions follow. The 13 copies of 16:9 found in memory aren't what it reads. The render targets stay 16:9 and are shown wider, so horizontal detail is spread out: use a higher internal resolution with it.
- The UI goes through the same sprite and text draws, so narrowing their x (`DrawUiItem`) puts it in the right shape in the middle 16:9. Whole-width items and big pieces reaching an edge (dimming comes in halves) stay wide; strips from an edge stay anchored to it. Movies have no camera, so while `sub_82703DF8` hasn't run for 300 ms the picture is plain 16:9 with bars.
- Not corrected: markers placed from a 3D position (lock-on, name tags) are computed in the widened picture and then narrowed with the rest, so they may sit off their targets away from the middle. Not checked in play.
- FXAA and sharpening are in the present shader, after the title has drawn its HUD, so they touch the HUD too; the overlay is drawn afterwards. Anisotropic filtering and the texture detail bias apply to samplers that are linear and have mips, which leaves the UI and shadow maps alone.
- DLSS needs depth, motion vectors and jitter. The title's motion buffer (second resolve of 0x0E75F000) only has objects; camera motion would have to come from depth and matrices, as `tools`-style reprojection does. It also needs splitting the picture before the HUD. DLSS 5 reportedly wants an RTX 50.
- Ray traced shadows: 292 caster draws, about 290000 triangles per frame, so the size is fine. The blocker is getting world positions from the title's vertex shaders: stream output (`AR_SHADOW_SO`) still captures 0 vertices, and the debug layer says nothing about why. Cut-out casters (mesh grates) would also need their alpha in the rays.

## Open issues

- Short bursts (about 2 s) where the GPU thread is uniformly ~4x slower, mostly with other games running.
- 120 fps setting: correct speed; light scenes 110-120 fps, the heaviest fights 56-85 (the GPU command processor
  thread is saturated, and the title's render thread is close behind).
- Angular light patches around characters in tiled deferred lighting (pixel shader E9891C vs 57FBD2), possibly a
  translation bug; thin vertical ticks under ledge edges in the Foundry.
- Bright washed-out frames while fighting or facing the sun: not looked into, may be the title's own bloom and hit flashes.
