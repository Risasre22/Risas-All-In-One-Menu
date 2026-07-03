@echo off
rem Launch the tester with the PowerShell console hidden, then close this window
rem immediately. Self-contained: does not depend on Windows Script Host / the .vbs.
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0RisaHotkeyTester.ps1"
exit
