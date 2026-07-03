# Hotkey Allocation

To free your everyday keys, the launcher relocates some mods' listeners onto the **unpressable F13–F24** keys and opens those mods itself. This page is the ledger of which of those keys are in use — useful for advanced users and for other mod authors who want to avoid clashes.

!!! note "You don't need this to use the mod"
    Everyday users can ignore this page — F1 opens the launcher and that's all you need. This is reference material for troubleshooting and integration.

## F13–F24 key ledger

| Key | Status | Owner / reason |
| --- | --- | --- |
| F13 | Reserved | Open Animation Replacer's native listener is parked here. |
| F14 | Fallback | Stock dMenu (no v2 API) is parked here. With the dMenu NG v2 build the key is disabled live and **Home stays free** — nothing parked. |
| F15 | Reserved | Community Shaders **Editor** listener. |
| F16 | Reserved | Immersive Equipment Displays parked here; **Backspace freed**. |
| F17 | Free | No current owner. |
| F18 | Reserved | KreatE relocated listener. |
| F19 | Reserved | Community Shaders **main menu** listener. |
| F20 | Free | No current owner. |
| F21 | Free | CatMenu listener is intercepted; its saved value can't react. |
| F22 | Fallback | ReShade compatibility path (non-add-on only). |
| F23 | Reserved | Community Shaders **Overlay** listener. |
| F24 | Reserved | Community Shaders **Effect** listener. |

**Immediately reusable:** F17, F20, F21
**Conditionally free:** F14 (whenever dMenu NG v2 is in use), F22 (if ReShade's non-add-on fallback isn't needed)

## Keys freed for your use

Because the launcher takes over these menus, the mods' **default** keys are freed back to you:

- **Home** — freed (dMenu, when the NG v2 build is used)
- **Backspace** — freed (Immersive Equipment Displays)
- **Shift+Home** — freed (Improved Camera; blocked only from IC, so still usable elsewhere)

## Keys still driven by a hotkey

A few integrations still work through a real (relocated or native) key rather than an API:

| Mod | Key(s) | Notes |
| --- | --- | --- |
| dMenu (stock only) | F14 | Used only when the dMenu NG API is absent. |
| KreatE | F18 | Relocated window/ImGui input listener. |
| Community Shaders | F15, F19, F23, F24 | Four separate menu actions. |
| Debug Menu | F3 | Managed key outside the F13–F24 pool. |
| ENB Editor | Shift+Enter | Native chord, caller-scoped. |
| ReShade (non-add-on) | F22 | Compatibility fallback only. |

## Escape behavior

**F1** opens and closes the launcher; **Escape** closes the active mod menu. While a menu that would otherwise let Skyrim's journal/pause pop is open (Debug Menu, ENB, KreatE, Dragonborn's Toolkit, CatMenu), Escape is intercepted so only that menu closes — the vanilla menu won't appear.

!!! info "Why modifier keys don't multiply the pool"
    Ctrl / Shift / Alt don't automatically create more free keys — some mods ignore modifiers and react to the base key. A chord is only reusable once that listener has been tested or isolated.
