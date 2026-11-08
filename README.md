# OpenRaven: Free & Open-Source PC Loader for Halo: Fireteam Raven

**Community:** join the [Discord](https://discord.gg/nuvpXycag8) for help and updates · support the project on [Ko-fi](https://ko-fi.com/wuzzy450)

**OpenRaven** is a free, open-source compatibility and enhancement layer for running the *Halo: Fireteam Raven* (v3.11) arcade game on standard Linux and Windows 10/11 PCs.

Original *Halo: Fireteam Raven* arcade cabinets run on custom x86_64 Linux hardware with a Raw Thrills RIO2 USB I/O board, IRTrack optical guns, and a 5.1 surround amplifier. OpenRaven runs the game in a rootless sandbox (`bwrap`), translates standard gamepads into arcade cabinet switches and IR gun aim, fixes modern `glibc` crashes, downmixes 5.1 surround audio to stereo, and adds an in-game **F1 Settings Overlay** with **AMD FSR 2.2** temporal upscaling, **Adaptive Frame Generation** (2x/3x/4x), **Hor+ Ultrawide / Aspect Ratio fixes**, and an experimental **Vulkan Neural Detail Pass** (WIP).

> **Note:** No copyrighted game files are included in this repository. You need your own copy of the game archive (e.g. `Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip`). The launcher finds it by itself (in the game folder, the folder above it, Downloads or Desktop) or asks you to pick it.

---

## Features

- **Zero Cost & Rootless**: Entirely free and open-source. Uses `bubblewrap` (`bwrap`) to mount `/pm/g7/halo`, `/pm/g7/dump`, and `/eFileMaps` in a private user namespace—no root modifications to `/` required.
- **Automatic Setup (Linux & Windows)**: One-click start scripts (`start_linux.sh` and `start_windows.bat`) find your game archives (or ask for them), extract the files into place, install missing dependencies, and launch the game.
- **Native Windows build**: `ravenloader.exe` runs the Linux game directly on Windows with your normal GPU driver. No WSL, no VM.
- **Plug-and-Play Controller Support (`rio2pad.so`)**:
  - Hooks the cabinet's RIO2 switch polling (`sw_get`) and IRTrack gun getters directly in memory.
  - Supports up to 4 Xbox-layout controllers (hot-plugging supported; XInput on Windows).
  - Enables Free Play by default, restores muted gameplay/cutscene audio (`gameVolume`), hides the *"Factory Setup Has Not Been Completed"* banner, and skips boot-time file checks & gun calibration straight to Attract Mode in ~12 seconds.
- **In-Game Settings Overlay & Graphics Enhancements (`ravengfx.so`)**:
  - Press **F1** at any time (keyboard or mouse) to open the live settings menu (`graphics.ini`).
  - **AMD FSR 2.2 Temporal Upscaling**: Replaces the engine's internal TAA pass using the engine's own jitter and motion vectors (`Native`, `Quality`, `Balanced`, `Performance`, `Ultra Performance`), keeping the HUD crisp at full screen resolution.
  - **Adaptive Motion-Vector Frame Generation**: 2x / 3x / 4x optical-flow + depth-dilated motion vector frame generation (`interpolate` or `extrapolate` modes) with unwarped HUD separation.
  - **Any Resolution & Aspect Ratio (Hor+)**: Unlocks the cabinet's `2.0` aspect ratio lock and patches the 3D camera projection matrix for 16:9, 16:10, 4:3, 21:9, and 32:9 displays, plus adjustable Field of View (`FOV_SCALE`).
  - **Engine Quality Overrides**: 16x Anisotropic Filtering, 4096/8192 Shadow Maps, 4-cascade Stable Shadows, full-res Screen-Space Reflections, SSAO, VSync, and FPS cap.
  - **HDR10 output (Windows)**: PQ / BT.2020 output with AutoHDR highlight expansion up to your screen's peak brightness (approach from Community Shaders' HDR addon).
  - **Neural Detail Pass (Experimental / WIP)**: Vulkan cooperative-matrix / compute U-Net post-processing pass trained on remastered game tiles.

---

## Quick Start

### Linux

1. Clone this repository:
   ```bash
   git clone https://github.com/wuzzy450/OpenRaven.git
   cd OpenRaven
   ```
2. Run the start script:
   ```bash
   ./start_linux.sh
   ```
3. On the first run it installs missing packages (`bubblewrap`, `7zip`, ...), finds your game archive (or opens a file picker / asks you to drag it in), extracts it and launches the game. You can also pass it directly: `./start_linux.sh game.zip`.

To force-close the game at any time (arcade builds have no quit button):
```bash
./openraven/stop_halo.sh
```

### Windows 10 / 11 (native)

Requirements: Windows 10/11 64-bit, a GPU with an OpenGL 4.6 driver (NVIDIA, AMD, Intel).

1. Download this repository (Code > Download ZIP) and unzip it anywhere with enough space (~11 GB).
2. Double-click **`start_windows.bat`** (or drop your archives onto it).
3. First run: it finds your game archive (or asks you to pick it) and extracts it (a few minutes).
4. The game starts fullscreen. Later runs start straight away.

