RISA HOTKEY RESTORE TESTER
===========================

1. Copy this entire folder into its own MO2 mod, for example "Risa's Tester".
2. Double-click RisaHotkeyTester.bat directly. You do not need to launch it through MO2.
3. The tester detects the Mod Organizer folder and selected profile automatically,
   then resolves files using that profile's mod priority plus the overwrite folder.

Suggested custom-restore test:
  A. Click "Snapshot + Randomize" before launching Skyrim.
  B. Launch Skyrim, let Risa manage the hotkeys, then choose
     "Restore My Original Settings" and exit.
  C. Run this tester again and click "Verify Custom Restore".
     Every present setting should be green.

Suggested supported-defaults test:
  A. Click "Snapshot + Randomize" before launching Skyrim.
  B. Launch Skyrim, then choose "Restore Supported Mod Defaults" and exit.
  C. Run this tester again and click "Verify Supported Defaults".
     Every present setting should be green.

"Restore Safety Snapshot" copies the complete original files back. Snapshots and
the expected randomized values are stored beside the tester under TesterData.
"Copy Results" copies the entire visible report to the clipboard and also writes
TesterData\LastResults.txt for quick sharing.
"Snapshot + Randomize" also safely snapshots and removes a stale
RisaAllInOneMenu_OriginalHotkeys.json so Risa captures the new randomized test
values instead of retaining results from an older run.

Close Skyrim before using buttons that write files.
