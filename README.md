# OpenRaven

**Play the *Halo: Fireteam Raven* arcade game on your PC.** Free and open source, for Windows 10/11 and Linux.

[![Halo: Fireteam Raven Mission 1 on PC with OpenRaven](https://img.youtube.com/vi/pZ166zhaEjs/maxresdefault.jpg)](https://youtu.be/pZ166zhaEjs)

▶ [Watch Mission 1 running on PC](https://youtu.be/pZ166zhaEjs) · 💬 [Discord](https://discord.gg/nuvpXycag8) for help and updates · ☕ [Support on Ko-fi](https://ko-fi.com/wuzzy450)

> 🆕 **v1.1.0 – The Online Coop Update!** Play together over the internet or your home network (up to 4 PCs, Windows and Linux mixed), let friends join from just a web browser, and play the full 4-player cabinet on one screen. See [Online coop](#online-coop) below.

*Halo: Fireteam Raven* (Raw Thrills / Play Mechanix / 343 Industries, 2018) is a four-player arcade rail shooter that only ever ran on arcade cabinets. OpenRaven runs the original v3.11 cabinet game on a normal PC with a controller or keyboard, in any resolution, with modern graphics options on top.

> **You need your own copy of the game.** No game files are included here. OpenRaven works with a v3.11 game archive (about 6 GB, as `.zip`, `.7z` or `.rar`). Setup recognises the files by their contents, so file names and folder layout don't matter.

---

## What you get

- **One-click setup.** Point it at your game archives once, and it unpacks everything and starts the game. After that it starts straight away.
- **Native on Windows.** Runs the game directly on your graphics driver. No WSL, no virtual machine, no emulator.
- **Online coop (new in v1.1.0).** Up to 4 PCs play one game together, over the internet or at home. Windows and Linux PCs can play in the same game. Automatic port forwarding (UPnP).
- **Browser stream (new in v1.1.0).** Only you need the game: up to 3 friends open a link in their web browser, see and hear the game live, and play as players 2–4 with mouse, keyboard or controller.
- **The 4-player cabinet (new in v1.1.0).** All 4 player spots on one screen, with a "hybrid" picture: menus as on the cabinet, missions filling your whole screen. The original 2-player cabinet is one setting away.
- **Controllers and keyboard.** Up to 4 controllers (one per player): Xbox-style and PlayStation (DualShock 4 / DualSense), plus mouse and keyboard.
- **Arcade quirks handled.** Free Play on, boot checks and gun calibration skipped, straight to the title screen in about 10 seconds. Muted cabinet audio fixed.
- **Any screen.** Any resolution and aspect ratio (16:9, 16:10, 21:9, 32:9, 4:3) with the correct proportions, and adjustable field of view.
- **Better graphics than the cabinet.** Up to 16x texture filtering, 8192 shadow maps with 4 cascades, full-resolution reflections and SSAO.
- **AMD FSR 2**, as anti-aliasing at native resolution or as an upscaler for slower GPUs.
- **Frame generation** (2x / 3x / 4x). Only fills refresh slots your screen can show, so it never lowers the game's own frame rate.
- **Native HDR10 on Windows and Linux**: the game's own highlights (fire, sun, muzzle flashes) are kept in 16-bit float instead of being cut off at white, up to your screen's peak brightness. Display resolution, refresh rate, HDR and peak brightness are detected on first launch. On Linux, HDR needs HDR turned on in the desktop (KDE Plasma 6) and `gamescope` installed; the game then runs inside gamescope.
- **In-game settings menu (F1).** Change everything live while you play.
- **Neural detail pass.** A small neural network trained on this game that restores texture and face detail (finer armor grit and scratches, sharper faces). Works on Windows and Linux with any Vulkan GPU; it costs about 8 ms per frame at 4K on an RTX 3080. Turn it on in the F1 menu.

Frame generation and the neural pass both work with HDR on.

Out of the box it uses maximum quality, native resolution, and FSR 2 as anti-aliasing, at 60 FPS like the arcade machine.

---

## Install and play

### Windows 10 / 11

**You need:** Windows 10 or 11 (64-bit), a graphics card with an up-to-date driver (NVIDIA, AMD or Intel), and about 11 GB of free space.

1. Download **`OpenRaven.zip`** from the [latest release](https://github.com/wuzzy450/OpenRaven/releases/latest) and unzip it anywhere (right-click → Extract All).
2. Double-click **`start_windows.bat`**.
3. **First run only:** it looks for your game archive in the OpenRaven folder, the folder above it, Downloads and Desktop. If it can't find it, a window opens so you can pick it. Unpacking takes a few minutes. You can also drag the archive onto `start_windows.bat`.
4. The game starts fullscreen. Next time, double-clicking `start_windows.bat` starts it straight away.

To quit, press **Alt+F4**. If something goes wrong, share `ravenloader.log` from the OpenRaven folder on the [Discord](https://discord.gg/nuvpXycag8).

### Linux

Download **`OpenRaven.zip`** from the [latest release](https://github.com/wuzzy450/OpenRaven/releases/latest), unzip it, and run `./start_linux.sh` in that folder. Or with git:

```bash
git clone https://github.com/wuzzy450/OpenRaven.git
cd OpenRaven
./start_linux.sh
```

The first run installs what it needs (asking for your password), finds your game archive or asks for it, unpacks it and starts the game. You can also give it the archive directly: `./start_linux.sh /path/to/game.zip`.

To force-close the game (arcade games have no quit button): `./openraven/stop_halo.sh`

### "Still needed: the v3.11 game program (game + halo.so)"

If setup unpacks your game data but then asks for the game program, your archive most likely contains the original, encrypted arcade executable, which is locked to the cabinet's HASP security dongle and only runs on the real machine with the dongle.

OpenRaven doesn't include a HASP decrypter or dongle emulator. It needs an already-unpacked, unencrypted v3.11 program (`game` and `halo.so`) to run. Because of copyright law and anti-circumvention laws such as the DMCA, the OpenRaven project can't distribute those files, provide tools that remove the dongle protection, or point you to where to get them. OpenRaven only ships its own loader code; you supply the game files.

If you have another copy of v3.11 that includes both files, run setup again and pick it. Your game data is kept, so setup only needs those two files.

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

## Online coop

There are two ways to play with friends. Pick one:

| | **Online coop** | **Browser stream** |
|---|---|---|
| Who needs the game? | Every player (each PC runs its own copy) | Only you (the host) |
| What friends need | OpenRaven + the game, Windows or Linux | Any web browser (PC, laptop) |
| Picture quality | Full quality on every PC | Video stream (720p), a little delay |
| Players | Up to 4 PCs | You + up to 3 browsers |
| Port to open on the router (internet only) | UDP 27500 | TCP 27600 |

Both work on a home network straight away. Over the internet only the **host** may need to open one port (see [Ports](#ports-and-the-router)).

### Online coop, step by step

Every player needs OpenRaven **v1.1.0** and the same v3.11 game set up and running solo first (see [Install and play](#install-and-play)). The host lets in only PCs with the same version, so everyone must update.

**The host** (one player, ideally the one with the best internet):

1. Start the game and press **F1**, go to the **Coop** tab.
2. Set **Mode** to `host`.
3. Set **Players** to the number of PCs in the game, your own included (2, 3 or 4).
4. Leave **Port** at `27500` and **Auto port forwarding** at `1`.
5. Optional: **Input delay**. `1`–`2` for players at home, `3`–`4` over the internet (the default 3 is fine for most).
6. Press **Enter** (Save), then **quit and restart the game** (Alt+F4 on Windows, `./openraven/stop_halo.sh` on Linux).
7. The game now waits on a black screen for the others. Press F1 → Coop: the **Status** section shows the address to give your friends:
   - friends in the same house: the home address, e.g. `192.168.1.20`
   - friends over the internet: your public address (also shown at [whatismyip.com](https://www.whatismyip.com)), e.g. `203.0.113.5`

**Every friend who joins:**

1. Start the game, press **F1**, go to the **Coop** tab.
2. Set **Mode** to `join`.
3. Select **Host address** and type the host's address (Backspace deletes). If the host picked another port than 27500, add it: `203.0.113.5:27501`.
4. Press **Enter** (Save), then quit and restart the game.

The game starts on all PCs as soon as everyone is in. Joiners copy the host's player count and cabinet by themselves. Player 1 is the host, the next PCs are players 2, 3 and 4, in the order they joined.

**To play solo again:** F1 → Coop → Mode `off`, Save, restart.

Windows only, without the F1 menu: `coop_host.bat` (hosts a 2-PC game) and `coop_join.bat` (asks for the host's address) do the same in one double-click.

**If something goes wrong:**
- *Stuck on the black screen:* the joiner can't reach the host. Check the address, check that both use the same port, and over the internet see [Ports](#ports-and-the-router).
- *"Different version" / not let in:* update everyone to the same OpenRaven release and the same game files.
- *A message about the game going out of sync, or a player gone:* shown at the top of the screen. Restart the game on every PC. Please share `ravenloader.log` (Windows) or `openraven/run_logs/last_play.log` (Linux) on the [Discord](https://discord.gg/nuvpXycag8).
- *The game feels laggy for everyone:* the slowest connection sets the pace. Raise **Input delay** by 1 on the host.

### Browser stream, step by step

Only the host runs the game; friends just need a browser.

1. Press **F1** → **Coop** tab → **Browser stream** section.
2. Set **Browser stream** to `1` (leave online coop **Mode** at `off`; the two can't be on together).
3. Press **Enter** (Save), then quit and restart the game.
4. Press F1 → Coop again: **Link for friends at home** (e.g. `http://192.168.1.20:27600`) and **Link over the internet** are shown. Send the right one to your friends.
5. Friends open the link and click the picture. They press **Enter** (or Start on a controller) to join as player 2, 3 or 4.

Browser controls: mouse to aim, left click fire, right click reload, middle click or **G** grenade, **Enter** or **1** start/join, **Backspace** back, **F** fullscreen. A controller plugged into the friend's PC works too if their browser allows it.

How many friends can play depends on the cabinet: the 4-player cabinet (the default) gives 3 spots, the 2-player cabinet 1. Extra browsers can still watch. If the picture stutters for a friend, lower **Stream quality** (5000 or 3000). The encoder uses your graphics card (NVIDIA, AMD or Intel) and falls back to the processor.

### Ports and the router

On a home network nothing needs to be set up. Over the internet, the **host's** router must let the game through:

- Online coop: **UDP** port **27500** (or the port you picked).
- Browser stream: **TCP** port **27600** (or the port you picked).

With **Auto port forwarding** on, OpenRaven asks the router to do this by itself (UPnP); F1 → Coop → Status says whether it worked. If it didn't, open your router's page (often http://192.168.1.1), find *Port forwarding* (or *Virtual server*, *NAT*), and forward that port to the home address shown in the Status section. Joiners never need to open a port. Some internet providers (CGNAT, many mobile connections) don't allow hosting at all; then let another player host.

### One PC, 4 players

The game now starts as the **4-player cabinet**: controllers 1–4 are players 1–4, all on one screen. **F1 → Cabinet → 4-player picture** picks how it looks:

- `hybrid` (default): menus show both cabinet screens once, missions fill your whole screen, cutscenes play 16:9.
- `1`: both cabinet screens side by side (32:9), with black bars on a 16:9 screen.
- `0`: squeezed into your screen.

For the original **2-player cabinet**: F1 → Cabinet → **Players** `2`, Save, restart.

### Known issues (v1.1.0)

The game was made for two cabinet screens side by side, and in the 4-player picture some of that still shows:

- **Doubled on-screen messages:** weapon pick-up alerts and other pop-ups can appear twice (once for each cabinet screen).
- **Doubled subtitles** during missions and cutscenes.
- **Score screen** at the end of a mission is shown 32:9 with black bars in the 4-player picture (so all 4 player spots fit).
- Other small doubled or misplaced bits of the screen (HUD pieces, hints) can show up the same way.
- The Microsoft Studios logo animation at the start of the game is not shown yet (an OpenRaven bug; we want it back in).
- PlayStation controllers (DualShock 4 / DualSense) are newly added to OpenRaven and not tested yet, on Windows or Linux.

None of these affect the game itself. Fixes are planned for later updates; please report anything else on the [Discord](https://discord.gg/nuvpXycag8).

---

## Settings menu (F1)

Press **F1** in game. Use the mouse, or the arrow keys, Tab, Enter (save) and Esc (close).

- **Display:** resolution, VSync, FPS cap, aspect fix, field of view, FPS counter, HDR on/off, peak brightness, paper white and highlight strength *(applies after a restart; Windows: turn on "Use HDR" in display settings first; Linux: HDR on in the desktop + gamescope)*.
- **Upscaling:** FSR 2 mode (native, quality, balanced, performance, ultra performance) and sharpness.
- **Frame generation:** off / 2x / 3x / 4x, interpolate or extrapolate.
- **Quality:** texture filtering, shadow resolution and cascades, stable shadows, reflections, SSAO, bloom, glow, lens flare.
- **Audio & Input:** volume, aim mode (cursor or absolute), stick speed.
- **Cabinet:** Free Play, skip boot checks, players (2 or 4) and the 4-player picture.
- **Coop:** online coop (host / join, address, players, port, input delay, auto port forwarding), status and links, browser stream.

Settings are saved in [`openraven/graphics.ini`](openraven/graphics.ini). Delete that file to go back to the game's own arcade settings.

**Slow GPU?** Set FSR 2 mode to *quality* or *performance*, and lower the shadow resolution to 4096.

---

## How it works

The cabinet runs a 64-bit Linux game with a Raw Thrills I/O board, IR light guns and a 5.1 amplifier. OpenRaven never modifies the game files. It loads the game as is and swaps the cabinet hardware for PC equivalents:

- **Input** (`rio2pad`): the cabinet's switch and gun-tracking reads are answered from your controllers and keyboard.
- **Graphics** (`ravengfx`): engine settings are overridden at start-up, the camera projection is corrected for your screen shape, and FSR 2, frame generation, HDR and the F1 menu hook into the game's OpenGL rendering.
- **Online coop** (`coopnet`): *lockstep*. Every PC runs its own copy of the game, and all copies get every player's buttons on the same frame, so they stay identical without sending the game itself. The PCs regularly compare a fingerprint of the game state to catch problems.
- **Browser stream** (`streamweb`, `streamenc`): the host's picture is encoded to H.264 on the graphics card and sent with the game's sound to the browser page; the browser sends back mouse, keyboard and pad input.
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
| `openraven/coop.c`, `openraven/coopnet.c`, `openraven/coopupnp.c` | Online coop: lockstep, network, UPnP port forwarding |
| `openraven/streamweb.c`, `openraven/streamenc.c` | Browser stream: web page and video encoder |
| `openraven/ravenfg.c` | Frame generation |
| `openraven/win/ravenhdr_win.c` | HDR10 output (Windows) |
| `openraven/ravenhdr.c` | HDR10 output (Linux, Vulkan) |
| `openraven/ravennn*.inc`, `openraven/vk/`, `openraven/neural/` | Neural detail pass (Vulkan) |

### Building from source

Prebuilt files are included, so you only need this if you change the code.

- **Linux:** `cd openraven && ./build.sh` (needs gcc/g++, X11 and Vulkan headers; `start_linux.sh` installs them).
- **Windows:** install [MSYS2](https://www.msys2.org/) with the clang64 toolchain, put `C:/msys64/clang64/bin` on your PATH, then run `make` in `openraven/win`.

---

## Credits

- **Wuzzy450**: creator ([Ko-fi](https://ko-fi.com/wuzzy450) · [Discord](https://discord.gg/nuvpXycag8))
- **AMD FidelityFX FSR 2**, and **JuanDiegoMontoya**'s MIT-licensed OpenGL port of it (`openraven/fsr2/`)

## License

OpenRaven's own code is MIT licensed. Included third-party files keep their own licenses: FSR 2 (MIT), the GCC runtime libraries in `openraven/cabinet_libs/` (GPL with the GCC Runtime Library Exception), and FMOD in `openraven/extralibs/` (FMOD license). Details for every included binary: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

OpenRaven is an unofficial fan project. It isn't affiliated with or endorsed by Microsoft, 343 Industries, Raw Thrills or Play Mechanix, and it contains no game data.
