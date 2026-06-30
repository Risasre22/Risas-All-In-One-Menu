# Risa's All In One Menu

Risa's All In One Menu is an SKSE plugin that provides one launcher for commonly used mod menus. It blocks supported menu hotkeys by default, freeing those keys for other gameplay mods, while still allowing every menu to be opened from the launcher.

Press **F1** to open the launcher. Press **F1** again or **Escape** to close it.

## Supported Menus

- SKSE Menu Framework
- Open Animation Replacer
- Immersive Equipment Displays
- Debug Menu
- dMenu
- Improved Camera SE
- ENB Editor
- Community Shaders
- ReShade
- FLICK
- KreatE
- CatMenu
- Dragonborn's Toolkit

Only installed integrations are shown.

## Features

- One launcher for every supported menu, opened from a single hotkey
- Configurable launcher hotkey with optional modifiers (Ctrl/Shift/Alt), double-tap, and an "Easy Close" mode (single key press closes an open menu)
- Rebindable hotkeys per mod: open Settings, click any mod's key, and press a new one. Your choices are saved and carry over through updates and new games
- Per-mod toggle to allow a mod's original hotkey, or leave it freed for other gameplay mods
- Conflict warning that flags when a mod's key matches the launcher key or another enabled mod, and auto-disables a mod set to the launcher key
- Settings organized into collapsible, categorized sections (Menus & Tools, Animation & Gear, Visual and Lighting)
- Drag-and-drop launcher button ordering
- Scalable UI with saved size and position
- Direct FLICK integration through its public API
- Runtime hotkey interception scoped to supported mods where possible
- Maintenance section with a "Restore Hotkeys for Uninstall" button and an optional logging toggle (off by default)

## Transparency and Safety

This project was developed with AI assistance and tested iteratively in Skyrim. Its source is published so users and mod authors can inspect exactly what it does.

The plugin:

- Contains no networking or telemetry code
- Logging is off by default; when enabled it stays local and excludes usernames, full local paths, save names, and unrelated mod lists
- Does not download or execute external files
- Does not contain Papyrus scripts
- Writes its own settings file
- Reads supported mods' hotkey configuration files, and writes to them to relocate each mod's hotkey to an unused key so the original key is freed for other mods. Every such change is reversible from Settings → Maintenance → Restore Hotkeys for Uninstall
- Uses MinHook for targeted input interception inside the Skyrim process

The `1` prefix in `1RisaAllInOneMenu.dll` is intentional. It allows the startup collision adjustment to run before Debug Menu reads its hotkey. Do not rename the DLL.

## Requirements

- Skyrim Script Extender (SKSE64)
- Address Library for SKSE Plugins
- SKSE Menu Framework

The supported menu mods themselves are optional.

## Installing, Updating, and Uninstalling

- **After installing or updating:** launch and exit the game once before playing so the plugin can apply its hotkey relocations. Some supported mods read their configuration before this plugin loads, so the changes take effect on the next launch.
- **Before uninstalling:** open the launcher, go to **Settings → Maintenance → Restore Hotkeys for Uninstall**, then restart once. This reverts every supported mod's hotkey to its original value. If you remove the plugin without doing this, those mods keep their relocated keys.

## Building

Requirements:

- Visual Studio 2022 with Desktop development with C++
- CMake 3.21 or newer
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

Set the `VCPKG_ROOT` environment variable to your vcpkg installation, then run:

```powershell
cmake -S SKSE_Plugin -B SKSE_Plugin/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build SKSE_Plugin/build --config Release
```

The DLL is written to `SKSE/Plugins/1RisaAllInOneMenu.dll`.

Windows users can alternatively run `SKSE_Plugin/build.bat` after setting `VCPKG_ROOT`.

## Reporting Issues

Logging is off by default. To capture a log, enable **Settings → Maintenance → Enable logging**, reproduce the issue, then include:

- `RisaAllInOneMenu.log` (in `Documents/My Games/Skyrim Special Edition/SKSE/`)
- Skyrim runtime version
- SKSE version
- Affected menu mod and version
- Exact key presses or button sequence used to reproduce the issue

## License

Original project code is available under the [MIT License](LICENSE). Third-party components and API headers retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
