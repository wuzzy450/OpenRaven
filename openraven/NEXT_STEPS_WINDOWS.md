# Native Windows version: START HERE (updated 2026-11-07)

**Goal (user decision, 2026-11-06):** a native Windows build. No WSL, no WSLg, no Linux VM. Players
double-click an `.exe` and the game runs on the real Windows GPU driver, as fast and clean as the
native Linux build. The old WSL version (`github_staging/`, commit `53ca067`) is laggy and glitchy;
it stays only as a fallback until the native one ships, then gets removed. The Linux build is not
touched by this work.

**How:** `openraven/win/ravenloader.exe` is an ELF loader. It loads the Linux game binaries into a
Windows process, links their imports to our Windows "shims" (functions that do the Linux job on
Windows), and runs the game's code natively on the CPU.

## Where it stands

**Milestones 1, 2, 3 done. The game boots natively on Windows and renders.**
On the RTX 3080: real NVIDIA OpenGL 4.6 context (the game asks for 3.3 compatibility), fullscreen
3840x2160, steady 60 FPS, ran 2.5 minutes with no crash. It stops on its boot screen:

> Welcome to Halo v03.11 / Initializing Systems, Please Be Patient. / FileSystem integrity test
> failed! ... Press START or TEST to continue.

That screen is **expected** (Linux shows it too; there `rio2pad.so` passes it with Start or
`RIO2PAD_SKIPBOOT=1`). So the next blocker is **input**. Screenshot of this frame:
`win/first_frame_windows.png`.

### Update 2026-11-07 (evening): attract mode runs, full screen, 60 FPS

**The game reaches attract mode in about 7 s and loops it at ~60 FPS in 3840x2160 (90 s+ runs,
no crash).** Picture fills the screen. No audio yet (FMOD calls return errors on purpose).

What changed:
- `win/rio2pad_win.c`: port of rio2pad (XInput + keyboard fallback, all game hooks), installed by
  `rio2pad_install()` in main.c before `run_inits`. Skipboot START pulse passes the file test.
  The shutdown-join and envelope-dlclose patches are dropped: the `pthread_join`/`timedjoin` shims
  accept a null thread, and `dlclose` was already a no-op. Keys (game window focused): Enter/1 =
  P1 START, 2 = P2 START, F2 = TEST, 5/6 = coin, Space = fire, R = reload, G = grenade, arrows = aim.
  Not tested with a real pad yet. `RIO2PAD_PROCLOG` prints nothing (proc table address unverified).
- `win/shim_ctx.c`: getcontext/setcontext/swapcontext/makecontext (glibc ucontext layout) and
  setjmp/_setjmp/__sigsetjmp/longjmp/siglongjmp/__longjmp_chk, in asm. The game's coroutines
  (makecontext fn 0x417db0, 0x448c60) run.
- `shim_fmt.c`: swprintf, __swprintf_chk, __fwprintf_chk.
- **Stack crash fixed (the "silent exit"):** threads were created reserve-only. Linux code has no
  stack probes, so a frame > 4 KB jumped over Windows' guard page and crashed; with no stack left
  the crash handler could not log, so the process vanished. All thread stacks (pthreads and the
  16 MB game thread) are now fully committed. `cdb -o -g` showed it (AV at game 0x91831a).
- **Scaling fixed:** the game renders at the size it gave XCreateWindow (1280x720) into the lower-left
  corner of the 4K window. `present_scaled()` in `shim_glx.c` copies that corner into an FBO and
  stretches it over the window (aspect kept) before each swap. `RAVEN_NOSCALE=1` turns it off.
  It is a 720p picture upscaled: rendering at native 4K is for step 7 (graphics.ini / ravengfx).
- Temporary no-audio fallback in `shim_lookup` (main.c): `FMOD_Memory_Initialize` returns 0, every
  other `FMOD_*` / `*4FMOD*` returns 28. Replace with the real mapping in step 5.
- `raven_log` writes through a raw Win32 handle now (no CRT FILE).

Next: play a round with keyboard/pad (START from attract) and fix what breaks, then audio.

### Update 2026-11-07 (night): audio works (step 1 done, user confirmed by ear)

