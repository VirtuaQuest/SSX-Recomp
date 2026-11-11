# Debugging reference

Switches are environment variables read once at startup (`AR_ENV` in `stdafx.h`). Set them on the command line of
a run from `run/`, for example `AR_TRACE_FRAME=2900 ../out/build/ARRecomp/ARRecomp.exe --game <iso> --screenshots`.

## Command line

| Argument | |
|---|---|
| `--game <iso or folder>` | Game files |
| `--xex`, `--user` | Executable and user data folder overrides |
| `--resolution WxH`, `--display windowed\|borderless\|fullscreen`, `--fps 30\|60`, `--render-scale 1-4`, `--antialiasing 0\|1`, `--anisotropic 0\|2\|4\|8\|16`, `--texture-lod-bias F`, `--sharpen 0-3`, `--ultrawide`, `--vsync` | Video settings (what the launcher passes) |
| `--online`, `--online-peer <IPs>`, `--player-name <name>`, `--controller <1-4>` | Multiplayer between copies (see LESSONS); which controller this copy reads |
| `--make-icon` | Takes the icon from the game, unseen and silent, into `user/game_icon.ico`, then exits (Setup uses this) |
| `--screenshots` | Saves frames and dumps to `run/screenshots` |
| `--trace`, `--gpu-survey`, `--headless`, `--dump-threads N` | Tracing, shader survey, no window, guest thread dump after N seconds |
| `--write-key-bindings` | Writes `user/key_bindings.ini` with the defaults if there's none, then exits |

`AR_LOG_FILE=<path>` writes the log there as well as to the console (Recomp Launcher sets it), with the last three
runs' logs kept beside it (`<name>-1.log` to `-3.log`). Started by Recomp Launcher (`RECOMPLAUNCHER` set), a build older
than the `VERSION` file of the source it's in asks before playing, and exits with 75 on No; `AR_NO_VERSION_CHECK=1`
skips that.
`RECOMPLAUNCHER_MODE=bigpicture` starts borderless when there's no `--display`.

