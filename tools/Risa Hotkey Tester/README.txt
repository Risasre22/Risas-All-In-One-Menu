RISA HOTKEY RESTORE TESTER
===========================

LAUNCHING (no console window)
  - Double-click RisaHotkeyTester.bat  (recommended - hides the PowerShell console and
    closes its own window instantly; works even if Windows Script Host is disabled).
  - RisaHotkeyTester.vbs also works for a zero-flash launch, but only if Windows Script
    Host is enabled on your PC. If double-clicking the .vbs does nothing, use the .bat.

SETUP
  1. Copy this entire folder into its own MO2 mod, for example "Risa's Tester".
  2. Launch it with the .vbs (or .bat) above - you do not need to launch it through MO2.

SOURCE (dropdown, top-left)
  - Skyrim (default): reads the game's real Data folder (physical install). The Skyrim
    path is taken from Mod Organizer's gamePath when available.
  - Mod Organizer: reads a MO2 profile's mod priority plus the overwrite folder. Use the
    Profile dropdown beside it to choose which profile to inspect.

VIEW OPTIONS
  - "Show full paths": off shows short relative paths (mods\..., overwrite\..., game\...);
    on shows complete file paths. The "Active file" column updates immediately.
  - Hover any button for a one-line reminder. Click "?  Help" for the full workflow.

SUGGESTED CUSTOM-RESTORE TEST
  A. Click "Snapshot + Randomize" before launching Skyrim.
  B. Launch Skyrim, let Risa manage the hotkeys, then choose
     "Restore My Original Settings" and exit.
  C. Run this tester again and click "Verify Custom Restore".
     Every present setting should be green.

SUGGESTED SUPPORTED-DEFAULTS TEST
  A. Click "Snapshot + Randomize" before launching Skyrim.
  B. Launch Skyrim, then choose "Restore Supported Mod Defaults" and exit.
  C. Run this tester again and click "Verify Supported Defaults".
     Every present setting should be green.

NOTES
  "Restore Safety Snapshot" copies the complete original files back. Snapshots and
  the expected randomized values are stored beside the tester under TesterData.
  "Copy Results" copies the entire visible report to the clipboard and also writes
  TesterData\LastResults.txt for quick sharing.
  "Snapshot + Randomize" also safely snapshots and removes a stale
  RisaAllInOneMenu_OriginalHotkeys.json so Risa captures the new randomized test
  values instead of retaining results from an older run.

  Close Skyrim before using buttons that write files.