Problems? Send `ravenloader.log` from the game folder.

---

## Controls

Up to 4 controllers are supported. Controller *N* controls Player *N*:

| Controller Input | Arcade Cabinet Action |
|---|---|
| **Right Stick** or **Left Stick** | Aim crosshair (Cursor or Absolute mode, configurable in **F1**) |
| **RT** (Right Trigger) | Fire weapon / Menu select |
| **RB** (Right Bumper) | Reload weapon *(shooting off-screen also reloads)* |
| **LT** (Left Trigger) | Throw Grenade |
| **Start** | Player START / Join game / Skip cutscene |
| **Back / Select / View** | Arcade TEST / Operator Diagnostic Menu |
| **Y** | Insert Coin *(not needed when Free Play is enabled)* |
| **F1** *(Keyboard)* | Open / Close **OpenRaven Settings Overlay** |

---

## In-Game Settings Overlay (F1)

Press **F1** while the game is running to open the OpenRaven configuration menu. You can navigate with **Arrow Keys + Tab + Enter/Esc** or with the **Mouse**.

- **Display**: Resolution (`native`, `1080p`, `1440p`, `4K`, ultrawide), Render Scale, VSync, FPS Cap, Hor+ Aspect Fix, Field of View, FPS Counter, HDR (Windows: on/off + your screen's peak brightness; needs *Use HDR* on in Windows display settings).
- **Upscaling & Frame Generation**:
  - **FSR 2.2**: `off` or `fsr2` (`native`, `quality`, `balanced`, `performance`, `ultra`) + live sharpness slider.
  - **Frame Generation**: `off`, `2x`, `3x`, `4x` (`interpolate` or `extrapolate`). Only adds frames the screen can show, so it never lowers the game's own FPS.
  - **Neural Detail (WIP)**: Experimental Vulkan neural enhancement pass *(currently in active training/development)*.
- **Quality**: Anisotropic Filtering (up to 16x), Shadow Map Resolution (up to 8192), Shadow Cascades (up to 4), Stable Shadows, SSAO, Reflections Resolution, Bloom/Glow sliders.
- **Audio & Input**: Stream Volume Boost (`100%`–`400%`), Aim Mode (`cursor` vs `abs`), Stick Speed.
- **Cabinet**: Free Play toggle, Skip Boot Checks toggle.

All settings are saved to [`openraven/graphics.ini`](openraven/graphics.ini).

---

## Project Layout

| Path | Description |
|---|---|
| `start_linux.sh` | One-click Linux installer & launcher (asks for game `.zip` on first run) |
| `start_windows.bat` | One-click Windows launcher (runs `openraven/setup_windows.ps1`: finds archives, sets up files, starts the game) |
| `openraven/win/` | Native Windows loader `ravenloader.exe` (ELF loader + Windows shims, input, overlay, FSR 2, frame gen, HDR) |
| `openraven/play_halo.sh` | Rootless `bwrap` sandbox launcher |
| `openraven/stop_halo.sh` | Force-closes the running arcade process |
| `openraven/build.sh` | Compiles `rio2pad.so`, `fsr2/libfsr2gl.a`, and `ravengfx.so` from source |
| `openraven/rio2pad.c` | Cabinet I/O (`sw_get`), IRTrack gun aim, audio volume, free play, and `glibc` crash-fix shim |
| `openraven/ravengfx.c` | Engine config overrides, Hor+ camera projection hook, and frame presentation hook |
| `openraven/ravenui.c` | F1 in-game settings overlay UI (OpenGL 3.3 + X11/XInput2) |
| `openraven/ravenfsr.cpp` | AMD FidelityFX Super Resolution 2.2 (OpenGL 4.6 backend) integration |
| `openraven/ravenfg.c` | Motion-vector & depth-dilated frame generation pass |
| `openraven/vk/` | Vulkan cooperative-matrix & compute backend for the experimental neural pass |

---

## Building from Source

Prebuilt binaries (`rio2pad.so` and `ravengfx.so`) are included, and `start_linux.sh` will also build them automatically if missing. To rebuild manually after editing any C/C++ files:

```bash
cd openraven
./build.sh
```

Windows (`ravenloader.exe` is prebuilt): install [MSYS2](https://www.msys2.org/) with the clang64 toolchain, then in a shell with `C:/msys64/clang64/bin` on PATH run `make` in `openraven/win`.

---

## Credits

- **Wuzzy450**: Creator ([Ko-fi](https://ko-fi.com/wuzzy450) · [Discord](https://discord.gg/nuvpXycag8))
- **AMD FidelityFX FSR 2.2** & **JuanDiegoMontoya** (`FidelityFX-FSR2-OpenGL`): MIT-licensed OpenGL FSR 2 implementation in `openraven/fsr2/`

## License

MIT License. OpenRaven is an unofficial fan compatibility project and is not affiliated with Microsoft, 343 Industries, Raw Thrills, or Play Mechanix. No copyrighted game data is distributed in this repository.
