@echo off
title OpenRaven coop - JOIN
:: Online coop, joining side. Start this after (or before) the host; the game waits until both are ready.
set /p HOSTADDR=Host address (IP or name): 
set RAVEN_COOP=join:%HOSTADDR%
call "%~dp0start_windows.bat" %*