- `win/shim_fmod.c`: all 60 FMOD C++ names the game imports map to the C API of the Windows
  FMOD **1.10.10** DLLs `fmod64.dll` + `fmodstudio64.dll` (copied into `win/` from
  `possible de dongle material/Super Bikes 3 x64 Fix/`; `RAVEN_FMODDIR` overrides the folder).
  Studio create always passes header version 0x00011010. ChannelControl calls try `FMOD_Channel_*`,
  then `FMOD_ChannelGroup_*` on an invalid-handle error. The game's SysV callbacks (channel, event)
  go through MS-ABI trampolines (one stored callback per kind; a second one is logged).
  Bank/sound paths go through `lx_path` (relative -> `pm/g7/halo/`).
- Volume: master channel group set to `RAVEN_VOLUME` (default 4.0, like the Linux 2x route + stream
  boost); kept if the game sets the master level. `setSoftwareFormat` is forced to stereo unless
  `RAVEN_SURROUND=1` (the game did not call it in a 60 s run; Windows/FMOD downmix by device).
  `RAVEN_NOAUDIO=1` = old silent fallback.
- 60 s run: Studio init 0, MasterBank/strings/perm/attract/level1 banks load (0), no missing C
  functions, 56-60 FPS, no crash. **Not yet checked by ear** (music, crackle, loudness vs Linux).
- License: FMOD DLLs may not be redistributable from this repo; decide in step 9.

### Update 2026-11-07 (late night): ravengfx ported (step 2, except the F1 overlay)

**The game now renders at native 3840x2160** (full frame, `present_scaled` no longer kicks in),
attract mode at 58-60 FPS, graphics.ini values reach the engine.
- `win/ravengfx_win.c`, installed from main.c before the argv is built: `cfg_get_int`/`cfg_get_float`
  hooks (graphics.ini, `RAVENGFX_CONF` overrides the path, `RAVENGFX_KEYLOG=file` logs every key:
  checked, all 10 engine keys show `(override)`), `video_set_mode` hook (RENDER_SIZE / RENDER_SCALE /
  `RAVENGFX_RENDER`), Hor+/FOV projection hook (2/2 sites, stub page in low memory at 0x10000000+),
  aspect-lock NOP. Per-frame `ravengfx_frame()` is called from `s_glXSwapBuffers` before
  `present_scaled`: VSYNC (forces the swap interval; the game's own calls can't change it), FPS_CAP,
  Hor+ refresh. This is where FSR 2 / frame gen / neural present hooks go.
- **Resolution:** `ravengfx_args` adds `-fWxH -n` to the game's argv like play_halo.sh:
  `RAVEN_RES=WxH` > graphics.ini `resolution` (`native` = primary monitor size) ; RAVEN_WINDOWED=1
  uses 1280x720.
- `-f` makes SDL use its legacy fullscreen (override cover window + `XReparentWindow` + wait for
  MapNotify). `shim_x11.c` now implements `XReparentWindow`: hides the cover window, keeps the game
  window top-level, queues Unmap/Reparent/MapNotify. `XIfEvent` "still waiting" now logs the
  window it waits for and the queue.
- Not ported (Linux-only debug): TANLOG, RAVENGFX_SHOT capture, gltrace.

### Update 2026-11-07: F1 overlay ported (step 2 done)

- `win/ravenui_win.c` (+ `win/ravenui_font.h`): port of `ravenui.c`. Same menu, CPU-drawn panel,
  one GL 3.3 quad from `ravengfx_frame()` before the swap. Keys: `GetAsyncKeyState` while one of
  our windows is in front (F1 / `RAVENGFX_UIKEY=F2`.., arrows, Tab/Shift+Tab, PgUp/PgDn, Enter
  saves, Esc). Mouse: `GetCursorPos` -> game window client pixels (`raven_game_hwnd()` in
  shim_glx.c), left/right click by button state, wheel from shim_x11's WM_MOUSEWHEEL
  (`ravenui_wheel`). The game still gets the keys/clicks too (as on Linux).
