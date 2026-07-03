' Launches the tester with NO console window at all.
' Double-click this file (recommended), or let RisaHotkeyTester.bat call it.
Dim shell, fso, here, ps1
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
ps1 = here & "\RisaHotkeyTester.ps1"
' 0 = hidden window, False = don't wait for it to close.
shell.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File """ & ps1 & """", 0, False
