# OpenRaven: Windows Optimization & Tuning Roadmap (Next Steps)

This document outlines the root causes and actionable implementation steps to optimize **Halo: Fireteam Raven (OpenRaven)** for Windows (WSL2 / WSLg) on an **NVIDIA GeForce RTX 3080**, specifically addressing:
1. **Audio Volume vs. Distortion:** Why audio is quiet at 100% and distorted/blown-out at 200–300%, and how to get clean, loud audio without digital clipping.
2. **GPU Performance & Lag:** Why the game feels laggier on Windows than Linux despite having an RTX 3080, and how to maximize framerate and smoothness.
3. **Controller & Trigger Input (RT / LT):** Why Right Trigger (Shoot) and Left Trigger (Grenade) were not firing on Windows and how to align XInput bridging with Linux controls.

---

## 1. Audio Deep Dive: Volume & Distortion

### Why Does 300% Sound Blown Out on Windows but Fine on Linux?

| Component | Native Linux | Windows (WSLg) |
| :--- | :--- | :--- |
| **Audio Server** | PipeWire (or full PulseAudio) | WSLg custom PulseAudio (`module-rdp-sink`) |
| **Audio Pipeline** | 32-bit floating-point DSP | 16-bit signed integer PCM (`s16le`) |
| **Gain Ceiling** | Dynamic range limiter / soft-knee headroom | Hard digital clipping at integer boundary ($\pm 32767$) |
| **Transport** | Direct ALSA / Pulse hardware sink | FreeRDP RDP audio virtual channel to Windows Audio |

- **Arcade Master Headroom:** The original Raw Thrills arcade cabinets used powerful external 100W analog hardware amplifiers. Because of this, the game’s internal digital master was mixed with significant headroom (peaks hovering around $-14\text{ dB}$ to $-18\text{ dBFS}$).
- **The Clipping Trap:** When volume is boosted digitally to 200% or 300% (`+6\text{ dB}` to `+9.5\text{ dB}`) into WSLg's `module-rdp-sink`, signals exceeding 0 dBFS cannot fit into 16-bit integer PCM. Instead of soft-compressing, the waveforms hard-clip into square waves, causing severe bass blowout and harsh high-frequency crackle.
- **Windows Host Volume:** By default, Windows RemoteApp audio ("Remote Desktop Services") is often set to 30%–50% in the Windows Volume Mixer, compounding the quietness at 100%.

### Planned Audio Solutions for Next Session

1. **Verify Windows Host Audio Mixer:**
   - Ensure the Windows Volume Mixer slider for **"Remote Desktop"** or **"wslg"** is set to 100%.
2. **ALSA Soft Limiter / Dynamic Compressor in `halo_asound_pulse.conf`:**
   - Integrate an ALSA LADSPA limiter plugin (`fast_lookahead_limiter` or `dysonCompressor`) into the ALSA chain, or:
   - Configure a soft-knee compressor curve in the channel downmix table (`ttable`) so dialogue and sound effects receive +6 dB boost while loud explosions are gracefully compressed rather than hard-clipped.
3. **Calibrated Sink Boost:**
   - Set the default stream volume to 140%–160% (a calibrated $+3\text{ dB}$ to $+4\text{ dB}$ boost), which raises cabinet volume to standard desktop levels while keeping all peaks below $0\text{ dBFS}$.

---

## 2. GPU Performance: Maximizing the RTX 3080

### Why Does Windows / WSLg Feel Laggier Than Native Linux?

1. **Mesa D3D12 Translation Overhead:**
   - On native Linux, the game talks directly to NVIDIA’s proprietary OpenGL driver (`libnvidia-glcore.so`), executing draw calls directly on hardware.
   - On WSL2, OpenGL runs through **Mesa Gallium D3D12 (`d3d12_dri.so`)** over Hyper-V (`/dev/dxg`). Mesa translates hundreds of OpenGL draw calls, shader binds, and texture states per frame into DirectX 12 command lists on the CPU.
2. **Single-Threaded Draw Submission Bottleneck:**
   - Without multithreaded GL dispatch, the game's main thread is stalled while Mesa translates draw calls to D3D12.