- Live items: volume -> `raven_fmod_set_volume` (`volume` 300% = boost 4.0; graphics.ini `volume`
  also sets the start level unless `RAVEN_VOLUME`), aim / stick_speed -> rio2pad_win globals.
  ravengfx_win now also maps graphics.ini `skip_boot`, `free_play`, `stick_speed`, `aim` to the
  RIO2PAD_* env vars like graphics.sh (env wins).
- Save writes graphics.ini in binary mode (keeps LF endings). Numeric choices match by value
  ("1.0000" = "1.0"; the Linux version jumped to the first choice).
- Overlay GL objects are kept per GL context (only 1 context presents today), and the draw now also
  turns off rasterizer discard, logic op, alpha-to-coverage, sample mask, clip distance 0 and sets
  `glDrawBuffer(GL_BACK)` (all restored after). `RAVENUI_CHECK=1` counts frames where the panel
  wrote no pixels (GL_SAMPLES_PASSED): 0 of ~3300 in a 60 s run.
- **User saw the overlay flicker once** in a run with `RAVEN_SHOT_EVERY=120` (a 4K readback + BMP
  write every 2 s = stutter). After the fixes the user checked with F1: no flicker.
- Tested by script: `RAVENGFX_UISCRIPT` keys `oudlrext` plus mouse `@x;y` (game-window pixels,
  SetCursorPos), `L`/`R` clicks, `W`/`V` wheel (real SendInput). Clicks on Show FPS, a tab, the
  wheel and a right-click on SSAO all worked (screens checked).

### Update 2026-11-07 (stopped mid-step 3): FSR 2 runs; open issues

- **FSR 2 builds and runs on Windows.** `win/Makefile` compiles `../ravenfsr.cpp` (now builds on
  both OSes: `#ifdef _WIN32` block; `long` -> `long long` for GL 64-bit values) and the FSR 2 library
  from `../fsr2/src` with clang++, links `-static`. `win/gl_hooks.c` (table `shims_glhook`, checked
  before `shims_gl`) intercepts glUseProgram / glUniformMatrix4fv / glActiveTexture / glBindTexture /
  glDraw* for ravenfsr. `ravenfsr_config()` from ravengfx_install, `ravengl_frame_end()` per frame.
  Log: `context 3840x2160 -> 3840x2160`, dispatch every frame, 52-60 FPS at FSR "native".
  The dispatch line (near 1, far 0.33, fov 179.9, jitter 0) is identical on Linux (logged on the
  boot screen), not a port bug. Not yet: compare with a Linux gfxshot frame, test quality mode.
- **User still sees black flickers** (with FSR on). `RAVEN_BLACKCHECK=1` (ravengfx_win.c: 8x8
  pixel grid per frame, async PBO readback, logs black runs and "dips"): the only black runs are
  12-frame ones at the same attract points **with FSR on and off** (game scene cuts), no dips.
  So either the flicker is those cuts, or it happens after our back buffer (presentation/DWM).
  Ruled out: cover window re-mapped (no), WM_ERASEBKGND (already returns 1). Next: ask the user
  when exactly it happens (scene changes? random? whole screen?), test with FSR off by eye,
  try `RAVEN_WINDOWED=1`, and check NVIDIA "present method" / DWM flip (PresentMon).
- **Desaturated characters for a second in some scenes:** user says Linux did this too (engine).
- **Music ducks / cuts out ("desert level" scene) = the game's cabinet settings, not FMOD.**
  FMOD logs no errors (shim now logs the first 5 failures of every call, bank unloads, and
  master/bus volume changes). The game sets `bus:/` to 1.26, then 0.13 (attract), then 0.00.
  Adjustments read: `sound/attractVolume = 25`, `sound/attractSoundFreq = 1`, gameVolume 0 (->100).
  `rio2pad_win.c` now returns attractVolume 100 (`RIO2PAD_ATTRACTVOL` overrides) and
  `RIO2PAD_ATTRACTFREQ` overrides attractSoundFreq. **Untested:** which attractSoundFreq value
  means "always" (was testing 1 vs 0 when stopped); then make that the default and add it to
  the F1 overlay / graphics.ini.

