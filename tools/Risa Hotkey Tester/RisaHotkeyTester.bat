@echo off
rem Hand off to the windowless VBS launcher and close this console immediately,
rem so no cmd window stays open while the tester is running.
start "" wscript.exe "%~dp0RisaHotkeyTester.vbs"
exit
