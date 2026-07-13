# Risa's All In One Menu

Risa's All In One Menu is an SKSE plugin that puts supported mod menus behind one configurable launcher. It disables, relocates, or intercepts their native hotkeys so those keys can be reused for gameplay while every installed menu remains available from one place.

Press **F1** by default to open or close the launcher. **Escape** also closes it.

## Supported Menus

- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
- Mod Configuration Menu (MCM)
- [SearchUI (Get Any Item Instantly)](https://www.nexusmods.com/skyrimspecialedition/mods/101290)
- [Open Animation Replacer](https://www.nexusmods.com/skyrimspecialedition/mods/92109)
- [Immersive Equipment Displays](https://www.nexusmods.com/skyrimspecialedition/mods/62001)
- [Debug Menu](https://www.nexusmods.com/skyrimspecialedition/mods/136456)
- [dMenu NG](https://www.nexusmods.com/skyrimspecialedition/mods/166751) using the API-enabled compatibility build offered by the FOMOD
- [Improved Camera SE](https://www.nexusmods.com/skyrimspecialedition/mods/93962) / Improved Camera NG
- [ENB Editor](http://enbdev.com/download_mod_tesskyrimse.html)
- [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603)
- [KreatE](https://www.nexusmods.com/skyrimspecialedition/mods/83757)
- [Community Shaders](https://www.nexusmods.com/skyrimspecialedition/mods/86492)
- [Skyrim Party Sheet - Player and Follower HUD](https://www.nexusmods.com/skyrimspecialedition/mods/167538)
- [Dragonborn's Toolkit](https://www.nexusmods.com/skyrimspecialedition/mods/181426)
- [CatMenu](https://www.nexusmods.com/skyrimspecialedition/mods/65958)
- [ReShade](https://reshade.me/)
- [Quick Armor Rebalance](https://www.nexusmods.com/skyrimspecialedition/mods/127967)
- Mod Function Menu
- Outfit Preview Selector
- Modex
- Hotkey Reminder

Only integrations detected in the current installation are displayed. See the [Integration Ledger](https://risasre22.github.io/Risas-All-In-One-Menu/integration-ledger/) for the current integration method and reliability level of each supported mod.

## Features

- One launcher for supported SKSE, Scaleform, and overlay menus
- Configurable launcher key with Ctrl, Shift, Alt, double-press, hold, and Easy Close options
- Rebindable per-menu shortcuts saved across updates and new games
- Settings and Exclusions controls for supported integrations
- Optional access to a mod's original shortcut while it is managed
- Duplicate-key and launcher-key conflict detection
- Dedicated Community Shaders and Skyrim Party Sheet submenus
- Drag-and-drop button ordering, adjustable font/UI scale, and saved window placement
- Immediate language switching when a compatible translation JSON is installed
- Official runtime APIs where available, with guarded compatibility integrations elsewhere
- Main troubleshooting log plus an optional audit log containing only files edited or restored
- Uninstall restoration using captured user settings or original supported-mod defaults

## Requirements

- Skyrim Script Extender (SKSE64)
- Address Library for SKSE Plugins
- SKSE Menu Framework

The supported menu mods themselves are optional.

## Installation

Install the release archive with a mod manager and choose any desired FOMOD options. The optional dMenu component is the API-enabled compatibility build used by this integration.

Some supported mods create their configuration files only after they have run once. When this menu changes a managed configuration file, a translucent ten-second notification appears at startup. Restart Skyrim once so every affected mod reloads its setting.

## Updating and Compatibility

Replace the previous version normally. Launcher settings are stored separately from the DLL and remain across updates.

When this mod or a supported mod receives an update, check the [Integration Ledger](https://risasre22.github.io/Risas-All-In-One-Menu/integration-ledger/) for compatibility. A supported mod can change its menu or hotkey implementation without notice.

Do not change a managed mod's native hotkey from inside that mod while it is being managed. Exclude it first, apply the exclusion, restart Skyrim if requested, and then change its native setting.

## Exclusions

The **Exclusions** tab stops this menu from managing selected mods and releases their native controls. When newly excluding a mod, choose one of these restore modes:

- **Restore User Original Settings** uses values captured before this menu first managed the mod.
- **Restore Original Mod Defaults** ignores captured values and applies the supported mod's original defaults.

Exclusions keep the shared captured-settings backup because other integrations may still need it. API and keyless integrations are released live when no configuration file needs restoring.

## Uninstallation

Before removing the mod:

1. Open **Settings > Maintenance**.
2. Select **Restore User Original Settings** to restore captured pre-install values, or **Restore Original Mod Defaults** to apply the supported mods' original defaults.
3. Confirm **Yes, restore for uninstall**.
4. Exit Skyrim when prompted.
5. Disable or remove Risa's All In One Menu before launching Skyrim again.

The user-original backup records settings that existed before this menu first managed them. It does not record hotkeys changed later from another mod's settings window or hotkeys assigned only inside this launcher.

After a successful full restore, `SKSE\Plugins\RisaAllInOneMenu_OriginalHotkeys.json` is archived as `SKSE\Plugins\RisaAllInOneMenu_OriginalHotkeys_Completed.json`. The active filename is cleared so a future installation captures fresh settings. If restoration or archiving is incomplete, the active backup is kept for retrying.

Removing the plugin without restoring first can leave supported mods assigned to managed internal keys. See [Files & Hotkeys Edited](https://risasre22.github.io/Risas-All-In-One-Menu/files-and-hotkeys/) for the exact files and settings involved.

## Translations

Translation files belong in:

`SKSE\Plugins\RisaAllInOneMenu\Translations\`

Select an installed translation from the language dropdown. Changes apply immediately. English (`en.json`) is included in the release archive.

## Building

Requirements:

- Visual Studio 2022 with Desktop development with C++
- CMake 3.21 or newer
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

Set `VCPKG_ROOT` to your vcpkg installation, then configure and build from the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md

cmake --build build --config Release
```

The default output is `build/Release/1RisaAllInOneMenu.dll`.

To deploy automatically to a local test installation, add an explicit cache value during configuration:

```powershell
-DRISA_DEPLOY_DIR="C:/Path/To/Your/Mod/SKSE/Plugins"
```

Windows users can alternatively run `build.bat` after setting `VCPKG_ROOT`.

## Reporting Issues

Enable **Settings > Maintenance > Enable logging**, reproduce the problem, and include:

- `RisaAllInOneMenu.log` from `Documents\My Games\Skyrim Special Edition\SKSE\`
- `RisaAllInOneMenu_FileChanges.log` when the issue involves hotkey management or restoration
- Skyrim runtime, SKSE version, and this mod's version
- The affected menu mod and its version
- The exact launcher button and key sequence used
- Whether the mod's own native hotkey still works

Do not delete `RisaAllInOneMenu_OriginalHotkeys.json` while troubleshooting a failed restore.

## License

Original project code is available under the [MIT License](LICENSE). Third-party headers and components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
