@echo off
title OpenRaven - Halo: Fireteam Raven (native Windows)
:: Finds your game archives, sets up the files, then starts the game. Drop archives on this file to use them.
:: This .bat may live deeper in the tree (github_staging\), so the real game folder is passed explicitly:
for %%I in ("%~dp0..") do set "GAME_ROOT=%%~fI"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0openraven\setup_windows.ps1" -Root "%GAME_ROOT%" %*
if errorlevel 1 (
    echo.
    echo [OpenRaven] Stopped with an error. Press any key to close...
    pause >nul
)