### Update 2026-11-08: flicker hunt
- User: flicker happens with FSR on and off, whole screen, seems random / near scene changes.
- New always-on log lines `[flicker]` (shim_glx.c swap: frame gap > 50 ms or SwapBuffers > 40 ms,
  foreground-window changes; shim_x11.c: activate/windowpos/display-change messages).
- **Found:** the debug screenshot was on by default (`RAVEN_SHOT_EVERY` 600) = a 0.4-0.55 s freeze
  every 10 s. Now default 0 (off). After that, steady 60 FPS except small 70-270 ms hitches and
  a 3.6 s freeze at level loads (level2 bank + glcgz/videos load on the render thread, right after
  the game's own 12-frame black cut). No focus/window changes during play.
- User: still flickers at scene changes and during the logos. `RAVEN_SHOT_RANGE=a-b` (480x270 BMP
  per frame, `ravenshots/sNNNNNN.bmp`) around a cut shows a clean fade, no bad frames: **what we
  present is correct, the flicker is after SwapBuffers** (present path / monitor). Suspects: G-SYNC/VRR
  flicker on frame-time swings, or independent-flip <-> composed switches. Test switch
  `RAVEN_COMPOSE=1` (window 1 px taller, top at -1: always DWM-composed), launcher
  `flicker_test_compose.bat` in the game folder. Double-click with no args now works (root = exe/../..).
- **FIXED (user confirmed):** no flicker with the 1 px taller window. Now the default in
  `set_fullscreen` (shim_x11.c); `RAVEN_COMPOSE=0` = old exact-size window. `flicker_test_compose.bat` in the game folder is obsolete (delete it).
  Small cost: DWM composition adds about one frame of latency. Next: attractSoundFreq (music
  ducking), then FSR quality mode check, then step 4. If it still flickers: compare level-load time with Linux
  (is our file I/O / per-fopen logging slow?), then PresentMon for present-mode switches.

### Update 2026-11-08: attract sound cut-out FIXED (needs user ear check)
- Tested `RIO2PAD_ATTRACTFREQ` 0/1/2 (150 s runs): 0 mutes at once, 1 and 2 still set `bus:/` to
  0.00 about 60-75 s into attract (desert scene). No value means "always".
- Fix: `s_Bus_SetVolume` (shim_fmod.c) ignores a 0 volume on the master bus `bus:/` and keeps the
  last level (log: `master bus mute blocked`). `RAVEN_ATTRACTMUTE=1` = game's original muting.
  Verified 150 s: bus stays 1.26. User confirmed by ear 2026-11-08.

### Update 2026-11-08: frame generation + HDR on Windows
- **Frame gen (step 4) runs.** Shared `ravenfg.c` now builds on Windows (`#ifdef _WIN32`: gl_resolve,
  QPC clock, Sleep+spin pacing, refresh from EnumDisplaySettings). gl_hooks.c calls ravenfg_draw before
  each draw (tracks program + draw FBO); shim_glx.c swap goes through ravenfg_present. Test: FPS cap 30
  + RAVENFG=2 -> "now 2x (real 30.0 FPS, screen 60 Hz)". NEURAL is stubbed (step 5). Not yet: PresentMon
  pacing check, user eye check.
- **HDR10 output (new, user request; modelled on Community Shaders "HDR Display").** `win/ravenhdr_win.c`:
  gamma 2.2 decode -> Pumbo AutoHDR (optional) -> x paper white -> BT.709->BT.2020 -> PQ. Presents through
  a DXGI flip swapchain (R10G10B10A2, G2084/P2020); GL renders into a D3D11 texture via WGL_NV_DX_interop.
  graphics.ini / F1 (Display): HDR, HDR_PAPERWHITE (203), HDR_PEAK (0 = from display), HDR_AUTOHDR (1),
  HDR_SATURATION (0.2). `RAVEN_HDR=1` forces on. Tested: Windows HDR detected (1000 nits), 60 FPS steady.
  Not yet checked by eye. Once turned on the DXGI path stays (off = SDR through DXGI).
- Linux HDR: not possible on the X11/GLX path (no HDR signalling). Would need a Vulkan/Wayland present
  (gamescope). Not started.

**Gotcha:** run the exe in a separate Bash command from `make`. With
`/c/msys64/usr/bin` first in PATH, MSYS2's `timeout` drops exported env vars (all `RAVEN*`
settings silently ignored).

