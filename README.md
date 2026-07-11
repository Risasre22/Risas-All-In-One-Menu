# Risa's All In One Menu

Risa's All In One Menu is an SKSE plugin that places supported mod menus behind one configurable launcher. It manages their native hotkeys so those keys can be reused for gameplay, while each original menu remains available through the launcher or an optional user-defined shortcut.

Press **F1** by default to open or close the launcher. **Escape** also closes it.

## Supported Menus

- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
- Mod Configuration Menu (MCM)
- [SearchUI (Get Any Item Instantly)](https://www.nexusmods.com/skyrimspecialedition/mods/101290)
- [Open Animation Replacer](https://www.nexusmods.com/skyrimspecialedition/mods/92109)
- [Immersive Equipment Displays](https://www.nexusmods.com/skyrimspecialedition/mods/62001)
- [Debug Menu](https://www.nexusmods.com/skyrimspecialedition/mods/136456)
- [dMenu and dMenu NG](https://www.nexusmods.com/skyrimspecialedition/mods/166751)
- [Improved Camera SE](https://www.nexusmods.com/skyrimspecialedition/mods/93962) / [Improved Camera NG](https://discord.gg/Hr7pRchWcf) *(NG is a Discord link, you can find the file in the updates channel)*
- [ENB Editor](http://enbdev.com/download_mod_tesskyrimse.html)
- [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603)
- [KreatE](https://www.nexusmods.com/skyrimspecialedition/mods/83757)
- [Community Shaders](https://www.nexusmods.com/skyrimspecialedition/mods/86492)
- [Skyrim Party Sheet - Player and Follower HUD](https://www.nexusmods.com/skyrimspecialedition/mods/167538)
- [Dragonborn's Toolkit](https://www.nexusmods.com/skyrimspecialedition/mods/181426)
- [CatMenu](https://www.nexusmods.com/skyrimspecialedition/mods/65958)
- [ReShade](https://reshade.me/)
- [Quick Armor Rebalance](https://www.nexusmods.com/skyrimspecialedition/mods/127967)

Only integrations detected in the current installation are displayed.

## Integration Table

**Legend:** 🟨 Gold status = perfect integration | ✅ Update-resilient integration | ⚠️ Compatibility update may be needed | ▶️ Direct open/close through a non-key method | ⌨️ Key freed live with no file edit | 💾 Key freed through INI/JSON/config editing | ✏️ API request pending or historically requested from the original mod author

| Status | Open/close | Key | Request | Hotkey | Mod | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **SKSE Menu Framework** | Added hotkey API. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Mod Configuration Menu (MCM)** | Opens directly to SkyUI's configuration panel. |
| ✅ |  | ⌨️ | ✏️ | F18 | SearchUI | Hotkey managed live through Papyrus; no INI edit. |
| ⚠️ |  | 💾 | ✏️ | F13 | Open Animation Replacer | Direct integration is version-dependent; primarily config-managed. |
| ⚠️ |  | 💾 | ✏️ | F16 | Immersive Equipment Displays 1.7.5b | Direct integration is version-dependent. |
| ⚠️ | ▶️ | ⌨️ |  | F4 | Debug Menu | Public API controls the menu and disables its hotkey live. |
| ⚠️ |  |  |  |  | dMenu | Not supported; use the included edit. |
| ⚠️ |  |  | ✏️ |  | dMenu NG | Not supported; use the included edit. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **dMenu NG (Risa edit)** | Supported API-enabled build. |
| ⚠️ |  | ⌨️ |  |  | Improved Camera SE 1.1.2.4228 | Key freed at the input level; no INI edit or public menu API. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Improved Camera NG 2.0.0.1215** | Uses the `ic menu` command path. |
| 🟨 ✅ |  | ⌨️ |  |  | **ENB Editor** | Key freed at the input level; no INI edit or public menu API. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **FLICK** | Uses the author's direct API. |
| ⚠️ |  | 💾 | ✏️ | F18 | KreatE | No author response; config-managed integration. |
| ⚠️ |  | 💾 | ✏️ | F15/F19/F23/F24 | Community Shaders | API request sent; compatibility depends on the current build. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Dragonborn's Toolkit** | Uses the public Toolkit API for open/close and live hotkey control. |
| ⚠️ | ▶️ | 💾 | ✏️ | F21 | CatMenu 2.0.1 | Direct integration is version-dependent. |
| ⚠️ | ▶️ | 💾 |  |  | ReShade 6.7.3+ | Add-on API opens the overlay; the overlay key is disabled through ReShade.ini. |
| ⚠️ |  |  | ✏️ |  | Skyrim Party Sheet | Blocked by DirectInput/raw input; author may add an API. |
| ⚠️ | ▶️ | ⌨️ | ✏️ |  | Quick Armor Rebalance | Opens through Papyrus; close API is still pending. |
|  | ▶️ |  | ✏️ |  | Modex | Not yet added; API discussion started. |
|  | ▶️ |  | ✏️ |  | Hotkey Reminder | Not yet added; awaiting response. |
|  |  |  | ✏️ |  | Armor & Cloth / Weapon Transmog | Not yet added; response pending. |
| ⚠️ |  | 💾 | ✏️ | F20 | Mod Function Menu | Key parked through TOML edit and opened by key injection. |
| ⚠️ | ▶️ | ⌨️ | ✏️ | Numpad 1 | Outfit Preview Selector | Opens through the mod event and manages its hotkey live through Papyrus. |

## Features

- One launcher for supported SKSE and overlay menus
- Configurable launcher key with Ctrl, Shift, Alt, double-tap, and Easy Close options
- Rebindable per-menu shortcuts saved across updates and new games
- Optional access to each menu's original shortcut
- Duplicate-key and launcher-key conflict detection
- Dedicated Community Shaders and Skyrim Party Sheet submenus
- Drag-and-drop button ordering, adjustable UI scale, and saved window placement
- Official APIs where available, with guarded compatibility integrations elsewhere
- Local diagnostic logging, disabled by default
- Restoration of captured original settings or supported defaults before uninstalling

## Requirements

- Skyrim Script Extender (SKSE64)
- Address Library for SKSE Plugins
- SKSE Menu Framework

The supported menu mods themselves are optional.

## Installation

Install the release archive with a mod manager. Some supported mods create their configuration files only after they have run once.

When Risa changes a managed configuration file, a translucent ten-second notification appears at startup. Restart Skyrim once before playing so every affected mod reads its updated setting.

## Updating

Replace the previous version normally. User settings are stored separately from the DLL and are retained across updates.

## Uninstallation

Before removing the mod:

1. Open **Settings > Maintenance**.
2. Select **Restore My Original Settings** to restore captured user values, or **Restore Supported Mod Defaults** to restore standard defaults.
3. Exit Skyrim.
4. Disable or remove Risa's All In One Menu before launching Skyrim again.

Removing the plugin without restoring first can leave supported mods assigned to Risa's managed internal keys.

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

- `RisaAllInOneMenu.log` from `Documents/My Games/Skyrim Special Edition/SKSE/`
- Skyrim runtime and SKSE versions
- The affected menu mod and its version
- The exact button and key sequence used

## License

Original project code is available under the [MIT License](LICENSE). Third-party headers and components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
