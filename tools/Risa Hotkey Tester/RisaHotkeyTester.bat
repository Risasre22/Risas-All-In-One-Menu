@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0RisaHotkeyTester.ps1"
endlocal
