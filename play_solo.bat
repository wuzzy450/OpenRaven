@echo off
title OpenRaven - 4-player cabinet, you as Player 1
:: The 4-player cabinet (one wide 32:9 screen, 4 stations). Only you join as P1;
:: the other three stations just sit on "JOIN NOW". Freeplay is on - just press START.
::
:: Player 1 controls (all built in, no recording, no test settings):
::   Mouse           aim (the cursor IS the crosshair). Only while this window is in front.
::   Left button     fire      Right button   reload      Middle button   grenade
::   Enter or 1      START     Space          fire        R               reload
::   G               grenade   Arrow keys     move aim    F2              operator menu
::   F12             screenshot (PNG in the game folder's "screenshots" folder)
::   Gamepad 1       right stick = aim (pushing a stick takes the aim back from the mouse),
::                   right trigger = fire, right bumper = reload, left trigger = grenade,
::                   Start = join. (A 2nd gamepad or the 2 key can join P2 any time.)
::
:: F1 opens the settings overlay (resolution, field of view, mouse aim + sensitivity,
:: stick speed, FPS cap, volume...) - changes save automatically to openraven\graphics.ini.
::
:: Optional knobs - uncomment (remove "rem ") to use:
:: rem set RIO2PAD_MOUSE=relative      mouse MOVES the aim instead of cursor=aim
:: rem set RIO2PAD_MOUSESENS=1.5        relative-mode speed
:: rem set RIO2PAD_AIM=abs              gamepad stick points directly (no dead-zone drift)
:: rem set RIO2PAD_STICKSPEED=0.5       cursor-mode stick speed, screen widths per second
:: rem set RAVEN_RES=2560x720           force the window size (32:9 = width x height/9*32)

set RIO2PAD_PLAYERS=4
:: Hybrid picture (fills your screen, nothing squeezed): menus show once, full screen;
:: mission select shows all 6 missions; cutscenes play the 16:9 movies; missions fill the
:: screen with HUD bars pinned to the top/bottom edges. set RAVEN_WIDE4=1 instead for the
:: cabinet-exact 32:9 picture with black bars.
set RAVEN_WIDE4=hybrid
:: Cabinet-style fullscreen: full monitor width at 32:9 (3840x1080 on a 4K screen, black bars
:: top/bottom). For a normal window instead, add this line back:  set RAVEN_WINDOWED=1
call "%~dp0start_windows.bat" %*
