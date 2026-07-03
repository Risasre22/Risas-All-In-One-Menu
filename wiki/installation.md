# Installation

## Requirements

- **Skyrim SE / AE** with **SKSE64**
- **SKSE Menu Framework** (the launcher renders through it)
- Any of the [supported mods](supported-mods.md) you want to control (all optional)

## Installing

Install with a FOMOD-aware mod manager (Mod Organizer 2, Vortex). The installer has one step:

1. **Main plugin** — always installed.
2. **Optional: dMenu NG (API build)** — an add-on `dmenu.dll` that adds live hotkey control to dMenu NG, so the launcher can free the **Home** key with no ini edit and no restart. Offered only if it's useful to you; the original dMenu, dMenu NG, and the included build are all supported.

!!! info "The optional dMenu build is not required"
    If you don't use dMenu, skip the optional add-on. If you do, it's auto-checked when dMenu is detected. Big thanks to **C0kAdam** for making that collaboration possible.

## First launch

!!! warning "Some mods only create their config after running once"
    A number of supported mods generate their configuration files **only after they've been launched once**. When you install a newly supported menu mod:

    1. Launch Skyrim once so that mod's config is created.
    2. Launch Skyrim **with Risa's All In One Menu enabled**, reach the main menu, exit, and relaunch.

    Integrations that use an official API usually work immediately without this extra restart.

## Load order

Risa's All In One Menu doesn't replace any menus and has no hard conflicts with the mods it manages. Standard SKSE plugin placement is fine. If you use the optional dMenu build, let it win over the original dMenu / dMenu NG plugin.

## Availability of some buttons

!!! note "Main-menu vs in-game"
    Blacked-out buttons — such as **Debug Menu** and **ENB** — are unavailable from Skyrim's main menu and become available after loading a save.
