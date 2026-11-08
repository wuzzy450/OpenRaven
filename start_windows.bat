@echo off
title OpenRaven - Halo: Fireteam Raven (native Windows)
:: Finds your game archives, sets up the files, then starts the game. Drop archives on this file to use them.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0openraven\setup_windows.ps1" %*
if errorlevel 1 (
    echo.
    echo [OpenRaven] Stopped with an error. Press any key to close...
    pause >nul
)