The build without a window: `tools\recomplauncher\prepare.cmd --game <iso>` (FFmpeg if missing, then
`out\setup\ARPrepare.exe --prepare --game <iso> [--dlc <folder>] [--any-version]`). It builds into `out\build` like
Setup and plays from `out\build\ARRecomp`, not `run\`. See [RECOMPLAUNCHER.md](RECOMPLAUNCHER.md).

## Tests (`tools/test`)

| Script | |
|---|---|
| `fight.sh <name> ["ENV=1"] ["--fps 60"]` | 200 s fight benchmark: frame rate and hitch count |
| `trace_frame.sh <frame>` | Logs and dumps one frame, writes a condensed summary |
| `shadow_flicker.sh <name> ["ENV=1"]` | Far shadow cascade every frame, flicker scores (`AR_DUMP_ONLY=1` turns the dumps and their GPU waits off) |
| `game_speed.sh <name> ["--fps 60"]` | World speed per real second, to check game speed at a frame rate |

Arguments after the name can be mixed: `VAR=value` words become environment settings, the rest go to the game
(`fight.sh run1 AR_SINGLE_THREAD=1 --fps 60`). `AR_ISO` and `AR_EXE` override the disc image and executable.
Analysis: `perdraw.py` (fight logs: GPU thread time per draw, the yardstick for renderer speed), `summ.py` (trace summary), `altscore.py` and `period.py` (flicker), `speed.py` (game speed), `drawsets.py`,
`worldcmp.py` and `regdiff.py` (`AR_SHADOW_LOG` logs: draws, constants, context registers per frame), `toggles.py`
and `markcmp.py` (far cascade draws appearing or vanishing at a frame; with `AR_MARK_DRAWS`, their guest call chains),
`pxdrop.py`, `pxjump.py` and `pxcorr.py` (`AR_SHADOW_PIXELS` logs: draws that stop rasterizing for a frame, sample
count jumps, sample counts against a dump region), `pxwindows.py` (runs of failing draws in a frame) and `pxprobe.py`
(whether a probe rendered where the draw failed). `pxdrop.py` is the most reliable flicker measure: it counts
casters that vanish for one frame, which `altscore.py` (image based) often misses.

## Frames, dumps and traces

| Switch | |
|---|---|
| `AR_TRACE_FRAME=<n>`, `AR_TRACE_FRAMES=<count>` | Log every draw and resolve of these frames and dump their resolves |
| `AR_DUMP_FRAME=<n>`, `AR_DUMP_FRAMES=<count>`, `AR_DUMP_EVERY=<n>` | Dump resolves of these frames without logging draws (every nth of them) |
| `AR_DUMP_ONLY=<hex addresses, comma separated>` | Limit dumps to resolves to these addresses (use it: dumps are big) |
| `AR_DUMP_ALPHA`, `AR_DUMP_SCALE=<value>` | Dump alpha as grey; scale for single float dumps (default 100 is white); 16 bit single channel resolves dump as grey |
| `AR_DUMP_SOURCES`, `AR_DUMP_TEXTURES`, `AR_DUMP_IMAGE=<path>` | Resolve sources, uploaded textures, the loaded image, the LZ decompressor's output |
| `AR_SCREENSHOT_INTERVAL=<frames>` | Screenshot period with `--screenshots` (default 300) |
| `AR_TRACE_PS=<hash prefix>`, `AR_TRACE_INPUTS`, `AR_TRACE_FETCH` | Constants of draws with a pixel shader, hashes of each draw's inputs, fetch details |
| `AR_HIGHLIGHT_PS=<prefixes>`, `AR_HIGHLIGHT_PRIM=<type>` | Draw those in magenta (prefixes comma separated). With `AR_DUMP_FRAME` and `AR_DUMP_ONLY` on a G-buffer, groups of shaders show which one draws something wrong |
| `AR_SHOW_EXPR=<HLSL float4>` | What highlighted draws output instead of magenta, e.g. `float4(XE_R(7).yzw, 1.0f)` (register 7 as the shader leaves it; `input.position` can split the screen to show several) |
| `AR_SKIP_PS=<prefixes>`, `AR_SKIP_VS=<prefixes>` | Skip draws by shader |
| `AR_SHADOW_LOG` | One line per shadow map draw (S, IB), its resolves (SR), clear rectangles (SC), EDRAM syncs into it, mid-frame upload flushes (FW) and the light matrices (L), for the `AR_DUMP_FRAME` frames |
| `AR_SHADOW_PIXELS=1\|2\|3` | With `AR_SHADOW_LOG`: occlusion query per shadow map draw, logged as PX lines (frame, cascade, shader, vertex buffer, index offset, count, ..., samples). 2 adds hashes of everything the draw reads (vertex data, indices, constants) checked again once the GPU is done (slow); 3 also draws each again without depth testing and logs the rasterized `area` |
| `AR_SHADOW_CONSTANTS=<vertex buffers>` | With the above: float constants (PC), context registers (PR) and system constants (PY) of far cascade draws from those buffers |
| `AR_MARK_DRAWS` | NOP packets before each title draw holding a hash of the guest call chain and the model (MK lines name the chains; IB lines show the marker) |
| `AR_CHECK_VERTEX_MEMO` | Vertex uploads reused within a command list although guest memory changed |
| `AR_SHADOW_STATS` | With `AR_SHADOW_PIXELS`: per draw pipeline statistics, pixel shader, alpha and depth state, texture 0 and writes to it |
| `AR_SHADOW_READBACK` | The far cascade copied every frame without waiting, texels written per grid cell logged as SM lines; `smflick.py` counts flicker from them (`shadow_flick.sh`, or `shadow_stats.sh` with the per draw logs) |
| `AR_DEPTH_ONLY_PS` | Runs the leftover pixel shader in depth only mode (the shadow flicker) |
| `AR_SHADOW_PIXELS=4..6` | Probe variants: 5 shrinks the viewport transform 64x about its centre, 6 also draws points |
| `AR_SHADOW_CPU_VS` | The far cascade's static caster shader run on the CPU per draw (`cpu inside/total`, depth range) |
| `AR_SHADOW_GPUCHECK` | The GPU copies each logged draw's inputs back after the draw; PX lines say if they differ from what the CPU wrote |
| `AR_SHADOW_FRESH=<c,v,i>`, `AR_SHADOW_REBIND`, `AR_SHADOW_LATE` | Probe with fresh copies of constants/vertices/indices; after rebinding everything; just before the far cascade's resolve |
| `AR_SHADOW_DEFAULT_COPY=1\|2`, `AR_SHADOW_DEFAULT_ONLY=<c,v,i>` | Probe reading copies in video memory (2: copies made, upload memory still read) |
| `AR_SHADOW_SO=<vertex buffers>` | Stream output capture of positions (wired up, captures nothing yet) |
| `AR_FORCE_SHADOW_CLEAR` | Experiment: clears the whole shadow map at the far cascade's first clear |
| `AR_LOG_RESOLVES`, `AR_LOG_UNMATCHED`, `AR_LOG_PRESENT`, `AR_LOG_CREATES=<frame>` | Resolves, textures overlapping resolves but not matched, presents, texture creations |

## Performance

| Switch | |
|---|---|
| `AR_PROFILE` | Per-stage draw timing every 300 frames (adds clock reads per draw) |
| `AR_LOG_FPS` | Frame rate every 300 frames and the GPU thread's busy time every 5 s, without the per-stage cost |
| `AR_GPU_TIMING` | GPU time per frame, split into layout conversions, render target syncs, resolves, composites and draws |
| `AR_GPU_PASSES=1` or `=2` (with `AR_GPU_TIMING`) | Also the 16 costliest render target bindings (1) or pixel shaders (2): GPU ms and draws a frame, from a timestamp where each starts (work between them, resolves and conversions, counts too) |
| `AR_HOST_PROFILE=<thread name part>` | Samples host threads, e.g. `command` for the GPU thread (no spaces) |
| `AR_THREAD_PROFILE` | Samples guest threads (slows the game; don't judge frame rates with it) |
| `AR_LOG_SWAP_LATENCY`, `AR_LOG_FENCES`, `AR_LOG_GPU_WAITS`, `AR_LOG_IBS` | Swap latency, fences, waits, indirect buffers |
| `AR_LOG_TIMEBASE` | Who reads the clock |
| `AR_SCAN_KEY` | F9 scans for values changing by a fixed amount per update while playing; F10 also logs their writers for 10 updates, slowing the game for those. Compare scans with `timerdiff.py` |
| `AR_SCAN_TIMERS=<update>\|@<seconds>`, `AR_SCAN_TIMERS_WATCH` | Values counting updates instead of time (run at `--fps 120`), and who writes them |
| `AR_LOG_PRIORITIES`, `AR_LOG_DELAYS`, `AR_LOG_READS`, `AR_LOG_SHA` | Kernel calls |
| `AR_CHECK_ALU` | Constant memory changing after LOAD_ALU_CONSTANT read it |
| `AR_D3D12_DEBUG`, `AR_D3D12_GBV` | D3D12 debug layer with its messages in the log; GPU-based validation (very slow) |
| `AR_CHECK_UPLOADS` | Upload memory static shadow casters read, checked again once the GPU is done with the frame |
| `AR_LOG_WAIT_TIMEOUTS` | Kernel waits with a timeout that time out, per thread and object (5 s summary) |
| `AR_DUMP_THREADS_FILE=<file>` | When that file appears, what every guest thread is doing, waiting ones too, with stacks: for a game that has stopped |

## Turning features off (bisecting)

| Switch | Turns off |
|---|---|
| `AR_SINGLE_THREAD` | Submission thread (records on the GPU thread) |
| `AR_NO_TEXTURE_POOL` | Reusing released textures |
| `AR_NO_NATIVE_RESET` | Guest strip indices with the host's restart value |
| `AR_NO_COMPOSITE` | Assembling textures from consecutive resolves |
| `AR_NO_COLOR_TO_DEPTH`, `AR_NO_LAYOUT` | Colour to depth EDRAM aliasing, MSAA layout conversion |
| `AR_NO_LAYOUT_BOUND`, `AR_LOG_LAYOUT=<frame>` | Layout conversions of whole render targets instead of their written rows; that frame's conversions (size, format, why) |
| `AR_NO_EARLY_DEPTH` | Pixel shaders output depth rounded to the guest's 24 bit float again (`xe_depth_24`), which costs early depth rejection |
| `AR_NO_PIPELINE_PREWARM` | Pipelines from the saved key list (`pipelines.bin.keys`) are loaded at their first draws instead of at startup |
| `AR_NO_FAST_MUL` | Every shader multiplication keeps shader model 3 rules (zero times anything is zero), not only those of values that may be infinite or NaN; slower, as the selects stop multiply-adds from fusing |
| `AR_NO_VOLUME_RESOLVES` | Resolves into slices of a 3D texture (RB_COPY_DEST_INFO bit 3) reaching the volume: SSX's snow terrain then shows flat polygons |
| `AR_NO_TILED_3D` | Tiled 3D textures untiled through the volume (Xenia's Tiled3D) instead of as 2D slices |
| `AR_NO_USER_CLIP_PLANES` | User clip planes (PA_CL_CLIP_CNTL ucp_ena, PA_CL_UCP_n) |
| `AR_NO_RESOLVED_MIPS` | Mip levels the title resolves itself after a texture's mip address (SSX's per-pixel exposure): such fetches read the base level only, which leaves trails |
| `AR_NO_7E3_CLAMP` | Clamping pixel shader output to 2_10_10_10_FLOAT targets to the console's 0 to 31.875 |
| `AR_NO_GEOMETRIC_SAMPLES` | 1x and 4x MSAA views of the same EDRAM reorder their samples as the console stores them (a layout conversion each switch) instead of keeping host texels at their geometric positions |
| `AR_NO_LAYOUT_SWAP` | Layout conversions copy the target and draw back into it, instead of drawing into a scratch that takes its place (or, for depth with stencil in use and no SV_StencilRef, through an R32F target) |
| `AR_GPU_TIMING` (with `AR_LOG_FPS`) | GPU time per frame by kind: layout conversions, syncs, resolves, draws |
| `AR_NO_POINT_SNAP`, `AR_NO_FILTER_OVERRIDE` | Point sample snapping, instruction filter overrides |
| `AR_NO_DEPTH_24` | Keeping depth to the guest's precision (see LESSONS) |
| `AR_NO_DEPTH_CLIP`, `AR_NO_DEPTH_TEST` | Depth clipping (clamps instead), depth testing |
| `AR_NO_STATE_CACHE`, `AR_FULL_HASH`, `AR_FULL_CONSTANTS`, `AR_FULL_VERTEX_BUFFERS` | Skipping redundant state, sampled hashes, constant and vertex range trimming |
| `AR_NO_BULK_REGISTERS` | Copying constant and register blocks without side effects in one piece |
| `AR_NO_CPU_SETS`, `AR_NULL_CUBE`, `AR_WAKE_ALL`, `AR_GPU_LAG` | Performance core selection; cube textures; experiments |
| `AR_SERIALIZE_FRAMES`, `AR_NO_VERTEX_CACHE`, `AR_STRIPS_AS_LISTS` | Experiments: GPU finishes each frame first; no cross-frame vertex cache; strips drawn as lists |
| `AR_UPLOAD_RING`, `AR_ROTATE_UPLOADS`, `AR_UPLOAD_WRITE_BACK`, `AR_UPLOAD_FENCE`, `AR_VRAM_UPLOADS` | Upload memory experiments: 1 GB without reuse for frames; shifted per frame; cache coherent; fenced; mirrored in video memory |

## SSX additions

| Switch | |
|---|---|
| `AR_TRACE_IMPORTS=<prefixes>` | Logs calls of imports whose names start with these (comma separated), e.g. `Xam,XMsg` |
| `AR_LOG_XMA`, `AR_NO_XMA` | XMA contexts as the title sets them up (init, buffers, valid bits, enable and disable); the decoder off (silent) |
| `AR_DUMP_AUDIO=<file>` | The final mix as raw 32 bit float stereo at 48 kHz (`ffmpeg -f f32le -ar 48000 -ac 2 -i <file> ...`), to check audio without listening |
| `AR_CACHE_DRIVE` | Mounts `cache:` (user/cache); off by default, see LESSONS |
| `AR_NO_HW_THREADS`, `AR_LOG_HW_THREADS` | Guest hardware threads pinned to host logical processors; where threads go |
| `AR_NO_TICKER_SLEEP`, `AR_LOG_TICKERS` | SSX's ticker threads yield instead of sleeping a millisecond (`kernel/title_hooks.cpp`); log each ticker's interval |
| `AR_NO_OFFLINE_MODE` | SSX's main menu shows "Connect to EA Server" again, and its online messages say to sign in to Xbox LIVE (`kernel/title_hooks.cpp`) |
| `AR_LOG_TEXT` | Every localized text lookup of SSX's (ID and English text), to find what a screen shows |
| `AR_NO_SIGNIN_BYPASS` | SSX shows its "You must be signed in to Xbox LIVE and the EA Servers" notice at boot again (`kernel/title_hooks.cpp`) |
| `AR_NO_KEYSTROKE_DEVICES` | Keystroke reads ignore their device flags (pad keys go to keyboard-only reads too) |
| `AR_CRASH_DUMP=<registers>` | 1 KB of guest memory where these registers (numbers, comma separated) point, after a crash |
| `AR_NO_TEXTURE_EXP_BIAS`, `AR_NO_BIASED_FETCH` | Fetch constants' exponent bias; unsigned biased components (then read as unsigned) |
| `AR_NO_MIPS`, `AR_NO_BC_DECOMPRESS` | Textures at their base level only; odd sized block compressed textures left as they are |
| `AR_CRASH_DUMP_THREADS` | Every guest thread's stack after a crash |
| `AR_D3D12_DEBUG` | Where the debug layer has no callback, its stored messages and the device removal reason are logged when a creation fails |

## Keyboard and mouse

SSX: every controller input has keys in `user/key_bindings.ini` (written with the defaults the first time, each with
what it does in SSX: see the README's table; a file with the previous title's defaults is replaced). `--write-key-bindings`
writes the file and exits (the launcher's "Edit keys"). `AR_MOUSE_STICK_GAIN` scales the mouse. Controller slots found
empty by the 250 ms connection check aren't asked about (0.1 ms per call to XInput for an empty slot, SSX asks about 300
times a second); `AR_NO_PAD_MASK` asks every time.
`kernel/keyboard_mouse.cpp` merges them into controller 0. `AR_NO_KBM` turns them off. Runs driven by `AR_REMOTE` or
`AR_AUTO_PRESS` leave them off unless `AR_KBM` is set, so a test window in front doesn't take your typing. The window's
icon is the title's own, in `user/game_icon.ico` (`AR_NO_GAME_ICON` leaves it out).

## Driving the game without a controller

`AR_AUTO_PRESS=<seconds>` and `AR_AUTO_SEQUENCE=<steps>` press buttons in order (A, B, X, Y, START, UP, DOWN, LT,
LTHOLD, MOVE<degrees>, WAIT). `AR_AUTO_FRAMES` steps by presented frames instead of time. Test runs also read a
connected controller.

`AR_REMOTE=<file>` drives the game live instead: lines appended to the file run in order. A line lists buttons, `L<degrees>` and
`R<degrees>` for the sticks and a duration in ms (`L0 A 400`); a bare number waits. Also `shot <name>` (saves
screenshots/<name>.bmp, needs no `--screenshots`), `scan`, `scanwatch`, and `find new|changed|same|list`, `find watch <addresses>`
(writers, for 120 updates), `find readwatch <addresses>` (readers too), `find addcpu <count>` (adds CPU players to the session) and `track <address> [floats]` to search and log memory. It steers by real time, so it doesn't depend on
frame rate, but walking to a place by timing alone isn't reliable. A connected controller still works alongside it.

`AR_FRAME_TRACE=<file>` writes small copies of the final picture and the resolves to `AR_FRAME_TRACE_BASES` (hex) every frame without waiting on the GPU (`AR_FRAME_TRACE_FROM`, `_FRAMES`, `_STEP`, `_FINAL_STEP` for the final picture (1 is guest size, about 2 MB a frame); `_LIST` logs what resolves there are); `tools/test/frametrace.py` reads them. They're about 1 MB a frame at step 2, so keep the range short. `AR_SHADOW_RECEIVER_CONSTANTS=<first>,<count>;...` logs float constants at each frame's first draw that samples the shadow maps (RC lines). `find poke <address> <hex>` sets a word before every update.

`AR_KBM=1 AR_KBM_TEST=1` tests the keyboard and mouse without real input, so nothing can land in another window: `key <vk> <0|1>`, `cursor <x> <y>` (on the 1280 x 720 picture) and `mouse <dx> <dy>` lines go to the remote file, and the game acts as if it were in front. Run it in a folder of its own with a copy of `user/` to leave saves and settings alone. `AR_LOG_CAMERA` logs which code sets where the camera looks, as it changes. `AR_LOG_RESULTS` logs the result screens' steps with the time each took; `AR_NO_RESULT_STEP_FIX` and `AR_VIEWER_ZOOM_SPEED` (1 is the original's, default 0.2), `AR_VIEWER_MOUSE_SPEED` go with those fixes.

`tools/test/world_speed.sh <name> --fps <n>` continues the campaign and measures running speed: it catches large errors such as
slow motion. Peaks vary between runs and routes (6-9 at every frame rate so far), so it can't show small ones.

`AR_ONLINE=1` is `--online`; `AR_NET_IP` overrides this machine's address; `AR_ONLINE_TRACE` logs packets and traffic per socket. `find watch` and `AR_FRAME_TRACE` work as usual on each copy, in its own folder.

Guest memory can also be read from outside with `ReadProcessMemory` at 0x100000000 + address, which doesn't disturb timing: snapshots over a run, with the player's position at player + 0x40, found through the word at 0x831C51C0 + 6208.

## Memory watches

`AR_WATCH_PHYSICAL`, `AR_WATCH_VIRTUAL`, `AR_WATCH_MOVIE`: log writes to an address or the intro movie's frame buffer.
`AR_WATCH_PHYSICAL_SIZE` widens the physical watch to a range (`AR_WATCH_DELAY` works for both kinds), `AR_WATCH_LIMIT` sets how many distinct writers are
logged (64; writes logged when watching one address), `AR_WATCH_STACK` how many frames of each writer's stack (8) and `AR_WATCH_DELAY=<seconds>` arms it late (memory written every frame makes the game crawl).