## Build and run

- Toolchain: MSYS2 clang64 (installed). In Git Bash:
  `export PATH=/c/msys64/clang64/bin:/c/msys64/usr/bin:$PATH; cd openraven/win; make`
- Run from the folder that contains `pm/` and `openraven/`: `./openraven/win/ravenloader.exe .`
  (it relaunches itself once; see "Loader design").
- Output in the root folder: `ravenloader.log` (every dlopen/dlsym/fopen, settings, FPS every 5 s,
  FATAL lines), `missing_imports.tsv`, `ravenshots/frameNNNNNN.bmp`, `ravenloader_proc/cpuinfo`.
  Last run kept as `win/last_run.log`.
- Stopping a run: `timeout 60 ./openraven/win/ravenloader.exe .` (exit 124 = still running = good).
- Viewing shots: Python has PIL; convert a BMP to a small PNG and look at it.

**Settings (environment variables, read from the Win32 environment):**

| Variable | Effect |
|---|---|
| `RAVEN_WINDOWED=1` | Normal 1280x720 window instead of fullscreen |
| `RAVEN_SHOT_EVERY=N` | BMP of the back buffer every N frames (default 600, 0 = off) |
| `RAVEN_GAME`, `RAVEN_HALOSO` | Override `openraven/3.11-dec/pm/g7/halo/{game,halo.so}` |
| `RAVEN_LIBSTDCXX`, `RAVEN_LIBGCC` | Override `pm/g7/halo/libstdc++.so.6`, `openraven/cabinet_libs/lib/x86_64-linux-gnu/libgcc_s.so.1` |

The log prints them on a `settings:` line.

## Gotchas (read before working)

- **Env vars in the Claude Bash tool:** `VAR=1 ./ravenloader.exe` does NOT reach the exe. Use
  `export VAR=1` first.
- **Never append with `cat >> file <<EOF`** on this drive: it once overwrote the *start* of
  `shim_core.c`. Use the Edit tool.
- Don't write C escapes (`\n`, `\t`) through sed/heredocs: they become real newlines. Use Edit.
- One-step `clang -o x a.c` fails ("unable to make temporary file"); the Makefile compiles `-c` then links.
- Don't add `-Wl,--stack`: a bigger main stack lands in the game's 0x400000 range. The game runs on
  its own 16 MB thread.
- Always the `3.11-dec` game/halo.so, never the old hand-patched `pm/g7/halo/game`.
- Every shim is `SV` (`__attribute__((sysv_abi))`). Linux `long` = 64-bit: use `long long` /
  `int64_t`. Linux `wchar_t` = 4 bytes. Struct layouts (stat, tm, XEvent, ...) must be Linux's.
- Any function pointer the game hands us (callbacks, qsort comparators, XIfEvent predicates,
  pthread start routines) is SysV: call it through an `SV` function-pointer type.
- Work loop: run, read the FATAL line (unimplemented function name, or crash with
  `module+offset` and stack return addresses), fix, repeat. Disassemble with
  `llvm-objdump -d --no-show-raw-insn --start-address=0x... <file>`; for crashes in our own exe,
  `llvm-nm -n ravenloader.exe` (image base 0x7ff600000000).

## Next steps, in order (user's list, 2026-11-07)

Work through these in order. Each one: build, run, check `ravenloader.log` + `ravenshots/`, then
update this file. Tell the user the answer to item 7 in the final message.

1. **Audio (milestone 6).** Replace the temporary no-audio fallback in `shim_lookup` (main.c:
   `fmod_ok` / `fmod_fail`). The game calls the FMOD **C++** API (~58 Linux-mangled names, e.g.
   `_ZN4FMOD6System...`, `_ZN4FMOD6Studio6System10initializeEijjPv`; full list:
   `grep FMOD ravenloader.log` / `runtime_imports.tsv`). Use the Windows FMOD Studio 1.10 DLLs
   (`fmod.dll`, `fmodstudio.dll`, `fmodstudioapi11020` era, matching the Linux .so version in
   `pm/g7/halo/`) and their **C** API (`FMOD_Studio_System_Create`, `FMOD_System_*`, ...): each C++
   method is `this` + args -> C function with the handle first, wrapped `SV`. Bank files load from
   remapped paths. Same stereo downmix + volume boost as `play_halo.sh` / `halo_asound.conf`.
   rio2pad's `gameVolume 0 -> 100` fix is already in `rio2pad_win.c`. Check: attract music and
   gameplay sound play, no crackle, volume like Linux.

