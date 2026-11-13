@echo off
title OpenRaven - 4-player cabinet, fills your screen (16:9)
:: Same 4-player game as play_solo.bat, but the picture fills the whole screen like the
:: 2-player version: RAVEN_WIDE4=0 runs the game at your screen's native 16:9, and the
:: mission camera keeps its full width of view and shows more above and below (vert+)
:: - no bars, no stretching, correct proportions.
:: Trade-off (why the 32:9 cabinet look is the default): the attract menus, cutscene
:: movies and the 4 side-by-side HUDs are authored for the cabinet's 32:9 screen, so they
:: look horizontally squeezed in this mode. Missions fill the screen normally.
:: Third option: replace RAVEN_WIDE4=0 with  set RAVEN_WIDE4FIT=1  - keeps the height and
:: cuts the sides instead (fills the screen too, but everything looks zoomed in).
:: F1 still works: "Field of view" tunes how much the camera shows.

set RIO2PAD_PLAYERS=4
set RAVEN_WIDE4=0
call "%~dp0start_windows.bat" %*
