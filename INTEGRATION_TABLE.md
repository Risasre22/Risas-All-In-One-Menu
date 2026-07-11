# Risa's All In One Menu: Mod Integration Table

## Legend

- `🟨` Gold status: perfect integration
- `✅` Update-resilient integration
- `⚠️` Compatibility update may be needed
- `▶️` Direct open/close through a non-key method
- `⌨️` Key freed live with no file edit
- `💾` Key freed through INI/JSON/config editing
- `✏️` API request pending or historically requested from the original mod author

| Status | Open/close | Key | Request | Hotkey | Mod | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **SKSE Menu Framework** | Added hotkey API. Thanks to [SkyrimThiago](https://www.nexusmods.com/skyrimspecialedition/users/12458883). |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Mod Configuration Menu (MCM)** | Opens directly to SkyUI's configuration panel. |
| ✅ |  | ⌨️ | ✏️ | F18 | SearchUI | Hotkey managed live through Papyrus; no INI edit. |
| ⚠️ |  | 💾 | ✏️ | F13 | Open Animation Replacer | Direct integration is version-dependent; primarily config-managed. |
| ⚠️ |  | 💾 | ✏️ | F16 | Immersive Equipment Displays 1.7.5b | Direct integration is version-dependent; author currently unreachable. |
| ⚠️ | ▶️ | ⌨️ |  | F4 | Debug Menu | Public API controls the menu and disables its hotkey live. |
| ⚠️ |  |  |  |  | dMenu | Not supported; use the included edit. |
| ⚠️ |  |  | ✏️ |  | dMenu NG | Not supported; use the included edit. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **dMenu NG (Risa edit)** | Supported API-enabled build. Thanks to [C0kAdam](https://www.nexusmods.com/skyrimspecialedition/users/67161596). |
| ⚠️ |  | ⌨️ |  |  | Improved Camera SE 1.1.2.4228 | Key freed at the input level; no INI edit or public menu API. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Improved Camera NG 2.0.0.1215** | Uses the `ic menu` command path. |
| 🟨 ✅ |  | ⌨️ |  |  | **ENB Editor** | Key freed at the input level; no INI edit or public menu API. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **FLICK** | Uses the author's direct API. Thanks to Fuzzlesz for adding it. |
| ⚠️ |  | 💾 | ✏️ | F18 | KreatE | No author response; config-managed integration. |
| ⚠️ |  | 💾 | ✏️ | F15/F19/F23/F24 | Community Shaders | API request sent; compatibility depends on the current build. |
| 🟨 ✅ | ▶️ | ⌨️ |  |  | **Dragonborn's Toolkit** | Uses the public Toolkit API for open/close and live hotkey control. Thanks to [Modslab](https://www.nexusmods.com/skyrimspecialedition/users/260641375). |
| ⚠️ | ▶️ | 💾 | ✏️ | F21 | CatMenu 2.0.1 | Direct integration is version-dependent; key requires config management. |
| ⚠️ | ▶️ | 💾 |  |  | ReShade 6.7.3+ | Add-on API opens the overlay; the overlay key is disabled through ReShade.ini. |
| ⚠️ |  |  | ✏️ |  | Skyrim Party Sheet | Blocked by DirectInput/raw input; author may add an API. |
| ⚠️ | ▶️ | ⌨️ | ✏️ |  | Quick Armor Rebalance | Opens through Papyrus; close API is still pending. |
|  | ▶️ |  | ✏️ |  | Modex | Not yet added; API discussion started. |
|  | ▶️ |  | ✏️ |  | Hotkey Reminder | Not yet added; awaiting response. |
|  |  |  | ✏️ |  | Armor & Cloth / Weapon Transmog | Not yet added; response pending. |
| ⚠️ |  | 💾 | ✏️ | F20 | Mod Function Menu | Reads engine input; key parked through TOML edit and opened by key injection. |
| ⚠️ | ▶️ | ⌨️ | ✏️ | Numpad 1 | Outfit Preview Selector | Uses the mod event for opening and manages its hotkey live through Papyrus; compatibility may change with updates. |

This table is maintained separately from the Nexus description so the full technical details can be updated without repeatedly editing the mod article.