2. **Port ravengfx (graphics settings) first: FSR 2 and frame gen depend on it.** `ravengfx.c`
   (460 lines) inline-hooks `cfg_get_int` @0x924650 / `cfg_get_float` @0x9247a0 and
   `video_set_mode` @0x4fb730 and reads `openraven/graphics.ini`. Port it as `win/ravengfx_win.c`
   (same pattern as `rio2pad_win.c`: VirtualProtect + abs jmp, hooks `SV`, install from main.c
   before `run_inits`). `resolution = native` should make the game **render at 3840x2160**, so
   `present_scaled()` (shim_glx.c) stops being needed at 4K. Keep it for lower render sizes.
   Port `ravenui.c` too (F1 overlay, writes graphics.ini). Present hooks go into `s_glXSwapBuffers`
   (shim_glx.c), before `present_scaled`.

3. **FSR 2 on Windows.** `ravenfsr.cpp` + `fsr2/` (Linux build of the FSR 2 GL path). Build it with
   MSYS2 clang64 into the exe, or as a native DLL. `graphics.ini` has `UPSCALER = fsr2`,
   `FSR_QUALITY`, `FSR_SHARPNESS`. Check: log says FSR 2 active, the render size drops (Quality =
   67%), the picture is sharp, no ghosting, compared with a Linux `gfxshot.sh` frame.

4. **Frame generation on Windows.** `ravenfg.c` (`FRAME_GEN`, `FG_MODE = interpolate`, adaptive,
   refresh rate auto). On Windows get the refresh rate from `EnumDisplaySettings`, pace with
   `wglSwapIntervalEXT` / DwmFlush. Check with PresentMon: shown FPS doubles when the game drops
   below refresh, with even frame pacing, and real FPS never goes down.

5. **Vulkan layer + neural rendering on Windows.** `ravennn_vk.inc` + `vk/` (compute shaders,
   `build_vk.sh`) + `neural/raven_nn.bin`. On Linux it ran as a separate daemon over TCP port 28765.
   On Windows, run it **in-process**: create a Vulkan device on the same GPU, share the frame with
   GL through `GL_EXT_memory_object_win32` + `GL_EXT_semaphore_win32` (no copies, no TCP).
   **User requirement: FP16 accumulation** in the neural conv shaders (`vk/conv*.comp`,
   cooperative-matrix path `conv_cm.comp`): f16 inputs/weights *and* f16 accumulators
   (`VK_KHR_shader_float16_int8`, `shaderFloat16`; for coop matrix, a `float16_t` C/result type,
   which needs a cmprops entry with f16 accumulate: `vk/cmprops` lists what the 3080 supports).
   Log at startup which accumulator type is active, and fall back to f32 only if the driver lacks
   it. Compare output against the f32 path (PSNR, or max error on a test frame) so the speedup does
   not cost visible quality. Check:
   the neural pass changes the picture as on Linux, and how much it costs in ms per frame (log it).

