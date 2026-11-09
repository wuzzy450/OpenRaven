# Third-party files

OpenRaven's own code is MIT licensed (see `LICENSE`). These files in the repo come from other projects
and keep their own licenses. Nothing here is game data: the game files come from your own copy.

| File(s) | What it is | License |
|---|---|---|
| `openraven/fsr2/` | AMD FidelityFX Super Resolution 2 and JuanDiegoMontoya's OpenGL port | MIT (`openraven/fsr2/src/LICENSE_FSR2.txt`) |
| `openraven/fsr2/src/gl/external/glad/` | glad OpenGL loader (generated code) | MIT / public domain (Khronos headers: Apache 2.0) |
| `openraven/cabinet_libs/.../libstdc++.so.6`, `libgcc_s.so.1` | GCC C++ runtime, taken from the arcade cabinet's own Linux system | GPL v3 with the GCC Runtime Library Exception: may be shipped with programs of any license |
| `openraven/extralibs/libGLU.so.1*` | Mesa GLU | SGI Free Software License B 2.0 (MIT-style) |
| `openraven/extralibs/libiw.so.30` | Wireless Tools library (the game links it but never uses it) | GPL v2. Source: <https://hewlettpackard.github.io/wireless-tools/> |
| `openraven/extralibs/libfmod.so.10`, `libfmodstudio.so.10` | FMOD Studio 1.10 for Linux, same version the game was built with | FMOD End User License Agreement (Firelight Technologies). See note below. |

## FMOD

FMOD is not open source. Firelight's license lets products made with FMOD ship its libraries, but not
a third party handing them out on their own. These two files are included so the game's own sound
code can run; if Firelight asks, they will be removed and the launchers will ask for them instead
(they come with the FMOD Studio API 1.10.x download from <https://www.fmod.com/download>, free account).

Windows uses the same two Linux files (loaded by `ravenloader.exe`, sound goes out through WASAPI).
No Windows FMOD DLLs are included; the old `fmod64.dll` / `fmodstudio64.dll` path in `shim_fmod.c` is
only a fallback for people who have them.

## Game

Halo: Fireteam Raven belongs to Microsoft / 343 Industries, Raw Thrills and Play Mechanix. OpenRaven
contains none of its files and is not affiliated with or endorsed by them.
