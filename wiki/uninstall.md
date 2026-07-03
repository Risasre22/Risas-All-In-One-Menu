# Uninstall & Restore

Because the launcher manages other mods' hotkeys, you should let it **put those hotkeys back** before you remove it. This is a one-click step in the launcher.

## Before uninstalling

1. Open the launcher (**F1**) and go to the **Settings** tab.
2. Choose one of the restore options:

    - **Restore My Original Settings** — reverts each managed mod to the value it had **before** Risa's Menu first changed it. Missing captured values fall back to the mod's safe default.
    - **Restore Supported Mod Defaults** — sets each managed mod to its **upstream default** hotkey, ignoring any captured custom value.

3. When restore finishes, choose to **quit to desktop** (or exit Skyrim normally).
4. **Disable or remove** Risa's All In One Menu *before* launching Skyrim again.

The restored hotkeys take effect on the next game launch.

!!! warning "Disable the mod before relaunching"
    If you relaunch Skyrim with the mod still enabled, it will re-apply its hotkey management. Restore → quit → disable the mod → then launch again.

## What restore touches

Restore only rewrites the specific hotkey values the launcher itself changed, in each managed mod's config. Unrelated settings in those files are left alone. Files that don't exist are skipped.

## If restore reports "incomplete"

If a restore reports that it was incomplete, it means one config file genuinely couldn't be written (for example it was read-only or locked). Every other mod is still restored, and the originals backup is **kept** so you can fix the cause and run restore again. Check `Documents\My Games\Skyrim Special Edition\SKSE\RisaAllInOneMenu.log` for the specific file.

!!! tip "Audit log"
    A second, plain-English log — `RisaAllInOneMenu_FileChanges.log` — records exactly which config files the mod edits (EDIT) and reverts (REVERT), and nothing else. Both logs can be toggled in the **Settings** tab.

## Fresh start

After a successful restore, the originals backup is removed so a future reinstall starts clean and captures your settings fresh.