6. **Test a full round.** From attract: START (Enter or pad Start) -> character select -> mission.
   Shoot (Space/RT), reload (R/RB), grenade (G/LT), aim with sticks/arrows, die -> continue
   countdown -> game over. Test with a real XInput pad too (the user's 8BitDo/Xbox). Fix every FATAL /
   crash on the way (still open: C++ exceptions untested; aligned_alloc > 16, __realpath_chk,
   /proc/meminfo, /sys/class/dmi/id/*; LAPACK, 20 functions). Run 15+ minutes with no crash.

7. **Tune the user's settings: 60 FPS with neural rendering ON, looking as good as possible**
   (RTX 3080, 4K screen). Start from the current `graphics.ini` (FSR 2, 4096 shadows, 4 splits, SSR
   1.0, SSAO, aniso 16, ENHANCED_BLOOM must stay 0). With neural on, measure frame time in the
   heaviest scene of a round. If it is under 60, trade in this order: FSR_QUALITY native -> quality,
   SHADOW_SPLITS 4 -> 3, SSR_BUFFER_SCALE 1.0 -> 0.75, then frame gen as the last resort. Save the
   result in `openraven/graphics.ini` and write in the comments which values were measured.

8. **Tell the user where to launch it.** Right now (dev build): from the game folder
   `F:\Halo Fireteam Raven (3.11)(2018)[Raw Thrills][TP]`, run `openraven\win\ravenloader.exe .`
   (Git Bash: `./openraven/win/ravenloader.exe .`). Before the GitHub release, add a double-click
   launcher at the folder root, `start_windows.bat` (replacing the WSL one), that runs
   `openraven\win\ravenloader.exe "%~dp0"` with the usual settings. Then tell the user: double-click
   `start_windows.bat` in the game folder (and make a desktop shortcut if they want one).

9. **Upload the new Windows version to GitHub** (`wuzzy450/OpenRaven`, local copy `github_staging/`,
   last commit `53ca067` is the WSL version). Ask the user before pushing. Steps:
   - Copy in `openraven/win/` sources + Makefile + a **prebuilt `ravenloader.exe`** (users don't
     compile), the new `start_windows.bat`, ported gfx/audio files, `graphics.ini`.
     Never commit game files (`pm/`, `3.11-dec/`, zips, FMOD DLLs if their license forbids it:
     then the launcher must find them in the user's own files, or tell the user where to get them).
   - Remove the WSL path: `windows_setup.ps1`, `play_halo_win.sh`, the WSL steps in README.
   - README: new Windows section (requirements: Windows 10/11 x64, OpenGL 4.x GPU driver; steps:
     unzip game, put OpenRaven in it, double-click `start_windows.bat`; keyboard + pad controls;
     graphics.ini / F1 overlay; troubleshooting = send `ravenloader.log`). Leave the Linux section.
   - Commit on a branch, push, open a PR (or push to main if the user says so).

## Loader design (what exists)

### Files in `openraven/win/`

| File | What it does |
|---|---|
| `main.c` | Relaunch + address reservation, DPI awareness, loads modules, canary patch, copy-reloc fix, inits, crash handler, starts the game thread |
| `elfload.c/.h`, `elf.h` | ELF mapping, relocations, exports, TLS (`__tls_get_addr`), `dl_iterate_phdr`, `elf_emulate_copy`, `elf_module_name` |
| `shim_core.c` | libc core: memory, strings, errno, stdio, fd I/O, path remap, stat/dirent, time, mmap/munmap/mincore, SysV shm, atexit/exit, `/proc/cpuinfo`, `raven_env` |
| `shim_libc2.c` | math, more strings, 32-bit wchar, stdio extras, qsort, rand, env, getcwd, clock_gettime, localtime/strftime, "not available" stubs |
| `shim_locale.c` | "C" locale for libstdc++ (glibc-layout `locale_t`), *_l functions, mbrtowc family, wmem*, gettext passthrough, writev |
| `shim_fmt.c` | printf/scanf family: asm thunks capture SysV varargs; v*printf take a SysV `va_list` |
| `shim_thread.c` | pthread (threads, mutex, cond, rwlock, keys, once) and semaphores over Win32 |
| `shim_dl.c` | dlopen/dlsym (every lookup logged; the switchboard for GL/X11/FMOD), dladdr |
| `shim_x11.c` | X11 over Win32 (see below) |
| `shim_glx.c` | GLX over WGL, FPS log, screenshots |
| `gen_gl.py` -> `gl_thunks.c` | 2945 generated SysV wrappers for every gl*/glu* in the MSYS2 headers (`make` regenerates) |
| `shim_devices.c` | udev + libiw: no devices |
| `runtime_imports.tsv` | All ~650 names the game looks up at runtime (`stub` = had no shim at milestone 2) |

Lookup order (`shim_lookup` in main.c): x11, glx, gl, core, fmt, thread, dl, libc2, locale,
devices; then exports of the loaded ELF modules; else a stub that logs the name and exits (code 3).

### Key facts found so far

- **Address space:** the game is non-PIE at 0x400000..0x23d3000; Windows uses that range early, so
  the loader relaunches itself suspended and reserves 0x400000..0x4000000 in the child
  (`RAVENLOADER_CHILD=1`). Our exe sits at 0x7ff600000000.
- **The 3.11-dec game is a memory dump.** Its ~650 real imports are resolved at startup by
  `halo.so` through `dlsym`; static imports are only ~183 libc/pthread/dl names.
- **Stale library data in the dump:** the game exports its own copies of library objects (vtables,
  typeinfo, `_S_empty_rep_storage`, `std::nothrow`) whose COPY relocations were stripped, so they held
  the original Linux process's addresses. `elf_emulate_copy` refills them from the loaded library.
- **Stack canary:** every `%fs:0x28` read (game 9591, halo.so 73, libstdc++ 1589) is rewritten to
  `%gs:` + a TEB TLS slot (`patch_canary`). No other `%fs:` use exists in these files.
- **TLS:** halo.so and libstdc++ use general-dynamic TLS only (DTPMOD64/DTPOFF64 +
  `__tls_get_addr`), supported. Initial-exec TPOFF64 is logged as unsupported (none seen).
- **C++ runtime:** the real Linux `libstdc++.so.6` (game's own, `pm/g7/halo/`) and `libgcc_s.so.1`
  (`openraven/cabinet_libs/`) are loaded after halo.so; their init runs first. Symbol lookup skips
  hidden (old) versions, but a module's reference to its own symbol binds to it (libgcc_s needs this
  for `__cpu_indicator_init`).
- **SDL:** the game has SDL2 (hg-8628, about 2.0.2) statically linked, using its X11 backend and
  `dlopen("libGL.so.1")` + `glXGetProcAddressARB`. SDL reads Display fields via Xlib macros, so the
  fake Display has the exact LP64 `_XPrivDisplay` layout (fd = 900, default_screen, screens, ...).
- **X11 layer (`shim_x11.c`):** X windows = Win32 windows (CS_OWNDC); Win32 messages -> 192-byte
  XEvent queue (keys, buttons, wheel, motion, configure, focus, expose, map, WM_DELETE_WINDOW);
  keycodes = Linux evdev + 8 with US keysyms; `_NET_WM_STATE` fullscreen (and every normal map unless
  `RAVEN_WINDOWED=1`) = borderless window covering the monitor; `select()` on fd 900 reports queued
  events; XRandR, XVidMode, XShm, XIM report "not available" (SDL falls back to the desktop mode).
- **GLX layer (`shim_glx.c`):** one RGBA8 / depth 24 / stencil 8 double-buffered config; a context
  becomes a real WGL context at its first MakeCurrent (`glXCreateContextAttribsARB` ->
  `wglCreateContextAttribsARB`, same attribute values); swap interval via `wglSwapIntervalEXT`.
  GL functions resolve lazily from opengl32 / glu32 / `wglGetProcAddress`; a missing one is FATAL (exit 7).
- **Paths:** Linux absolute paths map to `<root>/...`; `/proc/cpuinfo` -> generated file (the game
  crashes without a "MHz" line). getcwd reports `/pm/g7/halo`.
- **Game behavior on the way to the boot screen:** reads `eFileMaps/fileMap000..006`, the data
  archives and `gameconf.txt`, writes `pmuser/gamelog.txt`, scans USB (`/dev/bus/usb` etc.: none),
  starts several threads, then loads models/shaders/locales while drawing the boot console.

Context docs: `RUNNING_ON_PC.md` (all Linux patches, hooks, switch ids, env vars),
`NEXT_STEPS.md` (graphics features), `NEXT_STEPS_INPUT.md`.

## Why not fix WSL

WSLg copies every frame through its compositor and a remote-desktop link (no real vsync or
fullscreen), and WSL's OpenGL runs on Mesa's d3d12 translation (slow, missing features -> glitches).
weaselway (<https://github.com/weaselway/weaselway>) only swaps the display copy, still needs WSLg,
still uses d3d12, and is pinned to two WSL kernels. A native loader removes all of it.
