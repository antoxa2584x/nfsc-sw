# NFS Carbon (Xbox) static recompilation — notes for Claude

Xbox NTSC-U Need for Speed: Carbon (title 0x4541009E, XDK 5849, Most Wanted
engine: `NfsMWRelease.exe`), lifted to C with xboxrecomp and built for Linux
and Nintendo Switch (libnx NRO). Started 2026-10-03 from the NFSU1 port
(`..\nfsu1-xbox`), which came from the NFSU2 port (`..\nfsu2-xbox`; read its
CLAUDE.md for everything the runtime does on Horizon). Repo:
https://github.com/antoxa2584x/nfsuc-sw (`main`, commits as
`Anton Artemov <antoxa2584@gmail.com>`).

## Rules

- Never commit game data (disc, `default.xbe`, `switch_sd/`) or generated C
  (`gen/`). Ask before committing or pushing.
- The Switch build reads the **unpacked** disc at `sdmc:/switch/nfscx/game/`,
  never the ISO. Logs, `nfscx_env.txt` and shader caches live in
  `sdmc:/switch/nfscx/`.
- Linux test hygiene as NFSU2: one run at a time, kill by PID.
- Under gdb use `set disable-randomization off`.

## Layout and builds

| Where | What |
|---|---|
| `/root/nfscx/` (WSL) | `game/` extracted disc (NFS/ZZDATA*.BIN), `gen/` lifted C, `build-linux/`, `build-abi/` (`-DRECOMP_ABI_CHECK`), `build-switch*/`, `run/` |
| `/root/nfscx/iter.sh` | one bring-up iteration: regen, stub tracing, build, run `SECS` (pad script `PAD=`), seed from the log |
| `/root/nfscx/snap.sh` | run `SECS`, print the APU reset counter and thread stacks (`THREADS=all`) |
| `/root/nfscx/stubtrace.py` | makes every unresolved stub log its first hit (`[STUB]`) |
| `xboxrecomp/` | vendored toolkit = nfsu1-xbox's + the kernel fixes below |
| `src/recomp_manual.c` | library overrides remapped from NFSU2 + Carbon's DSOUND watchdog skip |

- Disc: `xboxrecomp\Need for Speed - Carbon (USA).iso` (also
  `Downloads\...Carbon (USA).7z`), `python3 -m tools.xiso unpack`.
- Regenerate: `tools/regen.sh`. Linux: `cmake --build /root/nfscx/build-linux -j12`.
- Switch: `JOBS=6 bash switch/build.sh` / `VULKAN=1 BUILD_DIR=/root/nfscx/build-switch-vk JOBS=6 bash switch/build.sh`.
- Eden: `nfsu2-xbox\switch_sd\switch\nfscx` is a junction to this repo's
  `switch_sd\switch\nfscx`. Use the GL NRO in Eden.
- Icon: SteamGridDB Carbon cover (`assets/icon.jpg`); loader logo still NFSU2's.

## Findings

- **Libraries = NFSU2's (XDK 5849):** every D3D/DSOUND/XPP/CRT override
  matched 100%. Map (NFSU2 -> Carbon): BlockOnTime 0x2E8F20 -> 0x35A0C0
  (device ptr 0x2F7798 -> 0x368118), BlockOnFence -> 0x35A6D0,
  PersistDisplay -> 0x35BBA0, KickOff -> 0x359EE0, vblank 0x2F1D80 ->
  0x362050, PGRAPH 0x2F22F0 -> 0x3625C0, flip queue -> 0x362350, D3D SSE mul
  -> 0x35EE20, DSP ack 0x32EB65 -> 0x39406B, AC97 reset 0x33518D -> 0x399A64
  (table 0x39A6B0), DSOUND lock -> 0x391BB0/0x391BD2, memmove x2 0x32C800 /
  0x32ED70, _ftol2 -> 0x32C47C, XGetVideoFlags -> 0x2E9C77. Entry 0x2EA1E0.
