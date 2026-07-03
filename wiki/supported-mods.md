# Supported Mods

The launcher opens each mod's **own** menu — it never replaces them. Where a mod offers an official API, that's used for reliable open/close/state tracking; otherwise the launcher uses a verified direct-open path or a single targeted config edit. Unrelated settings in a mod's config are preserved.

## Supported menus

| Mod | How it's integrated | Notes |
| --- | --- | --- |
| [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/104093) | API (keyless) | Required — the launcher renders through it. |
| [Open Animation Replacer](https://www.nexusmods.com/skyrimspecialedition/mods/92109) | Direct open | Opens through its own UI manager; native key parked. |
| [Immersive Equipment Displays](https://www.nexusmods.com/skyrimspecialedition/mods/62001) | Direct open | Opens via its native render task; **Backspace freed**. |
| [Debug Menu](https://www.nexusmods.com/skyrimspecialedition/mods/136456) | Managed key | Driven through a managed hotkey. |
| [dMenu](https://www.nexusmods.com/skyrimspecialedition/mods/97221) / [dMenu NG](https://www.nexusmods.com/skyrimspecialedition/mods/166751) | API (keyless) | With the optional NG API build, **Home is freed live** — no restart. |
| [Improved Camera SE](https://www.nexusmods.com/skyrimspecialedition/mods/93962) / NG | Command | Opens via `ic menu`; **Shift+Home freed** for others. |
| ENB Editor | Managed chord | Native Shift+Enter chord, caller-scoped. |
| [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) | API (keyless) | Native key disabled via its official API. |
| [KreatE](https://www.nexusmods.com/skyrimspecialedition/mods/83757) | Relocated key | Listener moved off its default key. |
| [Community Shaders](https://www.nexusmods.com/skyrimspecialedition/mods/86492) | Managed keys | Has its own **sub-menu** (main / editor / overlay / effect). |
| [Dragonborn's Toolkit](https://www.nexusmods.com/skyrimspecialedition/mods/181426) | API (keyless) | Official open/close/is-open interface. |
| [CatMenu](https://www.nexusmods.com/skyrimspecialedition/mods/65958) | Direct open | Native listener intercepted. |
| [ReShade](https://reshade.me/) | Add-on API (keyless) | Overlay driven through the ReShade add-on API. |
| [Skyrim Party Sheet](https://www.nexusmods.com/skyrimspecialedition/mods/167538) | Input sink (keyless) | Has its own **sub-menu**; no hidden keys reserved. |

!!! tip "Sub-menus"
    **Community Shaders** and **Skyrim Party Sheet** each open a dedicated sub-menu inside the launcher, so their multiple actions are all reachable from one place.

## Original hotkeys

Supported menu hotkeys are managed by the launcher so they don't open their menus accidentally. You can turn any individual original hotkey back on from the **Settings** tab.

When an original hotkey is enabled, it can open and close its menu even if the mod's internal key has been reassigned by the launcher.

!!! tip "See a mod's original + managed key"
    Hover over a hotkey setting in the **Settings** tab to see the mod's original hotkey and the internal key currently assigned by Risa's Menu. For the full technical ledger of which keys are reserved, see [Hotkey Allocation](hotkey-allocation.md).

## Compatibility

This mod does not replace any supported menus — it opens the original menus and manages their hotkey behavior. Official APIs are used where available; other integrations use direct menu control or configuration-based hotkey management, and unrelated settings in supported config files are left untouched.
