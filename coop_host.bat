@echo off
title OpenRaven coop - HOST
:: Online coop, host side. The game waits at a black screen until the other PC has joined.
:: Over the internet: forward UDP port 27500 on your router to this PC and give your friend your public IP.
set RAVEN_COOP=host
set RAVEN_COOP_PLAYERS=2
echo [OpenRaven] Hosting coop on UDP port 27500. Your addresses:
ipconfig | findstr /c:"IPv4"
call "%~dp0start_windows.bat" %*