- **Unresolved stubs are silent failures:** a direct call to an address not
  detected as a function becomes `sub_X(){ g_esp += 4; }`. 0x2F5F15 (a
  queue's get-next, tail-jumped to) returned garbage -> crash at 0x85000003;
  0x39B15C (XPP, after zero padding) never ran -> USB never started.
  `stubtrace.py` + `[STUB]` lines find the ones that run.
- **DSOUND EP watchdog (Carbon's DSOUND only, flags 0x400B):**
  sub_00394A2D checks `*[0x39AD08]` (EP DSP memory 0xFE85A018) for the EP
  firmware's 0xCCCCCC heartbeat and otherwise resets the APU (sub_003947ED).
  The APU model reads GP/EP memory as 0 (and must keep doing so: DSOUND's
  mailboxes there "complete" that way), so it reset ~60x/s. The wrapper
  skips the reset called from that check (return 0x394AD1).
- **NtSetTimerEx was a no-op** (toolkit): the timer was created and never
  armed; Carbon's intro movie froze on one frame waiting on it. Now
  SetWaitableTimer (one-shot; APC/period reported once). NtCreateTimer now
  honours TimerType (notification = manual reset). NtQueueApcThread is still
  a stub (logs its first calls; Carbon has not called it so far).
- **Pad never enumerated (fixed):** a byte of XPP's driver table at
  0x39AFFC decodes as `call 0x0039E9B0` -- the last byte of an instruction
  inside the USB enumerator sub_0039E8F0. As a function start it cut the
  enumerator after its first call, so after SET_ADDRESS + the 2 ms timer DPC
  (0x39EA3B) GET_DESCRIPTOR(config) was never sent. Toolkit fix
  (tools/disasm/functions.py `_prune_data_call_targets`, test
  test_prune_data_call_targets.py): a call target that splits a decoded
  instruction and is only called from outside every function is dropped. Then
  two more missed XPP functions (0x39D01D, 0x39C5C9; seeded). Debug recipe:
  `--trace-functions` with every XPP address + a logging wrapper on the URB
  completion dispatcher sub_0039C1DC ([urb+8] = next step's callback).
  (The 6 h timer at 0x4C5CC0 armed by game code is the auto power-off: not
  related.)
- **Console crash at boot (fix awaiting hardware test):** exception 257 at
  guest 0xF3E96000 -- D3DX copies a surface into the tiled framebuffer
  aperture (0xF0000000, a view of the contiguous window). Horizon cannot
  alias (`[NX] second view of a mapping refused`, `tiled aperture ...
  failed`), Eden and Linux do not fault. Device-aware functions now fold
  0xF0000000-0xF7FFFFFF onto 0x80000000 (XBOX_DEV_FOLD in recomp_types.h,
  also for rep movs/stos block forms and MEMF/MEMD/SMEM64 via mmio_rewrite.py),
  and D3D, D3DX, XGRPH are lifted device-aware (regen.sh --mmio-sections).
  Game code (.text) writing through a tiled pointer would still fault.
- **Perf ports from NFSU2 (2026-10-06):** toolkit synced to nfsu2-xbox
  66b876d (off-screen batch culling, Vulkan instancing, render-scale edge
  snap, C/LFE downmix). Native leaves found by byte-matching NFSU2's bodies
  (RECOMP_NATIVE=0 off, RECOMP_NATIVE_CHECK=1 compares; 0 mismatches in a
  Linux career race): _ftol2 0x32C47C, SSE 4x4 mul 0x2EFCB4 / D3D 0x35EE20,
  box x matrix 0xFE4E0, frustum box 0x103F50 (Carbon sums in another order
  and its K/K2 are 0x3A3C28/0x3A3880), 0x5D1F0, 0x5DC70, 0x48B60.
  RECOMP_FRAME_LAG=1: the renderer sub_00112B60 starts each frame with
  sub_0010B240 = BlockOnFence([0x45C254], ret 0x10B24B) + Swap.
- **3.7 fps for good after ~90 s (console, VK, 2026-10-06):** every frame
  `FLIP_STALL: no flip retired in 250 ms`, read == write alternating 1/0.
  The vblank/PGRAPH wrappers read D3D's retire counter (+0x1BC) before
  taking the dispatch lock, so a retire by the other handler in between
  was counted twice and the executor's flip index ran one off (with two
  buffers, never recovers). Fixed both ways: counter read at DISPATCH, and
  a FLIP_STALL timeout steps flip_read (resync). Same code in nfsu2-xbox,
  patched there too (its "stutter after ~30 min" may be this).
- **Car through hittable objects (fixed, Linux-verified):** props (signs,
  cones) become Smackables via the ESpawnSmackable event (vtable 0x3BEF78,
  handler sub_001D38C0 -> factory sub_000255A0 -> creator sub_00200340 ->
  ctor sub_001FF4A0). The handler asks the car `[vt+0x24]` = sub_002A87D0
  "may not smack?"; its jne at 0x2A8813 is reached from `test al, al` (jmp)
  and `inc al` (fall-through), the flag states did not merge and it lifted
  as `if (_flags)` -- never taken -> always "may not". Toolkit fix
  (translator.py `_materialised_joins`, test_flag_join_materialise.py): each
  predecessor computes the join's condition into `_mfN`. Carbon: 411 -> 392
  fallback sites (rest are tail_jump_alias entries that start with a jcc);
  NFSU2 has 1431, not yet regenerated with this.
  Also fixed on the way: gap-prologue split of sub_001A7E00 ([STUB]
  0x1A7E67, functions.py), cvtss2si/cvtsd2si lifted as truncation
  (RECOMP_CVT_SI, rounds; 0x80000000 out of range).
  Pad script: `lsl lsr lsu lsd` = left stick (steering in races).
- Render scale works in menus (1280x480 AA surface) and races (640x480) on
  Linux, GL thread too. Console settings must be `nfscx_env.txt`; boot logs
  `[switch] env ...` per line or `no ... nfscx_env.txt`.
- Linux test profile: the alias from switch_sd/.../UDATA copied into
  /root/nfscx/game/UDATA. Pad: start at 45 s -> main menu (Career);
  right x3 = Quick Race; times count from the first pad read.
- Seeds (config/seed_functions.json): thread entries, thunks 0x30B870/75/7A,
  0x312D7B, 0x325469, 0x2F6BE6 (after a no-return call + int3), XPP 0x39B15C,
  0x39BD9A, ...

## Status (2026-10-03)

- Linux: intro FMV, title, pad works (Start -> profile prompt -> Career menu
  with the 3D car), 30-60 fps.
- Hardware: crashed at 2.5 s in D3DX (tiled aperture); fix above built, not
  yet tested. Eden: ran, but no pad -- fixed with the enumerator fix.
- Switch: GL and Vulkan NROs in switch_sd/switch/nfscx/.