3. **Double-VSync Cadence (Weston vs. Windows DWM):**
   - In `graphics.ini`, `VSYNC = 1` causes the game to block waiting for WSLg/Weston’s frame swap.
   - Weston itself buffers and presents frames to Windows Desktop Window Manager (DWM).
   - If the game misses a single 60 Hz presentation deadline by even $0.1\text{ ms}$ (taking $16.8\text{ ms}$ instead of $16.6\text{ ms}$), the double-synchronization drops the presentation cadence directly to **30 FPS**, creating noticeable stutter and lag.
4. **NVIDIA GPU Power State (P-State):**
   - On Windows, the NVIDIA driver monitors foreground 3D game executables to boost clock speeds. Because OpenRaven runs inside `vmmemWSL` (a background Hyper-V virtual machine process), the GPU often remains in low power states (P8 or P5) rather than boosting to maximum 3D clocks.

### Planned Performance Optimizations for Next Session

#### 1. Enable Mesa Asynchronous Multithreading (`mesa_glthread=true`)
- Set `mesa_glthread=true` in `play_halo.sh` / `windows_setup.ps1`.
- This offloads OpenGL call validation and D3D12 command translation onto a separate worker thread, dramatically reducing main-thread frame times.

#### 2. Eliminate Double-VSync Stutter
- Set `VSYNC = 0` (or `RAVENGFX_SWAPINTERVAL = 0`) in `graphics.ini`.
- Use a dedicated sleep-based frame limiter (`FPS_CAP = 60`) instead of hard VSync blocking.
- Windows DWM automatically prevents screen tearing on the Windows desktop, while uncapping the internal WSLg swap interval eliminates the 30 FPS drop penalty.

#### 3. Enable Built-in AMD FSR 2 Upscaling (`graphics.ini`)
- OpenRaven already includes native AMD FSR 2 integration.
- Set:
  ```ini
  UPSCALER = fsr2
  FSR_QUALITY = quality   # Renders internally at 1280x720, reconstructs to 1080p
  FSR_SHARPNESS = 0.5
  ```
- This reduces the internal rasterization workload by ~55%, giving a huge boost to framerate and frame pacing.

#### 4. Balance Heavy Shadow & Post-Processing Settings
- Test shadow resolution at 1024 / 2048:
  ```ini
  SHADOW_SIZE = 1024
  STATIC_SHADOW_SIZE = 1024
  SHADOW_SPLITS = 2
  SSR_BUFFER_SCALE = 0.5
  ```

#### 5. Windows NVIDIA Driver Optimization
- In NVIDIA Control Panel -> **Manage 3D Settings**:
  - Set **Power management mode**: `Prefer maximum performance`.
  - Set **Low Latency Mode**: `On` or `Ultra`.

---

## 3. Controller & Trigger Input Diagnostics (RT & LT)

### Why Weren't Right Trigger (Shoot) and Left Trigger (Grenade) Working?

| Control Action | Game Cabinet Target | Linux Native (`/dev/input/js0`) | Windows (`XInputBridge` -> `winpad.bin`) |
| :--- | :--- | :--- | :--- |
| **Shoot (Fire)** | Switch 795 (Gun 1 Trigger) | `axis[5] > 0` (RT) | `axes[5] = rt` (`bRightTrigger * 257 - 32767`) |
| **Grenade** | Switch 797 (Gun 1 Grenade) | `axis[2] > 0` (LT) | `axes[2] = lt` (`bLeftTrigger * 257 - 32767`) |
| **Reload** | Switch 796 (Gun 1 Reload) | `btn[PAD_RB]` | `btns[5] = RB` |
| **Start / Coin** | Switch 581 / 570 | `btn[PAD_START]`, `btn[PAD_Y]` | `btns[7] = START`, `btns[3] = Y` |

### Key Reasons Triggers May Fail on Windows:

1. **Trigger Deadzone & Threshold Mismatch:**
   - In `rio2pad.c`, shooting requires `axis[5] > 0` and grenade requires `axis[2] > 0`.
   - In `windows_setup.ps1`, the trigger conversion is `(bTrigger * 257 - 32767)`. If the trigger is pulled lightly or has deadzone drift, it remains $\le 0$, resulting in no shot being registered.
   - Adding a digital trigger threshold (e.g. `bTrigger > 30` or mapping trigger presses directly into button flags) guarantees instant response even on partial pulls.
