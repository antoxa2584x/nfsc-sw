<div align="center">

<img src="https://cdn2.steamgriddb.com/logo/c0eab2dce3fc614a18251fb483e71dee.png" alt="Need for Speed: Carbon" width="520">

### Xbox static recompilation for Nintendo Switch and Linux

The original Xbox (NTSC-U) release of **Need for Speed: Carbon**, lifted
instruction by instruction to C and running natively, with no emulator.

![Switch](https://img.shields.io/badge/Nintendo%20Switch-homebrew-E60012?logo=nintendoswitch&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-x86__64-FCC624?logo=linux&logoColor=black)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3-AC162C?logo=vulkan&logoColor=white)
![OpenGL](https://img.shields.io/badge/OpenGL-renderer-5586A4?logo=opengl&logoColor=white)
![Version](https://img.shields.io/badge/version-0.2-blue)

[Features](#-features) · [Playing on Switch](#-playing-on-switch) · [Building](#%EF%B8%8F-building) · [Configuration](#%EF%B8%8F-configuration) · [Status](#-status)

</div>

---

> [!IMPORTANT]
> **No game data is included.** You need your own copy of the Xbox disc,
> extracted (`default.xbe`, `NFS/`, `TDATA/`, ...).

## ✨ Features

- 🏁 **Native code**: the whole game runs as recompiled C, built with
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
- 🎮 **Two renderers**: Vulkan (NVK on Switch) and OpenGL, with render scaling
  up to 4x
- 🔊 **Audio** through an emulated Xbox APU, with 5.1 downmixed to stereo
- 🕹️ **Pads** through an emulated Xbox USB host (OHCI), Joy-Con and Pro
  Controller on Switch, SDL controllers on Linux
- 🛠️ Shares its runtime with the
  [NFSU2](https://github.com/antoxa2584x/nfsu2-sw) port (Carbon uses the
  same XDK 5849 libraries)

## 🕹️ Playing on Switch

1. Copy the NRO to `sdmc:/switch/nfscx/` (`nfscx.nro` for OpenGL,
   `nfscx-vulkan.nro` for Vulkan).
2. Copy the **extracted** disc (not the ISO) to `sdmc:/switch/nfscx/game/`.
3. Start it with **title takeover**: hold **R** while launching any game.
   Applet mode leaves too little memory.

```
sdmc:/switch/nfscx/
├── nfscx.nro           or nfscx-vulkan.nro
├── nfscx_env.txt       optional settings, KEY=VALUE per line
└── game/
    ├── default.xbe
    ├── NFS/
    └── ...
```

Buttons map by label (Switch A = Xbox A). Settings go in `nfscx_env.txt`
(see [Configuration](#%EF%B8%8F-configuration)); the log (`nfscx_log.txt`) and
the shader cache are written to the same folder.

## 🛠️ Building

<details open>
<summary><b>1. Lift the XBE to C</b></summary>

```sh
# Writes NFSC_GEN_DIR (default /root/nfscx/gen)
NFSC_XBE=/path/to/game/default.xbe NFSC_GEN_DIR=/path/to/gen tools/regen.sh
```

</details>

<details open>
<summary><b>2a. Linux</b> (SDL2 + OpenGL or Vulkan)</summary>

```sh
cmake -S . -B build -G Ninja -DNFSU2_GEN_DIR=/path/to/gen   # add -DNFSU2_VULKAN=ON for Vulkan
cmake --build build
NFSU2_GAME_DIR=/path/to/game build/nfsu2_recomp
```

The CMake options and runtime variables keep the `NFSU2_` prefix of the port
this one came from.

</details>

<details open>
<summary><b>2b. Nintendo Switch</b> (devkitA64, switch-sdl2, switch-mesa)</summary>

```sh
NFSU2_GEN_DIR=/path/to/gen NFSU2_GAME_SRC=/path/to/game JOBS=6 switch/build.sh
# Vulkan build (needs mesa-switch NVK and glslang for Switch)
VULKAN=1 JOBS=6 NFSU2_GEN_DIR=/path/to/gen switch/build.sh
```

</details>

### Project layout

| Path | What |
|---|---|
| `src/main.c` | boot and runtime defaults |
| `src/recomp_manual.c` | hand-written overrides of lifted functions (D3D/DSOUND/XPP library hooks, DSOUND watchdog skip, native leaf functions) |
| `src/switch_nx.c` | Switch log device, env file, exception handler, loading screen |
| `config/seed_functions.json` | entry points the static pass cannot see |
| `xboxrecomp/` | the toolkit (MIT), vendored with this port's changes: NV2A renderers (Vulkan, OpenGL), SDL audio, Switch platform layer, kernel and translator fixes |
| `tools/regen.sh` | XBE → lifted C (`gen/`, never committed) |
| `switch/build.sh` | Switch NRO build + SD-card staging |

## ⚙️ Configuration

On Linux these are ordinary environment variables. On the Switch they go in
`sdmc:/switch/nfscx/nfscx_env.txt`, one `KEY=VALUE` per line (`#` starts a
comment); the log lists each one as `[switch] env KEY=VALUE`. Anything left
out runs at its default. Most of the list is for debugging, so the sections
below are folded.

```ini
# sdmc:/switch/nfscx/nfscx_env.txt
RECOMP_FRAME_LAG=1
RECOMP_GL_THREAD=1
RECOMP_GIL_EAGER=1
RECOMP_GL_SCALE=1.5
```

<details>
<summary><b>Build and code generation</b></summary>

| Variable | Meaning |
|---|---|
| `XBOXRECOMP_DIR` | Toolkit to build or regenerate with (default: the vendored `xboxrecomp/`). |
| `NFSC_XBE` | `default.xbe` to lift (`tools/regen.sh`). |
| `NFSC_GEN_DIR` | `tools/regen.sh`: directory for the lifted C (default `/root/nfscx/gen`). |
| `NFSU2_GEN_DIR` | CMake / `switch/build.sh`: the lifted C to build. |
| `NFSU2_GAME_SRC` | `switch/build.sh`: extracted disc to stage (default `/root/nfscx/game`). |
| `SD_ROOT` | `switch/build.sh`: staging SD-card root (default `<repo>/switch_sd`). |
| `BUILD_DIR` | `switch/build.sh`: build directory. |
| `JOBS` | `switch/build.sh`: parallel compile jobs (6 fits `-O2` in RAM). |
| `VULKAN=1` | `switch/build.sh`: build the Vulkan renderer (`nfscx-vulkan.nro`). |
| `LTO=1` | `switch/build.sh`: link-time optimisation (`nfscx[-vulkan]-lto.nro`, next to the normal one). |
| `NVK_SDK`, `GLSLANG_DIR` | `switch/build.sh` with `VULKAN=1`: mesa-switch NVK install and Switch glslang. |

</details>

<details>
<summary><b>Game and host</b></summary>

| Variable | Meaning |
|---|---|
| `NFSU2_GAME_DIR` | Extracted disc (Linux; default `./game`). The Switch always uses `sdmc:/switch/nfscx/game/`. |
| `NFSU2_GL=0` | Use the executor's CPU renderer instead of the GPU renderer. |
| `NFSU2_APU=0` | With `RECOMP_AC97_READY=plain`: no emulated APU (no sound). |
| `NFSU2_SIM_STEPS` | Longest game-time step per frame, in 1/60 s (default 6 = 100 ms; 3 = the original 50 ms cap, which slows races below 20 fps). |
| `NFSU2_EXIT_TRACE=1` | Print the guest state at exit. |
| `RECOMP_WIDESCREEN=0` | Tell the game the TV is 4:3 (default 16:9). |
| `HOME`, `XDG_DATA_HOME` | Linux: fallback save directory (`$XDG_DATA_HOME/xboxrecomp`, else `~/.local/share/xboxrecomp`) when none is configured. The game's own saves go to `<game>/UDATA`. |

</details>

<details>
<summary><b>Switch (Horizon)</b></summary>

| Variable | Meaning |
|---|---|
| `NFSU2_NO_LOG=1` / `NFSU2_LOG=0` | No log file, no `[perf]` reports, no profiler. |
| `NFSU2_LOG_SYNC=1` | Write every log line to the card at once (slow; for hangs). |
| `NFSU2_LOADER=0` | No loading screen (logo + bar) before the first frame. |
| `RECOMP_NX_PROFILE` | Sampling profiler into `prof.bin`: 1 = busy threads, 2 = every thread above 2%. |
| `RECOMP_NX_PROFILE_AFTER=<s>` | Start the profiler after s seconds. |
| `NFSU2_CPU_MHZ`, `NFSU2_GPU_MHZ`, `NFSU2_MEM_MHZ` | Overclock (opt-in; more heat and battery). The log shows `[clock] ...`. |
| `RECOMP_NX_SHM=1` | Guest RAM from shared memory instead of code memory. |
| `RECOMP_NX_AUDIO_PRIO=0` | Leave the APU and SDL audio threads at priority 59 (default: above the game threads). |
| `RECOMP_NX_GUEST_RT=1` | Run the time-critical guest thread (EA mixer) at a real-time host priority. |
| `RECOMP_NX_JOYCON=single` | One sideways Joy-Con per player. |
| `RECOMP_NX_JOYCON_ROTATE=0` | Don't rotate the sideways Joy-Con stick. |
| `RECOMP_GUEST_ONE_CORE` | 1 = pin guest threads to one core, 2 = guest threads only (interrupts float). |

</details>

<details>
<summary><b>Scheduling and kernel</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_GIL=0` | No guest lock (guest threads run in parallel; races the title's streaming). |
| `RECOMP_GIL_EAGER=1` | Hand the guest lock over by guest priority (main thread and time-critical threads pre-empt). |
| `RECOMP_KERNEL_FAST=0` | Release the guest lock in every kernel call, even the cheap time queries and IRQL changes. |
| `RECOMP_FRAME_LAG=1` | Wait on the previous frame's fence instead of the current one (game and GPU work overlap). |
| `RECOMP_VBLANK` | Vblank interrupt for D3D (default 1). |
| `RECOMP_WORKERS=inline` | Run the title's worker routines inline instead of on threads. |
| `RECOMP_ASYNC_IO=1` | Asynchronous NtReadFile. |
| `RECOMP_CS_MODE=single` | Every guest critical section behind one recursive lock. |
| `RECOMP_NATIVE=0` | Use the lifted culling/math functions instead of the native C versions. |
| `RECOMP_NATIVE_CHECK=1` | Run both and report any difference. |
| `RECOMP_QUIET` | 1 = no periodic kernel/GPU summaries (Switch default), 0 = show them. |
| `RECOMP_NO_LOG=1` | No `xbox_kernel.log`. |
| `XBOX_LOG_LEVEL` | Kernel log level (0 errors … trace). |
| `RECOMP_KERNEL_LOG_BUDGET` | Kernel calls logged before the log goes quiet. |

</details>

<details>
<summary><b>Graphics (GL and Vulkan renderers)</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_PB_EXEC` | Execute the NV2A pushbuffer (default 1; the title draws through it). |
| `RECOMP_GL_SCALE` | Render resolution multiple, 0.5..4 (fractions allowed). |
| `RECOMP_GL_THREAD=1` | GL renderer: GL calls on their own thread. |
| `RECOMP_GL_DIRECT=0` | Convert every vertex to float4 instead of uploading it as stored. |
| `RECOMP_GL_DXT=0` | Decode DXT textures on the CPU instead of uploading them compressed. |
| `RECOMP_GL_SHARED_Z=0` | One depth buffer per colour surface (old behaviour; lights show through walls). |
| `RECOMP_GL_TEX_MB` | Texture cache budget in MB (least recently used go first). |
| `RECOMP_PROG_CACHE` | Shader/pipeline cache file (`progcache.bin`), `=0` off. |
| `RECOMP_VP=0` | No vertex programs (their batches are skipped). |
| `RECOMP_GL_DUMP=<prefix>[,every]` | Write presented frames as BMP (every N frames, default 60). |
| `RECOMP_GL_WATCH=<hex va>` | Log draws into or sampling that address, with pixel read-backs. |
| `RECOMP_GL_TRACE=1` | Shader sources and compile/link errors. |
| `RECOMP_GL_FINISH=1` | Wait for the GPU after every operation (finds the call that hangs it). |
| `RECOMP_FPS_LOG=1` | Presented frames per 10 s. |
| `RECOMP_VK_HEADLESS=1` | Vulkan on Linux: no window (also implied by `SDL_VIDEODRIVER=offscreen`). |
| `RECOMP_VK_VALIDATION=1` | Vulkan on Linux: Khronos validation layer. |
| `RECOMP_VK_TRACE=<n>` | Vulkan: name every step of the first n draws/clears/flips. |
| `RECOMP_TEX_STATS=1` | List each texture format and size decoded. |
| `RECOMP_TEX_STATE=1`, `RECOMP_TEX_DUMP=<prefix>`, `RECOMP_TEX_DUMP_EVERY=<n>` | Texture register state; dump textures to BMP. |
| `RECOMP_FB_DUMP=<prefix>`, `RECOMP_FB_WINDOW=1`, `RECOMP_FB_VA=<va>`, `RECOMP_FB_WINDOW_DUMP_EVERY=<n>` | Framebuffer window / dumps (CPU presenter). |
| `RECOMP_FFP_TRACE`, `RECOMP_SKIP_TRACE`, `RECOMP_TRACE_FLIP=<n>`, `RECOMP_PB_EXEC_VERBOSE`, `RECOMP_PB_UNHANDLED_ALL`, `RECOMP_PB_SCAN`, `RECOMP_NV2A_TRACE`, `RECOMP_RASTER_TEST`, `RECOMP_FIND_NAN`, `RECOMP_FIND_QUAD` | Pushbuffer / NV2A debugging traces. |
| `RECOMP_FMV_HOST`, `RECOMP_FMV_DUMP=<prefix>` | Host-side movie player (off) and movie frame dumps. |

</details>

<details>
<summary><b>Audio</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_AUDIO=0` | No host audio device (the APU then paces by wall clock). |
| `RECOMP_AUDIO_VOLUME` | Master volume 0..100 (default 50; output is soft-limited to −6 dBFS). |
| `RECOMP_AUDIO_BLOCKS` | Host queue depth in 256-sample blocks (default 8, Switch 12). |
| `RECOMP_AUDIO_DUMP=<path>,<start s>,<secs>` | Record the exact PCM sent to the device (48 kHz s16 stereo), written once full. |
| `RECOMP_APU_VOICE_DUMP=<voice hex>,<path>,<start s>,<secs>` | Record one APU voice before/after its filter (float32) and log its registers once a second (v0F4 = menu music). |
| `RECOMP_AC97_READY` | AC97 codec presence for DirectSound (default `plain`). |
| `RECOMP_APU_MIXDOWN_ALL=0` | Mix only bins 0/1 to stereo instead of every bin. |
| `RECOMP_APU_DSP_ACK=<addr,...>`, `RECOMP_DSP_ACK=<addr,...>` | Acknowledge DSP command doorbells at these guest addresses. |
| `RECOMP_APU_TRACE=1` | Front-end methods and a per-second voice summary (disturbs the audio itself). |
| `RECOMP_APU_RING_STATS=1` | Stale ring-voice reads every 10 s (Linux; always on in the Switch `[perf]` report). |

</details>

<details>
<summary><b>Input</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_USB` | Emulated OHCI USB with the pads (default 1). |
| `RECOMP_USB_PADS=1` | Only one pad (default two). |
| `RECOMP_USB_PORT`, `RECOMP_USB_PORT2` | Root port of pad 1 / pad 2 (0-based). |
| `RECOMP_USB_HC`, `RECOMP_USB_NDP` | Host controller for the pads and its number of ports. |
| `RECOMP_PAD_LAYOUT=position` | Map buttons by position (Xbox layout) instead of by label. |
| `RECOMP_PAD_SCRIPT`, `RECOMP_PAD2_SCRIPT` | Timed presses, e.g. `4000:start:300,9000:a:200` (ms from the first pad read). |
| `RECOMP_PAD_PRESS=<mask>` | Press these buttons periodically. |
| `RECOMP_KEYBOARD=1` | Keyboard as a pad (Linux). |
| `RECOMP_RUMBLE=0` | No rumble. |
| `RECOMP_RUMBLE_TRACE=1`, `RECOMP_INPUT_DIAG=1`, `RECOMP_KEY_TRACE=1`, `RECOMP_USB_TRACE=1` | Input debugging traces. |

</details>

<details>
<summary><b>Debugging</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_WATCHDOG_SECS=<s>` | Snapshot the main guest thread after s seconds without progress (and exit). |
| `RECOMP_WATCHDOG_KEEP=1` | Keep running after the watchdog snapshot. |
| `RECOMP_WATCH=<addr>`, `RECOMP_WATCH_RAW=1` | Name the guest code that changes a guest dword. |
| `RECOMP_KERNEL_WATCH=<addr>`, `RECOMP_KERNEL_WATCH_ALL=1` | Sample a guest dword around every kernel call. |
| `RECOMP_PEEK=<addr,...>`, `RECOMP_PEEK_CHAIN=<addr,off,...>` | Print guest dwords / follow a pointer chain periodically. |
| `RECOMP_POKE=<addr:val,...>` | Write guest dwords at boot. |
| `RECOMP_TRAP_NULL=1` | Fault on guest null-page access. |
| `RECOMP_UNIMPL_TRAP=1` | Abort at the first unimplemented instruction. |
| `RECOMP_IRQL_TRACE=1` | Log the first IRQL transitions. |
| `RECOMP_CS_WATCH=<va>`, `RECOMP_CS_TRACE_CRT=1` | Critical-section tracing. |
| `RECOMP_TRACE_ARGS=<n>`, `RECOMP_TRACE_DEREF=1`, `RECOMP_TRACE_BUDGET`, `RECOMP_TRACE_PROFILE=1` | Function-entry traces (functions chosen at regen) and a call profile. |
| `RECOMP_FORCE_RETURN` | Read but currently has no effect. |

</details>

## 🚦 Status

| Platform | State |
|---|---|
| 🐧 Linux | Intro movie, title, profile, Career menu with the 3D car, races; 30-60 fps |
| 🎮 Switch hardware | Bring-up: GL and Vulkan NROs build and boot |

Still open: hardware testing of the tiled-framebuffer fix, collision with some
hittable objects, cube maps, bump/dot-product texture modes, fixed-function
lighting.

## 📚 More

- [`CLAUDE.md`](CLAUDE.md): detailed engineering notes

<div align="center">
<sub>Need for Speed and Need for Speed: Carbon are trademarks of Electronic
Arts. This is an unofficial fan project, not affiliated with or endorsed by EA
or Microsoft.</sub>
</div>
