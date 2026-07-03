@echo off
rem Launch the tester with the PowerShell console hidden, then close this window
rem immediately, so no console lingers while the tester is open.
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0RisaHotkeyTester.ps1"
exit
