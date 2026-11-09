# OpenRaven

**Play the *Halo: Fireteam Raven* arcade game on your PC.** Free and open source, for Windows 10/11 and Linux.

[![Halo: Fireteam Raven Mission 1 on PC with OpenRaven](https://img.youtube.com/vi/pZ166zhaEjs/maxresdefault.jpg)](https://youtu.be/pZ166zhaEjs)

▶ [Watch Mission 1 running on PC](https://youtu.be/pZ166zhaEjs) · 💬 [Discord](https://discord.gg/nuvpXycag8) for help and updates · ☕ [Support on Ko-fi](https://ko-fi.com/wuzzy450)

*Halo: Fireteam Raven* (Raw Thrills / Play Mechanix / 343 Industries, 2018) is a four-player arcade rail shooter that only ever ran on arcade cabinets. OpenRaven runs the original v3.11 cabinet game on a normal PC with a controller or keyboard, in any resolution, with modern graphics options on top.

> **You need your own copy of the game.** No game files are included here. OpenRaven works with the usual v3.11 game archive (for example `Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip`, as `.zip`, `.7z` or `.rar`).

---

## What you get

- **One-click setup.** Point it at your game archive once, and it unpacks everything and starts the game. After that it starts straight away.
- **Native on Windows.** Runs the game directly on your graphics driver. No WSL, no virtual machine, no emulator.
- **Controllers and keyboard.** Up to 4 Xbox-style controllers (one per player), plus keyboard controls.
- **Arcade quirks handled.** Free Play on, boot checks and gun calibration skipped, straight to the title screen in about 10 seconds. Muted cabinet audio fixed.
- **Any screen.** Any resolution and aspect ratio (16:9, 16:10, 21:9, 32:9, 4:3) with the correct proportions, and adjustable field of view.
- **Better graphics than the cabinet.** Up to 16x texture filtering, 8192 shadow maps with 4 cascades, full-resolution reflections and SSAO.
- **AMD FSR 2**, as anti-aliasing at native resolution or as an upscaler for slower GPUs.
- **Frame generation** (2x / 3x / 4x). Only fills refresh slots your screen can show, so it never lowers the game's own frame rate.
- **HDR10 on Windows**, with highlights (sky, explosions, lights) expanded up to your screen's peak brightness.
- **In-game settings menu (F1).** Change everything live while you play.
- **Neural detail pass** *(experimental, Linux only for now)*. A small neural network that restores texture and face detail.

Out of the box it uses maximum quality, native resolution, and FSR 2 as anti-aliasing, at 60 FPS like the arcade machine.

---

## Install and play

### Windows 10 / 11

**You need:** Windows 10 or 11 (64-bit), a graphics card with an up-to-date driver (NVIDIA, AMD or Intel), and about 11 GB of free space.

1. Download OpenRaven: **Code → Download ZIP** at the top of this page, and unzip it anywhere.
2. Double-click **`start_windows.bat`**.
3. **First run only:** it looks for your game archive in the OpenRaven folder, the folder above it, Downloads and Desktop. If it can't find it, a window opens so you can pick it. Unpacking takes a few minutes.
4. The game starts fullscreen. Next time, double-clicking `start_windows.bat` starts it straight away.

To quit, press **Alt+F4**. If something goes wrong, share `ravenloader.log` from the OpenRaven folder on the [Discord](https://discord.gg/nuvpXycag8).

### Linux

```bash
git clone https://github.com/wuzzy450/OpenRaven.git
cd OpenRaven
./start_linux.sh
```

The first run installs what it needs (asking for your password), finds your game archive or asks for it, unpacks it and starts the game. You can also give it the archive directly: `./start_linux.sh /path/to/game.zip`.

To force-close the game (arcade games have no quit button): `./openraven/stop_halo.sh`

---

## Controls

Each controller is one player (controller 1 = player 1, and so on).

| Action | Controller | Keyboard and mouse |
|---|---|---|
| Aim | Right or left stick | Mouse, or arrow keys |
| Fire / select | RT | Left click, or Space |
| Reload | RB *(or shoot off-screen)* | Right click, or R |
| Grenade | LT | Middle click, or G |
| Start / join / skip cutscene | Start | Enter or 1 (player 2: 2) |
| Operator menu (TEST) | Back / View | F2 |
| Insert coin *(not needed with Free Play)* | Y | 5 (player 2: 6) |
| Settings menu | – | **F1** |

The mouse and keyboard play as player 1, next to controller 1. The mouse takes over the aim when you
move it and gives it back when you push a stick. In the F1 menu (Input), **Mouse aim** picks
`screen` (the crosshair follows the pointer, like the cabinet's light gun; the default), `relative`
(the mouse pushes the crosshair, with **Mouse sensitivity**) or off.

---

## Settings menu (F1)

Press **F1** in game. Use the mouse, or the arrow keys, Tab, Enter (save) and Esc (close).

- **Display:** resolution, VSync, FPS cap, aspect fix, field of view, FPS counter, HDR on/off and your screen's peak brightness *(Windows; turn on "Use HDR" in Windows display settings first)*.
- **Upscaling:** FSR 2 mode (native, quality, balanced, performance, ultra performance) and sharpness.
- **Frame generation:** off / 2x / 3x / 4x, interpolate or extrapolate.
- **Quality:** texture filtering, shadow resolution and cascades, stable shadows, reflections, SSAO, bloom, glow, lens flare.
- **Audio & Input:** volume, aim mode (cursor or absolute), stick speed.
- **Cabinet:** Free Play, skip boot checks.

Settings are saved in [`openraven/graphics.ini`](openraven/graphics.ini). Delete that file to go back to the game's own arcade settings.

**Slow GPU?** Set FSR 2 mode to *quality* or *performance*, and lower the shadow resolution to 4096.

---

## How it works

The cabinet runs a 64-bit Linux game with a Raw Thrills I/O board, IR light guns and a 5.1 amplifier. OpenRaven never modifies the game files. It loads the game as is and swaps the cabinet hardware for PC equivalents:

- **Input** (`rio2pad`): the cabinet's switch and gun-tracking reads are answered from your controllers and keyboard.
- **Graphics** (`ravengfx`): engine settings are overridden at start-up, the camera projection is corrected for your screen shape, and FSR 2, frame generation, HDR and the F1 menu hook into the game's OpenGL rendering.
- **Linux:** the game runs in a rootless `bubblewrap` sandbox that recreates the cabinet's folder layout.
- **Windows:** `ravenloader.exe` is a loader for Linux programs. It loads the game's own Linux binaries into a Windows process and connects them to Windows replacements for the Linux functions they use (files, threads, windows, OpenGL, input, sound). The game's own audio engine plays through Windows audio.

### Project layout

| Path | What it is |
|---|---|
| `start_windows.bat`, `openraven/setup_windows.ps1` | Windows launcher: finds and unpacks the game, starts `ravenloader.exe` |
| `start_linux.sh`, `openraven/play_halo.sh` | Linux launcher and sandbox |
| `openraven/win/` | Windows loader (`ravenloader.exe`, prebuilt) and its sources |
| `openraven/rio2pad.c`, `openraven/win/rio2pad_win.c` | Controller and keyboard input, cabinet fixes |
| `openraven/ravengfx.c`, `openraven/win/ravengfx_win.c` | Engine settings, aspect-ratio fix, frame hooks |
| `openraven/ravenui.c`, `openraven/win/ravenui_win.c` | F1 settings menu |
| `openraven/ravenfsr.cpp`, `openraven/fsr2/` | FSR 2 (OpenGL) |
| `openraven/ravenfg.c` | Frame generation |
| `openraven/win/ravenhdr_win.c` | HDR10 output |
| `openraven/ravennn*.inc`, `openraven/vk/`, `openraven/neural/` | Neural detail pass (Vulkan) |

### Building from source

Prebuilt files are included, so you only need this if you change the code.

- **Linux:** `cd openraven && ./build.sh` (needs gcc/g++, X11 and Vulkan headers; `start_linux.sh` installs them).
- **Windows:** install [MSYS2](https://www.msys2.org/) with the clang64 toolchain, put `C:/msys64/clang64/bin` on your PATH, then run `make` in `openraven/win`.

---

## Credits

- **Wuzzy450**: creator ([Ko-fi](https://ko-fi.com/wuzzy450) · [Discord](https://discord.gg/nuvpXycag8))
- **AMD FidelityFX FSR 2**, and **JuanDiegoMontoya**'s MIT-licensed OpenGL port of it (`openraven/fsr2/`)
- **Pumbo** (Filoppi): the AutoHDR curve used for HDR highlights

## License

OpenRaven's own code is MIT licensed. Included third-party files keep their own licenses: FSR 2 (MIT), the GCC runtime libraries in `openraven/cabinet_libs/` (GPL with the GCC Runtime Library Exception), and FMOD in `openraven/extralibs/` (FMOD license). Details for every included binary: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

OpenRaven is an unofficial fan project. It isn't affiliated with or endorsed by Microsoft, 343 Industries, Raw Thrills or Play Mechanix, and it contains no game data.
