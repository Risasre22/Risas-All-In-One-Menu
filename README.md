# Risa's All In One Menu

Risa's All In One Menu is an SKSE plugin that places supported mod menus behind one configurable launcher. It manages their native hotkeys so those keys can be reused for gameplay, while each original menu remains available through the launcher or an optional user-defined shortcut.

Press **F1** by default to open or close the launcher. **Escape** also closes it.

## Supported Menus

- SKSE Menu Framework
- Open Animation Replacer
- Immersive Equipment Displays
- Debug Menu
- dMenu and dMenu NG
- Improved Camera SE
- ENB Editor
- FLICK
- KreatE
- Community Shaders
- Skyrim Party Sheet
- Dragonborn's Toolkit
- CatMenu
- ReShade

Only integrations detected in the current installation are displayed.

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
