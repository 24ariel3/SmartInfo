@echo off
setlocal EnableExtensions DisableDelayedExpansion
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ota_update.ps1" %*
exit /b %errorlevel%
