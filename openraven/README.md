# OpenRaven: Free & Open-Source Arcade Loader for Halo: Fireteam Raven

**OpenRaven** is a lightweight compatibility loader (emulator wrapper) designed to run *Halo: Fireteam Raven* on standard PC hardware. 

The game runs on custom Linux x86_64 PC-based hardware inside original cabinets. OpenRaven emulates the proprietary Sentinel HASP security dongle and watchdog systems, allowing the game to boot natively under standard Linux distributions (like Ubuntu, SteamOS, Batocera) or under Windows 10/11 using **WSL2 (Windows Subsystem for Linux 2)** with full GPU acceleration (WSLg).

> **This install (2026-11-04):** see **`RUNNING_ON_PC.md`** for how to run it on a Linux PC with
> gamepads (`./play_halo.sh`), and **`NEXT_STEPS.md`** for the current plan (settings overlay, graphics).

---

## Features
- **Zero Cost**: Entirely free and open-source. Bypasses the need for paid/subscription-based loaders.
- **Dongle Emulation**: Emulates Sentinel HASP licensing calls in user-space, reading EEPROM mapping tables directly from configuration files (`fff4` and `00000000000000001111111111110100`).
- **Watchdog Redirection**: Intercepts watchdog check routines and safely redirects them to `/dev/null` to prevent cabinet reset cycles.
- **High Performance**: Runs the game natively on the CPU/GPU with direct OpenGL context creation.

---

## Installation & Setup

### 1. Prerequisite: Directory Structure
By default, the game binaries expect to reside at `/pm/g7/halo/`.
1. Create the directory:
   ```bash
   sudo mkdir -p /pm/g7/halo
   sudo chown -R $USER:$USER /pm/g7
   ```
2. Copy the game files (the ROM dump) into `/pm/g7/halo/`.

### 2. Install Dependencies
Run the dependency setup script (requires `sudo` for package installations):
```bash
chmod +x install_deps.sh
./install_deps.sh
```

### 3. Obtain FMOD Audio Libraries
Because the FMOD Studio API libraries are proprietary, they are not packaged in this repository.
1. Create a free developer account at [FMOD Downloads](https://fmod.com/download).
2. Download the **FMOD Studio Programmers API** for Linux (choose version `1.10.xx` to match the game's compiled import requirements).
3. Extract the following shared libraries from the API tarball:
   - `libfmod.so.10.xx`
   - `libfmodstudio.so.10.xx`
4. Rename and place them directly in the game directory `/pm/g7/halo/`:
   - `/pm/g7/halo/libfmod.so.10`
   - `/pm/g7/halo/libfmodstudio.so.10`

### 4. Build OpenRaven
Compile the hook library:
```bash
make
```
This produces `libopenraven.so` inside the project folder.

---

## Launching the Game

To launch the game with the emulator hook preloaded:
```bash
chmod +x openraven.sh
./openraven.sh
```
The script preloads `libopenraven.so` via `LD_PRELOAD`, overrides HASP and watchdog calls, configures the audio server, and launches the game executable.

---

## Input Configuration (Controllers & Lightguns)

The patched game binary uses **SDL2** for input. You can map standard USB gamepads, keyboards, mouse axes (for gun simulation), or dedicated lightguns (like Sinden or Gun4IR) using the standard SDL2 database mappings.

To configure your controllers:
1. Obtain the SDL2 mapping string for your controller. You can find strings or generate one using tools like *SDL2 Gamepad Tool*.
2. Export the mapping string as an environment variable before running the game:
   ```bash
   export SDL_GAMECONTROLLERCONFIG="<your-mapping-string-here>"
   ./openraven.sh
   ```

*Note: For lightguns, ensure they are calibrated inside the game's internal diagnostic/service menu.*