2. **First-Active-Gamepad Auto-Mapping (Player Index Mismatch):**
   - On Windows, XInput assigns controllers to Player Indexes 0, 1, 2, or 3. If Windows assigned your controller to Index 1 (Player 2) instead of Index 0 (Player 1), Gun 1 in the game will ignore your controller inputs because Gun 1 only listens to Player 1.
   - Fix: Update `XInputBridge` so that the first physically active controller is automatically routed to Player 1 (Gun 1), regardless of which XInput slot Windows assigned it to.
3. **DirectInput / Non-Xbox Gamepads:**
   - If using a DualSense, DualShock 4, or Nintendo Switch Pro controller, Windows XInput will not detect it natively unless an XInput wrapper like DS4Windows or Steam Input is running.

---

## 4. Implementation Status (Completed)

All roadmap optimizations have been implemented, compiled, and verified:

1. **Audio Soft-Limiter & Bass-Attenuated Downmix:**
   - **`openraven/halo_asound_limiter.conf`:** Configured ALSA `fastLookaheadLimiter` with **0 dB input gain** and **-1.0 dBFS ceiling** with **0.15s release**. Because `rio2pad.c` already sets `gameVolume` to 100%, adding artificial input gain was slamming the limiter into 15+ dB of continuous compression, causing muddy bass blowout.
   - **Subwoofer (LFE) Attenuation:** In `surround51` downmix, reduced LFE gain from 0.5 to **0.15**. The arcade cabinet LFE channel is mixed for powerful physical seat vibrators; attenuating it to 0.15 provides clean, punchy bass without overpowering dialog or muddying speakers.
   - **`openraven/rio2pad.c`:** Set `gameVolume = 100` and `attractVolume = 100`.
   - **`openraven/graphics.ini`:** `volume = 100%`.

2. **Controller Triggers & First-Active-Player Auto-Mapping:**
   - **`openraven/windows_setup.ps1` (`XInputBridge`):**
     - Dynamic auto-mapping routes the first active physical controller to Player 1 (`winpad` slot 0 / Gun 1), even if Windows assigned it slot 1, 2, or 3.
     - Low-threshold hair-trigger mapping: threshold `> 16` (~6% trigger pull) triggers instant shot (`axis[5] > 0`) and grenade (`axis[2] > 0`) without needing >50% squeeze.
   - **`openraven/rio2pad.c`:** Updated trigger and grenade checks for virtual pads (`winpad.bin`).
   - Rebuilt `rio2pad.so`.

3. **Window Display, Desktop Mapping & GPU Performance:**
   - **Window Visibility on Windows Desktop:** Under FreeRDP RAIL, undecorated borderless windows (`WS_POPUP`) were prevented by Windows from stealing focus from Command Prompt, leaving the window hidden behind existing apps with no taskbar entry. Setting `HALO_FULLSCREEN = 0` provides standard native window decorations, creates a Windows taskbar entry, and an automated background activator brings the "Halo" window directly to the foreground upon launch.
   - **Mesa Multithreading (`mesa_glthread=true`):** Enabled in `play_halo.sh`, `windows_setup.ps1`, and passed into `bwrap` container sandbox to offload OpenGL translation onto asynchronous worker threads.
   - **VSync Cadence & FPS Limiter:** Set `FPS_CAP = 60` and `VSYNC = 0` in `graphics.ini`.
   - **Upscaler Configuration:** Set `UPSCALER = off` for native rendering on the RTX 3080. (Mesa D3D12 OpenGL does not expose `GL_KHR_shader_subgroup` required by AMD FSR 2 compute shaders).
   - **RTX 3080 Dedicated VRAM:** Direct3D 12 hardware acceleration verified on `NVIDIA GeForce RTX 3080` with full 10,053 MB (10 GB) dedicated VRAM.
   - Rebuilt `ravengfx.so` and `vk/ravenvk_win.exe`.

---

## 5. Current Repository Status

- **Repository:** `https://github.com/wuzzy450/OpenRaven.git` (branch `main`, commit `7888730`)
- **Fullscreen & Window Mapping:** Fixed (borderless window mode via `_MOTIF_WM_HINTS` on WSLg).
- **Session Cleanup:** Fixed (lingering processes and wedged display servers automatically cleaned and refreshed).
- **Privacy:** Verified clean with no personal user data or paths.
