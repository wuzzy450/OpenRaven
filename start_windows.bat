@echo off
title OpenRaven - Halo: Fireteam Raven (native Windows)
:: Finds your game archives, sets up the files, then starts the game. Drop archives on this file to use them.
:: Game folder = this .bat's own folder (the unzipped OpenRaven release). Only in the dev layout, where this
:: .bat sits in github_staging\ inside an installed game folder, is the folder above it used instead.
for %%I in ("%~dp0.") do set "GAME_ROOT=%%~fI"
if exist "%~dp0..\openraven\win\ravenloader.exe" for %%I in ("%~dp0..") do set "GAME_ROOT=%%~fI"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0openraven\setup_windows.ps1" -Root "%GAME_ROOT%" %*
if errorlevel 1 (
    echo.
    echo [OpenRaven] Stopped with an error. Press any key to close...
    pause >nul
)
