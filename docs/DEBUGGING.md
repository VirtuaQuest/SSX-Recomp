# Debugging reference

Switches are environment variables read once at startup (`AR_ENV` in `stdafx.h`). Set them on the command line of
a run from `run/`, for example `AR_TRACE_FRAME=2900 ../out/build/ARRecomp/ARRecomp.exe --game <iso> --screenshots`.

## Command line

| Argument | |
|---|---|
| `--game <iso or folder>` | Game files |
| `--xex`, `--user` | Executable and user data folder overrides |
| `--resolution WxH`, `--display windowed\|borderless\|fullscreen`, `--fps 30\|60\|120`, `--render-scale 1-4`, `--antialiasing N`, `--shadows on\|off` | Video settings (what the launcher passes) |
| `--screenshots` | Saves frames and dumps to `run/screenshots` |
| `--trace`, `--gpu-survey`, `--headless`, `--dump-threads N` | Tracing, shader survey, no window, guest thread dump after N seconds |

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
| `AR_DUMP_FRAME=<n>`, `AR_DUMP_FRAMES=<count>` | Dump resolves of these frames without logging draws |
| `AR_DUMP_ONLY=<hex addresses, comma separated>` | Limit dumps to resolves to these addresses (use it: dumps are big) |
| `AR_DUMP_ALPHA`, `AR_DUMP_SCALE=<value>` | Dump alpha as grey; scale for single float dumps (default 100 is white) |
| `AR_DUMP_SOURCES`, `AR_DUMP_TEXTURES`, `AR_DUMP_IMAGE=<path>`, `AR_DUMP_DECOMPRESS=<address>` | Resolve sources, uploaded textures, the loaded image, the LZ decompressor's output |
| `AR_SCREENSHOT_INTERVAL=<frames>` | Screenshot period with `--screenshots` (default 300) |
| `AR_TRACE_PS=<hash prefix>`, `AR_TRACE_INPUTS`, `AR_TRACE_FETCH` | Constants of draws with a pixel shader, hashes of each draw's inputs, fetch details |
| `AR_HIGHLIGHT_PS=<prefix>`, `AR_HIGHLIGHT_PRIM=<type>` | Draw those in magenta |
| `AR_SKIP_PS=<prefixes>`, `AR_SKIP_VS=<prefixes>` | Skip draws by shader |
| `AR_SHADOW_LOG` | One line per shadow map draw (S, IB), its resolves (SR), clear rectangles (SC), EDRAM syncs into it, mid-frame upload flushes (FW) and the light matrices (L), for the `AR_DUMP_FRAME` frames |
| `AR_SHADOW_PIXELS=1\|2\|3` | With `AR_SHADOW_LOG`: occlusion query per shadow map draw, logged as PX lines (frame, cascade, shader, vertex buffer, index offset, count, ..., samples). 2 adds hashes of everything the draw reads (vertex data, indices, constants) checked again once the GPU is done (slow); 3 also draws each again without depth testing and logs the rasterized `area` |
| `AR_SHADOW_CONSTANTS=<vertex buffers>` | With the above: float constants (PC), context registers (PR) and system constants (PY) of far cascade draws from those buffers |
| `AR_MARK_DRAWS` | NOP packets before each title draw holding a hash of the guest call chain and the model (MK lines name the chains; IB lines show the marker) |
| `AR_LOG_MODEL_DRAWS=<frame>,<count>`, `AR_HASH_MODEL_DRAWS` | The engine's model draw calls with guest call chains; per-frame count and hash |
| `AR_LOG_DRAW_CALLS`, `AR_LOG_QUEUES`, `AR_LOG_QUEUE_ITEMS=<queues>` | The title's draw packet writers per 5 s; render queue executions and their items |
| `AR_CHECK_VERTEX_MEMO` | Vertex uploads reused within a command list although guest memory changed |
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
| `AR_LOG_FENCE_WAITS` | Time the title's D3D spends waiting for GPU fences, per caller (82245630 is its frame pacing) |
| `AR_HOST_PROFILE=<thread name part>` | Samples host threads, e.g. `command` for the GPU thread (no spaces) |
| `AR_THREAD_PROFILE` | Samples guest threads (slows the game; don't judge frame rates with it) |
| `AR_LOG_SWAP_LATENCY`, `AR_LOG_FENCES`, `AR_LOG_GPU_WAITS`, `AR_LOG_IBS` | Swap latency, fences, waits, indirect buffers |
| `AR_LOG_QUERIES`, `AR_LOG_QUERY_RESULTS` | Occlusion query waits (5 s summary) and results |
| `AR_LOG_TIMEBASE`, `AR_LOG_QPC`, `AR_LOG_TIMESTEP` | Who reads the clock; the game's time step |
| `AR_LOG_UI_TIME`, `AR_LOG_UI_TIMESTEP` | How fast UI time runs per real second (1 is right); calls per UI step hook |
| `AR_SCAN_TIMERS=<update>\|@<seconds>`, `AR_SCAN_TIMERS_WATCH` | Values counting updates instead of time (run at `--fps 120`), and who writes them |
| `AR_WATCH_UI_TIME=<seconds>`, `AR_LOG_UI_ITEMS=<frame>,<count>` | Writers of UI element times; the UI's sprite draw items |
| `AR_LOG_PRIORITIES`, `AR_LOG_DELAYS`, `AR_LOG_READS`, `AR_LOG_SHA`, `AR_LOG_SAVE_ERRORS` | Kernel calls |
| `AR_CHECK_ALU` | Constant memory changing after LOAD_ALU_CONSTANT read it |
| `AR_D3D12_DEBUG`, `AR_D3D12_GBV` | D3D12 debug layer with its messages in the log; GPU-based validation (very slow) |
| `AR_CHECK_UPLOADS` | Upload memory static shadow casters read, checked again once the GPU is done with the frame |
| `AR_LOG_WAIT_TIMEOUTS` | Kernel waits with a timeout that time out, per thread and object (5 s summary) |

## Turning features off (bisecting)

| Switch | Turns off |
|---|---|
| `AR_SINGLE_THREAD` | Submission thread (records on the GPU thread) |
| `AR_NO_TEXTURE_POOL` | Reusing released textures |
| `AR_NO_NATIVE_RESET` | Guest strip indices with the host's restart value |
| `AR_WAIT_QUERIES` | Answering occlusion queries at once (polls like before) |
| `AR_NO_TIMESTEP_FIX`, `AR_NO_UI_TIMESTEP_FIX` | Normal game speed above 30 fps; normal UI speed (HUD, radar, dialogue, results) above 30 fps |
| `AR_NO_COMPOSITE` | Assembling textures from consecutive resolves |
| `AR_NO_COLOR_TO_DEPTH`, `AR_NO_LAYOUT` | Colour to depth EDRAM aliasing, MSAA layout conversion |
| `AR_NO_POINT_SNAP`, `AR_NO_FILTER_OVERRIDE` | Point sample snapping, instruction filter overrides |
| `AR_NO_DEPTH_CLIP`, `AR_NO_DEPTH_TEST` | Depth clipping (clamps instead), depth testing |
| `AR_NO_STATE_CACHE`, `AR_FULL_HASH`, `AR_FULL_CONSTANTS`, `AR_FULL_VERTEX_BUFFERS` | Skipping redundant state, sampled hashes, constant and vertex range trimming |
| `AR_NO_BULK_REGISTERS` | Copying constant and register blocks without side effects in one piece |
| `AR_NO_CPU_SETS`, `AR_NULL_CUBE`, `AR_WAKE_ALL`, `AR_GPU_LAG` | Performance core selection; cube textures; experiments |
| `AR_SERIALIZE_FRAMES`, `AR_NO_VERTEX_CACHE`, `AR_STRIPS_AS_LISTS` | Experiments: GPU finishes each frame first; no cross-frame vertex cache; strips drawn as lists |
| `AR_UPLOAD_RING`, `AR_ROTATE_UPLOADS`, `AR_UPLOAD_WRITE_BACK`, `AR_UPLOAD_FENCE`, `AR_VRAM_UPLOADS` | Upload memory experiments: 1 GB without reuse for frames; shifted per frame; cache coherent; fenced; mirrored in video memory |

## Driving the game without a controller

`AR_AUTO_PRESS=<seconds>` and `AR_AUTO_SEQUENCE=<steps>` press buttons in order (A, B, X, Y, START, UP, DOWN, LT,
LTHOLD, MOVE<degrees>, WAIT). `AR_AUTO_FRAMES` steps by presented frames instead of time. Test runs also read a
connected controller.

## Memory watches

`AR_WATCH_PHYSICAL`, `AR_WATCH_VIRTUAL`, `AR_WATCH_MOVIE`: log writes to an address or the intro movie's frame buffer.
`AR_WATCH_PHYSICAL_SIZE` widens the physical watch to a range (`AR_WATCH_DELAY` works for both kinds), `AR_WATCH_LIMIT` sets how many distinct writers are
logged (64) and `AR_WATCH_DELAY=<seconds>` arms it late (memory written every frame makes the game crawl).
