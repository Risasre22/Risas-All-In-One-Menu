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
- FLICK
- KreatE
- Community Shaders

Only installed integrations are shown.

## Features

- Configurable launcher hotkey
- Per-mod controls for restoring original hotkeys
- Drag-and-drop button ordering
- Scalable UI with saved position
- Direct FLICK integration through its public API
- Runtime hotkey interception scoped to supported mods where possible
- Collision management for Debug Menu and dMenu
- Detailed startup diagnostics in `RisaAllInOneMenu.log`
- Logs Skyrim and SKSE versions, detected integrations, resolved hotkeys, configuration results, and hook status

## Transparency and Safety

This project was developed with AI assistance and tested iteratively in Skyrim. Its source is published so users and mod authors can inspect exactly what it does.

The plugin:

- Contains no networking or telemetry code
- Keeps diagnostics local and excludes usernames, full local paths, save names, and unrelated mod lists
- Does not download or execute external files
- Does not contain Papyrus scripts
- Reads supported mods' hotkey configuration files
- Writes its own settings file
- May reassign Debug Menu or dMenu configuration keys when they conflict with the launcher or another managed menu
- Uses MinHook for targeted input interception inside the Skyrim process

The `1` prefix in `1RisaAllInOneMenu.dll` is intentional. It allows the startup collision adjustment to run before Debug Menu reads its hotkey. Do not rename the DLL.

## Requirements

- Skyrim Script Extender (SKSE64)
- Address Library for SKSE Plugins
- SKSE Menu Framework

The supported menu mods themselves are optional.

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

Include the following with bug reports:

- `RisaAllInOneMenu.log`
- Skyrim runtime version
- SKSE version
- Affected menu mod and version
- Exact key presses or button sequence used to reproduce the issue

## License

Original project code is available under the [MIT License](LICENSE). Third-party components and API headers retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
