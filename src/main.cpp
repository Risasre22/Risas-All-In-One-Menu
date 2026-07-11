// ============================================================================
// Risa's All In One Menu — SKSE Plugin
//
// HOW THE BLOCK WORKS
//   We hook keyboard input at TWO levels simultaneously:
//
//   1. IDirectInputDevice8A::GetDeviceState (vtable slot 9)
//      Skyrim's BSWin32KeyboardDevice calls this each frame to poll the full
//      256-byte key state.  We zero state[toggleDIK] before returning, so
//      the engine generates no ButtonEvent for MF's key.
//
//   2. GetAsyncKeyState (User32.dll)
//      Belt-and-suspenders: if MF polls key state via WinAPI instead of DI8
//      we return 0 for MF's virtual key.
//
//   F1 → open/close our launcher (BSTEventSink + render check).
//   ESC → close launcher (BSTEventSink + render check).
//   Launcher button → opens MF's exported main window directly.
// ============================================================================

// CommonLib MUST come before any Windows API headers (REX/W32 enforces this)
#include <spdlog/sinks/basic_file_sink.h>
#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include "SKSEMenuFramework.hpp"

// Windows / DirectInput / MinHook — AFTER CommonLib
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <MinHook.h>
#include <intrin.h>

// ReShade add-on bridge (implemented in reshade_addon.cpp; the ReShade SDK headers are kept out
// of this file). Lets the launcher drive ReShade's overlay through its own API instead of a key.
#include "reshade_bridge.h"


#include <fstream>
#include <string>
#include <atomic>
#include <algorithm>
#include <format>
#include <filesystem>
#include <chrono>
#include <array>
#include <thread>
#include <vector>
#include <sstream>
#include <set>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <cstring>
#include <initializer_list>
#include "dmenu_api.h"
#include "DragonbornsToolkitAPI.h"

// ============================================================================
// dMenu NG external-control API (optional).
// dMenu NG (github c0kadam/dmenu-NG) broadcasts an SKSE message (0x444D4946 "DMIF", sender
// "dmenu") at kPostLoad carrying a dmenu_api::Interface, and also exports dMenu_GetInterface().
// When present we open/close/query dMenu directly through it -- no key simulation, real menu
// state -- and fall back to the F14 relocation + engine-injection path for stock dMenu.
// ============================================================================
static std::atomic<const dmenu_api::Interface*> g_DMenuApi{ nullptr };

static bool HasDMenuApi() {
    const auto* api = g_DMenuApi.load();
    return api && api->IsMenuOpen && api->OpenMenu && api->CloseMenu;
}

// v2 adds live control of dMenu's keyboard toggle key. Gate on interfaceVersion FIRST (short-circuit)
// and structSize so we never read the v2 members off a smaller v1 interface.
static bool HasDMenuV2Api() {
    const auto* api = g_DMenuApi.load();
    return api && api->interfaceVersion >= 2 &&
           api->structSize >= sizeof(dmenu_api::Interface) &&
           api->GetToggleKeyMkb && api->SetToggleKeyMkb;
}

// Grab the interface straight from dmenu.dll's export, in case the SKSE broadcast was missed.
static void AcquireDMenuApiFromExport() {
    if (g_DMenuApi.load()) return;
    HMODULE h = ::GetModuleHandleA("dmenu.dll");
    if (!h) return;
    auto fn = reinterpret_cast<dmenu_api::GetInterfaceFn>(::GetProcAddress(h, "dMenu_GetInterface"));
    if (!fn) return;
    // Request version 0 ("give me whatever you have"): a stock/older dMenu rejects a request for a
    // version higher than its own, so asking for our kInterfaceVersion would return null on v1 dMenu.
    if (const auto* iface = fn(0); iface && iface->interfaceVersion >= 1) {
        g_DMenuApi.store(iface);
        SKSE::log::info("dMenu API acquired via dmenu.dll export (interface v{}).", iface->interfaceVersion);
    }
}

// SKSE message listener for sender "dmenu": stores the broadcast API interface.
static void DMenuApiMessageHandler(SKSE::MessagingInterface::Message* msg) {
    if (!msg || msg->type != dmenu_api::kMessageInterface || !msg->data) return;
    const auto* iface = reinterpret_cast<const dmenu_api::Interface*>(msg->data);
    if (iface->interfaceVersion < 1) return;
    g_DMenuApi.store(iface);
    SKSE::log::info("dMenu API received via SKSE message (interface v{}).", iface->interfaceVersion);
}

static constexpr std::string_view kRisaMenuVersion = "1.5.5";
static std::string g_RuntimeVersion = "Unknown";
static std::string g_SKSEVersion = "Unknown";
static std::string g_RuntimeEdition = "Unknown";

// ============================================================================
// Logging switches
// ============================================================================
// Master logging switch. On by default; the Settings toggle turns it off.
static std::atomic<bool> g_LoggingEnabled{ true };
// Dedicated audit log: records ONLY the config files this mod edits or reverts (for transparency).
static std::atomic<bool> g_FileChangeLogEnabled{ true };
static std::atomic<bool> g_InRestore{ false }; // true while RestoreAllModDefaults runs (EDIT vs REVERT)
// Set once a full Restore has run: Risa stops re-relocating keys for the rest of the session, so restored
// values (e.g. Debug Menu back on its original F1) stay put instead of being managed away again. Cleared
// on next launch (fresh session re-manages normally).
static std::atomic<bool> g_ManagementSuspended{ false };
static std::shared_ptr<spdlog::logger> g_FileChangeLog;
static void ApplyLogLevel() {
    const auto lvl = g_LoggingEnabled.load() ? spdlog::level::info : spdlog::level::off;
    spdlog::set_level(lvl);
    spdlog::flush_on(lvl);
}
// Append one line to the file-change audit log naming exactly which config file was touched.
static void LogFileChange(const std::string& path, const std::string& detail) {
    if (!g_FileChangeLogEnabled.load() || !g_FileChangeLog) return;
    g_FileChangeLog->info("{:<7} {}{}", g_InRestore.load() ? "REVERT" : "EDIT", path,
        detail.empty() ? std::string() : ("  |  " + detail));
    g_FileChangeLog->flush();
}

// ============================================================================
// DIK (hardware scan code) + VK (virtual key) lookup table
// ============================================================================
struct KeyEntry { const char* name; WORD dik; WORD vk; };
static constexpr KeyEntry kKeyTable[] = {
    { "F1",  0x3B, VK_F1  }, { "F2",  0x3C, VK_F2  }, { "F3",  0x3D, VK_F3  },
    { "F4",  0x3E, VK_F4  }, { "F5",  0x3F, VK_F5  }, { "F6",  0x40, VK_F6  },
    { "F7",  0x41, VK_F7  }, { "F8",  0x42, VK_F8  }, { "F9",  0x43, VK_F9  },
    { "F10", 0x44, VK_F10 }, { "F11", 0x57, VK_F11 }, { "F12", 0x58, VK_F12 },
    // F13-F15: valid scan/VK codes with no physical key on a standard keyboard — used as
    // "unpressable" launcher-only hotkeys for the ImGui mods (OAR/dMenu/IC).
    { "F13", 0x64, VK_F13 }, { "F14", 0x65, VK_F14 }, { "F15", 0x66, VK_F15 },
    { "F16", 0x67, VK_F16 }, { "F17", 0x68, VK_F17 }, { "F18", 0x69, VK_F18 }, { "F19", 0x6A, VK_F19 },
    { "F20", 0x6B, VK_F20 }, { "F21", 0x6C, VK_F21 }, { "F22", 0x6D, VK_F22 }, { "F23", 0x6E, VK_F23 },
    { "F24", 0x76, VK_F24 },
    { "INSERT",    0xD2, VK_INSERT   }, { "DELETE",    0xD3, VK_DELETE   },
    { "HOME",      0xC7, VK_HOME     }, { "END",       0xCF, VK_END      },
    { "PAGEUP",    0xC9, VK_PRIOR    }, { "PAGEDOWN",  0xD1, VK_NEXT     },
    { "BACKSPACE", 0x0E, VK_BACK     }, { "TAB",       0x0F, VK_TAB      },
    { "CAPSLOCK",  0x3A, VK_CAPITAL  },
    { "A", 0x1E, 'A' }, { "B", 0x30, 'B' }, { "C", 0x2E, 'C' }, { "D", 0x20, 'D' },
    { "E", 0x12, 'E' }, { "F", 0x21, 'F' }, { "G", 0x22, 'G' }, { "H", 0x23, 'H' },
    { "I", 0x17, 'I' }, { "J", 0x24, 'J' }, { "K", 0x25, 'K' }, { "L", 0x26, 'L' },
    { "M", 0x32, 'M' }, { "N", 0x31, 'N' }, { "O", 0x18, 'O' }, { "P", 0x19, 'P' },
    { "Q", 0x10, 'Q' }, { "R", 0x13, 'R' }, { "S", 0x1F, 'S' }, { "T", 0x14, 'T' },
    { "U", 0x16, 'U' }, { "V", 0x2F, 'V' }, { "W", 0x11, 'W' }, { "X", 0x2D, 'X' },
    { "Y", 0x15, 'Y' }, { "Z", 0x2C, 'Z' },
    // Number row (top of keyboard) - named so they're distinct from the numpad below.
    { "1", 0x02, '1' }, { "2", 0x03, '2' }, { "3", 0x04, '3' }, { "4", 0x05, '4' }, { "5", 0x06, '5' },
    { "6", 0x07, '6' }, { "7", 0x08, '7' }, { "8", 0x09, '8' }, { "9", 0x0A, '9' }, { "0", 0x0B, '0' },
    // Numpad - explicit names so "Num 1" never collapses to "1" or "End" (needs Num Lock to bind).
    { "Num 0", 0x52, VK_NUMPAD0 }, { "Num 1", 0x4F, VK_NUMPAD1 }, { "Num 2", 0x50, VK_NUMPAD2 },
    { "Num 3", 0x51, VK_NUMPAD3 }, { "Num 4", 0x4B, VK_NUMPAD4 }, { "Num 5", 0x4C, VK_NUMPAD5 },
    { "Num 6", 0x4D, VK_NUMPAD6 }, { "Num 7", 0x47, VK_NUMPAD7 }, { "Num 8", 0x48, VK_NUMPAD8 },
    { "Num 9", 0x49, VK_NUMPAD9 }, { "Num *", 0x37, VK_MULTIPLY }, { "Num -", 0x4A, VK_SUBTRACT },
    { "Num +", 0x4E, VK_ADD }, { "Num .", 0x53, VK_DECIMAL }, { "Num /", 0xB5, VK_DIVIDE },
};

// ============================================================================
// MF INI config
// ============================================================================
using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
static GetAsyncKeyState_t g_OrigGetAsyncKeyState = nullptr;

static long long NowMs();
static std::atomic<long long> g_LastLauncherToggleMs{ 0 };

struct MFConfig {
    std::string toggleKeyName;
    std::string toggleMode;
    WORD toggleDIK = 0; // DirectInput / hardware scan code
    WORD toggleVK  = 0; // Windows virtual key code
};
static MFConfig g_MFConfig;

struct OARConfig {
    bool enabled = false;
    WORD toggleDIK = 0;
    WORD toggleVK = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};
static OARConfig g_OARConfig;

struct IEDConfig {
    bool enabled = false;
    std::string toggleKeysStr;
    WORD toggleDIK = 0;
    WORD toggleVK = 0;
};
static IEDConfig g_IEDConfig;

struct ENBConfig {
    bool enabled = false;
    int combinationVK = 16; // default Shift
    int editorVK = 13;      // default Enter
};
static ENBConfig g_ENBConfig;

struct DebugMenuConfig {
    bool enabled = false;
    WORD toggleDIK = 59; // F1 default
    WORD toggleVK = VK_F1;
    // Runtime API — resolved from DebugMenu.dll exports at startup
    using FnSetMenuOpen       = void(*)(bool open);
    using FnIsMenuOpen        = bool(*)();
    using FnSetHotkeysEnabled = void(*)(bool enabled);
    FnSetMenuOpen       setMenuOpen       = nullptr;
    FnIsMenuOpen        isMenuOpen        = nullptr;
    FnSetHotkeysEnabled setHotkeysEnabled = nullptr;
};
static DebugMenuConfig g_DebugMenuConfig;

struct DMenuConfig {
    bool enabled = false;
    WORD toggleDIK = 199; // Home default (0xC7)
    WORD toggleVK = VK_HOME;
    WORD modifierDIK = 0;
    WORD modifierVK = 0;
};
static DMenuConfig g_DMenuConfig;

struct ImprovedCameraConfig {
    bool enabled = false;
    WORD toggleDIK = 199; // Home default (0xC7)
    WORD toggleVK = VK_HOME; // Home default (0x24)
    // Improved Camera's MenuKey is a single key with no modifier (ImprovedCameraSE.ini
    // MenuKey=0x24 = plain Home). It shares this key with dMenu; the two are told apart
    // by input channel (IC polls GetAsyncKeyState, dMenu reads WM_KEYDOWN), not by a modifier.
    WORD modifierDIK = 0;
    WORD modifierVK = 0;
};
static ImprovedCameraConfig g_ImprovedCameraConfig;

struct FLICKConfig {
    bool enabled = false;
    WORD toggleDIK = 0x41; // Default to F7 (65 decimal = 0x41)
    WORD toggleVK = 0;
};
static FLICKConfig g_FLICKConfig;

struct KreatEConfig {
    bool enabled = false;
    WORD toggleDIK = 0xCF;
    WORD toggleVK = VK_END;
};
static KreatEConfig g_KreatEConfig;

struct CSConfig {
    bool enabled = false;
    WORD toggleDIK = 0xCF; // Community Shaders menu default = End
    WORD toggleVK = VK_END;
    WORD editorDIK = 0xCF;
    WORD editorVK = VK_END;
    WORD editorModifierDIK = 0x2A;
    WORD editorModifierVK = VK_SHIFT;
    WORD overlayDIK = 0x44;
    WORD overlayVK = VK_F10;
    WORD effectDIK = 0x37;
    WORD effectVK = VK_MULTIPLY;
};
static CSConfig g_CSConfig;

struct PartySheetConfig {
    bool enabled = false;
    WORD settingsDIK = 0x2D;   // X
    WORD partyDIK = 0x40;      // F6
    WORD inspectDIK = 0x15;    // Y
    WORD characterDIK = 0x16;  // U
    WORD modifierDIK = 0;
};
static PartySheetConfig g_PartySheetConfig;

struct CatMenuConfig {
    bool enabled = false;
    int toggleImGuiKey = 577; // ImGuiKey_F6
    WORD toggleDIK = 0x40;
    WORD toggleVK = VK_F6;
};
static CatMenuConfig g_CatMenuConfig;

struct ModFunctionMenuConfig {
    bool enabled = false;
    WORD toggleDIK = 0x3B; // F1 (Mod Function Menu's default iHotkey = 59)
    WORD toggleVK = VK_F1;
};
static ModFunctionMenuConfig g_ModFunctionMenuConfig;

struct OPSConfig {
    bool enabled = false;
    WORD toggleDIK = 0x4F; // Numpad 1 (Outfit Preview Selector's default Hotkey = 79)
    WORD toggleVK = VK_NUMPAD1;
};
static OPSConfig g_OPSConfig;

struct DragonbornConfig {
    bool enabled = false;
    std::string toggleKeyName = "F1";
    WORD toggleDIK = 0x3B;
    WORD toggleVK = VK_F1;
};
static DragonbornConfig g_DragonbornConfig;

struct ReShadeConfig {
    bool enabled = false;
    WORD toggleVK = VK_HOME;   // ReShade.ini [INPUT] KeyOverlay default = 36 (Home)
    WORD toggleDIK = 0xC7;
    WORD modifierDIK = 0;      // ctrl/shift/alt flags from KeyOverlay
};
static ReShadeConfig g_ReShadeConfig;

// Minimal prefix of FLICK's official packed C API (FUCK_API.h). Keeping only the
// fields through SetMenuOpen/IsMenuOpen/SetHotkeyEnabled avoids depending on FLICK's ImGui types.
#pragma pack(push, 1)
struct FLICKInterface {
    std::uint32_t version;
    void* functionsBeforeSetMenuOpen[12];
    void (*SetMenuOpen)(bool);
    bool (*IsMenuOpen)();
    void* functionsBeforeSetHotkeyEnabled[200];
    void (*SetHotkeyEnabled)(bool);
};
#pragma pack(pop)

enum class ActiveMenu {
    None,
    MF,
    OAR,
    IED,
    ENB,
    DebugMenu,
    DMenu,
    ImprovedCamera,
    FLICK,
    KreatE,
    CS,
    PartySheet,
    CatMenu,
    Dragonborn,
    MCM,
    ReShade,
    SearchUI,
    QAR,
    ModFunctionMenu,
    OPS
};
static std::atomic<ActiveMenu> g_ActiveMenu{ ActiveMenu::None };
static std::atomic<bool> g_OpenMCMWhenJournalReady{ false };

// Menus that keep the OS/game cursor hidden even while open — either key-toggled or drawing their
// own ImGui software cursor. For these a hidden cursor does NOT mean the menu closed, so the
// cursor-based self-heal must skip them; otherwise it wrongly resets our active-menu state and the
// launcher key desyncs their open/close (e.g. KreatE would fail to close on F1 and just re-toggle).
// Their real close is caught by the explicit ESC handler / toggle path instead.
static bool MenuIgnoresOSCursor(ActiveMenu am) {
    switch (am) {
        case ActiveMenu::KreatE:
        case ActiveMenu::FLICK:
        case ActiveMenu::ImprovedCamera:
        case ActiveMenu::PartySheet:
        case ActiveMenu::CatMenu:
        case ActiveMenu::DMenu:
        case ActiveMenu::DebugMenu:
        case ActiveMenu::ENB:
        case ActiveMenu::Dragonborn:
        case ActiveMenu::ReShade:
        case ActiveMenu::QAR:
        case ActiveMenu::ModFunctionMenu:
            return true;
        default:
            return false;
    }
}

// Launcher sub-view: which mod's "hub" of sub-buttons the Launcher tab is showing.
// 0 = none (normal button grid), 1 = Community Shaders hub, 2 = Skyrim Party Sheet hub.
static std::atomic<int> g_LauncherSubView{ 0 };
static std::atomic<bool> g_RememberSubView{ true };
static std::atomic<int> g_ActiveCSSub{ 0 };
static std::atomic<int> g_ActivePartySheetSub{ 0 };
static std::atomic<std::uint64_t> g_PartyInspectRequest{ 0 };
static std::atomic<RE::FormID> g_LastPartyInspectActor{ 0 };

static std::atomic<bool> g_ShowExclusionsTab{ true };
static std::atomic<bool> g_HideExcludedInLauncher{ false };
static std::string g_Language = "en";
static std::map<std::string, std::string> g_Translations;
static std::map<std::string, std::string> g_EnglishTranslations;



static std::string TrimStr(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
static std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
}

static std::string SanitizeLanguageCode(std::string value) {
    value = TrimStr(value);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        if (c == '_') return '-';
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty()) return "en";
    for (const char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return "en";
    }
    return value;
}

static void FlattenTranslations(const nlohmann::json& node, const std::string& prefix, std::map<std::string, std::string>& out) {
    if (!node.is_object()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string key = prefix.empty() ? it.key() : (prefix + "." + it.key());
        if (it.value().is_string()) {
            out[key] = it.value().get<std::string>();
        } else if (it.value().is_object()) {
            FlattenTranslations(it.value(), key, out);
        }
    }
}

static std::filesystem::path FindTranslationFile(const std::string& language) {
    const std::array<std::filesystem::path, 2> paths = {
        std::filesystem::path("SKSE/Plugins/RisaAllInOneMenu/Translations") / (language + ".json"),
        std::filesystem::path("Data/SKSE/Plugins/RisaAllInOneMenu/Translations") / (language + ".json")
    };

    for (const auto& candidate : paths) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

static bool TranslationFileExists(const std::string& language) {
    return !FindTranslationFile(language).empty();
}

static bool LoadTranslationFile(const std::string& language, std::map<std::string, std::string>& out) {
    out.clear();
    const std::filesystem::path path = FindTranslationFile(language);
    std::ifstream file;
    if (!path.empty()) {
        file.open(path);
    }
    if (!file.is_open()) {
        SKSE::log::warn("Translations: {}.json not found in SKSE/Plugins/RisaAllInOneMenu/Translations.", language);
        return false;
    }
    try {
        nlohmann::json json;
        file >> json;
        FlattenTranslations(json, "", out);
        SKSE::log::info("Translations: loaded {} string(s) from {}.", out.size(), path.string());
        return true;
    } catch (const std::exception& e) {
        SKSE::log::error("Translations: failed to parse {}: {}", path.string(), e.what());
        out.clear();
        return false;
    }
}

static void LogMissingTranslationKeysForCurrentLanguage() {
    if (g_Language == "en" || g_EnglishTranslations.empty()) return;

    std::vector<std::string> missingKeys;
    missingKeys.reserve(g_EnglishTranslations.size());
    for (const auto& [key, value] : g_EnglishTranslations) {
        if (g_Translations.find(key) == g_Translations.end()) {
            missingKeys.push_back(key);
        }
    }

    const std::size_t total = g_EnglishTranslations.size();
    const std::size_t loaded = g_Translations.size();
    const std::size_t missing = missingKeys.size();
    if (missing == 0) {
        SKSE::log::info("Translations: '{}' is complete ({} / {} English string keys present).",
            g_Language, loaded, total);
        return;
    }

    SKSE::log::warn("Translations: '{}' is missing {} of {} English string key(s); missing keys will fall back to English.",
        g_Language, missing, total);

    constexpr std::size_t kMaxMissingKeysToLog = 40;
    const std::size_t count = missingKeys.size() < kMaxMissingKeysToLog ? missingKeys.size() : kMaxMissingKeysToLog;
    for (std::size_t i = 0; i < count; ++i) {
        SKSE::log::warn("Translations: missing '{}' key: {}", g_Language, missingKeys[i]);
    }
    if (missingKeys.size() > kMaxMissingKeysToLog) {
        SKSE::log::warn("Translations: '{}' has {} additional missing key(s) not listed.",
            g_Language, missingKeys.size() - kMaxMissingKeysToLog);
    }
}

static void LoadTranslations() {
    g_Language = SanitizeLanguageCode(g_Language);
    const bool englishLoaded = LoadTranslationFile("en", g_EnglishTranslations);
    if (!englishLoaded) {
        SKSE::log::error("Translations: base English file failed to load; missing-key diagnostics are unavailable.");
    }
    if (g_Language == "en") {
        g_Translations.clear();
        return;
    }
    if (!LoadTranslationFile(g_Language, g_Translations)) {
        SKSE::log::warn("Translations: falling back to English because language '{}' could not be loaded.", g_Language);
        g_Translations.clear();
    } else if (englishLoaded) {
        LogMissingTranslationKeysForCurrentLanguage();
    }
}

static const char* Tr(const char* key, const char* fallback) {
    if (!key || !*key) return fallback ? fallback : "";
    if (const auto it = g_Translations.find(key); it != g_Translations.end()) return it->second.c_str();
    if (const auto it = g_EnglishTranslations.find(key); it != g_EnglishTranslations.end()) return it->second.c_str();
    return fallback ? fallback : key;
}

static std::string TrLabel(const char* key, const char* fallback, const char* suffix) {
    std::string text = Tr(key, fallback);
    if (suffix) text += suffix;
    return text;
}

struct LanguageOption {
    const char* code;
    const char* nativeName;
    const char* englishName;
};

static constexpr std::array<LanguageOption, 13> kLanguageOptions{ {
    { "en", "English", "English" },
    { "zh-cn", "简体中文", "Chinese (Simplified)" },
    { "ja", "Japanese", "Japanese" },
    { "ko", "Korean", "Korean" },
    { "cyrillic", "Cyrillic", "Cyrillic" },
    { "th", "Thai", "Thai" },
    { "tr", "Turkish", "Turkish" },
    { "fr", "French", "French" },
    { "de", "German", "German" },
    { "it", "Italian", "Italian" },
    { "pt", "Portuguese", "Portuguese" },
    { "ru", "Russian", "Russian" },
    { "es", "Spanish", "Spanish" },
} };

static std::vector<LanguageOption> GetAvailableLanguageOptions() {
    std::vector<LanguageOption> out;
    out.reserve(kLanguageOptions.size());
    for (const auto& opt : kLanguageOptions) {
        if (std::string(opt.code) == "en" || TranslationFileExists(opt.code)) {
            out.push_back(opt);
        }
    }
    if (out.empty()) {
        out.push_back(kLanguageOptions[0]);
    }
    return out;
}

static const LanguageOption& CurrentLanguageOption() {
    const std::string current = SanitizeLanguageCode(g_Language);
    for (const auto& opt : kLanguageOptions) {
        if (current == opt.code) return opt;
    }
    return kLanguageOptions[0];
}

static float LanguageFontScale(float scale) {
    // Language selection should only change strings. Font size is controlled by
    // the user's UI Scale + Font Scale settings, not by the selected language.
    return scale;
}

static bool ButtonFit(const char* label, ImGuiMCP::ImVec2 size, float currentScale, float minScale = 0.72f) {
    const char* safeLabel = label ? label : "";
    std::string visibleLabel = safeLabel;
    if (const size_t idPos = visibleLabel.find("##"); idPos != std::string::npos) {
        visibleLabel.resize(idPos);
    }
    float availableW = size.x;
    if (availableW < 0.0f) {
        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);
        availableW = avail.x;
    }
    if (availableW <= 0.0f) {
        return ImGuiMCP::Button(safeLabel, size);
    }

    ImGuiMCP::ImVec2 textSize;
    ImGuiMCP::CalcTextSize(&textSize, safeLabel, nullptr, true, -1.0f);
    const float innerW = availableW - (18.0f * currentScale);
    float fittedScale = currentScale;
    if (innerW > 12.0f && textSize.x > innerW) {
        const float ratio = innerW / textSize.x;
        const float floorScale = currentScale * minScale;
        fittedScale = currentScale * ratio;
        if (fittedScale < floorScale) fittedScale = floorScale;
    }

    if (fittedScale < currentScale - 0.001f) {
        ImGuiMCP::SetWindowFontScale(fittedScale);
    }
    const bool pressed = ImGuiMCP::Button(safeLabel, size);
    if (fittedScale < currentScale - 0.001f) {
        ImGuiMCP::SetWindowFontScale(currentScale);
        if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGuiMCP::SetItemTooltip("%s", visibleLabel.c_str());
        }
    }
    return pressed;
}

static bool IconButtonCentered(const char* id, const std::string& icon, ImGuiMCP::ImVec2 size,
    float yNudge = 0.0f, float xNudge = 0.0f) {
    const bool pressed = ImGuiMCP::Button(id, size);

    ImGuiMCP::ImVec2 itemMin;
    ImGuiMCP::ImVec2 itemMax;
    ImGuiMCP::GetItemRectMin(&itemMin);
    ImGuiMCP::GetItemRectMax(&itemMax);

    ImGuiMCP::ImVec2 iconSize;
    ImGuiMCP::CalcTextSize(&iconSize, icon.c_str(), nullptr, false, -1.0f);
    const float x = itemMin.x + ((itemMax.x - itemMin.x) - iconSize.x) * 0.5f + xNudge;
    const float y = itemMin.y + ((itemMax.y - itemMin.y) - iconSize.y) * 0.5f + yNudge;

    auto* drawList = ImGuiMCP::GetWindowDrawList();
    auto* font = ImGuiMCP::GetFont();
    const float fontSize = ImGuiMCP::GetFontSize();
    ImGuiMCP::ImDrawListManager::AddText(drawList, font, fontSize,
        ImGuiMCP::ImVec2(x, y), IM_COL32(245, 245, 245, 255), icon.c_str());

    return pressed;
}

static bool IsPluginPresent(const char* moduleName) {
    if (::GetModuleHandleA(moduleName)) return true;

    std::string dllName = moduleName;
    if (!dllName.ends_with(".dll") && !dllName.ends_with(".DLL")) dllName += ".dll";
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("Data/SKSE/Plugins") / dllName, ec);
}

static void LoadMFConfig() {
    const std::filesystem::path ini = "Data/SKSE/Plugins/SKSEMenuFramework.ini";
    if (!std::filesystem::exists(ini)) { SKSE::log::warn("LoadMFConfig: ini not found."); return; }
    std::ifstream f(ini);
    std::string line;
    while (std::getline(f, line)) {
        auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        std::string key = ToUpper(TrimStr(line.substr(0, eq)));
        std::string val = TrimStr(line.substr(eq + 1));
        if (key == "TOGGLEKEY")  g_MFConfig.toggleKeyName = ToUpper(val);
        if (key == "TOGGLEMODE") g_MFConfig.toggleMode    = ToUpper(val);
    }
    // Resolve DIK + VK from the key name
    std::string u = g_MFConfig.toggleKeyName;
    for (const auto& e : kKeyTable) {
        if (u == e.name) { g_MFConfig.toggleDIK = e.dik; g_MFConfig.toggleVK = e.vk; break; }
    }
    SKSE::log::info("LoadMFConfig: ToggleKey='{}' DIK=0x{:02X} VK=0x{:02X} Mode='{}'",
        g_MFConfig.toggleKeyName, g_MFConfig.toggleDIK, g_MFConfig.toggleVK, g_MFConfig.toggleMode);
}

static WORD VKFromDIK(WORD dik) {
    for (const auto& e : kKeyTable) {
        if (dik == e.dik) return e.vk;
    }
    // Reconstruct the Windows scan code from the DirectInput scan code
    WORD sc = dik & 0x7F;
    if (dik & 0x80) {
        sc |= 0xE000;
    }
    WORD vk = (WORD)::MapVirtualKeyA(sc, 3); // MAPVK_VSC_TO_VK_EX
    if (vk == 0) {
        vk = (WORD)::MapVirtualKeyA(sc & 0xFF, 1); // MAPVK_VSC_TO_VK
    }
    return vk;
}

static WORD DIKFromVK(WORD vk) {
    for (const auto& e : kKeyTable) {
        if (vk == e.vk) return e.dik;
    }
    WORD sc = (WORD)::MapVirtualKeyA(vk, 4); // MAPVK_VK_TO_VSC_EX
    if (sc == 0) {
        sc = (WORD)::MapVirtualKeyA(vk, 0); // MAPVK_VK_TO_VSC
    }
    WORD dik = (sc & 0xFF);
    if ((sc & 0xE000) == 0xE000) {
        dik |= 0x80;
    }
    return dik;
}

static WORD VKFromImGuiKey(int key) {
    // Dear ImGui's named F-key range is contiguous: F1=572 through F24=595.
    if (key >= 572 && key <= 595) return static_cast<WORD>(VK_F1 + (key - 572));
    return 0;
}

static std::string NameFromDIK(WORD dik) {
    if (dik == 0) return "None";
    for (const auto& e : kKeyTable) {
        if (dik == e.dik) return e.name;
    }
    LONG lParam = static_cast<LONG>(dik) << 16;
    if (dik == 0x9C || dik == 0x9D || dik == 0xB5 || dik == 0xB8 || (dik >= 0xC7 && dik <= 0xD3)) {
        lParam |= (1 << 24);
    }
    char buf[128];
    int len = ::GetKeyNameTextA(lParam, buf, sizeof(buf));
    if (len > 0) {
        return std::string(buf, len);
    }
    return std::format("Key 0x{:02X}", dik);
}

static std::string FormatHotkey(WORD dik, bool ctrl = false, bool shift = false, bool alt = false) {
    if (dik == 0) return "Unassigned";
    if (dik == 0x67 || dik == 0x68) return "Disabled (launcher only)";

    std::string result;
    if (ctrl) result += "CTRL + ";
    if (shift) result += "SHIFT + ";
    if (alt) result += "ALT + ";
    result += NameFromDIK(dik);
    return result;
}

static std::string FormatModifiedHotkey(WORD dik, WORD modifierDIK) {
    if (modifierDIK == 0) return FormatHotkey(dik);
    if (modifierDIK == 0x1D) return FormatHotkey(dik, true, false, false);
    if (modifierDIK == 0x2A || modifierDIK == 0x36) return FormatHotkey(dik, false, true, false);
    if (modifierDIK == 0x38 || modifierDIK == 0xB8) return FormatHotkey(dik, false, false, true);
    return NameFromDIK(modifierDIK) + " + " + FormatHotkey(dik);
}

static std::filesystem::path FindModFile(std::initializer_list<const char*> candidates);
// FLICK reads keybinds_user.ini when it exists (generated once the user changes any FLICK setting),
// and it overrides keybinds.ini. So target the user file if present, otherwise the base file.
static std::filesystem::path FindFLICKKeybinds();

static bool ParseBoolInt(const std::string& value) {
    const auto upper = ToUpper(TrimStr(value));
    return upper == "1" || upper == "TRUE" || upper == "YES" || upper == "ON";
}

static void LoadOARConfig() {
    g_OARConfig.enabled = IsPluginPresent("OpenAnimationReplacer");
    if (!g_OARConfig.enabled) return;

    const std::filesystem::path ini = "Data/SKSE/Plugins/OpenAnimationReplacer.ini";
    if (!std::filesystem::exists(ini)) {
        SKSE::log::warn("LoadOARConfig: ini not found.");
        return;
    }

    std::ifstream f(ini);
    std::string line;
    while (std::getline(f, line)) {
        auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        std::string key = ToUpper(TrimStr(line.substr(0, eq)));
        std::string val = TrimStr(line.substr(eq + 1));

        try {
            if (key == "BENABLEUI") g_OARConfig.enabled = ParseBoolInt(val);
            if (key == "UTOGGLEUIKEY") g_OARConfig.toggleDIK = static_cast<WORD>(std::stoul(val));
            if (key == "UTOGGLEUIKEYCTRL") g_OARConfig.ctrl = ParseBoolInt(val);
            if (key == "UTOGGLEUIKEYSHIFT") g_OARConfig.shift = ParseBoolInt(val);
            if (key == "UTOGGLEUIKEYALT") g_OARConfig.alt = ParseBoolInt(val);
        } catch (...) {
            SKSE::log::warn("LoadOARConfig: failed to parse '{}={}'.", key, val);
        }
    }

    g_OARConfig.toggleVK = VKFromDIK(g_OARConfig.toggleDIK);
    SKSE::log::info("LoadOARConfig: Enabled={} ToggleKey='{}' DIK=0x{:02X} VK=0x{:02X} Ctrl={} Shift={} Alt={}",
        g_OARConfig.enabled, NameFromDIK(g_OARConfig.toggleDIK), g_OARConfig.toggleDIK, g_OARConfig.toggleVK,
        g_OARConfig.ctrl, g_OARConfig.shift, g_OARConfig.alt);
}

static bool IsCursorShowing() {
    CURSORINFO pci = { sizeof(CURSORINFO) };
    if (::GetCursorInfo(&pci)) {
        return (pci.flags & CURSOR_SHOWING) != 0;
    }
    return false;
}

static bool IsUserTyping() {
    auto* ui = RE::UI::GetSingleton();
    return ui && (ui->IsMenuOpen("Console") || ui->IsMenuOpen("TextEntryMenu"));
}

// Opt-in: allow the launcher to open while the console is open (the console pauses time, letting
// menus be edited with time stopped, and some editors need a console-picked target). Off by default.
static std::atomic<bool> g_AllowOpenInConsole{ true };

// Typing gate for the LAUNCHER hotkey only. Real text-entry fields (rename/search boxes) always
// block it, but the console is allowed through when the user opted in. Mod-alias suppression keeps
// using IsUserTyping() so those keys never fire while the console has focus.
static bool IsLauncherTypingBlocked() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;
    if (ui->IsMenuOpen("TextEntryMenu")) return true;
    if (ui->IsMenuOpen("Console")) return !g_AllowOpenInConsole.load();
    return false;
}

static bool IsConsoleOpen() {
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen("Console");
}

static SKSEMenuFramework::Model::WindowInterface* g_LauncherWindow = nullptr;
static std::atomic<bool> g_ConfigRestartRequired{ false };
static std::atomic<long long> g_RestartNoticeStartedMs{ 0 };

// Opt-in: keep the game world running (animations, fire, NPCs) while our UI is open. By default the
// menu pauses the game (SKSE Menu Framework does this); turning this ON overrides that so the world
// keeps moving. Off by default (i.e. the game stays paused unless the user opts in).
static std::atomic<bool> g_KeepGameRunning{ true };

// "Keep the game running" clears the game's freezeTime that SKSE Menu Framework sets while a blocking
// window is open. We must NOT touch the window's BlockUserInput flag to do this: that flag is what
// gives the menu its mouse cursor and input capture, so dropping it kills the cursor and lets the
// game grab the mouse. Instead we leave BlockUserInput true and just override freezeTime.
//
// The catch: while BlockUserInput is true the framework suppresses the keyboard-process hook, so this
// can't be driven from there (that was the original bug - it never ran while the menu was up). It is
// called from RenderLauncher instead, which the framework invokes every frame it draws our window,
// regardless of input blocking - so our clear lands after the framework's freeze each frame.
//
// freezeTime is a single global shared with every other framework window, so our override MUST be
// scoped tightly: it only runs while OUR launcher window is open (NOT g_ActiveMenu, which includes the
// SKSE MF menu and other mods' menus), and when our window closes we hand freezeTime back exactly as
// the framework set it (frozen). Otherwise our "false" leaks into whatever framework window opens next
// - e.g. the SKSE MF menu would come up un-frozen because it inherited our value.
static std::atomic<bool> g_FreezeOverridden{ false }; // we currently hold freezeTime down for our menu
static std::atomic<bool> g_FreezeSaved{ false };      // the value to restore (what the framework set)

// Undo our freezeTime override, if any, restoring the value the framework had. Idempotent.
static void RestoreFreezeOverride() {
    if (g_FreezeOverridden.exchange(false)) {
        auto* main = RE::Main::GetSingleton();
        if (main) main->freezeTime = g_FreezeSaved.load();
    }
}

static void UpdateTimeFreeze() {
    auto* main = RE::Main::GetSingleton();
    if (!main) return;
    // Strictly OUR launcher window - nobody else's menu. When it isn't open, release the override so we
    // never affect another framework window's freeze.
    const bool ours = g_KeepGameRunning.load() && g_LauncherWindow && g_LauncherWindow->IsOpen.load();
    if (!ours) {
        RestoreFreezeOverride();
        return;
    }
    if (main->freezeTime) {
        // Framework has frozen for our window. Remember its value once, then hold time running.
        if (!g_FreezeOverridden.load()) {
            g_FreezeSaved.store(true);
            g_FreezeOverridden.store(true);
        }
        main->freezeTime = false;
    }
}

static long long RestartNoticeNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool g_ExclusionsPopupOpen = false; // true while the exclusions confirmation popup is showing

static void MarkConfigRestartRequired(const char* source) {
    const bool first = !g_ConfigRestartRequired.exchange(true);
    SKSE::log::info("Configuration changed: {}. Skyrim restart notice {}.",
        source, first ? "armed" : "already armed");
    if (g_LauncherWindow) {
        g_LauncherWindow->IsOpen.store(true);
        // Don't drop BlockUserInput (which hides the cursor) while the exclusions popup is up - the user
        // needs the mouse to finish that dialog. The restart notice is deferred until the popup closes.
        if (!g_ExclusionsPopupOpen) {
            g_LauncherWindow->BlockUserInput.store(false);
        }
    }
}

static bool IsExternalMenuOpen() {
    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
        return false;
    }
    if (NowMs() - g_LastLauncherToggleMs.load() < 250) {
        return false;
    }
    if (IsCursorShowing()) {
        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            if (ui->IsMenuOpen("Console") || 
                ui->IsMenuOpen("TextEntryMenu") || 
                ui->IsMenuOpen("Journal Menu") ||
                ui->IsMenuOpen("MapMenu") ||
                ui->IsMenuOpen("Book Menu") ||
                ui->IsMenuOpen("ContainerMenu") ||
                ui->IsMenuOpen("InventoryMenu") ||
                ui->IsMenuOpen("StatsMenu") ||
                ui->IsMenuOpen("Loading Menu") ||
                ui->IsMenuOpen("BarterMenu") ||
                ui->IsMenuOpen("MagicMenu") ||
                ui->IsMenuOpen("Dialogue Menu") ||
                ui->IsMenuOpen("Crafting Menu") ||
                ui->IsMenuOpen("GiftMenu") ||
                ui->IsMenuOpen("Lockpicking Menu") ||
                ui->IsMenuOpen("TweenMenu") ||
                ui->IsMenuOpen("RaceSex Menu") ||
                ui->IsMenuOpen("MessageBoxMenu") ||
                ui->IsMenuOpen("Tutorial Menu") ||
                ui->IsMenuOpen("MainMenu") ||
                ui->IsMenuOpen("StartMenu") ||
                ui->IsApplicationMenuOpen() || 
                ui->IsModalMenuOpen()) {
                return false;
            }

        }
        if (SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
            return false;
        }
        return true;
    }
    return false;
}

static void LoadIEDConfig() {
    g_IEDConfig.enabled = IsPluginPresent("ImmersiveEquipmentDisplays");
    if (!g_IEDConfig.enabled) return;

    const std::filesystem::path ini = "Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini";
    if (!std::filesystem::exists(ini)) {
        SKSE::log::warn("LoadIEDConfig: ini not found.");
        return;
    }

    std::ifstream f(ini);
    std::string line;
    while (std::getline(f, line)) {
        auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        std::string key = ToUpper(TrimStr(line.substr(0, eq)));
        std::string val = TrimStr(line.substr(eq + 1));

        try {
            if (key == "TOGGLEKEYS") g_IEDConfig.toggleKeysStr = val;
        } catch (...) {
            SKSE::log::warn("LoadIEDConfig: failed to parse '{}={}'.", key, val);
        }
    }

    std::string primaryKeyStr = g_IEDConfig.toggleKeysStr;
    auto plusPos = primaryKeyStr.find_last_of('+');
    if (plusPos != std::string::npos) {
        primaryKeyStr = TrimStr(primaryKeyStr.substr(plusPos + 1));
    }
    
    try {
        if (primaryKeyStr.starts_with("0x") || primaryKeyStr.starts_with("0X")) {
            g_IEDConfig.toggleDIK = static_cast<WORD>(std::stoul(primaryKeyStr, nullptr, 16));
        } else {
            g_IEDConfig.toggleDIK = static_cast<WORD>(std::stoul(primaryKeyStr));
        }
    } catch (...) {
        SKSE::log::warn("LoadIEDConfig: failed to parse key code from '{}'.", primaryKeyStr);
    }

    g_IEDConfig.toggleVK = VKFromDIK(g_IEDConfig.toggleDIK);
    SKSE::log::info("LoadIEDConfig: Enabled={} ToggleKeysStr='{}' DIK=0x{:02X} VK=0x{:02X}",
        g_IEDConfig.enabled, g_IEDConfig.toggleKeysStr, g_IEDConfig.toggleDIK, g_IEDConfig.toggleVK);
}

static void LoadENBConfig() {
    const std::filesystem::path ini = "enblocal.ini";
    if (!std::filesystem::exists(ini)) {
        SKSE::log::warn("LoadENBConfig: enblocal.ini not found in game root.");
        return;
    }

    g_ENBConfig.enabled = true;

    std::ifstream f(ini);
    std::string line;
    while (std::getline(f, line)) {
        auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        std::string key = ToUpper(TrimStr(line.substr(0, eq)));
        std::string val = TrimStr(line.substr(eq + 1));

        try {
            if (key == "KEYCOMBINATION") g_ENBConfig.combinationVK = std::stoi(val);
            if (key == "KEYEDITOR")      g_ENBConfig.editorVK      = std::stoi(val);
        } catch (...) {
            SKSE::log::warn("LoadENBConfig: failed to parse '{}={}'.", key, val);
        }
    }

    SKSE::log::info("LoadENBConfig: Enabled={} KeyCombination={} KeyEditor={}",
        g_ENBConfig.enabled, g_ENBConfig.combinationVK, g_ENBConfig.editorVK);
}

static void ResolveDebugMenuApi(const char* context) {
    if (HMODULE hMod = ::GetModuleHandleA("DebugMenu.dll")) {
        if (!g_DebugMenuConfig.setMenuOpen) {
            g_DebugMenuConfig.setMenuOpen = reinterpret_cast<DebugMenuConfig::FnSetMenuOpen>(
                ::GetProcAddress(hMod, "DebugMenu_SetMenuOpen"));
        }
        if (!g_DebugMenuConfig.isMenuOpen) {
            g_DebugMenuConfig.isMenuOpen = reinterpret_cast<DebugMenuConfig::FnIsMenuOpen>(
                ::GetProcAddress(hMod, "DebugMenu_IsMenuOpen"));
        }
        if (!g_DebugMenuConfig.setHotkeysEnabled) {
            g_DebugMenuConfig.setHotkeysEnabled = reinterpret_cast<DebugMenuConfig::FnSetHotkeysEnabled>(
                ::GetProcAddress(hMod, "DebugMenu_SetHotkeyEnabled"));
        }
        if (!g_DebugMenuConfig.setHotkeysEnabled) {
            g_DebugMenuConfig.setHotkeysEnabled = reinterpret_cast<DebugMenuConfig::FnSetHotkeysEnabled>(
                ::GetProcAddress(hMod, "DebugMenu_SetHotkeysEnabled"));
        }
        SKSE::log::info("{}: DebugMenu API exports SetMenuOpen={:p} IsMenuOpen={:p} SetHotkeyEnabled={:p}",
            context,
            reinterpret_cast<void*>(g_DebugMenuConfig.setMenuOpen),
            reinterpret_cast<void*>(g_DebugMenuConfig.isMenuOpen),
            reinterpret_cast<void*>(g_DebugMenuConfig.setHotkeysEnabled));
    } else {
        SKSE::log::info("{}: DebugMenu.dll not loaded yet; API resolve deferred.", context);
    }
}

static void LoadDebugMenuConfig() {
    g_DebugMenuConfig.enabled = IsPluginPresent("DebugMenu");
    if (!g_DebugMenuConfig.enabled) return;

    // Resolve the exported API from DebugMenu.dll so we can close it directly.
    if (HMODULE hMod = ::GetModuleHandleA("DebugMenu.dll")) {
        g_DebugMenuConfig.setMenuOpen = reinterpret_cast<DebugMenuConfig::FnSetMenuOpen>(
            ::GetProcAddress(hMod, "DebugMenu_SetMenuOpen"));
        g_DebugMenuConfig.isMenuOpen = reinterpret_cast<DebugMenuConfig::FnIsMenuOpen>(
            ::GetProcAddress(hMod, "DebugMenu_IsMenuOpen"));
        g_DebugMenuConfig.setHotkeysEnabled = reinterpret_cast<DebugMenuConfig::FnSetHotkeysEnabled>(
            ::GetProcAddress(hMod, "DebugMenu_SetHotkeyEnabled"));
        if (!g_DebugMenuConfig.setHotkeysEnabled) {
            g_DebugMenuConfig.setHotkeysEnabled = reinterpret_cast<DebugMenuConfig::FnSetHotkeysEnabled>(
                ::GetProcAddress(hMod, "DebugMenu_SetHotkeysEnabled"));
        }
        if (g_DebugMenuConfig.setMenuOpen)
            SKSE::log::info("LoadDebugMenuConfig: DebugMenu_SetMenuOpen resolved via DLL export.");
        else
            SKSE::log::warn("LoadDebugMenuConfig: DebugMenu_SetMenuOpen NOT found — will fall back to key inject.");
    }

    // Default values
    g_DebugMenuConfig.toggleDIK = 59; // F1 default
    g_DebugMenuConfig.toggleVK = VK_F1;

    // Check user settings first (MCM Helper stores settings here)
    std::filesystem::path ini = "Data/MCM/Settings/DebugMenu.ini";
    if (!std::filesystem::exists(ini)) {
        ini = "Data/MCM/Config/DebugMenu/settings.ini";
    }

    if (std::filesystem::exists(ini)) {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
            auto eq = line.find('='); if (eq == std::string::npos) continue;
            std::string key = ToUpper(TrimStr(line.substr(0, eq)));
            std::string val = TrimStr(line.substr(eq + 1));

            try {
                if (key == "UOPENMENUHOTKEY") {
                    g_DebugMenuConfig.toggleDIK = static_cast<WORD>(std::stoul(val));
                }
            } catch (...) {
                SKSE::log::warn("LoadDebugMenuConfig: failed to parse '{}={}'.", key, val);
            }
        }
    }

    g_DebugMenuConfig.toggleVK = VKFromDIK(g_DebugMenuConfig.toggleDIK);
    SKSE::log::info("LoadDebugMenuConfig: Enabled={} ToggleKey='{}' DIK=0x{:02X} VK=0x{:02X}",
        g_DebugMenuConfig.enabled, NameFromDIK(g_DebugMenuConfig.toggleDIK), g_DebugMenuConfig.toggleDIK, g_DebugMenuConfig.toggleVK);
}

static void LoadDMenuConfig() {
    g_DMenuConfig.enabled = IsPluginPresent("dmenu");
    if (!g_DMenuConfig.enabled) return;

    // Default values
    g_DMenuConfig.toggleDIK = 199; // Home default (0xC7)
    g_DMenuConfig.toggleVK = VK_HOME;
    g_DMenuConfig.modifierDIK = 0;
    g_DMenuConfig.modifierVK = 0;

    const std::filesystem::path ini = "Data/SKSE/Plugins/dmenu/dmenu.ini";
    if (std::filesystem::exists(ini)) {
        std::ifstream f(ini);
        std::string line;
        bool foundMkbToggle = false;
        bool foundMkbModifier = false;
        while (std::getline(f, line)) {
            auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
            auto eq = line.find('='); if (eq == std::string::npos) continue;
            std::string key = ToUpper(TrimStr(line.substr(0, eq)));
            std::string val = TrimStr(line.substr(eq + 1));

            try {
                if (key == "KEY_TOGGLE_DMENU_MKB") {
                    g_DMenuConfig.toggleDIK = static_cast<WORD>(std::stoul(val));
                    foundMkbToggle = true;
                } else if (key == "KEY_TOGGLE_DMENU" && !foundMkbToggle) {
                    g_DMenuConfig.toggleDIK = static_cast<WORD>(std::stoul(val));
                }
                if (key == "KEY_TOGGLE_MODIFIER_MKB") {
                    g_DMenuConfig.modifierDIK = static_cast<WORD>(std::stoul(val));
                    foundMkbModifier = true;
                } else if (key == "KEY_TOGGLE_MODIFIER" && !foundMkbModifier) {
                    g_DMenuConfig.modifierDIK = static_cast<WORD>(std::stoul(val));
                }
            } catch (...) {
                SKSE::log::warn("LoadDMenuConfig: failed to parse '{}={}'.", key, val);
            }
        }
    }

    g_DMenuConfig.toggleVK = VKFromDIK(g_DMenuConfig.toggleDIK);
    g_DMenuConfig.modifierVK = g_DMenuConfig.modifierDIK != 0 ? VKFromDIK(g_DMenuConfig.modifierDIK) : 0;
    SKSE::log::info("LoadDMenuConfig: Enabled={} ToggleKey='{}' DIK=0x{:02X} VK=0x{:02X} ModifierDIK=0x{:02X} ModifierVK=0x{:02X}",
        g_DMenuConfig.enabled, NameFromDIK(g_DMenuConfig.toggleDIK), g_DMenuConfig.toggleDIK, g_DMenuConfig.toggleVK, g_DMenuConfig.modifierDIK, g_DMenuConfig.modifierVK);
}

static bool IsDMenuModifierDown() {
    if (!g_DMenuConfig.enabled || g_DMenuConfig.modifierVK == 0) return true;
    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };
    return (keyState(g_DMenuConfig.modifierVK) & 0x8000) != 0;
}

static void LoadImprovedCameraConfig() {
    g_ImprovedCameraConfig.enabled = IsPluginPresent("ImprovedCameraSE");
    if (!g_ImprovedCameraConfig.enabled) return;

    // Improved Camera's menu is MenuKey + LEFT SHIFT (its ini says "You need to hold down
    // Left Shift before pressing the key"). So the toggle is Shift + MenuKey(Home).
    g_ImprovedCameraConfig.toggleVK = VK_HOME; // Home default (0x24)
    g_ImprovedCameraConfig.modifierVK = VK_LSHIFT;

    const std::filesystem::path ini = "Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini";
    if (std::filesystem::exists(ini)) {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
            auto eq = line.find('='); if (eq == std::string::npos) continue;
            std::string key = ToUpper(TrimStr(line.substr(0, eq)));
            std::string val = TrimStr(line.substr(eq + 1));

            try {
                if (key == "MENUKEY") {
                    unsigned long vkVal = 0;
                    if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
                        vkVal = std::stoul(val, nullptr, 16);
                    } else {
                        vkVal = std::stoul(val);
                    }
                    g_ImprovedCameraConfig.toggleVK = static_cast<WORD>(vkVal);
                }
            } catch (...) {
                SKSE::log::warn("LoadImprovedCameraConfig: failed to parse '{}={}'.", key, val);
            }
        }
    }

    // Left Shift modifier (DIK 0x2A).
    g_ImprovedCameraConfig.modifierDIK = g_ImprovedCameraConfig.modifierVK != 0
        ? DIKFromVK(g_ImprovedCameraConfig.modifierVK) : 0;

    // For Home, MapVirtualKey(VK_HOME, 0) gives 0x47, but DIK_HOME is 0xC7.
    // We should make sure if toggleVK is VK_HOME, we use 0xC7 (199).
    if (g_ImprovedCameraConfig.toggleVK == VK_HOME) {
        g_ImprovedCameraConfig.toggleDIK = 0xC7;
    } else {
        g_ImprovedCameraConfig.toggleDIK = DIKFromVK(g_ImprovedCameraConfig.toggleVK);
    }

    SKSE::log::info("LoadImprovedCameraConfig: Enabled={} ToggleKey='{}' DIK=0x{:02X} VK=0x{:02X} ModifierDIK=0x{:02X} ModifierVK=0x{:02X}",
        g_ImprovedCameraConfig.enabled, NameFromDIK(g_ImprovedCameraConfig.toggleDIK), g_ImprovedCameraConfig.toggleDIK, g_ImprovedCameraConfig.toggleVK, g_ImprovedCameraConfig.modifierDIK, g_ImprovedCameraConfig.modifierVK);
}

static void LoadFLICKConfig() {
    g_FLICKConfig.enabled = IsPluginPresent("FUCK");
    if (!g_FLICKConfig.enabled) return;

    const auto iniPath = FindFLICKKeybinds();
    if (!iniPath.empty()) {
        std::ifstream infile(iniPath);
        if (infile.is_open()) {
            std::string line;
            while (std::getline(infile, line)) {
                auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
                auto eq = line.find('='); if (eq == std::string::npos) continue;
                std::string key = ToUpper(TrimStr(line.substr(0, eq)));
                std::string val = TrimStr(line.substr(eq + 1));
                if (key == "ITOGGLEFUCK_KEY") {
                    try {
                        int code = std::stoi(val);
                        if (code > 0 && code < 256) {
                            g_FLICKConfig.toggleDIK = (WORD)code;
                        }
                    } catch (...) {}
                }
            }
            infile.close();
        }
    } else {
        SKSE::log::warn("LoadFLICKConfig: keybinds.ini not found at any known path; using F7.");
    }
    g_FLICKConfig.toggleVK = VKFromDIK(g_FLICKConfig.toggleDIK);
    SKSE::log::info("LoadFLICKConfig: enabled={}, toggleDIK={:#x}, toggleVK={:#x}",
        g_FLICKConfig.enabled, g_FLICKConfig.toggleDIK, g_FLICKConfig.toggleVK);
}

static void LoadKreatEConfig() {
    g_KreatEConfig.enabled = IsPluginPresent("KreatE");
    if (!g_KreatEConfig.enabled) return;

    const std::filesystem::path ini = "Data/KreatE/UserSettings.ini";
    if (std::filesystem::exists(ini)) {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line)) {
            const auto sc = line.find(';');
            if (sc != std::string::npos) line = line.substr(0, sc);
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (ToUpper(TrimStr(line.substr(0, eq))) != "GUITOGGLEKEYS") continue;
            try {
                const auto value = std::stoul(TrimStr(line.substr(eq + 1)), nullptr, 0);
                if (value > 0 && value < 256) g_KreatEConfig.toggleVK = static_cast<WORD>(value);
            } catch (...) {
                SKSE::log::warn("LoadKreatEConfig: failed to parse GUIToggleKeys.");
            }
        }
    } else {
        SKSE::log::info("LoadKreatEConfig: UserSettings.ini has not been generated yet; using End for this launch.");
    }

    g_KreatEConfig.toggleDIK = g_KreatEConfig.toggleVK == VK_END
        ? 0xCF : DIKFromVK(g_KreatEConfig.toggleVK);
    SKSE::log::info("LoadKreatEConfig: enabled=true, toggleDIK={:#x}, toggleVK={:#x}",
        g_KreatEConfig.toggleDIK, g_KreatEConfig.toggleVK);
}

static void LoadCSConfig() {
    g_CSConfig.enabled = IsPluginPresent("CommunityShaders");
    if (!g_CSConfig.enabled) return;

    std::filesystem::path jsonPath = "Data/SKSE/Plugins/CommunityShaders/SettingsUser.json";
    if (!std::filesystem::exists(jsonPath)) {
        jsonPath = "Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json";
    }

    g_CSConfig.toggleVK = VK_END;
    g_CSConfig.editorVK = VK_END;
    g_CSConfig.editorModifierVK = VK_SHIFT;
    g_CSConfig.overlayVK = VK_F10;
    g_CSConfig.effectVK = VK_MULTIPLY;
    if (std::filesystem::exists(jsonPath)) {
        try {
            std::ifstream f(jsonPath);
            nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("Menu") && j["Menu"].is_object()) {
                const auto& menu = j["Menu"];
                const auto readVK = [&](const char* key, WORD& target) {
                    if (menu.contains(key) && menu[key].is_number_integer()) {
                        const int value = menu[key].get<int>();
                        if (value > 0 && value < 256) target = static_cast<WORD>(value);
                    }
                };
                readVK("ToggleKey", g_CSConfig.toggleVK);
                readVK("OverlayToggleKey", g_CSConfig.overlayVK);
                readVK("EffectToggleKey", g_CSConfig.effectVK);
                if (menu.contains("CSEditorToggleKey") && menu["CSEditorToggleKey"].is_array()) {
                    const auto& chord = menu["CSEditorToggleKey"];
                    if (chord.size() >= 2 && chord[0].is_number_integer() && chord[1].is_number_integer()) {
                        const int modifier = chord[0].get<int>();
                        const int key = chord[1].get<int>();
                        if (modifier >= 0 && modifier < 256) g_CSConfig.editorModifierVK = static_cast<WORD>(modifier);
                        if (key > 0 && key < 256) g_CSConfig.editorVK = static_cast<WORD>(key);
                    }
                }
            }
        } catch (...) {
            SKSE::log::warn("LoadCSConfig: failed to parse {}", jsonPath.string());
        }
    }

    g_CSConfig.toggleDIK = g_CSConfig.toggleVK == VK_END ? 0xCF : DIKFromVK(g_CSConfig.toggleVK);
    g_CSConfig.editorDIK = g_CSConfig.editorVK == VK_END ? 0xCF : DIKFromVK(g_CSConfig.editorVK);
    g_CSConfig.editorModifierDIK = g_CSConfig.editorModifierVK == 0 ? 0 : DIKFromVK(g_CSConfig.editorModifierVK);
    g_CSConfig.overlayDIK = DIKFromVK(g_CSConfig.overlayVK);
    g_CSConfig.effectDIK = DIKFromVK(g_CSConfig.effectVK);
    SKSE::log::info("LoadCSConfig: enabled=true, main={}, editor={}+{}, overlay={}, effect={}",
        FormatHotkey(g_CSConfig.toggleDIK), FormatHotkey(g_CSConfig.editorModifierDIK),
        FormatHotkey(g_CSConfig.editorDIK), FormatHotkey(g_CSConfig.overlayDIK),
        FormatHotkey(g_CSConfig.effectDIK));
}

static void LoadPartySheetConfig() {
    g_PartySheetConfig.enabled = IsPluginPresent("SkyrimPartySheet");
    if (!g_PartySheetConfig.enabled) return;

    g_PartySheetConfig.settingsDIK = 0x2D;
    g_PartySheetConfig.partyDIK = 0x40;
    g_PartySheetConfig.inspectDIK = 0x15;
    g_PartySheetConfig.characterDIK = 0x16;
    g_PartySheetConfig.modifierDIK = 0;

    const std::filesystem::path ini = "Data/SKSE/Plugins/PartyUserSettings.ini";
    if (std::filesystem::exists(ini)) {
        const auto readDIK = [&](const char* key, WORD& target) {
            char value[64]{};
            ::GetPrivateProfileStringA("Hotkeys", key, "", value, sizeof(value), ini.string().c_str());
            const std::string text = TrimStr(value);
            if (text.empty()) return;
            try {
                const auto parsed = std::stoul(text, nullptr, 0);
                if (parsed < 256) target = static_cast<WORD>(parsed);
            } catch (...) {
                const auto wanted = ToUpper(text);
                for (const auto& entry : kKeyTable) {
                    if (wanted == entry.name) {
                        target = entry.dik;
                        break;
                    }
                }
            }
        };
        readDIK("MenuSettingsKey", g_PartySheetConfig.settingsDIK);
        readDIK("PartySheetHotkey", g_PartySheetConfig.partyDIK);
        readDIK("InspectCardHotkey", g_PartySheetConfig.inspectDIK);
        readDIK("CharacterSheetHotkey", g_PartySheetConfig.characterDIK);
        readDIK("ModifierKey", g_PartySheetConfig.modifierDIK);
    } else {
        SKSE::log::info("LoadPartySheetConfig: PartyUserSettings.ini has not been generated; using X/F6/Y/U defaults.");
    }

    SKSE::log::info("LoadPartySheetConfig: settings={}, party={}, inspect={}, character={}, modifier={}",
        FormatHotkey(g_PartySheetConfig.settingsDIK), FormatHotkey(g_PartySheetConfig.partyDIK),
        FormatHotkey(g_PartySheetConfig.inspectDIK), FormatHotkey(g_PartySheetConfig.characterDIK),
        FormatHotkey(g_PartySheetConfig.modifierDIK));
}

// ---- Outfit Preview Selector (OPS) ----
// OPS is a Papyrus/Scaleform mod (ESP OutfitPreviewSelector.esp). It opens/closes its "CustomMenu" in
// response to SKSE mod events, exposes its open state through RE::UI, and stores its toggle key in the
// MCM "Hotkey" property (a DirectInput scancode; default 79 = Numpad 1) on its config quest
// OutfitPreviewSeectorQ (script OutfitPreviewSe). We open/close via the events, track state via
// RE::UI, and free the key by setting Hotkey to 0 (no INI, no file edit; lives in the save).
static bool IsOPSInstalled() {
    // OPS support is temporarily DROPPED: its menu freezes input when opened with Risa loaded, and its
    // installed version has no clean external open/close (the author's OPS_NativePreviewOpen event isn't
    // in it yet). Reporting "not installed" hides the button, the settings row, and all key management,
    // so Risa never touches OPS. The rest of the OPS integration is kept for when the author ships a
    // proper API. To re-enable, restore the LookupModByName check below.
    return false;
#if 0
    auto* dh = RE::TESDataHandler::GetSingleton();
    return dh && dh->LookupModByName("OutfitPreviewSelector.esp") != nullptr;
#endif
}

static void SendOPSModEvent(const char* eventName) {
    if (auto* src = SKSE::GetModCallbackEventSource()) {
        SKSE::ModCallbackEvent ev{ eventName, "", 0.0f, nullptr };
        src->SendEvent(&ev);
        SKSE::log::info("SendOPSModEvent: sent '{}'.", eventName);
    }
}

static bool IsOPSMenuOpen() {
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen("CustomMenu");
}

static bool GetOPSObject(RE::BSScript::Internal::VirtualMachine* vm,
                         RE::BSTSmartPointer<RE::BSScript::Object>& out) {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!vm || !dh) return false;
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) return false;
    for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
        if (!quest) continue;
        const auto handle = policy->GetHandleForObject(static_cast<RE::VMTypeID>(quest->GetFormType()), quest);
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        if (vm->FindBoundObject(handle, "OutfitPreviewSe", obj) && obj) {
            out = obj;
            return true;
        }
    }
    return false;
}

// Set OPS's MCM "Hotkey" property (a DXScancode) by locating its config quest's script instance.
// keyCode 0 = disabled (frees the key). Needs a save loaded so the quest script is bound; returns
// false until then, so the retry loop keeps trying.
static bool SetOPSHotkey(int keyCode) {
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    RE::BSTSmartPointer<RE::BSScript::Object> obj;
    if (!GetOPSObject(vm, obj)) return false;
    if (auto* prop = obj->GetProperty("Hotkey")) {
        const int oldKey = prop->GetSInt();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;

        if (oldKey > 0 && oldKey != keyCode) {
            auto* args = RE::MakeFunctionArguments(static_cast<std::int32_t>(oldKey));
            vm->DispatchMethodCall(obj, "UnregisterForKey", args, cb);
            SKSE::log::info("SetOPSHotkey: unregistered OutfitPreviewSe key {}.", oldKey);
        }

        prop->SetSInt(keyCode);
        SKSE::log::info("SetOPSHotkey: set OutfitPreviewSe.Hotkey = {}.", keyCode);

        if (keyCode > 0 && oldKey != keyCode) {
            auto* args = RE::MakeFunctionArguments(static_cast<std::int32_t>(keyCode));
            vm->DispatchMethodCall(obj, "RegisterForKey", args, cb);
            SKSE::log::info("SetOPSHotkey: registered OutfitPreviewSe key {}.", keyCode);
        }

        return true;
    }
    return false;
}

static void LoadOPSConfig() {
    g_OPSConfig.enabled = IsOPSInstalled();
    if (!g_OPSConfig.enabled) return;
    g_OPSConfig.toggleDIK = 0x4F;
    g_OPSConfig.toggleVK = VK_NUMPAD1;
    SKSE::log::info("LoadOPSConfig: enabled=true (Outfit Preview Selector detected).");
}

// Read Mod Function Menu's current [Controls.Keyboard] iHotkey (a DirectInput scancode) from its TOML.
// iHotkey also appears under [Controls.Gamepad], which we must ignore, so track the section. Default 59 (F1).
static int ReadModFunctionMenuHotkey() {
    int hotkey = 59;
    const std::filesystem::path tomlPath = "Data/SKSE/Plugins/ccld_ModFunctionMenu.toml";
    if (!std::filesystem::exists(tomlPath)) return hotkey;
    std::ifstream in(tomlPath);
    std::string line, section;
    while (std::getline(in, line)) {
        std::string t = TrimStr(line);
        if (!t.empty() && t.front() == '[') { section = ToUpper(t); continue; }
        if (section != "[CONTROLS.KEYBOARD]") continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos || ToUpper(TrimStr(t.substr(0, eq))) != "IHOTKEY") continue;
        std::string v = TrimStr(t.substr(eq + 1));
        const auto c = v.find('#'); if (c != std::string::npos) v = TrimStr(v.substr(0, c));
        try { hotkey = std::stoi(v); } catch (...) {}
        break;
    }
    return hotkey;
}

static void LoadModFunctionMenuConfig() {
    g_ModFunctionMenuConfig.enabled = IsPluginPresent("ccld_ModFunctionMenu");
    if (!g_ModFunctionMenuConfig.enabled) return;

    const int hotkey = ReadModFunctionMenuHotkey();
    g_ModFunctionMenuConfig.toggleDIK = static_cast<WORD>(hotkey & 0xFF);
    g_ModFunctionMenuConfig.toggleVK  = VKFromDIK(g_ModFunctionMenuConfig.toggleDIK);
    SKSE::log::info("LoadModFunctionMenuConfig: enabled=true, iHotkey={}, toggleDIK={:#x}, toggleVK={:#x}",
        hotkey, g_ModFunctionMenuConfig.toggleDIK, g_ModFunctionMenuConfig.toggleVK);
}

static void LoadCatMenuConfig() {
    g_CatMenuConfig.enabled = IsPluginPresent("catmenu");
    if (!g_CatMenuConfig.enabled) return;

    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/catmenu/settings.json";
    if (std::filesystem::exists(jsonPath)) {
        try {
            std::ifstream f(jsonPath);
            const nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("toggle_key")) {
                g_CatMenuConfig.toggleImGuiKey = j["toggle_key"].get<int>();
            }
        } catch (...) {
            SKSE::log::warn("LoadCatMenuConfig: failed to parse {}.", jsonPath.string());
        }
    }

    g_CatMenuConfig.toggleVK = VKFromImGuiKey(g_CatMenuConfig.toggleImGuiKey);
    g_CatMenuConfig.toggleDIK = DIKFromVK(g_CatMenuConfig.toggleVK);
    if (g_CatMenuConfig.toggleVK == 0 || g_CatMenuConfig.toggleDIK == 0) {
        SKSE::log::warn("LoadCatMenuConfig: unsupported ImGui toggle key {}; falling back to F6.",
            g_CatMenuConfig.toggleImGuiKey);
        g_CatMenuConfig.toggleImGuiKey = 577;
        g_CatMenuConfig.toggleVK = VK_F6;
        g_CatMenuConfig.toggleDIK = 0x40;
    }
    SKSE::log::info("LoadCatMenuConfig: enabled=true, ImGuiKey={}, toggleDIK={:#x}, toggleVK={:#x}",
        g_CatMenuConfig.toggleImGuiKey, g_CatMenuConfig.toggleDIK, g_CatMenuConfig.toggleVK);
}

static void LoadDragonbornConfig() {
    g_DragonbornConfig.enabled = IsPluginPresent("SkyrimCheatMenu");
    if (!g_DragonbornConfig.enabled) return;

    bool explicitlyUnassigned = false;
    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/SkyrimCheatMenu.json";
    if (std::filesystem::exists(jsonPath)) {
        try {
            std::ifstream f(jsonPath);
            const nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("toggleKey")) {
                if (j["toggleKey"].is_number_integer()) {
                    const int dik = j["toggleKey"].get<int>();
                    if (dik == 0) {
                        g_DragonbornConfig.toggleDIK = 0;
                        g_DragonbornConfig.toggleVK = 0;
                        g_DragonbornConfig.toggleKeyName = "UNASSIGNED";
                        explicitlyUnassigned = true;
                    } else if (dik > 0 && dik < 256) {
                        g_DragonbornConfig.toggleDIK = static_cast<WORD>(dik);
                        g_DragonbornConfig.toggleVK = VKFromDIK(g_DragonbornConfig.toggleDIK);
                        g_DragonbornConfig.toggleKeyName = NameFromDIK(g_DragonbornConfig.toggleDIK);
                    }
                } else if (j["toggleKey"].is_string()) {
                    g_DragonbornConfig.toggleKeyName = ToUpper(j["toggleKey"].get<std::string>());
                }
            }
        } catch (...) {
            SKSE::log::warn("LoadDragonbornConfig: failed to parse {}.", jsonPath.string());
        }
    }

    bool found = explicitlyUnassigned ||
        (g_DragonbornConfig.toggleVK != 0 && g_DragonbornConfig.toggleDIK != 0 &&
         g_DragonbornConfig.toggleKeyName != "F1");
    if (!found) {
        for (const auto& key : kKeyTable) {
            if (g_DragonbornConfig.toggleKeyName == key.name) {
                g_DragonbornConfig.toggleDIK = key.dik;
                g_DragonbornConfig.toggleVK = key.vk;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        SKSE::log::warn("LoadDragonbornConfig: unsupported toggle key '{}'; falling back to F1.",
            g_DragonbornConfig.toggleKeyName);
        g_DragonbornConfig.toggleKeyName = "F1";
        g_DragonbornConfig.toggleDIK = 0x3B;
        g_DragonbornConfig.toggleVK = VK_F1;
    }
    SKSE::log::info("LoadDragonbornConfig: enabled=true, key={}, toggleDIK={:#x}, toggleVK={:#x}",
        g_DragonbornConfig.toggleKeyName, g_DragonbornConfig.toggleDIK, g_DragonbornConfig.toggleVK);
}

static void LoadReShadeConfig() {
    // ReShade is a dxgi.dll proxy (not an SKSE plugin). Detect via its ini or a ReShade export.
    const std::filesystem::path ini = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" });
    HMODULE dxgi = ::GetModuleHandleA("dxgi.dll");
    const bool reshadeDxgi = dxgi && (::GetProcAddress(dxgi, "ReShadeRegisterAddon") ||
                                      ::GetProcAddress(dxgi, "ReShadeRegisterEffectRuntime"));
    g_ReShadeConfig.enabled = !ini.empty() || reshadeDxgi;
    if (!g_ReShadeConfig.enabled) return;

    // [INPUT] KeyOverlay=VK,ctrl,shift,alt  (default 36,0,0,0 = Home)
    if (!ini.empty()) {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (ToUpper(TrimStr(line.substr(0, eq))) != "KEYOVERLAY") continue;
            int n[4] = { 0, 0, 0, 0 };
            int idx = 0;
            std::stringstream ss(TrimStr(line.substr(eq + 1)));
            std::string tok;
            while (idx < 4 && std::getline(ss, tok, ',')) { try { n[idx] = std::stoi(TrimStr(tok)); } catch (...) {} idx++; }
            if (n[0] > 0) g_ReShadeConfig.toggleVK = static_cast<WORD>(n[0]);
            g_ReShadeConfig.modifierDIK = n[1] ? 0x1D : n[2] ? 0x2A : n[3] ? 0x38 : 0;
            break;
        }
    }
    g_ReShadeConfig.toggleDIK = (g_ReShadeConfig.toggleVK == VK_HOME) ? 0xC7 : DIKFromVK(g_ReShadeConfig.toggleVK);
    SKSE::log::info("LoadReShadeConfig: enabled=true, KeyOverlay VK=0x{:02X} DIK=0x{:02X} mod=0x{:02X}",
        g_ReShadeConfig.toggleVK, g_ReShadeConfig.toggleDIK, g_ReShadeConfig.modifierDIK);
}

static bool IsImprovedCameraModifierDown() {
    if (!g_ImprovedCameraConfig.enabled || g_ImprovedCameraConfig.modifierVK == 0) return true;
    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };
    return (keyState(g_ImprovedCameraConfig.modifierVK) & 0x8000) != 0;
}

static std::atomic<float> g_WindowPosX{ 692.0f };
static std::atomic<float> g_WindowPosY{ 153.0f };
static std::atomic<float> g_SettingsWindowHeight{ 835.0f }; // user-resized Settings tab height (persisted)
static std::atomic<float> g_LauncherFontScale{ 0.85f };
static std::atomic<float> g_LauncherTextScale{ 0.95f };
static std::atomic<long long> g_LanguageComboActiveUntilMs{ 0 };
static std::atomic<float> g_HotkeyLayoutNameColumnW{ 178.57f };
static std::atomic<float> g_HotkeyLayoutExpandColumnW{ 38.86f };
static std::atomic<float> g_HotkeyLayoutKeyColumnW{ 167.63f };
static std::atomic<float> g_HotkeyLayoutSpacerWeight{ 0.78f };
static std::atomic<float> g_HotkeyLayoutAdvColumnW{ 70.70f };
static std::atomic<float> g_HotkeyLayoutToggleColumnW{ 113.78f };
static std::atomic<float> g_HotkeyLayoutTableRightMargin{ 0.00f };
static std::atomic<float> g_HotkeyLayoutAdvButtonWMult{ 1.39f };
static std::atomic<float> g_HotkeyLayoutAdvButtonHMult{ 0.92f };
static std::atomic<float> g_HotkeyLayoutAdvIconXNudge{ 1.18f };
static std::atomic<float> g_HotkeyLayoutAdvIconYNudge{ 2.85f };
static std::atomic<float> g_HotkeyLayoutToggleHeightMult{ 0.86f };
static std::atomic<float> g_HotkeyLayoutToggleMinH{ 28.78f };
static std::atomic<float> g_HotkeyLayoutToggleWidthMult{ 1.76f };
static std::atomic<float> g_HotkeyLayoutToggleRightPad{ 16.04f };
static std::atomic<WORD> g_LauncherHotkeyDIK{ 0x3B };  // Default F1 (0x3B)
static std::atomic<WORD> g_LauncherHotkeyVK{ 0x70 };   // Default VK_F1 (0x70)
static std::atomic<bool> g_LauncherHotkeyCtrl{ false };
static std::atomic<bool> g_LauncherHotkeyShift{ false };
static std::atomic<bool> g_LauncherHotkeyAlt{ false };
static std::atomic<bool> g_LauncherHotkeyDoubleTap{ false };
// When on: opening still needs the full hotkey (modifier and/or double-tap), but a single
// bare press of the key alone closes whatever is open.
static std::atomic<bool> g_LauncherHotkeyEasyClose{ true };
static std::atomic<bool> g_LauncherHotkeyHold{ false };
static std::atomic<int> g_LauncherHoldDurationMs{ 500 };
static long long g_LauncherHoldStartedMs = 0;   // when the launcher key started being held (0 = not held)
static bool g_LauncherHoldTriggered = false;    // fired once this hold; reset on release
static std::atomic<long long> g_LastLauncherKeyPressMs{ 0 };

static std::atomic<bool> g_UnblockOAR{ false };
static std::atomic<bool> g_UnblockIED{ false };
static std::atomic<bool> g_UnblockENB{ false };
static std::atomic<bool> g_UnblockDebugMenu{ false };
static std::atomic<bool> g_UnblockCS{ false };
static std::atomic<bool> g_UnblockDMenu{ false };
static std::atomic<bool> g_UnblockImprovedCamera{ false };
static std::atomic<bool> g_UnblockFLICK{ false };
static std::atomic<bool> g_UnblockKreatE{ false };
static std::atomic<bool> g_UnblockCatMenu{ false };
static std::atomic<bool> g_UnblockDragonborn{ false };
static std::atomic<bool> g_UnblockReShade{ false }; // Home opens/closes ReShade live (bridged in HookedGetRawInputData)
static std::atomic<bool> g_UnblockMF{ false };      // cosmetic: MF has no managed original hotkey
static std::atomic<bool> g_UnblockSearchUI{ false };// ON = user's key opens SearchUI; OFF = key freed, button still opens
static std::atomic<bool> g_UnblockMCM{ false };     // ON = user's key opens MCM; OFF = key freed, button still opens
static std::atomic<bool> g_UnblockQAR{ false };     // ON = user's key opens QAR; OFF = key freed, button still opens
static std::atomic<bool> g_UnblockModFunctionMenu{ false }; // ON = user's key opens Mod Function Menu; OFF = key freed, button still opens
static std::atomic<bool> g_UnblockOPS{ false };     // ON = user's key opens Outfit Preview Selector; OFF = key freed, button still opens
static std::atomic<bool> g_UnblockCSEditor{ false };  // Community Shaders sub-toggles (rebindable in settings)
static std::atomic<bool> g_UnblockCSOverlay{ false };
static std::atomic<bool> g_UnblockCSEffect{ false };
static std::atomic<bool> g_UnblockPartySettings{ false };
static std::atomic<bool> g_UnblockPartySheet{ false };
static std::atomic<bool> g_UnblockPartyInspect{ false };
static std::atomic<bool> g_UnblockPartyCharacter{ false };
static std::atomic<bool> g_PartySheetDirectDispatch{ false };

// User-rebindable "alias" hotkey per mod: the key the launcher listens for to open that mod.
// Defaults to the mod's ORIGINAL key; the mod's real key stays relocated, so changing this
// edits no mod files — it only changes which key our bridge watches. Index order matches the
// chordDown[] array in PollOriginalHotkeyAliases.
// Indices 0..AI_Dragonborn are the chordDown[] watcher mods. AI_ReShade (raw-input bridge) and
// AI_MF (own bridge below) are rebindable too but handled outside the chordDown loop.
enum AliasIdx { AI_OAR = 0, AI_IED, AI_ENB, AI_DMenu, AI_IC, AI_FLICK, AI_DebugMenu, AI_KreatE, AI_CS, AI_CatMenu, AI_Dragonborn, AI_ReShade, AI_MF, AI_CSEditor, AI_CSOverlay, AI_CSEffect, AI_PartySettings, AI_PartySheet, AI_PartyInspect, AI_PartyCharacter, AI_SearchUI, AI_MCM, AI_QAR, AI_ModFunctionMenu, AI_OPS, AI_COUNT };
static std::atomic<WORD> g_AliasDik[AI_COUNT];
static std::atomic<bool> g_AliasCtrl[AI_COUNT];
static std::atomic<bool> g_AliasShift[AI_COUNT];
static std::atomic<bool> g_AliasAlt[AI_COUNT];
static std::atomic<bool> g_ExcludeMod[AI_COUNT] = { false };
static bool g_TempExcludeState[AI_COUNT] = { false };
static bool g_SettingsActive = false;
static bool g_ExclusionsActive = false;
// g_ExclusionsPopupOpen is declared earlier (near MarkConfigRestartRequired) so that function can see it.
static std::string g_ExclusionsDetailText = "";
static int g_ExclusionStep = 0;
static const char* const g_AliasIds[AI_COUNT] = {
    "OAR", "IED", "ENB", "dMenu", "ImprovedCamera", "FLICK", "DebugMenu", "KreatE", "CS", "CatMenu", "Dragonborn", "ReShade", "MF",
    "CSEditor", "CSOverlay", "CSEffect", "PartySettings", "PartySheet", "PartyInspect", "PartyCharacter", "SearchUI", "MCM", "QAR", "ModFunctionMenu", "OPS"
};
static std::atomic<bool>* const g_AliasUnblock[AI_COUNT] = {
    &g_UnblockOAR, &g_UnblockIED, &g_UnblockENB, &g_UnblockDMenu, &g_UnblockImprovedCamera,
    &g_UnblockFLICK, &g_UnblockDebugMenu, &g_UnblockKreatE, &g_UnblockCS, &g_UnblockCatMenu,
    &g_UnblockDragonborn, &g_UnblockReShade, &g_UnblockMF,
    &g_UnblockCSEditor, &g_UnblockCSOverlay, &g_UnblockCSEffect,
    &g_UnblockPartySettings, &g_UnblockPartySheet, &g_UnblockPartyInspect, &g_UnblockPartyCharacter,
    &g_UnblockSearchUI, &g_UnblockMCM, &g_UnblockQAR, &g_UnblockModFunctionMenu, &g_UnblockOPS
};
static std::atomic<int> g_CapturingAlias{ -1 }; // which alias row is in "press a key" capture (-1 = none)
static std::atomic<long long> g_KeyCaptureStartedMs{ 0 }; // when the key capturing was started (for 3s timeout)
static std::atomic<int> g_AliasTriggerMode[AI_COUNT]; // 0 = Single Press, 1 = Double Press, 2 = Hold
static std::atomic<int> g_AliasHoldDurationMs[AI_COUNT]; // hold duration in ms
static int g_ActiveHotkeyOptionsAlias = -1; // which alias index has advanced options open
static std::atomic<bool> g_OpenHotkeyOptionsPopup{ false };
static std::atomic<long long> g_SuppressAliasUntilMs{ 0 }; // brief grace after a rebind so the just-pressed key doesn't fire the mod

// True when this alias matches the launcher hotkey exactly (key + modifiers).
static bool AliasEqualsLauncher(int i) {
    return g_AliasDik[i].load() == g_LauncherHotkeyDIK.load() &&
           g_AliasCtrl[i].load() == g_LauncherHotkeyCtrl.load() &&
           g_AliasShift[i].load() == g_LauncherHotkeyShift.load() &&
           g_AliasAlt[i].load() == g_LauncherHotkeyAlt.load();
}
static bool CheckAliasTrigger(int idx, bool chordDownVal);

static bool AliasMatchesChord(int i, WORD keyDIK, WORD modifierDIK = 0) {
    const bool ctrl = modifierDIK == 0x1D || modifierDIK == 0x9D;
    const bool shift = modifierDIK == 0x2A || modifierDIK == 0x36;
    const bool alt = modifierDIK == 0x38 || modifierDIK == 0xB8;
    return g_AliasDik[i].load() == keyDIK &&
           g_AliasCtrl[i].load() == ctrl &&
           g_AliasShift[i].load() == shift &&
           g_AliasAlt[i].load() == alt;
}

static bool AliasMatchesExact(int i, WORD keyDIK, bool ctrl, bool shift, bool alt) {
    return g_AliasDik[i].load() == keyDIK &&
           g_AliasCtrl[i].load() == ctrl &&
           g_AliasShift[i].load() == shift &&
           g_AliasAlt[i].load() == alt;
}

static void InitAliasDefaults() {
    auto set = [](int i, WORD d, bool c, bool s, bool a) {
        g_AliasDik[i].store(d); g_AliasCtrl[i].store(c); g_AliasShift[i].store(s); g_AliasAlt[i].store(a);
        g_AliasTriggerMode[i].store(0); // Default to Single Press
        g_AliasHoldDurationMs[i].store(500); // default hold time
    };
    set(AI_OAR, 0x18, false, true, false);   // Shift + O
    set(AI_IED, 0x0E, false, false, false);  // Backspace
    set(AI_ENB, 0x1C, false, true, false);   // Shift + Enter
    set(AI_DMenu, 0xC7, false, false, false);// Home
    set(AI_IC, 0xC7, false, true, false);    // Shift + Home
    set(AI_FLICK, 0x41, false, false, false);// F7
    set(AI_DebugMenu, 0x3B, false, false, false); // F1
    set(AI_KreatE, 0xCF, false, false, false);// End
    set(AI_CS, 0xCF, false, false, false);   // End
    set(AI_CatMenu, 0x40, false, false, false); // F6
    set(AI_Dragonborn, 0x3B, false, false, false); // F1
    set(AI_ReShade, 0xC7, false, false, false); // Home
    set(AI_MF, 0x3D, false, false, false);      // F3
    set(AI_CSEditor, 0xCF, false, true, false); // Shift + End (CS Editor)
    set(AI_CSOverlay, 0x44, false, false, false); // F10 (CS Overlay)
    set(AI_CSEffect, 0x37, false, false, false);  // Numpad * (CS Effect)
    set(AI_PartySettings, 0x2D, false, false, false);  // X
    set(AI_PartySheet, 0x40, false, false, false);     // F6
    set(AI_PartyInspect, 0x15, false, false, false);   // Y
    set(AI_PartyCharacter, 0x16, false, false, false); // U
    set(AI_SearchUI, 0x3E, false, false, false);       // F4 (SearchUI's own default)
    set(AI_MCM, 0x00, false, false, false);            // none until the user picks one
    set(AI_QAR, 0x00, false, false, false);            // none until the user picks one
    set(AI_ModFunctionMenu, 0x3B, false, false, false); // F1 (Mod Function Menu's own default)
    set(AI_OPS, 0x4F, false, false, false);            // Numpad 1 (Outfit Preview Selector's own default)
}

static void RestoreAliasToDefault(int idx) {
    auto set = [](int i, WORD d, bool c, bool s, bool a) {
        g_AliasDik[i].store(d); g_AliasCtrl[i].store(c); g_AliasShift[i].store(s); g_AliasAlt[i].store(a);
        g_AliasTriggerMode[i].store(0);
        g_AliasHoldDurationMs[i].store(500);
    };
    switch (idx) {
        case AI_OAR: set(AI_OAR, 0x18, false, true, false); break;
        case AI_IED: set(AI_IED, 0x0E, false, false, false); break;
        case AI_ENB: set(AI_ENB, 0x1C, false, true, false); break;
        case AI_DMenu: set(AI_DMenu, 0xC7, false, false, false); break;
        case AI_IC: set(AI_IC, 0xC7, false, true, false); break;
        case AI_FLICK: set(AI_FLICK, 0x41, false, false, false); break;
        case AI_DebugMenu: set(AI_DebugMenu, 0x3B, false, false, false); break;
        case AI_KreatE: set(AI_KreatE, 0xCF, false, false, false); break;
        case AI_CS: set(AI_CS, 0xCF, false, false, false); break;
        case AI_CatMenu: set(AI_CatMenu, 0x40, false, false, false); break;
        case AI_Dragonborn: set(AI_Dragonborn, 0x3B, false, false, false); break;
        case AI_ReShade: set(AI_ReShade, 0xC7, false, false, false); break;
        case AI_MF: set(AI_MF, 0x3D, false, false, false); break;
        case AI_CSEditor: set(AI_CSEditor, 0xCF, false, true, false); break;
        case AI_CSOverlay: set(AI_CSOverlay, 0x44, false, false, false); break;
        case AI_CSEffect: set(AI_CSEffect, 0x37, false, false, false); break;
        case AI_PartySettings: set(AI_PartySettings, 0x2D, false, false, false); break;
        case AI_PartySheet: set(AI_PartySheet, 0x40, false, false, false); break;
        case AI_PartyInspect: set(AI_PartyInspect, 0x15, false, false, false); break;
        case AI_PartyCharacter: set(AI_PartyCharacter, 0x16, false, false, false); break;
        case AI_SearchUI: set(AI_SearchUI, 0x3E, false, false, false); break;
        case AI_MCM: set(AI_MCM, 0x00, false, false, false); break;
        case AI_QAR: set(AI_QAR, 0x00, false, false, false); break;
        case AI_ModFunctionMenu: set(AI_ModFunctionMenu, 0x3B, false, false, false); break;
        case AI_OPS: set(AI_OPS, 0x4F, false, false, false); break;
        default: break;
    }
}

static bool InternalENBKeyAllowed() {
    const bool ctrl = g_ENBConfig.combinationVK == VK_CONTROL;
    const bool shift = g_ENBConfig.combinationVK == VK_SHIFT;
    const bool alt = g_ENBConfig.combinationVK == VK_MENU;
    return g_UnblockENB.load() && AliasMatchesExact(AI_ENB,
        DIKFromVK(static_cast<WORD>(g_ENBConfig.editorVK)), ctrl, shift, alt);
}
static bool InternalAliasKeyAllowed(int alias, std::atomic<bool>& enabled, WORD keyDIK, WORD modifierDIK = 0) {
    return enabled.load() && AliasMatchesChord(alias, keyDIK, modifierDIK);
}
// Retained only long enough to restore Blocking for users migrating from the
// removed "Keep KreatE Open With Launcher" option.
static std::atomic<int> g_LegacyKreatEOriginalBlocking{ -1 };
static std::atomic<long long> g_LastOriginalHotkeyMs{ 0 };

static std::atomic<bool> g_WaitingForHotkeyPress{ false };
// True while the user is binding any key (launcher hotkey OR a mod alias) — suppress all
// launcher/mod hotkey actions so the key being pressed only sets the binding. Also stays true
// for a short grace window after a capture so the just-pressed (still-held) key can't toggle.
static long long NowMs();
static bool IsRebinding() {
    return g_WaitingForHotkeyPress.load() || g_CapturingAlias.load() >= 0 || NowMs() < g_SuppressAliasUntilMs.load();
}

static std::vector<std::string> g_ButtonOrder = {
    "MF", "MCM", "SearchUI", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "KreatE", "CS", "PartySheet", "CatMenu", "Dragonborn", "ReShade", "QAR", "ModFunctionMenu", "OPS"
};
// Every launcher button id that should exist in the order. On load we append any missing from a
// user's saved order so newly added buttons (e.g. QAR) still show up for existing installs.
static const std::vector<std::string> kAllButtonIds = {
    "MF", "MCM", "SearchUI", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "KreatE", "CS", "PartySheet", "CatMenu", "Dragonborn", "ReShade", "QAR", "ModFunctionMenu", "OPS"
};

static void SaveButtonOrder() {
    const std::filesystem::path iniPath = "Data/SKSE/Plugins/RisaAllInOneMenu.ini";
    std::ofstream outfile(iniPath, std::ios::trunc);
    if (outfile.is_open()) {
        outfile << "[General]\n";
        outfile << "ButtonOrder = ";
        for (size_t i = 0; i < g_ButtonOrder.size(); ++i) {
            outfile << g_ButtonOrder[i];
            if (i + 1 < g_ButtonOrder.size()) {
                outfile << ",";
            }
        }
        outfile << "\n";
        outfile << "Language = " << g_Language << "\n";
        outfile << "FontScale = " << g_LauncherFontScale.load() << "\n";
        outfile << "TextScale = " << g_LauncherTextScale.load() << "\n";
        outfile << "WindowPosX = " << g_WindowPosX.load() << "\n";
        outfile << "WindowPosY = " << g_WindowPosY.load() << "\n";
        outfile << "SettingsHeight = " << g_SettingsWindowHeight.load() << "\n";
        outfile << "HotkeyDIK = " << g_LauncherHotkeyDIK.load() << "\n";
        outfile << "HotkeyCtrl = " << (g_LauncherHotkeyCtrl.load() ? 1 : 0) << "\n";
        outfile << "HotkeyShift = " << (g_LauncherHotkeyShift.load() ? 1 : 0) << "\n";
        outfile << "HotkeyAlt = " << (g_LauncherHotkeyAlt.load() ? 1 : 0) << "\n";
        outfile << "HotkeyDoubleTap = " << (g_LauncherHotkeyDoubleTap.load() ? 1 : 0) << "\n";
        outfile << "HotkeyHold = " << (g_LauncherHotkeyHold.load() ? 1 : 0) << "\n";
        outfile << "HotkeyHoldMs = " << g_LauncherHoldDurationMs.load() << "\n";
        outfile << "HotkeyEasyClose = " << (g_LauncherHotkeyEasyClose.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalOAR = " << (g_UnblockOAR.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalIED = " << (g_UnblockIED.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalENB = " << (g_UnblockENB.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalDebugMenu = " << (g_UnblockDebugMenu.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalDMenu = " << (g_UnblockDMenu.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalImprovedCamera = " << (g_UnblockImprovedCamera.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalFLICK = " << (g_UnblockFLICK.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalKreatE = " << (g_UnblockKreatE.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalCS = " << (g_UnblockCS.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalCatMenu = " << (g_UnblockCatMenu.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalDragonborn = " << (g_UnblockDragonborn.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalReShade = " << (g_UnblockReShade.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalMF = " << (g_UnblockMF.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalSearchUI = " << (g_UnblockSearchUI.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalMCM = " << (g_UnblockMCM.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalQAR = " << (g_UnblockQAR.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalModFunctionMenu = " << (g_UnblockModFunctionMenu.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalOPS = 0\n";
        outfile << "EnableLogging = " << (g_LoggingEnabled.load() ? 1 : 0) << "\n";
        outfile << "EnableFileChangeLog = " << (g_FileChangeLogEnabled.load() ? 1 : 0) << "\n";
        outfile << "AllowOpenInConsole = " << (g_AllowOpenInConsole.load() ? 1 : 0) << "\n";
        outfile << "KeepGameRunning = " << (g_KeepGameRunning.load() ? 1 : 0) << "\n";
        outfile << "RememberSubView = " << (g_RememberSubView.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalCSEditor = " << (g_UnblockCSEditor.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalCSOverlay = " << (g_UnblockCSOverlay.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalCSEffect = " << (g_UnblockCSEffect.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalPartySettings = " << (g_UnblockPartySettings.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalPartySheet = " << (g_UnblockPartySheet.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalPartyInspect = " << (g_UnblockPartyInspect.load() ? 1 : 0) << "\n";
        outfile << "EnableOriginalPartyCharacter = " << (g_UnblockPartyCharacter.load() ? 1 : 0) << "\n";
        outfile << "ShowExclusionsTab = " << (g_ShowExclusionsTab.load() ? 1 : 0) << "\n";
        outfile << "HideExcludedInLauncher = " << (g_HideExcludedInLauncher.load() ? 1 : 0) << "\n";
        {
            std::string exclList = "";
            bool first = true;
            for (int i = 0; i < AI_COUNT; ++i) {
                if (g_ExcludeMod[i].load()) {
                    if (!first) exclList += ",";
                    exclList += g_AliasIds[i];
                    first = false;
                }
            }
            outfile << "ExclusionList = " << exclList << "\n";
        }
        for (int i = 0; i < AI_COUNT; ++i) {
            outfile << "Alias" << g_AliasIds[i] << " = " << g_AliasDik[i].load() << ","
                    << (g_AliasCtrl[i].load() ? 1 : 0) << "," << (g_AliasShift[i].load() ? 1 : 0) << ","
                    << (g_AliasAlt[i].load() ? 1 : 0) << "," << g_AliasTriggerMode[i].load() << ","
                    << g_AliasHoldDurationMs[i].load() << "\n";
        }
        outfile.close();
        SKSE::log::info("SaveButtonOrder: Successfully saved button order and settings.");
    } else {
        SKSE::log::error("SaveButtonOrder: Failed to open ini for writing.");
    }
}

static std::vector<std::string> SplitStr(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        std::string trimmed = TrimStr(item);
        if (!trimmed.empty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

static void LoadButtonOrder() {
    InitAliasDefaults(); // seed alias keys with each mod's original; file values below override
    const std::filesystem::path iniPath = "Data/SKSE/Plugins/RisaAllInOneMenu.ini";
    std::vector<std::string> loaded;
    if (std::filesystem::exists(iniPath)) {
        std::ifstream infile(iniPath);
        if (infile.is_open()) {
            std::string line;
            while (std::getline(infile, line)) {
                auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
                auto eq = line.find('='); if (eq == std::string::npos) continue;
                std::string key = ToUpper(TrimStr(line.substr(0, eq)));
                std::string val = TrimStr(line.substr(eq + 1));
                if (key == "BUTTONORDER") {
                    loaded = SplitStr(val, ',');
                } else if (key == "LANGUAGE") {
                    g_Language = SanitizeLanguageCode(val);
                } else if (key == "FONTSCALE") {
                    try {
                        g_LauncherFontScale.store(std::stof(val));
                    } catch (...) {}
                } else if (key == "TEXTSCALE") {
                    try {
                        g_LauncherTextScale.store(std::stof(val));
                    } catch (...) {}
                } else if (key == "WINDOWPOSX") {
                    try { g_WindowPosX.store(std::stof(val)); } catch (...) {}
                } else if (key == "WINDOWPOSY") {
                    try { g_WindowPosY.store(std::stof(val)); } catch (...) {}
                } else if (key == "SETTINGSHEIGHT") {
                    try { g_SettingsWindowHeight.store(std::stof(val)); } catch (...) {}
                } else if (key == "HOTKEYDIK") {
                    try {
                        WORD dik = (WORD)std::stoi(val);
                        g_LauncherHotkeyDIK.store(dik);
                        g_LauncherHotkeyVK.store(VKFromDIK(dik));
                    } catch (...) {}
                } else if (key == "HOTKEYCTRL") {
                    try { g_LauncherHotkeyCtrl.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HOTKEYSHIFT") {
                    try { g_LauncherHotkeyShift.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HOTKEYALT") {
                    try { g_LauncherHotkeyAlt.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HOTKEYDOUBLETAP") {
                    try { g_LauncherHotkeyDoubleTap.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HOTKEYHOLD") {
                    try { g_LauncherHotkeyHold.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HOTKEYHOLDMS") {
                    try { g_LauncherHoldDurationMs.store(std::stoi(val)); } catch (...) {}
                } else if (key == "HOTKEYEASYCLOSE") {
                    try { g_LauncherHotkeyEasyClose.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALOAR" || key == "UNBLOCKOAR") {
                    try { g_UnblockOAR.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALIED" || key == "UNBLOCKIED") {
                    try { g_UnblockIED.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALENB" || key == "UNBLOCKENB") {
                    try { g_UnblockENB.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALDEBUGMENU" || key == "UNBLOCKDEBUGMENU") {
                    try { g_UnblockDebugMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALDMENU" || key == "UNBLOCKDMENU") {
                    try { g_UnblockDMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALIMPROVEDCAMERA" || key == "UNBLOCKIMPROVEDCAMERA") {
                    try { g_UnblockImprovedCamera.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALFLICK" || key == "UNBLOCKFLICK") {
                    try { g_UnblockFLICK.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALKREATE" || key == "UNBLOCKKREATE") {
                    try { g_UnblockKreatE.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALCS") {
                    try { g_UnblockCS.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALCATMENU") {
                    try { g_UnblockCatMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALDRAGONBORN") {
                    try { g_UnblockDragonborn.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALRESHADE") {
                    try { g_UnblockReShade.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALMF") {
                    try { g_UnblockMF.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALSEARCHUI") {
                    try { g_UnblockSearchUI.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALMCM") {
                    try { g_UnblockMCM.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALQAR") {
                    try { g_UnblockQAR.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALMODFUNCTIONMENU") {
                    try { g_UnblockModFunctionMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALOPS") {
                    g_UnblockOPS.store(false);
                } else if (key == "ENABLELOGGING") {
                    try { g_LoggingEnabled.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEFILECHANGELOG") {
                    try { g_FileChangeLogEnabled.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ALLOWOPENINCONSOLE") {
                    try { g_AllowOpenInConsole.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "KEEPGAMERUNNING") {
                    try { g_KeepGameRunning.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "REMEMBERSUBVIEW") {
                    try { g_RememberSubView.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALCSEDITOR") {
                    try { g_UnblockCSEditor.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALCSOVERLAY") {
                    try { g_UnblockCSOverlay.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALCSEFFECT") {
                    try { g_UnblockCSEffect.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALPARTYSETTINGS") {
                    try { g_UnblockPartySettings.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALPARTYSHEET") {
                    try { g_UnblockPartySheet.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALPARTYINSPECT") {
                    try { g_UnblockPartyInspect.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "ENABLEORIGINALPARTYCHARACTER") {
                    try { g_UnblockPartyCharacter.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "SHOWEXCLUSIONSTAB") {
                    try { g_ShowExclusionsTab.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "HIDEEXCLUDEDINLAUNCHER") {
                    try { g_HideExcludedInLauncher.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "EXCLUSIONLIST") {
                    for (int i = 0; i < AI_COUNT; ++i) g_ExcludeMod[i].store(false);
                    std::stringstream ss(val);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        token = TrimStr(token);
                        for (int i = 0; i < AI_COUNT; ++i) {
                            if (ToUpper(g_AliasIds[i]) == ToUpper(token)) {
                                g_ExcludeMod[i].store(true);
                                break;
                            }
                        }
                    }
                } else if (key.rfind("ALIAS", 0) == 0) {
                    const std::string rest = key.substr(5);
                    for (int i = 0; i < AI_COUNT; ++i) {
                        if (ToUpper(g_AliasIds[i]) == rest) {
                            const auto parts = SplitStr(val, ',');
                            try {
                                if (parts.size() >= 1) g_AliasDik[i].store(static_cast<WORD>(std::stoi(parts[0])));
                                if (parts.size() >= 4) {
                                    g_AliasCtrl[i].store(std::stoi(parts[1]) != 0);
                                    g_AliasShift[i].store(std::stoi(parts[2]) != 0);
                                    g_AliasAlt[i].store(std::stoi(parts[3]) != 0);
                                }
                                if (parts.size() >= 5) {
                                    g_AliasTriggerMode[i].store(std::stoi(parts[4]));
                                } else {
                                    g_AliasTriggerMode[i].store(0);
                                }
                                if (parts.size() >= 6) {
                                    g_AliasHoldDurationMs[i].store(std::stoi(parts[5]));
                                }
                            } catch (...) {}
                            break;
                        }
                    }
                } else if (key == "KREATEORIGINALBLOCKING") {
                    try { g_LegacyKreatEOriginalBlocking.store(std::stoi(val)); } catch (...) {}
                }
            }
            infile.close();
        }
    }

    std::vector<std::string> defaultOrder = kAllButtonIds; // includes newer buttons (QAR) so they get appended

    std::vector<std::string> newOrder;
    for (const auto& id : loaded) {
        if (std::find(defaultOrder.begin(), defaultOrder.end(), id) != defaultOrder.end()) {
            if (std::find(newOrder.begin(), newOrder.end(), id) == newOrder.end()) {
                newOrder.push_back(id);
            }
        }
    }

    for (const auto& id : defaultOrder) {
        if (std::find(newOrder.begin(), newOrder.end(), id) == newOrder.end()) {
            newOrder.push_back(id);
        }
    }

    g_ButtonOrder = newOrder;
    ApplyLogLevel(); // honor the saved logging toggle
    LoadTranslations();
    SKSE::log::info("LoadButtonOrder: Loaded button order successfully.");
}

// ============================================================================
// State
// ============================================================================
static SKSEMenuFramework::Model::InputEvent* g_FrameworkInputEvent = nullptr;
static std::atomic<bool> g_WaitForLauncherKeyRelease{ false };
static std::atomic<bool> g_AllowIEDOpen{ false };
static std::atomic<int> g_AllowIEDOpenPolls{ 0 };
static std::atomic<long long> g_AllowIEDOpenUntilMs{ 0 };

static std::atomic<bool> g_AllowENBOpen{ false };
static std::atomic<long long> g_AllowENBOpenUntilMs{ 0 };
static std::atomic<long long> g_ENBOpenRequestedMs{ 0 };

static std::atomic<bool> g_AllowDebugMenuOpen{ false };
static std::atomic<long long> g_AllowDebugMenuOpenUntilMs{ 0 };
static std::atomic<int> g_DebugMenuPassCount{ 0 };
static std::atomic<bool> g_AllowCSOpen{ false };
static std::atomic<long long> g_AllowCSOpenUntilMs{ 0 };
static std::array<std::atomic<int>, 4> g_CSKeyPassCount{}; // main, editor, overlay, effect
static std::atomic<bool> g_AllowESCSinkBlock{ false };
// Keep stripping ESC from the game for a short window AFTER we close an ESC-ignoring menu on ESC.
// Otherwise the strip lifts the instant the menu closes (g_ActiveMenu -> None) while ESC is still
// physically held, and that residual ESC pops Skyrim's system menu one frame later.
static std::atomic<long long> g_SuppressEscUntilMs{ 0 };

// These menus ignore ESC (they don't close on it) and let ESC fall through to the game, which pops
// Skyrim's journal/pause menu. While one of them is the active menu we strip ESC from the game's
// input reads so that can't happen; our physical-ESC handler (which reads the key at OS level, so
// it's unaffected by this strip) still sees ESC and closes the menu through its own path.
static bool EscBlockedForActiveMenu() {
    switch (g_ActiveMenu.load()) {
    case ActiveMenu::DebugMenu:
    case ActiveMenu::ENB:
    case ActiveMenu::KreatE:
    case ActiveMenu::Dragonborn:
    case ActiveMenu::CatMenu:
    case ActiveMenu::DMenu:  // stock dMenu / dMenu NG leak ESC to the game (vanilla menu) when we close
                             // them; strip ESC from the game while open. v2 "Risa dmenu" is unaffected.
        return true;
    default:
        return false;
    }
}

// After a mod is opened from a launcher button it can take up to ~1s to actually appear.
// Pressing the launcher key during that window desyncs our open/closed tracking (we'd
// "close" a menu that hasn't finished opening). Ignore the launcher key until this time.
static std::atomic<long long> g_MenuOpenLockUntilMs{ 0 };

static std::atomic<bool> g_AllowDMenuOpen{ false };
static std::atomic<long long> g_AllowDMenuOpenUntilMs{ 0 };

static std::atomic<bool> g_AllowImprovedCameraOpen{ false };
static std::atomic<long long> g_AllowImprovedCameraOpenUntilMs{ 0 };

static std::atomic<bool> g_AllowFLICKOpen{ false };
static std::atomic<long long> g_AllowFLICKOpenUntilMs{ 0 };
static std::atomic<bool> g_FLICKHotkeyManaged{ false };
static std::atomic<long long> g_LastFLICKHotkeyAttemptMs{ 0 };
static std::atomic<long long> g_FLICKHotkeyReassertUntilMs{ 0 };
static std::atomic<bool> g_FLICKKeyRelocated{ false }; // ini toggle key parked on F14 (frees F7)

static std::atomic<bool> g_AllowKreatEOpen{ false };
static std::atomic<long long> g_AllowKreatEOpenUntilMs{ 0 };
static std::atomic<bool> g_KreatEIniManaged{ false };
static std::atomic<long long> g_LastKreatEIniAttemptMs{ 0 };

static std::atomic<bool> g_CatMenuIniManaged{ false };
static std::atomic<long long> g_LastCatMenuIniAttemptMs{ 0 };
static std::atomic<bool> g_ModFunctionMenuIniManaged{ false };
static std::atomic<long long> g_LastModFunctionMenuIniAttemptMs{ 0 };
static std::atomic<bool> g_OPSKeyManaged{ false };
static std::atomic<long long> g_LastOPSKeyAttemptMs{ 0 };

static std::atomic<bool> g_AllowDragonbornOpen{ false };
static std::atomic<long long> g_AllowDragonbornOpenUntilMs{ 0 };
static std::atomic<bool> g_AllowSearchUIOpen{ false };
static std::atomic<long long> g_AllowSearchUIOpenUntilMs{ 0 };
// True when we successfully registered as a ReShade add-on (6.x add-on build present). Only the
// add-on build is supported: ReShade's overlay is driven via RisaReShade::SetOverlayOpen and its
// overlay hotkey is disabled in ReShade.ini (Home + F22 both freed). When false (non-add-on build),
// ReShade is left completely unmanaged - not relocated, not opened from the launcher.
static std::atomic<bool> g_ReShadeAddonActive{ false };
static std::atomic<bool> g_DragonbornIniManaged{ false };
static std::atomic<long long> g_LastDragonbornIniAttemptMs{ 0 };

static bool IsSearchUIAvailable();
extern std::atomic<int> g_SearchUIEffectiveDIK;

static constexpr WORD kEscapeDIK   = 0x01;  // ESC scan code
static constexpr WORD kTabDIK      = 0x0F;  // TAB scan code

static long long NowMs();
static bool CheckLauncherDoubleTap(long long now);
static void InjectEngineKey(WORD dik, WORD modifier = 0);
static void AddScanInput(INPUT& input, WORD scanCode, bool keyUp);
static void CloseLauncher(bool external = false);
static void ForceCloseSKSEMenuFramework();
static void ToggleLauncher();
static void SyncDMenuKeyViaApi();
static void SyncSearchUIKey();
static void TryManageFLICKHotkey(bool lateRetry = false);
static bool IsGameLoaded();
static void OpenMCM();
static void SimulateModifiedKey(WORD modifierDIK, WORD keyDIK);
static void PostKeyToGameWindow(WORD vk);
static void PostKeyWithScanToGameWindow(WORD vk, WORD scan);
static void TryHookModSinks();
static void TryHookCatMenuNativeKey();
static FLICKInterface* GetFLICKInterface();
static bool SetFLICKHotkeyEnabled(bool enabled);
static bool IsENBOpeningTransition();
static bool DispatchKeyToCSSink(WORD dik);
static void CloseActiveModMenu(ActiveMenu active);
static void OpenAnimationReplacer();
static void OpenImmersiveEquipmentDisplays();
static bool SetIEDOpen(bool open);
static bool IsIEDOpen();
static void OpenENB();
static void OpenDebugMenu();
static void OpenSKSEMenuFramework();
static void OpenDMenu();
static void OpenImprovedCamera();
static bool SetImprovedCameraOpen(bool open);
static bool IsImprovedCameraOpen();
static bool ToggleImprovedCameraWithRegisteredCommand();
static bool ToggleImprovedCameraWithRegisteredCommand();
static bool DispatchManagedSinkToggle(int which);
static void ArmCSKeyPass(int index);
static void OpenFLICK();
static void OpenKreatE();
static void OpenCommunityShaders();
static void OpenCSEditor();
static void OpenCSOverlay();
static void OpenCSEffect();
static void EnterPartySheetHub();
static void OpenPartySettings();
static void OpenPartySheet();
static void OpenPartyInspect();
static void OpenPartyCharacter();
static bool DispatchPartySheetKey(WORD dik);
static bool ShowPartyInspectForCrosshair();
static void OpenCatMenu();
static void OpenDragonbornToolkit();
static void OpenReShade();
static void OpenQAR();
static bool IsQARInstalled();
static void OpenModFunctionMenu();
static void LoadModFunctionMenuConfig();
static void TryManageModFunctionMenuHotkey(bool lateRetry = false);
static void OpenOPS();
static bool IsOPSInstalled();
static void LoadOPSConfig();
static void TryManageOPSHotkey(bool lateRetry = false);
static bool SetOPSHotkey(int keyCode);
static bool SetOAROpen(bool open);
static bool SetCatMenuOpen(bool open);
static bool SetDragonbornOpen(bool open);
static void TryManageKreatEHotkey(bool lateRetry = false);
static void TryManageCSHotkey(bool lateRetry = false);
static void TryManageCatMenuHotkey(bool lateRetry = false);
static void TryManageDragonbornHotkey(bool lateRetry = false);
static void RestoreLegacyKreatEBlocking();
static bool RestoreAllModDefaults(bool forceModDefaults);
static void RequestGameExit();

// ----------------------------------------------------------------------------
// Game-window state used to keep Win32 filtering scoped to Skyrim.
// Managed menu keys remain in Skyrim's InputEvent stream and are filtered only when
// their owning mod processes them.
// Targeted sink and caller hooks keep those keys available to gameplay bindings.
// Injected inputs from launcher buttons pass through short per-mod allow windows.
static std::atomic<HWND>  g_GameHWND{ nullptr };
// Improved Camera reads its chord through GetAsyncKeyState.
// Its polling hook is caller-scoped so other consumers still see the key.

// WndProc subclass state.
//   g_OrigWndProc  — the proc our FRONT instance chains down to (updated when we
//                    re-assert to the front after another mod chained on top of us).
//   g_GameWndProc  — the ORIGINAL game proc, captured once. Used as a fixed
//                    loop-breaker: when our proc is re-entered via the chain (a mod
//                    chained on top of our old proc), we jump straight here instead
//                    of looping back through the chain.
//   g_WndReentry   — per-thread re-entrancy depth guard.
static bool IsCallerModule(void* returnAddr, const char* moduleName); // defined below

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t g_OrigGetRawInputData = nullptr;
static std::atomic<bool> g_RawCtrlDown{ false };
static std::atomic<bool> g_RawShiftDown{ false };
static std::atomic<bool> g_RawAltDown{ false };
static std::array<std::atomic<bool>, 256> g_RawKeyDown{};
static std::atomic<long long> g_NeutralizeCSModifiersUntilMs{ 0 };

static UINT WINAPI HookedGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    UINT res = g_OrigGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (res != (UINT)-1 && uiCommand == RID_INPUT && pData && cbSizeHeader == sizeof(RAWINPUTHEADER)) {
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            USHORT vkey = raw->data.keyboard.VKey;
            USHORT flags = raw->data.keyboard.Flags;
            bool isDown = (flags & RI_KEY_BREAK) == 0;
            const bool firstDown = vkey < g_RawKeyDown.size() && isDown && !g_RawKeyDown[vkey].exchange(true);
            if (vkey < g_RawKeyDown.size() && !isDown) g_RawKeyDown[vkey].store(false);
            if (vkey == VK_CONTROL || vkey == VK_LCONTROL || vkey == VK_RCONTROL) g_RawCtrlDown.store(isDown);
            if (vkey == VK_SHIFT || vkey == VK_LSHIFT || vkey == VK_RSHIFT) g_RawShiftDown.store(isDown);
            if (vkey == VK_MENU || vkey == VK_LMENU || vkey == VK_RMENU) g_RawAltDown.store(isDown);
            // A modifier key-up can be missed if the game loses focus while it's held (Alt+Tab, etc.),
            // leaving the sticky flag stuck "down" so every modifier-chord alias afterward reads a
            // phantom modifier and never matches. Re-sync all three from the real OS state each
            // keyboard event so they self-correct.
            if (g_OrigGetAsyncKeyState) {
                g_RawCtrlDown.store((g_OrigGetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                g_RawShiftDown.store((g_OrigGetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                g_RawAltDown.store((g_OrigGetAsyncKeyState(VK_MENU) & 0x8000) != 0);
            }

            // IED reads WM_INPUT raw keyboard data directly (separate from DirectInput's
            // curState, the event-sink stream, and WM_KEYDOWN) — none of our other blocks
            // touch this channel. Hide the toggle key ONLY from IED's own read (caller-scoped,
            // like the GetAsyncKeyState/GetKeyState blocks) so the key stays fully alive for the
            // game, our launcher, and IED itself when the user enables the original hotkey.
            if (g_IEDConfig.enabled && vkey == g_IEDConfig.toggleVK &&
                !InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK)) {
                const bool allowed = g_AllowIEDOpen.load() && NowMs() <= g_AllowIEDOpenUntilMs.load();
                if (!allowed && IsCallerModule(_ReturnAddress(), "ImmersiveEquipmentDisplays")) {
                    raw->data.keyboard.VKey = 0xFF; // invalid VK — hidden from IED's raw-input read only
                    raw->data.keyboard.MakeCode = 0;
                }
            }

            const auto canOpenModal = [&]() {
                return g_ActiveMenu.load() == ActiveMenu::None &&
                    !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                    !IsExternalMenuOpen() && !IsENBOpeningTransition() &&
                    NowMs() >= g_MenuOpenLockUntilMs.load();
            };
            const auto runCSAlias = [&](int action) {
                switch (action) {
                    case 1:
                        if (g_UnblockCS.load() && (g_ActiveMenu.load() == ActiveMenu::CS || canOpenModal()))
                            OpenCommunityShaders();
                        break;
                    case 2:
                        if (!g_UnblockCSEditor.load()) break;
                        if (g_ActiveMenu.load() == ActiveMenu::CS && g_ActiveCSSub.load() == 1)
                            CloseActiveModMenu(ActiveMenu::CS);
                        else if (canOpenModal()) OpenCSEditor();
                        break;
                    case 3:
                        if (g_UnblockCSOverlay.load() && canOpenModal()) OpenCSOverlay();
                        break;
                    case 4:
                        if (g_UnblockCSEffect.load() && canOpenModal()) OpenCSEffect();
                        break;
                }
            };

            // CS and Improved Camera aliases are owned here, at the real keyboard make edge.
            // Their old DirectInput polling path saw several consumer snapshots for one press,
            // causing repeats and making modifier chords unreliable.
            if (firstDown && !IsRebinding() && !IsUserTyping() && NowMs() >= g_SuppressAliasUntilMs.load()) {
                const auto aliasMatchesRaw = [&](int i) {
                    return vkey == VKFromDIK(g_AliasDik[i].load()) &&
                        g_RawCtrlDown.load() == g_AliasCtrl[i].load() &&
                        g_RawShiftDown.load() == g_AliasShift[i].load() &&
                        g_RawAltDown.load() == g_AliasAlt[i].load() && !AliasEqualsLauncher(i);
                };
                const auto aliasChordCompletedRaw = [&](int i) {
                    const WORD keyVK = VKFromDIK(g_AliasDik[i].load());
                    const bool keyDown = keyVK < g_RawKeyDown.size() && g_RawKeyDown[keyVK].load();
                    const bool eventCompletesChord = vkey == keyVK ||
                        (g_AliasCtrl[i].load() && (vkey == VK_CONTROL || vkey == VK_LCONTROL || vkey == VK_RCONTROL)) ||
                        (g_AliasShift[i].load() && (vkey == VK_SHIFT || vkey == VK_LSHIFT || vkey == VK_RSHIFT)) ||
                        (g_AliasAlt[i].load() && (vkey == VK_MENU || vkey == VK_LMENU || vkey == VK_RMENU));
                    return eventCompletesChord && keyDown &&
                        g_RawCtrlDown.load() == g_AliasCtrl[i].load() &&
                        g_RawShiftDown.load() == g_AliasShift[i].load() &&
                        g_RawAltDown.load() == g_AliasAlt[i].load() && !AliasEqualsLauncher(i);
                };
                const auto runCSAliasWithModifierIsolation = [&](int action, int idx) {
                    const bool hasModifier = g_AliasCtrl[idx].load() || g_AliasShift[idx].load() || g_AliasAlt[idx].load();
                    if (hasModifier) {
                        // CS evaluates its InputCombo on the hidden key's release and asks
                        // GetAsyncKeyState for modifiers. Hide the physical alias modifiers only
                        // from CommunityShaders.dll while that hidden press is in flight.
                        g_NeutralizeCSModifiersUntilMs.store(NowMs() + 500);
                        SKSE::log::info("Raw input: Community Shaders action {} isolating physical modifiers.", action);
                    }
                    runCSAlias(action);
                };

                const auto logAliasProbe = [&](const char* name, int i, bool enabled) {
                    if (vkey != VKFromDIK(g_AliasDik[i].load())) return;
                    SKSE::log::info(
                        "Alias probe [{}]: enabled={}, required={}{}{}{}, observed={}{}{}{}, exact={}, launcherConflict={}, active={}, lockMs={}.",
                        name, enabled,
                        g_AliasCtrl[i].load() ? "Ctrl+" : "", g_AliasShift[i].load() ? "Shift+" : "",
                        g_AliasAlt[i].load() ? "Alt+" : "", NameFromDIK(g_AliasDik[i].load()),
                        g_RawCtrlDown.load() ? "Ctrl+" : "", g_RawShiftDown.load() ? "Shift+" : "",
                        g_RawAltDown.load() ? "Alt+" : "", NameFromDIK(g_AliasDik[i].load()),
                        aliasMatchesRaw(i), AliasEqualsLauncher(i), static_cast<int>(g_ActiveMenu.load()),
                        std::max<long long>(0, g_MenuOpenLockUntilMs.load() - NowMs()));
                };
                logAliasProbe("ImprovedCamera", AI_IC, g_UnblockImprovedCamera.load());
                logAliasProbe("CS", AI_CS, g_UnblockCS.load());
                logAliasProbe("CSEditor", AI_CSEditor, g_UnblockCSEditor.load());
                logAliasProbe("CSOverlay", AI_CSOverlay, g_UnblockCSOverlay.load());
                logAliasProbe("CSEffect", AI_CSEffect, g_UnblockCSEffect.load());

                if (g_UnblockImprovedCamera.load() && g_ImprovedCameraConfig.enabled && CheckAliasTrigger(AI_IC, aliasMatchesRaw(AI_IC))) {
                    if (g_ActiveMenu.load() == ActiveMenu::ImprovedCamera || IsImprovedCameraOpen() || canOpenModal()) {
                        SKSE::log::info("Raw input: Improved Camera alias edge.");
                        OpenImprovedCamera();
                    }
                } else if (g_UnblockCS.load() && g_CSConfig.enabled && CheckAliasTrigger(AI_CS, aliasMatchesRaw(AI_CS))) {
                    if (g_ActiveMenu.load() == ActiveMenu::CS || canOpenModal()) {
                        SKSE::log::info("Raw input: Community Shaders main alias edge.");
                        runCSAliasWithModifierIsolation(1, AI_CS);
                    }
                } else if (g_UnblockCSEditor.load() && g_CSConfig.enabled && CheckAliasTrigger(AI_CSEditor, aliasChordCompletedRaw(AI_CSEditor))) {
                    if ((g_ActiveMenu.load() == ActiveMenu::CS && g_ActiveCSSub.load() == 1) || canOpenModal()) {
                        SKSE::log::info("Raw input: Community Shaders Editor alias edge.");
                        runCSAliasWithModifierIsolation(2, AI_CSEditor);
                    }
                } else if (g_UnblockCSOverlay.load() && g_CSConfig.enabled && CheckAliasTrigger(AI_CSOverlay, aliasMatchesRaw(AI_CSOverlay))) {
                    if (canOpenModal()) {
                        SKSE::log::info("Raw input: Community Shaders Overlay alias edge.");
                        runCSAliasWithModifierIsolation(3, AI_CSOverlay);
                    }
                } else if (g_UnblockCSEffect.load() && g_CSConfig.enabled && CheckAliasTrigger(AI_CSEffect, aliasMatchesRaw(AI_CSEffect))) {
                    if (canOpenModal()) {
                        SKSE::log::info("Raw input: Community Shaders Effect alias edge.");
                        runCSAliasWithModifierIsolation(4, AI_CSEffect);
                    }
                }
            }
            if (vkey == g_LauncherHotkeyVK.load() && isDown && !IsRebinding()) {
                const auto activeMenu = g_ActiveMenu.load();
                if (activeMenu == ActiveMenu::ReShade || activeMenu == ActiveMenu::IED) {
                    const auto now = NowMs();
                    const bool modifiersMatch =
                        g_RawCtrlDown.load() == g_LauncherHotkeyCtrl.load() &&
                        g_RawShiftDown.load() == g_LauncherHotkeyShift.load() &&
                        g_RawAltDown.load() == g_LauncherHotkeyAlt.load();
                    bool gestureComplete = g_LauncherHotkeyEasyClose.load();
                    if (!gestureComplete && modifiersMatch && !g_LauncherHotkeyHold.load()) {
                        gestureComplete = g_LauncherHotkeyDoubleTap.load()
                            ? CheckLauncherDoubleTap(now)
                            : now - g_LastLauncherToggleMs.load() > 700;
                    }
                    if (gestureComplete && now - g_LastLauncherToggleMs.load() > 200) {
                        g_LastLauncherToggleMs.store(now);
                        std::thread([activeMenu]() {
                            CloseActiveModMenu(activeMenu);
                        }).detach();
                        SKSE::log::info("HookedGetRawInputData: launcher gesture detected, closing {}.",
                            activeMenu == ActiveMenu::IED ? "IED" : "ReShade");
                    }
                }
            }

            // "Enable default hotkey" for ReShade: bridge its (rebindable) alias key live (no restart).
            // ReShade's overlay key is disabled via the add-on API, so the alias does nothing natively —
            // we forward it here (the raw-input channel fires under ReShade whether the overlay is open or not).
            const WORD reshadeAliasVK = VKFromDIK(g_AliasDik[AI_ReShade].load());
            const bool reshadeModsOk = g_RawCtrlDown.load() == g_AliasCtrl[AI_ReShade].load() &&
                                       g_RawShiftDown.load() == g_AliasShift[AI_ReShade].load() &&
                                       g_RawAltDown.load() == g_AliasAlt[AI_ReShade].load();
            static std::atomic<long long> s_lastReShadeAliasMs{ 0 };
            // The modifier requirement is dropped from the gate: while ReShade's overlay is open it
            // captures input and the tracked modifier state reads false, which broke closing a
            // modifier-chord alias. OPEN still requires the exact chord (reshadeModsOk) so it can't
            // fire accidentally; CLOSE only needs the base key, since the overlay is already up.
            if (reshadeAliasVK != 0 && vkey == reshadeAliasVK && isDown && !IsRebinding() &&
                !AliasEqualsLauncher(AI_ReShade) && g_UnblockReShade.load() && g_ReShadeConfig.enabled) {
                const auto now = NowMs();
                if (now - s_lastReShadeAliasMs.load() > 700) {
                    const auto active = g_ActiveMenu.load();
                    if (active == ActiveMenu::ReShade) {
                        s_lastReShadeAliasMs.store(now);
                        std::thread([]() { CloseActiveModMenu(ActiveMenu::ReShade); }).detach();
                        SKSE::log::info("HookedGetRawInputData: ReShade alias closing ReShade (base key).");
                    } else if (active == ActiveMenu::None && reshadeModsOk && !IsExternalMenuOpen() &&
                               now >= g_MenuOpenLockUntilMs.load()) {
                        s_lastReShadeAliasMs.store(now);
                        std::thread([]() { OpenReShade(); }).detach();
                        SKSE::log::info("HookedGetRawInputData: ReShade alias opening ReShade.");
                    }
                }
            }
        }
    }
    return res;
}

static std::atomic<WNDPROC> g_OrigWndProc{ nullptr };
static std::atomic<WNDPROC> g_GameWndProc{ nullptr };
static thread_local int g_WndReentry = 0;

static bool IsENBCombinationDown() {
    if (!g_ENBConfig.enabled) return false;
    if (g_ENBConfig.combinationVK == 0) return true;
    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };
    return (keyState(g_ENBConfig.combinationVK) & 0x8000) != 0;
}

static bool IsENBOpeningTransition() {
    const long long requested = g_ENBOpenRequestedMs.load();
    if (requested == 0) return false;

    if (g_ActiveMenu.load() != ActiveMenu::ENB) {
        g_ENBOpenRequestedMs.store(0);
        return false;
    }

    const long long elapsed = NowMs() - requested;
    if (elapsed < 3000) return true;

    g_ENBOpenRequestedMs.store(0);
    SKSE::log::warn("ENB opening transition timed out after {} ms; F1 restored.", elapsed);
    return false;
}

static void CloseActiveModMenu(ActiveMenu active) {
    SKSE::log::info("CloseActiveModMenu: active={}, console={}.",
        static_cast<int>(active), IsConsoleOpen() ? "OPEN" : "closed");
    if (active == ActiveMenu::OAR) {
        if (SetOAROpen(false)) {
            SKSE::log::info("CloseActiveModMenu: closed OAR directly.");
        }
    } else if (active == ActiveMenu::IED) {
        // Close via simulated ESC — IED's own native close, which is proven to work. We do NOT use
        // the native task-stop flag (SetIEDOpen(false)): that offset is unverified, doesn't actually
        // close IED on this build, and always "succeeds" silently, leaving IED on screen while our
        // state says closed. Open + is-open still use the verified native adapter; only close is ESC.
        std::thread([]() {
            // NOTE: do NOT set g_AllowESCSinkBlock here — IED reads ESC from the buffered input,
            // and that block strips ESC, preventing it from closing. IED consumes ESC itself while
            // open, so the game won't pause.
            INPUT downInput{};
            AddScanInput(downInput, kEscapeDIK, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT upInput{};
            AddScanInput(upInput, kEscapeDIK, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            SKSE::log::info("CloseActiveModMenu: closed IED via simulated ESC.");
        }).detach();
    } else if (active == ActiveMenu::ENB) {
        g_ENBOpenRequestedMs.store(0);
        g_MenuOpenLockUntilMs.store(NowMs() + 1000);
        
        std::thread([]() {
            g_AllowENBOpen.store(true);
            g_AllowENBOpenUntilMs.store(NowMs() + 1000);

            WORD combDIK = DIKFromVK(g_ENBConfig.combinationVK);
            WORD editDIK = DIKFromVK(g_ENBConfig.editorVK);

            std::array<INPUT, 2> downInputs{};
            UINT downCount = 0;
            if (combDIK != 0) AddScanInput(downInputs[downCount++], combDIK, false);
            if (editDIK != 0) AddScanInput(downInputs[downCount++], editDIK, false);
            ::SendInput(downCount, downInputs.data(), sizeof(INPUT));

            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            std::array<INPUT, 2> upInputs{};
            UINT upCount = 0;
            if (editDIK != 0) AddScanInput(upInputs[upCount++], editDIK, true);
            if (combDIK != 0) AddScanInput(upInputs[upCount++], combDIK, true);
            ::SendInput(upCount, upInputs.data(), sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_AllowENBOpen.store(false);
            g_AllowENBOpenUntilMs.store(0);
            
            SKSE::log::info("CloseActiveModMenu: Closed ENB.");
        }).detach();
    } else if (active == ActiveMenu::DebugMenu) {
        // Suppress ESC from leaking to Skyrim's system menu while the menu closes.
        g_SuppressEscUntilMs.store(NowMs() + 800);
        // Clear active state immediately — we own the close from here.
        g_ActiveMenu.store(static_cast<ActiveMenu>(0)); // ActiveMenu::None

        if (!g_DebugMenuConfig.setMenuOpen) {
            ResolveDebugMenuApi("CloseActiveModMenu");
            if (g_DebugMenuConfig.setMenuOpen) {
                SKSE::log::info("CloseActiveModMenu: Resolved DebugMenu_SetMenuOpen dynamically on close.");
            }
        }

        if (g_DebugMenuConfig.setMenuOpen) {
            // Best path: call Debug Menu's own exported API to close it cleanly.
            g_DebugMenuConfig.setMenuOpen(false);
            SKSE::log::info("CloseActiveModMenu: Closed DebugMenu via exported SetMenuOpen(false).");
        } else if (g_LauncherHotkeyVK.load() == g_DebugMenuConfig.toggleVK) {
            // Launcher key IS Debug Menu's key — the physical key already passed through,
            // just open a short allow window so the sink lets it complete the toggle.
            g_AllowDebugMenuOpen.store(true);
            g_AllowDebugMenuOpenUntilMs.store(NowMs() + 200);
            SKSE::log::info("CloseActiveModMenu: Closing DebugMenu via physical hotkey pass-through.");
        } else {
            // Close by re-toggling the key through the engine (same path that opened it). Debug Menu's
            // toggle is read by its input sink; we open the allow-window so our filter passes it.
            g_AllowDebugMenuOpen.store(true);
            g_AllowDebugMenuOpenUntilMs.store(NowMs() + 500);
            InjectEngineKey(g_DebugMenuConfig.toggleDIK);
            SKSE::log::info("CloseActiveModMenu: closing DebugMenu via engine injection.");
        }
    } else if (active == ActiveMenu::DMenu) {
        if (const auto* api = g_DMenuApi.load(); api && api->CloseMenu) {
            api->CloseMenu();
            SKSE::log::info("CloseActiveModMenu: closed dMenu via NG API.");
        } else {
            // dMenu toggles on its key — re-emit it through the engine to close.
            g_AllowDMenuOpen.store(true);
            g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);
            InjectEngineKey(g_DMenuConfig.toggleDIK);
            SKSE::log::info("CloseActiveModMenu: closing dMenu via engine key event.");
        }
    } else if (active == ActiveMenu::ImprovedCamera) {
        // IC closes on ESC, so send a PLAIN simulated ESC. Do NOT use g_AllowESCSinkBlock here: that
        // strips ESC from the same input IC reads, which is why the earlier ESC path never closed it.
        // The native adapter is version-locked, so only use it if it supports the installed build.
        if (SetImprovedCameraOpen(false)) {
            SKSE::log::info("CloseActiveModMenu: closed Improved Camera directly.");
        } else {
            SimulateModifiedKey(0, kEscapeDIK);
            SKSE::log::info("CloseActiveModMenu: closed Improved Camera via simulated ESC.");
        }
    } else if (active == ActiveMenu::FLICK) {
        if (auto* flick = GetFLICKInterface(); flick && flick->SetMenuOpen) {
            flick->SetMenuOpen(false);
            SKSE::log::info("CloseActiveModMenu: Closed FLICK through its API.");
        }
    } else if (active == ActiveMenu::KreatE) {
        g_AllowKreatEOpen.store(true);
        g_AllowKreatEOpenUntilMs.store(NowMs() + 1000);
        SimulateModifiedKey(0, g_KreatEConfig.toggleDIK);
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            g_AllowKreatEOpen.store(false);
            g_AllowKreatEOpenUntilMs.store(0);
        }).detach();
        SKSE::log::info("CloseActiveModMenu: toggled KreatE closed with {}.", NameFromDIK(g_KreatEConfig.toggleDIK));
    } else if (active == ActiveMenu::CS) {
        const int sub = g_ActiveCSSub.load();
        if (IsConsoleOpen()) {
            // With the console open, CS's toggle key won't close it, but Escape does (the user confirmed
            // Escape closes both the Main Menu and the Editor). So F1 just simulates Escape here.
            SimulateModifiedKey(0, kEscapeDIK);
            SKSE::log::info("CloseActiveModMenu: closing Community Shaders sub {} via simulated Escape (console open).", sub);
        } else if (sub == 1) {
            // The CS Editor's own close command is Escape.
            SimulateModifiedKey(0, kEscapeDIK);
            SKSE::log::info("CloseActiveModMenu: closing Community Shaders sub {} via engine key event.", sub);
        } else {
            g_AllowCSOpen.store(true);
            g_AllowCSOpenUntilMs.store(NowMs() + 1000);
            ArmCSKeyPass(0);
            InjectEngineKey(g_CSConfig.toggleDIK);
            SKSE::log::info("CloseActiveModMenu: closing Community Shaders sub {} via engine key event.", sub);
        }
        g_ActiveCSSub.store(0);
    } else if (active == ActiveMenu::PartySheet) {
        // Party Sheet consumes Escape internally for all four modal surfaces. Dispatch it only
        // to Party Sheet so closing from F1 cannot open Skyrim's pause menu.
        DispatchPartySheetKey(kEscapeDIK);
        g_ActivePartySheetSub.store(0);
        SKSE::log::info("CloseActiveModMenu: sent Escape directly to Skyrim Party Sheet.");
    } else if (active == ActiveMenu::CatMenu) {
        if (SetCatMenuOpen(false)) {
            SKSE::log::info("CloseActiveModMenu: closed CatMenu directly.");
        }
    } else if (active == ActiveMenu::Dragonborn) {
        if (SetDragonbornOpen(false)) {
            SKSE::log::info("CloseActiveModMenu: closed Dragonborn's Toolkit directly.");
        }
    } else if (active == ActiveMenu::QAR) {
        // QAR has no programmatic close yet - simulate Esc, which closes its window when the user has
        // its "Escape closes windows" setting on. QAR blocks game input while open, so Esc won't leak.
        SimulateModifiedKey(0, kEscapeDIK);
        SKSE::log::info("CloseActiveModMenu: closing QAR via simulated Escape.");
    } else if (active == ActiveMenu::SearchUI) {
        // SearchUI's UITextEntryMenu closes through its native Tab action; it has no close API.
        InjectEngineKey(kTabDIK);
        SKSE::log::info("CloseActiveModMenu: closing SearchUI via injected Tab.");
    } else if (active == ActiveMenu::ModFunctionMenu) {
        SKSE::log::info("MFM_TRACE close-request: active={}, launcherOpen={}, lockRemainingMs={}, toggleDIK={}",
            static_cast<int>(g_ActiveMenu.load()),
            g_LauncherWindow && g_LauncherWindow->IsOpen.load(),
            std::max<long long>(0, g_MenuOpenLockUntilMs.load() - NowMs()),
            NameFromDIK(g_ModFunctionMenuConfig.toggleDIK));
        // Mod Function Menu's hotkey is a real toggle (pressing it while open calls Close()). So we close
        // it the same way we open it - inject F20 through the engine queue it reads. No Esc involved at
        // all; the user's own physical Esc still closes it too, by default.
        const WORD toggleDIK = g_ModFunctionMenuConfig.toggleDIK;
        std::thread([toggleDIK]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            SKSE::log::info("MFM_TRACE delayed-close-inject: sending {} after F1 handoff.",
                NameFromDIK(toggleDIK));
            InjectEngineKey(toggleDIK);
        }).detach();
        SKSE::log::info("CloseActiveModMenu: scheduled Mod Function Menu close with {} after handoff.",
            NameFromDIK(toggleDIK));
    } else if (active == ActiveMenu::OPS) {
        // Close via the author's intended SKSE mod event (matches OPS_NativePreviewOpen).
        SendOPSModEvent("OPS_NativePreviewClose");
        SKSE::log::info("CloseActiveModMenu: closing Outfit Preview Selector via OPS_NativePreviewClose.");
    } else if (active == ActiveMenu::ReShade) {
        if (g_ReShadeAddonActive.load() && RisaReShade::RuntimeReady()) {
            // Keyless close through ReShade's own API.
            RisaReShade::SetOverlayOpen(false);
            g_ActiveMenu.store(ActiveMenu::None);
            SKSE::log::info("CloseActiveModMenu: closed ReShade overlay via add-on API.");
        } else {
            // Fallback: ReShade's overlay closes on ESC (native) — and ESC reaches it even though
            // its overlay captures other input. (ReShade pauses the input update, so F1 can't be
            // detected while it's open; the user can also just press ESC themselves.)
            SimulateModifiedKey(0, kEscapeDIK);
            SKSE::log::info("CloseActiveModMenu: closed ReShade overlay via simulated ESC.");
        }
    } else if (active == ActiveMenu::MCM) {
        g_OpenMCMWhenJournalReady.store(false);
        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            SKSE::log::info("CloseActiveModMenu: closed MCM through the Journal Menu queue.");
        } else {
            SKSE::log::error("CloseActiveModMenu: UIMessageQueue unavailable; could not close MCM.");
        }
    } else if (active == ActiveMenu::MF) {
        ForceCloseSKSEMenuFramework();
        SKSE::log::info("CloseActiveModMenu: Closed SKSE Menu Framework.");
    }
    g_ActiveMenu.store(ActiveMenu::None);
}

static bool AreLauncherModifiersOk() {
    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };
    bool ctrlPressed = (keyState(VK_CONTROL) & 0x8000) != 0;
    bool shiftPressed = (keyState(VK_SHIFT) & 0x8000) != 0;
    bool altPressed = (keyState(VK_MENU) & 0x8000) != 0;

    if (g_LauncherHotkeyCtrl.load() != ctrlPressed) return false;
    if (g_LauncherHotkeyShift.load() != shiftPressed) return false;
    if (g_LauncherHotkeyAlt.load() != altPressed) return false;
    return true;
}

// Double-tap detection. Returns true only on the SECOND tap that lands within the window.
// Window upper bound is generous (300 ms) so the natural 140-250 ms double-tap always counts;
// the small lower bound (40 ms) ignores the same physical press arriving twice from two input
// paths in the same frame, which would otherwise false-trigger on a single tap.
static constexpr long long kDoubleTapMinMs = 40;
static constexpr long long kDoubleTapMaxMs = 300;
static bool CheckLauncherDoubleTap(long long now) {
    if (!g_LauncherHotkeyDoubleTap.load()) return true;
    const long long last = g_LastLauncherKeyPressMs.load();
    const long long gap = now - last;

    if (last != 0 && gap < kDoubleTapMinMs) {
        return false; // duplicate of the same press — ignore, keep waiting for a real 2nd tap
    }
    if (last != 0 && gap <= kDoubleTapMaxMs) {
        g_LastLauncherKeyPressMs.store(0);
        SKSE::log::info("Launcher double-tap: detected (gap={} ms).", gap);
        return true;
    }
    g_LastLauncherKeyPressMs.store(now);
    SKSE::log::info("Launcher double-tap: first tap registered — waiting for the second.");
    return false;
}

// Toggle Risa's launcher (or close the active mod menu). Debounced. Called from the
// low-level keyboard hook on a physical launcher-hotkey press, and as a fallback from
// the DI8 GetDeviceState path when the LL hook isn't active.
static void ToggleLauncher() {
    SKSE::log::info("ToggleLauncher: launcher hotkey pressed.");
    if (g_ConfigRestartRequired.load()) {
        SKSE::log::info("ToggleLauncher: ignored while the restart notice is visible.");
        return;
    }
    if (IsLauncherTypingBlocked()) {
        SKSE::log::info("ToggleLauncher: hotkey pressed but nothing launched (user is typing).");
        return;
    }
    if (IsENBOpeningTransition()) {
        SKSE::log::info("ToggleLauncher: hotkey pressed but nothing launched (ENB opening transition).");
        return;
    }
    if (NowMs() < g_MenuOpenLockUntilMs.load()) {
        SKSE::log::info("ToggleLauncher: hotkey pressed but nothing launched (menu-open lock active).");
        return;
    }
    long long now = NowMs();
    const long long lastToggle = g_LastLauncherToggleMs.load();
    if (now - lastToggle <= 200) {
        SKSE::log::info("ToggleLauncher: ignored duplicate trigger within 200ms.");
        return;
    }
    if (!g_LauncherHotkeyDoubleTap.load() && !g_LauncherHotkeyEasyClose.load() &&
        now - lastToggle <= 700) {
        SKSE::log::info("ToggleLauncher: hotkey pressed but nothing launched (debounced).");
        return;
    }
    g_LastLauncherToggleMs.store(now);

    auto active = g_ActiveMenu.load();
    // IED has a reliable native "is it open" query, so resync it here if it was closed externally.
    // Do NOT guess from the cursor for other menus: many (FLICK, Improved Camera, Party Sheet,
    // CatMenu) don't show the OS cursor while open, so a cursor check wrongly reported them closed
    // and made F1 open the launcher instead of closing them. Stale state after ESC is instead
    // cleared by the physical-ESC handler in HookedKbProcess the instant ESC is pressed.
    if (active == ActiveMenu::IED && !IsIEDOpen()) {
        g_ActiveMenu.store(ActiveMenu::None);
        active = ActiveMenu::None;
        SKSE::log::info("ToggleLauncher: IED closed externally; synchronized native menu state.");
    }
    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
        CloseLauncher();
        SKSE::log::info("ToggleLauncher: Launcher closed.");
    } else if (active != ActiveMenu::None) {
        CloseActiveModMenu(active);
        SKSE::log::info("ToggleLauncher: closed active mod menu.");
    } else if (g_LauncherWindow) {
        ForceCloseSKSEMenuFramework();
        g_LauncherWindow->IsOpen.store(true);
        g_LauncherWindow->BlockUserInput.store(true); // keep the cursor + mouse capture; freeze handled separately
        SKSE::log::info("ToggleLauncher: Launcher opened (keepRunning={}).", g_KeepGameRunning.load());
    } else {
        SKSE::log::info("ToggleLauncher: hotkey pressed but nothing launched (no launcher window).");
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Re-entrancy guard: if we re-asserted ourselves to the front of the subclass
    // chain while another mod (dMenu/FLICK/IC) had chained on top of our OLD proc,
    // that mod will call down into HookedWndProc again. Break the cycle by going
    // straight to the original game proc instead of looping back through the chain.
    if (g_WndReentry > 0) {
        if (g_WndReentry > 8) {
            // Safety valve — should never happen if g_GameWndProc is the real game
            // proc; guards against a stack overflow if the chain ever cycles.
            return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
        }
        ++g_WndReentry;
        struct InnerGuard { ~InnerGuard() { --g_WndReentry; } } _inner;
        WNDPROC game = g_GameWndProc.load();
        if (game) return ::CallWindowProc(game, hWnd, uMsg, wParam, lParam);
        return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    ++g_WndReentry;
    struct ReentryGuard { ~ReentryGuard() { --g_WndReentry; } } _reentryGuard;

    if (IsRebinding()) {
        return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
    }

    const bool isEscapeMsg = (wParam == VK_ESCAPE && (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP)) ||
                             (wParam == 27 && uMsg == WM_CHAR);
    if (isEscapeMsg) {
        if (g_ActiveMenu.load() == ActiveMenu::DebugMenu &&
            (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) &&
            (lParam & 0x40000000) == 0 &&
            !IsRebinding()) {
            const long long now = NowMs();
            if (now >= g_MenuOpenLockUntilMs.load() && now - g_LastOriginalHotkeyMs.load() > 300) {
                g_LastOriginalHotkeyMs.store(now);
                g_SuppressEscUntilMs.store(now + 400);
                SKSE::log::info("HookedWndProc: Escape closing DebugMenu via its toggle close path.");
                CloseActiveModMenu(ActiveMenu::DebugMenu);
            }
            return 0;
        }
        if (EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load()) {
            SKSE::log::info("HookedWndProc: blocked Escape window message (uMsg=0x{:04X}).", uMsg);
            return 0;
        }
    }

    if (!IsUserTyping()) {
        // 1. Block SKSE Menu Framework's hotkey completely
        if (g_MFConfig.toggleVK != 0 && g_MFConfig.toggleMode != "OFF" && wParam == static_cast<WPARAM>(g_MFConfig.toggleVK)) {
            if ((g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) ||
                (g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load())) {
                return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
            }
            SKSE::log::info("WndProc: BLOCKED SKSE Menu Framework key 0x{:02X}", wParam);
            return 0;
        }

        // 2. Check for Risa's menu hotkey and external menu transitions
        if (!g_WaitingForHotkeyPress.load() && wParam == static_cast<WPARAM>(g_LauncherHotkeyVK.load())) {
            // Let simulated keys shared with the launcher pass to their target menu.
            if ((g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) ||
                (g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load())) {
                return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
            }

            // Do not let F1 clear ENB's tracked state while the delayed Shift+Enter
            // request is still waiting for the editor to become visible.
            if (IsENBOpeningTransition()) {
                return 0;
            }

            if (AreLauncherModifiersOk()) {
                bool triggered = false;
                if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
                    bool isRepeat = (lParam & 0x40000000) != 0;
                    if (!isRepeat) {
                        long long now = NowMs();
                        const bool dtap = g_LauncherHotkeyDoubleTap.load();
                        const bool act = g_LauncherHotkeyHold.load() ? false
                                       : (dtap ? CheckLauncherDoubleTap(now)
                                               : (now - g_LastLauncherToggleMs.load() > 700));
                        if (act) {
                                g_LastLauncherToggleMs.store(now);
                                triggered = true;
                                auto active = g_ActiveMenu.load();
                                if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
                                    CloseLauncher();
                                    SKSE::log::info("WndProc: Launcher closed.");
                                } else if (active != ActiveMenu::None) {
                                    CloseActiveModMenu(active);
                                    SKSE::log::info("WndProc: closed active mod menu.");
                                } else {
                                    if (g_LauncherWindow) {
                                        ForceCloseSKSEMenuFramework();
                                        g_LauncherWindow->IsOpen.store(true);
                                        g_LauncherWindow->BlockUserInput.store(true); // keep the cursor + mouse capture; freeze handled separately
                                        SKSE::log::info("WndProc: Launcher opened (keepRunning={}).", g_KeepGameRunning.load());
                                    }
                                }
                            }
                        }
                    }

                // Philosophy: Risa frees keys, so it never swallows its OWN launcher key either. Always
                // pass the key through to the game and other WndProc hooks - Risa still opened the menu
                // above via its own detection. If the user also binds this key in-game, both happen; that's
                // their choice to manage.
                (void)triggered;
                return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
            }
            return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
        }





        // 3. ENB Shift+Enter editor key handling
        if (g_ENBConfig.enabled && wParam == static_cast<WPARAM>(g_ENBConfig.editorVK) && !InternalENBKeyAllowed()) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                bool combDown = IsENBCombinationDown();
                bool allowed = IsExternalMenuOpen() || (g_AllowENBOpen.load() && NowMs() <= g_AllowENBOpenUntilMs.load());
                SKSE::log::info("WndProc: ENB editor key (0x{:02X}, msg=0x{:04X}) checked. combDown={}, allowed={}, allowedUntil={}", 
                    wParam, uMsg, combDown, allowed, g_AllowENBOpenUntilMs.load());
                if (combDown) {
                    if (allowed) {
                        // let it pass to open/close
                    } else {
                        return 0; // block
                    }
                }
            }
        }

        // 4. IED toggle key handling
        if (g_IEDConfig.enabled && wParam == static_cast<WPARAM>(g_IEDConfig.toggleVK) &&
            !InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK)) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) {
                bool allowed = g_AllowIEDOpen.load() && NowMs() <= g_AllowIEDOpenUntilMs.load();
                SKSE::log::info("WndProc: IED toggle key (0x{:02X}, msg=0x{:04X}) checked. allowed={}, allowedUntil={}", 
                    wParam, uMsg, allowed, g_IEDConfig.toggleDIK);
                if (allowed) {
                    // let it pass
                } else {
                    return 0; // block
                }
            }
        }

        // 5. DebugMenu toggle key handling
        if (g_DebugMenuConfig.enabled && wParam == static_cast<WPARAM>(g_DebugMenuConfig.toggleVK) &&
            !InternalAliasKeyAllowed(AI_DebugMenu, g_UnblockDebugMenu, g_DebugMenuConfig.toggleDIK)) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) {
                bool allowed = g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load();
                SKSE::log::info("WndProc: DebugMenu toggle key (0x{:02X}, msg=0x{:04X}) checked. allowed={}, allowedUntil={}", 
                    wParam, uMsg, allowed, g_AllowDebugMenuOpenUntilMs.load());
                if (allowed) {
                    // let it pass
                } else {
                    SKSE::log::info("WndProc: BLOCKED DebugMenu key 0x{:02X} (msg=0x{:04X})", wParam, uMsg);
                    return 0; // block
                }
            }
        }

        // 5.5. FLICK toggle key handling
        if (g_FLICKConfig.enabled && wParam == static_cast<WPARAM>(g_FLICKConfig.toggleVK) &&
            !InternalAliasKeyAllowed(AI_FLICK, g_UnblockFLICK, g_FLICKConfig.toggleDIK)) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) {
                bool allowed = g_AllowFLICKOpen.load() && NowMs() <= g_AllowFLICKOpenUntilMs.load();
                SKSE::log::info("WndProc: FLICK toggle key (0x{:02X}, msg=0x{:04X}) checked. allowed={}, allowedUntil={}", 
                    wParam, uMsg, allowed, g_AllowFLICKOpenUntilMs.load());
                if (allowed) {
                    return 0; // block from WndProc (since FLICK reads from DirectInput, this prevents double-toggle)
                } else {
                    SKSE::log::info("WndProc: BLOCKED FLICK key 0x{:02X} (msg=0x{:04X})", wParam, uMsg);
                    return 0; // block
                }
            }
        }

        if (g_KreatEConfig.enabled && wParam == static_cast<WPARAM>(g_KreatEConfig.toggleVK) &&
            !InternalAliasKeyAllowed(AI_KreatE, g_UnblockKreatE, g_KreatEConfig.toggleDIK)) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
                if (!allowed) {
                    SKSE::log::info("WndProc: BLOCKED KreatE key 0x{:02X} (msg=0x{:04X})", wParam, uMsg);
                    return 0;
                }
            }
        }

        if (g_DragonbornConfig.enabled && wParam == static_cast<WPARAM>(g_DragonbornConfig.toggleVK) &&
            !InternalAliasKeyAllowed(AI_Dragonborn, g_UnblockDragonborn, g_DragonbornConfig.toggleDIK)) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                const bool allowed = g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load();
                if (!allowed) return 0;
            }
        }

        // 6. dMenu / Improved Camera shared toggle key (both default to plain Home).
        //
        //    The two mods read this key through DIFFERENT channels, which is what lets
        //    us open one without the other even though they share the exact same key:
        //      - dMenu reads it from WM_KEYDOWN in its own WndProc subclass.
        //      - Improved Camera reads it by polling GetAsyncKeyState (handled separately
        //        in HookedGetAsyncKeyState/HookedGetKeyState).
        //
        //    So here, on the WM channel, we only ever care about dMenu:
        //      - Default: swallow the key  -> dMenu stays closed (and so does anything
        //        else downstream that reads this key from the message stream).
        //      - Opening dMenu: let it through during the dMenu allow-window.
        //      - Opening Improved Camera: we KEEP swallowing the WM (IC opens via its
        //        GetAsyncKeyState poll and never needs this message), so dMenu can't
        //        ride along on the simulated Home.
        bool isDMenuKey = g_DMenuConfig.enabled && wParam == static_cast<WPARAM>(g_DMenuConfig.toggleVK);
        bool isImpCamKey = g_ImprovedCameraConfig.enabled && wParam == static_cast<WPARAM>(g_ImprovedCameraConfig.toggleVK);

        if (isDMenuKey || isImpCamKey) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                bool dMenuAllowed = isDMenuKey && g_AllowDMenuOpen.load() && NowMs() <= g_AllowDMenuOpenUntilMs.load();
                bool passToDMenu  = dMenuAllowed || (isDMenuKey &&
                    InternalAliasKeyAllowed(AI_DMenu, g_UnblockDMenu, g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK));
                if (!passToDMenu && !IsUserTyping()) {
                    return 0; // swallow on the WM channel -> dMenu blocked
                }
            }
        }

    }
    return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
}

static void SubclassGameWindow() {
    // Re-assert ourselves to the FRONT of the WndProc subclass chain whenever another
    // mod (dMenu / FLICK / Improved Camera, all of which subclass too) has chained on
    // top of us. Being in front is what lets us swallow blocked keys' WM_KEYDOWN before
    // those mods ever see them. The re-entrancy guard in HookedWndProc keeps this from
    // creating an infinite call cycle through the chain.
    auto* renderWindow = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
    if (!renderWindow || !renderWindow->hWnd) return;

    HWND hwnd = reinterpret_cast<HWND>(renderWindow->hWnd);
    g_GameHWND.store(hwnd); // used by the LL hook to scope suppression to when the game is focused
    WNDPROC currentWndProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtr(hwnd, GWLP_WNDPROC));

    if (currentWndProc == HookedWndProc) return; // already at the front

    WNDPROC displaced = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

    // Capture the original game proc exactly once — it is our fixed loop-breaker.
    WNDPROC expected = nullptr;
    g_GameWndProc.compare_exchange_strong(expected, displaced);

    g_OrigWndProc.store(displaced);
    SKSE::log::info("SubclassGameWindow: (re)asserted front of WndProc chain (displaced={:p}, game={:p}).",
        (void*)displaced, (void*)g_GameWndProc.load());
}

// ============================================================================
// Helper to identify caller module via return address
// ============================================================================
static bool IsCallerModule(void* returnAddr, const char* moduleName) {
    HMODULE hMod = ::GetModuleHandleA(moduleName);
    if (!hMod) {
        std::string nameWithExt = std::string(moduleName) + ".dll";
        hMod = ::GetModuleHandleA(nameWithExt.c_str());
    }
    if (!hMod) return false;
    
    HMODULE hCaller = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(returnAddr), &hCaller)) {
        if (hCaller == hMod) return true;
    }

    void* frames[20];
    USHORT count = ::CaptureStackBackTrace(0, 20, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE hFrameMod = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(frames[i]), &hFrameMod)) {
            if (hFrameMod == hMod) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Hook 1 — IDirectInputDevice8A::GetDeviceState  (vtable slot 9)

// Zeros MF's toggle key byte in the 256-byte keyboard state array.
// ============================================================================
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
static GetDeviceState_t g_OrigGetDeviceState = nullptr;
static std::atomic<bool> g_DI8HookFiredOnce{ false };
static std::atomic<long long> g_LastDIKeyboardPollMs{ 0 };
static std::atomic<long long> g_LastLauncherFallbackPressMs{ 0 };
static std::atomic<bool> g_LauncherFallbackLogged{ false };

// ----------------------------------------------------------------------------
// Per-mod block for the ImGui mods (dMenu / FLICK / IED / Improved Camera).
//
// These don't read their toggle through any interceptable call — they read the
// engine keyboard's cached immediate state (BSWin32KeyboardDevice::curState, what
// IsPressed() returns). We zero ONLY each managed mod's toggle key in that array
// every frame, so the mod's poll sees "not pressed". The engine builds gameplay
// ButtonEvents from the BUFFERED stream (diObjData / GetDeviceData), NOT from
// curState, so vanilla keybinds on these keys are unaffected. While a mod is being
// opened from our launcher (its allow-window) or the user unblocked it, we leave
// the key alone so it can open normally.
// ----------------------------------------------------------------------------
static void SuppressManagedPollKeys() {
    auto* idm = RE::BSInputDeviceManager::GetSingleton();
    if (!idm) return;
    auto* kb = idm->GetKeyboard();
    if (!kb) return;
    const long long now = NowMs();
    auto zap = [&](bool enabled, WORD dik, bool unblock, bool allow, long long until, int aliasIdx) {
        if (!enabled || dik == 0) return;
        if (unblock) return;                 // user allows this mod's own hotkey
        if (aliasIdx >= 0 && aliasIdx < AI_COUNT && g_ExcludeMod[aliasIdx].load()) return; // Excluded!
        if (allow && now <= until) return;   // we're opening this mod from the launcher right now
        kb->curState[dik] = 0;
        kb->prevState[dik] = 0;
    };
    // dMenu is managed only through the v2 API (its key is disabled live), so we never zero its key
    // here — that keeps Home free for gameplay. Stock dMenu (no API) is no longer supported.
    zap(g_FLICKConfig.enabled, g_FLICKConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_FLICK, g_UnblockFLICK, g_FLICKConfig.toggleDIK),
        g_AllowFLICKOpen.load(), g_AllowFLICKOpenUntilMs.load(), AI_FLICK);
    zap(g_IEDConfig.enabled, g_IEDConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK),
        g_AllowIEDOpen.load(), g_AllowIEDOpenUntilMs.load(), AI_IED);
    // NOTE: Improved Camera is intentionally NOT zeroed here. It reads its Shift+Home chord via
    // user32 GetAsyncKeyState/GetKeyState, which we already block caller-scoped (only ImprovedCameraSE
    // sees 0). Zeroing Home in the global curState would strip it from the game and every other mod —
    // the exact opposite of this mod's purpose (keep the key free for other uses).
    zap(g_KreatEConfig.enabled, g_KreatEConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_KreatE, g_UnblockKreatE, g_KreatEConfig.toggleDIK),
        g_AllowKreatEOpen.load(), g_AllowKreatEOpenUntilMs.load(), AI_KreatE);
    zap(g_DragonbornConfig.enabled, g_DragonbornConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_Dragonborn, g_UnblockDragonborn, g_DragonbornConfig.toggleDIK),
        g_AllowDragonbornOpen.load(), g_AllowDragonbornOpenUntilMs.load(), AI_Dragonborn);
    zap(g_DebugMenuConfig.enabled, g_DebugMenuConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_DebugMenu, g_UnblockDebugMenu, g_DebugMenuConfig.toggleDIK),
        g_AllowDebugMenuOpen.load(), g_AllowDebugMenuOpenUntilMs.load(), AI_DebugMenu);
    zap(IsSearchUIAvailable(), static_cast<WORD>(g_SearchUIEffectiveDIK.load()),
        g_UnblockSearchUI.load() && !AliasEqualsLauncher(AI_SearchUI),
        g_AllowSearchUIOpen.load(), g_AllowSearchUIOpenUntilMs.load(), AI_SearchUI);
}

// Hook BSWin32KeyboardDevice::Process (vtable slot 2). The engine reads the buffered
// keyboard stream and dispatches gameplay ButtonEvents inside this call. We let it run
// untouched (vanilla binds intact), then scrub the managed mods' toggle keys from the
// keyboard's persistent buffered state — so ImGui mods that read that buffer later in the
// frame (during render) see nothing. Refilled next frame, so it's purely per-mod.
using KbProcess_t = void(*)(RE::BSWin32KeyboardDevice*, float);
static KbProcess_t g_OrigKbProcess = nullptr;

// Launcher-open injection: SendInput can't reach the buffered DInput stream, so we emit the
// key event through the engine itself via BSInputDevice::SetButtonState (main thread, here in
// the keyboard Process hook). Drives a clean press -> hold -> release over a few frames.
static std::atomic<WORD> g_OpenInjectDIK{ 0 };
static std::atomic<WORD> g_OpenInjectMod{ 0 }; // optional modifier (e.g. Shift) held across the key
static std::atomic<int>  g_OpenInjectStep{ 0 };
// ManagedSink + 1. dMenu and IED consume this on Skyrim's input thread so their
// original sink sees a private synthetic event that is never broadcast globally.
static std::atomic<int>  g_DirectSinkToggleRequest{ 0 };
static std::atomic<int>  g_DirectSinkToggleRetries{ 0 };

static bool OAROriginalMoved();
static bool IEDOriginalMoved();
static bool ENBOriginalMoved();
static bool DMenuOriginalMoved();
static bool ImprovedCameraOriginalMoved();
static bool FLICKOriginalMoved();
static bool DebugMenuOriginalMoved();
static bool KreatEOriginalMoved();
static bool CSOriginalMoved();
static bool CatMenuOriginalMoved();
static bool DragonbornOriginalMoved();
static void PollOriginalHotkeyAliases(const BYTE* state);

// Emit a single key press through the engine (used for both opening and closing toggle-style
// mods from the launcher). SendInput can't reach the engine's buffered input, so this is the
// only reliable way to drive these mods.
static void InjectEngineKey(WORD dik, WORD modifier) {
    if (dik == 0) return;
    const WORD replaced = g_OpenInjectDIK.load();
    g_OpenInjectStep.store(0);
    g_OpenInjectMod.store(modifier);
    g_OpenInjectDIK.store(dik);
    SKSE::log::info("InjectEngineKey: queued {}{}{} (replaced={}).",
        modifier ? NameFromDIK(modifier) : "", modifier ? "+" : "", NameFromDIK(dik),
        replaced ? NameFromDIK(replaced) : "none");
}

static void HookedKbProcess(RE::BSWin32KeyboardDevice* self, float a_dt) {
    // Zero ESC in the raw device state BEFORE the original Process() runs.
    // Mods like Debug Menu read their toggle key directly from curState/prevState inside Process(),
    // bypassing the DI buffer we patch in GetDeviceData. If ESC is physically held while we're
    // suppressing it, zeroing it here ensures Debug Menu's internal handler never sees it.
    if (self && (EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load())) {
        self->curState[0x01]  = 0;
        self->prevState[0x01] = 0;
    }
    g_OrigKbProcess(self, a_dt);
    if (!self) return;
    const long long now = NowMs();

    UpdateTimeFreeze(); // pause/unpause time to match our UI, if the option is on

    // Launcher "Hold to open/close": when enabled, the edge triggers are gated off (below), and we
    // fire the toggle once the launcher key has been held long enough. Mirrors the per-mod hold.
    if (g_LauncherHotkeyHold.load() && !IsRebinding() && !IsUserTyping()) {
        const int lvk = g_LauncherHotkeyVK.load();
        const auto keyState = g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState : &::GetAsyncKeyState;
        const bool held = lvk != 0 && (keyState(lvk) & 0x8000) != 0 && AreLauncherModifiersOk();
        if (held) {
            if (g_LauncherHoldStartedMs == 0) {
                g_LauncherHoldStartedMs = now;
            } else if (!g_LauncherHoldTriggered &&
                       now - g_LauncherHoldStartedMs >= (std::max)(50, g_LauncherHoldDurationMs.load())) {
                g_LauncherHoldTriggered = true;
                SKSE::log::info("Launcher hold: fired after {} ms.", g_LauncherHoldDurationMs.load());
                ToggleLauncher(); // let ToggleLauncher own the debounce timestamp
            }
        } else {
            g_LauncherHoldStartedMs = 0;
            g_LauncherHoldTriggered = false;
        }
    }



    if (const int request = g_DirectSinkToggleRequest.exchange(0); request > 0) {
        if (!DispatchManagedSinkToggle(request - 1)) {
            if (g_DirectSinkToggleRetries.fetch_sub(1) > 1) {
                int empty = 0;
                g_DirectSinkToggleRequest.compare_exchange_strong(empty, request);
            } else {
                SKSE::log::error("HookedKbProcess: private sink toggle timed out; managed sink was not found.");
            }
        }
    }

    // Resync when a mod menu is closed WITHOUT going through us — e.g. pressing ESC on the
    // Community Shaders window. Once a mod menu has actually shown the game cursor, the cursor
    // disappearing means it closed, so reset our active-menu state; otherwise the launcher key
    // would keep "toggling" a menu we wrongly think is still open and reopen it. Runs every frame
    // (the in-launcher self-heal only runs while our window renders, which misses this case).
    // ReShade manages its own cursor/overlay and is handled by its raw-input path, so skip it.
    {
        static bool s_sawCursorForActive = false;
        const auto am = g_ActiveMenu.load();
        if (am == ActiveMenu::None || MenuIgnoresOSCursor(am)) {
            s_sawCursorForActive = false;
        } else if (IsCursorShowing()) {
            s_sawCursorForActive = true;
        } else if (s_sawCursorForActive) {
            g_ActiveMenu.store(ActiveMenu::None);
            s_sawCursorForActive = false;
            // The menu was closed by ESC, not by us — backdate the toggle timestamp so the next F1
            // opens the launcher cleanly instead of being spent "closing" a menu we no longer track.
            g_LastLauncherToggleMs.store(now - 1000);
            SKSE::log::info("HookedKbProcess: active menu closed externally (cursor lost); state reset to None.");
        }
    }

    // "Enable original hotkey" bridge (pressing a mod's original key opens it) normally runs
    // from the slot-9 DirectInput poll — which ReShade kills. When the DI poll is stale (ReShade
    // active) drive the same bridge here from the engine's curState so the toggles work under
    // ReShade exactly as they do without it. Skipped when the DI poll is live, so no double-fire.
    if (now - g_LastDIKeyboardPollMs.load(std::memory_order_relaxed) > 250) {
        PollOriginalHotkeyAliases(self->curState);
    }

    // ReShade-robust launcher-key detection. This hook runs every frame on the engine's INPUT
    // update (ReShade only disrupts rendering, not this), and curState reflects the real key
    // even when ReShade has made our slot-9 DirectInput poll go stale AND a mod overlay is
    // eating the SKSE InputEvent — the two situations that otherwise leave F1 undetectable.
    // Only acts when the DI poll is stale, so non-ReShade setups are completely unaffected.
    {
        // Read the PHYSICAL key (GetAsyncKeyState), not curState: when a mod like KreatE has
        // its input-blocking on, it keeps the key out of the engine's curState, so a curState
        // read would miss F1. GetAsyncKeyState is OS-level and immune to that block (and to ReShade).
        const WORD lvk = g_LauncherHotkeyVK.load();
        const SHORT ast = lvk == 0 ? 0 : (g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(lvk) : ::GetAsyncKeyState(lvk));
        const bool keyDown = lvk != 0 && (ast & 0x8000) != 0;
        static bool s_kbArmed = true;
        if (keyDown) {
            if (s_kbArmed) {
                s_kbArmed = false;
                const bool diStale = now - g_LastDIKeyboardPollMs.load(std::memory_order_relaxed) > 250;
                const bool simulating = (g_AllowDebugMenuOpen.load() && now <= g_AllowDebugMenuOpenUntilMs.load()) ||
                                        (g_AllowDragonbornOpen.load() && now <= g_AllowDragonbornOpenUntilMs.load());
                const bool lockActive = IsENBOpeningTransition() || now < g_MenuOpenLockUntilMs.load();
                const bool recentToggle = now - g_LastLauncherToggleMs.load() < 250;
                // Several mod menus (IED, KreatE, Improved Camera, ...) swallow input while open, so
                // the normal F1 channels miss the key and F1 can't close them. Their native menus
                // don't make the DI poll go stale, so run this OS-level read whenever ANY mod menu is
                // the active menu. The debounce guards below stop a double-toggle if a normal channel
                // also catches the key.
                const bool capturingMenu = g_ActiveMenu.load() != ActiveMenu::None;
                if ((diStale || capturingMenu) && !simulating && !lockActive && !recentToggle &&
                    !IsRebinding() && !IsUserTyping()) {
                    const bool launcherOpen = g_LauncherWindow && g_LauncherWindow->IsOpen.load();
                    const bool somethingOpen = launcherOpen || g_ActiveMenu.load() != ActiveMenu::None;
                    if (g_LauncherHotkeyEasyClose.load() && somethingOpen) {
                        ToggleLauncher();
                    } else if (AreLauncherModifiersOk() && !g_LauncherHotkeyHold.load()) {
                        if (g_LauncherHotkeyDoubleTap.load()) {
                            if (CheckLauncherDoubleTap(now)) ToggleLauncher();
                        } else if (now - g_LastLauncherToggleMs.load() > 700) {
                            ToggleLauncher();
                        }
                    }
                }
            }
        } else {
            s_kbArmed = true;
        }
    }

    // KreatE blocks the normal key channel (curState) while its UI is open, so its alias can't be
    // seen to CLOSE it (opening works via the normal poll while KreatE is closed). Read the alias
    // PHYSICALLY (GetAsyncKeyState, immune to KreatE's block) while KreatE is the active menu.
    if (g_ActiveMenu.load() == ActiveMenu::KreatE && g_UnblockKreatE.load() && !IsRebinding() && !IsUserTyping()) {
        const auto async = [&](int vk) {
            return vk != 0 && ((g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk)) & 0x8000) != 0;
        };
        const WORD kvk = VKFromDIK(g_AliasDik[AI_KreatE].load());
        const bool kDown = async(kvk) &&
            async(VK_CONTROL) == g_AliasCtrl[AI_KreatE].load() &&
            async(VK_SHIFT) == g_AliasShift[AI_KreatE].load() &&
            async(VK_MENU) == g_AliasAlt[AI_KreatE].load();
        static bool s_kreateCloseArmed = false; // require a release before the first close (key is held right after opening)
        if (kDown) {
            if (s_kreateCloseArmed && now - g_LastOriginalHotkeyMs.load() > 400) {
                s_kreateCloseArmed = false;
                g_LastOriginalHotkeyMs.store(now);
                SKSE::log::info("HookedKbProcess: KreatE alias (physical) closing KreatE.");
                CloseActiveModMenu(ActiveMenu::KreatE);
            }
        } else {
            s_kreateCloseArmed = true;
        }
    }

    // IED closes on ESC, not by re-pressing its toggle key, and it swallows input while open — so
    // the original/alias key can't close it on its own. While IED is open (any open path) and the
    // user enabled its hotkey, read the alias PHYSICALLY and close IED (via ESC) on a fresh press.
    if (g_UnblockIED.load() && IsIEDOpen() && !IsRebinding() && !IsUserTyping()) {
        const auto async = [&](int vk) {
            return vk != 0 && ((g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk)) & 0x8000) != 0;
        };
        const WORD ivk = VKFromDIK(g_AliasDik[AI_IED].load());
        const bool iDown = async(ivk) &&
            async(VK_CONTROL) == g_AliasCtrl[AI_IED].load() &&
            async(VK_SHIFT) == g_AliasShift[AI_IED].load() &&
            async(VK_MENU) == g_AliasAlt[AI_IED].load();
        static bool s_iedCloseArmed = false; // require a release first (key is held right after opening)
        if (iDown) {
            if (s_iedCloseArmed && now - g_LastOriginalHotkeyMs.load() > 400) {
                s_iedCloseArmed = false;
                g_LastOriginalHotkeyMs.store(now);
                SKSE::log::info("HookedKbProcess: IED alias (physical) closing IED.");
                CloseActiveModMenu(ActiveMenu::IED);
            }
        } else {
            s_iedCloseArmed = true;
        }
    }

    // Unified physical-ESC handling while any mod menu is active. Reads ESC at OS level so it works
    // regardless of which mod is capturing input. Two cases:
    //   * Self-closing menus (OAR, Improved Camera, Party Sheet, SKSE MF, IED) consume
    //     ESC and close themselves — we ONLY clear our active-menu state so the next F1 opens the
    //     launcher (this replaces the old cursor guess that misfired and broke F1-close).
    //   * ESC-ignoring menus (Debug Menu, ENB, KreatE, Dragonborn, CatMenu) are closed explicitly via
    //     CloseActiveModMenu (each mod's own path / API). Skyrim's ESC menu is already blocked for
    //     these the whole time they're open (EscBlockedForActiveMenu), so nothing leaks to the game.
    {
        const auto escMenu = g_ActiveMenu.load();
        const bool escDown = (g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(VK_ESCAPE)
                                                      : ::GetAsyncKeyState(VK_ESCAPE)) & 0x8000;
        static bool s_escWasDown = false;
        if (escMenu != ActiveMenu::None && escMenu != ActiveMenu::ReShade &&
            !IsRebinding() && (escMenu == ActiveMenu::DebugMenu || escMenu == ActiveMenu::SearchUI || !IsUserTyping()) &&
            now >= g_MenuOpenLockUntilMs.load() && !IsENBOpeningTransition()) {
            if (escDown && !s_escWasDown && now - g_LastOriginalHotkeyMs.load() > 300) {
                g_LastOriginalHotkeyMs.store(now);
                // dMenu: only the "Risa's dmenu" build closes itself on ESC — stock dMenu / dMenu NG
                // ignore it. So always close dMenu explicitly through its API/key (harmlessly double-
                // closes the v2 build). It isn't in EscBlockedForActiveMenu because dMenu blocks the
                // game's input itself while open, so ESC can't leak, and the v2 build still reads ESC.
                // DebugMod does not handle ESC natively. Close it with the same path as its second F1
                // press: the exported API when available, otherwise its configured toggle key.
                const bool explicitClose = EscBlockedForActiveMenu()
                                           || escMenu == ActiveMenu::DMenu
                                           || escMenu == ActiveMenu::FLICK
                                           || escMenu == ActiveMenu::DebugMenu
                                           || escMenu == ActiveMenu::SearchUI;
                if (explicitClose) {
                    // Keep ESC stripped from the game for a moment after we close the menu, so the
                    // still-held ESC can't pop Skyrim's system menu once g_ActiveMenu clears to None.
                    g_SuppressEscUntilMs.store(now + 400);
                    SKSE::log::info("HookedKbProcess: ESC closing menu ({}) via its own path.",
                        static_cast<int>(escMenu));
                    CloseActiveModMenu(escMenu);
                } else {
                    // Self-closing menu: it dismisses itself on ESC; just clear our stale state. Keep ESC
                    // stripped from the game briefly so a still-held ESC can't pop Skyrim's system menu
                    // once the menu closes (matters for menus not covered by EscBlockedForActiveMenu).
                    if (escMenu == ActiveMenu::ModFunctionMenu) {
                        SKSE::log::info("MFM_TRACE ESC-state-clear: MFM reported/appeared self-closed; launcherOpen={}, lockRemainingMs={}",
                            g_LauncherWindow && g_LauncherWindow->IsOpen.load(),
                            std::max<long long>(0, g_MenuOpenLockUntilMs.load() - now));
                    }
                    g_ActiveMenu.store(ActiveMenu::None);
                    g_LastLauncherToggleMs.store(now - 1000);
                    g_SuppressEscUntilMs.store(now + 400);
                    SKSE::log::info("HookedKbProcess: ESC on self-closing menu ({}); cleared active-menu state.",
                        static_cast<int>(escMenu));
                }
            }
        }
        s_escWasDown = escDown;
        if (now < g_SuppressEscUntilMs.load() && escDown) {
            g_SuppressEscUntilMs.store(now + 150);
        }
    }

    // Synthesize a key event through the engine to open the mod the launcher requested.
    if (WORD openDik = g_OpenInjectDIK.load()) {
        const WORD mod = g_OpenInjectMod.load();
        int step = g_OpenInjectStep.fetch_add(1);
        const bool isSearchUI = (openDik == static_cast<WORD>(g_SearchUIEffectiveDIK.load()));
        const bool isDebugMenu = (openDik == g_DebugMenuConfig.toggleDIK);
        const int holdFrames = (isSearchUI || isDebugMenu) ? 1 : 4;
        if (step == 0) {                                                       // modifier (if any) + key down
            SKSE::log::info("InjectEngineKey: emitting key-down for {}{}{}.",
                mod ? NameFromDIK(mod) : "", mod ? "+" : "", NameFromDIK(openDik));
            if (mod) self->SetButtonState(mod, 0.0f, false, true);
            self->SetButtonState(openDik, 0.0f, false, true);
        } else if (step < holdFrames) {                                                 // held a few frames
            if (mod) self->SetButtonState(mod, a_dt, true, true);
            self->SetButtonState(openDik, a_dt, true, true);
        } else if (step == holdFrames) {                                               // key up (modifier still held)
            self->SetButtonState(openDik, a_dt, true, false);
        } else if (step == holdFrames + 1) {                                               // modifier up
            if (mod) self->SetButtonState(mod, a_dt, true, false);
        } else {
            SKSE::log::info("InjectEngineKey: completed {}{}{}.",
                mod ? NameFromDIK(mod) : "", mod ? "+" : "", NameFromDIK(openDik));
            g_OpenInjectDIK.store(0); g_OpenInjectMod.store(0); g_OpenInjectStep.store(0);
        }
    }
    auto clear = [&](bool enabled, WORD dik, bool unblock, bool allow, long long until, int aliasIdx) {
        if (!enabled || dik == 0 || unblock) return;
        if (aliasIdx >= 0 && aliasIdx < AI_COUNT && g_ExcludeMod[aliasIdx].load()) return; // Excluded!
        if (allow && now <= until) return;
        self->curState[dik]  = 0;
        self->prevState[dik] = 0;
        for (auto& od : self->diObjData) {
            if ((od.ofs & 0xFF) == dik) { od.data = 0; od.ofs = 0; }
        }
    };

    // Moved original aliases are commands owned by this plugin. Consume their buffered
    // events so the same chord does not also activate a gameplay action.
    const auto pressed = [&](WORD dik) { return dik < 256 && (self->curState[dik] & 0x80) != 0; };
    const bool shift = pressed(0x2A) || pressed(0x36);
    const bool ctrl = pressed(0x1D) || pressed(0x9D);
    const bool alt = pressed(0x38) || pressed(0xB8);
    const bool plain = !shift && !ctrl && !alt;
    clear(g_OARConfig.enabled && g_UnblockOAR.load() && OAROriginalMoved() && shift && !ctrl && !alt,
        0x18, false, false, 0, AI_OAR);
    clear(g_IEDConfig.enabled && g_UnblockIED.load() && IEDOriginalMoved() && plain,
        0x0E, false, false, 0, AI_IED);
    clear(g_ENBConfig.enabled && g_UnblockENB.load() && ENBOriginalMoved() && shift && !ctrl && !alt,
        0x1C, false, false, 0, AI_ENB);
    clear(g_DMenuConfig.enabled && g_UnblockDMenu.load() && DMenuOriginalMoved() && plain,
        0xC7, false, false, 0, AI_DMenu);
    clear(g_ImprovedCameraConfig.enabled && g_UnblockImprovedCamera.load() && ImprovedCameraOriginalMoved() && shift && !ctrl && !alt,
        0xC7, false, false, 0, AI_IC);
    clear(g_FLICKConfig.enabled && g_UnblockFLICK.load() && FLICKOriginalMoved() && plain,
        0x41, false, false, 0, AI_FLICK);
    clear(g_DebugMenuConfig.enabled && g_UnblockDebugMenu.load() && DebugMenuOriginalMoved() &&
            g_LauncherHotkeyDIK.load() != 0x3B && plain,
        0x3B, false, false, 0, AI_DebugMenu);
    clear(g_KreatEConfig.enabled && g_UnblockKreatE.load() && KreatEOriginalMoved() && plain,
        0xCF, false, false, 0, AI_KreatE);
    clear(g_CatMenuConfig.enabled && g_UnblockCatMenu.load() && CatMenuOriginalMoved() && plain,
        0x40, false, false, 0, AI_CatMenu);
    clear(g_DragonbornConfig.enabled && g_UnblockDragonborn.load() && DragonbornOriginalMoved() &&
            g_LauncherHotkeyDIK.load() != 0x3B && plain,
        0x3B, false, false, 0, AI_Dragonborn);

    clear(g_DMenuConfig.enabled, g_DMenuConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_DMenu, g_UnblockDMenu, g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK),
        g_AllowDMenuOpen.load(), g_AllowDMenuOpenUntilMs.load(), AI_DMenu);
    clear(g_FLICKConfig.enabled, g_FLICKConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_FLICK, g_UnblockFLICK, g_FLICKConfig.toggleDIK),
        g_AllowFLICKOpen.load(), g_AllowFLICKOpenUntilMs.load(), AI_FLICK);
    clear(g_IEDConfig.enabled, g_IEDConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK),
        g_AllowIEDOpen.load(), g_AllowIEDOpenUntilMs.load(), AI_IED);
    clear(g_ImprovedCameraConfig.enabled, g_ImprovedCameraConfig.toggleDIK,
        false,
        g_AllowImprovedCameraOpen.load(), g_AllowImprovedCameraOpenUntilMs.load(), AI_IC);
    clear(g_KreatEConfig.enabled, g_KreatEConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_KreatE, g_UnblockKreatE, g_KreatEConfig.toggleDIK),
        g_AllowKreatEOpen.load(), g_AllowKreatEOpenUntilMs.load(), AI_KreatE);
    clear(g_DragonbornConfig.enabled, g_DragonbornConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_Dragonborn, g_UnblockDragonborn, g_DragonbornConfig.toggleDIK),
        g_AllowDragonbornOpen.load(), g_AllowDragonbornOpenUntilMs.load(), AI_Dragonborn);
    clear(g_DebugMenuConfig.enabled, g_DebugMenuConfig.toggleDIK,
        InternalAliasKeyAllowed(AI_DebugMenu, g_UnblockDebugMenu, g_DebugMenuConfig.toggleDIK),
        g_AllowDebugMenuOpen.load(), g_AllowDebugMenuOpenUntilMs.load(), AI_DebugMenu);
    clear(IsSearchUIAvailable(), static_cast<WORD>(g_SearchUIEffectiveDIK.load()),
        g_UnblockSearchUI.load() && !AliasEqualsLauncher(AI_SearchUI),
        g_AllowSearchUIOpen.load(), g_AllowSearchUIOpenUntilMs.load(), AI_SearchUI);
}

static void TryHookKeyboardProcess() {
    static std::atomic<bool> done{ false };
    if (done.load()) return;
    auto* idm = RE::BSInputDeviceManager::GetSingleton();
    if (!idm) return;
    auto* kb = idm->GetKeyboard();
    if (!kb) return;
    void** vt = *reinterpret_cast<void***>(kb);
    void* fn = vt[2]; // Process(float)
    if (MH_CreateHook(fn, reinterpret_cast<void*>(HookedKbProcess), reinterpret_cast<void**>(&g_OrigKbProcess)) == MH_OK &&
        MH_EnableHook(fn) == MH_OK) {
        done.store(true);
        SKSE::log::info("TryHookKeyboardProcess: hooked BSWin32KeyboardDevice::Process at {:p}.", fn);
    }
}

static bool OAROriginalMoved() {
    return g_OARConfig.toggleDIK != 0x18 || !g_OARConfig.shift || g_OARConfig.ctrl || g_OARConfig.alt;
}

static bool IEDOriginalMoved() { return g_IEDConfig.toggleDIK != 0x0E; }
static bool ENBOriginalMoved() {
    return g_ENBConfig.combinationVK != VK_SHIFT || g_ENBConfig.editorVK != VK_RETURN;
}
static bool DMenuOriginalMoved() {
    return g_DMenuConfig.toggleDIK != 0xC7 || g_DMenuConfig.modifierDIK != 0;
}
static bool ImprovedCameraOriginalMoved() {
    return g_ImprovedCameraConfig.toggleDIK != 0xC7 || g_ImprovedCameraConfig.modifierDIK != 0x2A;
}
static bool FLICKOriginalMoved() { return g_FLICKConfig.toggleDIK != 0x41; }
static bool DebugMenuOriginalMoved() { return g_DebugMenuConfig.toggleDIK != 0x3B; }
static bool KreatEOriginalMoved() { return g_KreatEConfig.toggleVK != VK_END; }
static bool CSOriginalMoved() { return g_CSConfig.toggleVK != VK_END; }
static bool CatMenuOriginalMoved() { return g_CatMenuConfig.toggleImGuiKey != 577; }
static bool DragonbornOriginalMoved() { return g_DragonbornConfig.toggleKeyName != "F1"; }

static long long g_AliasLastPressedMs[AI_COUNT] = { 0 };
static long long g_AliasHoldStartedMs[AI_COUNT] = { 0 };
static bool g_AliasHoldTriggered[AI_COUNT] = { false };

static bool CheckAliasTrigger(int idx, bool chordDownVal) {
    static bool wasChordDown[AI_COUNT] = { false };
    if (!chordDownVal) {
        g_AliasHoldStartedMs[idx] = 0;
        g_AliasHoldTriggered[idx] = false;
        wasChordDown[idx] = false;
        return false;
    }

    const int mode = g_AliasTriggerMode[idx].load();
    const long long now = NowMs();

    const bool pressed = chordDownVal && !wasChordDown[idx];
    wasChordDown[idx] = chordDownVal;

    if (mode == 0) { // Single Press
        return pressed;
    }
    else if (mode == 1) { // Double Press
        if (pressed) {
            const long long diff = now - g_AliasLastPressedMs[idx];
            g_AliasLastPressedMs[idx] = now;
            if (diff > 40 && diff <= 300) {
                return true;
            }
        }
        return false;
    }
    else if (mode == 2) { // Hold to Trigger
        if (g_AliasHoldTriggered[idx]) {
            return false;
        }
        if (g_AliasHoldStartedMs[idx] == 0) {
            g_AliasHoldStartedMs[idx] = now;
        }
        else if (now - g_AliasHoldStartedMs[idx] >= (std::max)(50, g_AliasHoldDurationMs[idx].load())) {
            g_AliasHoldTriggered[idx] = true;
            return true;
        }
        return false;
    }
    return false;
}

static void PollOriginalHotkeyAliases(const BYTE* state) {
    if (!state || IsRebinding() || NowMs() < g_SuppressAliasUntilMs.load() || IsUserTyping()) return;

    const auto down = [&](WORD dik) { return dik < 256 && (state[dik] & 0x80) != 0; };
    const bool shift = down(0x2A) || down(0x36);
    const bool ctrl = down(0x1D) || down(0x9D);
    const bool alt = down(0x38) || down(0xB8);
    const bool plain = !shift && !ctrl && !alt;
    (void)plain;

    // A mod's user-rebindable alias key is "down" when its key is held with exactly its modifiers.
    const auto aliasDown = [&](int i) {
        const WORD d = g_AliasDik[i].load();
        return d != 0 && down(d) &&
               g_AliasCtrl[i].load() == ctrl && g_AliasShift[i].load() == shift && g_AliasAlt[i].load() == alt;
    };
    const auto physicallyDown = [](int vk) {
        return vk != 0 && ((g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk)) & 0x8000) != 0;
    };
    const auto physicalKeyDown = [&](WORD dik) {
        return physicallyDown(VKFromDIK(dik));
    };
    const auto physicalAliasDown = [&](int i) {
        const WORD vk = VKFromDIK(g_AliasDik[i].load());
        const bool physicalCtrl = physicallyDown(VK_CONTROL) || physicallyDown(VK_LCONTROL) || physicallyDown(VK_RCONTROL);
        const bool physicalShift = physicallyDown(VK_SHIFT) || physicallyDown(VK_LSHIFT) || physicallyDown(VK_RSHIFT);
        const bool physicalAlt = physicallyDown(VK_MENU) || physicallyDown(VK_LMENU) || physicallyDown(VK_RMENU);
        return physicallyDown(vk) &&
               physicalCtrl == g_AliasCtrl[i].load() &&
               physicalShift == g_AliasShift[i].load() &&
               physicalAlt == g_AliasAlt[i].load();
    };

    // KreatE opens via a SendInput End-tap; if the physical End is still held when we send
    // it, no fresh key-down edge is produced and KreatE ignores it. So KreatE alone defers
    // its action until the original key is released.
    static bool kreateTogglePending = false;
    static long long kreateTogglePendingSince = 0;
    if (kreateTogglePending) {
        const long long now = NowMs();
        if (now - kreateTogglePendingSince > 2000) {
            kreateTogglePending = false;
            SKSE::log::warn("DirectInput: timed out waiting for End release for KreatE.");
        } else if (!down(g_AliasDik[AI_KreatE].load())) {
            kreateTogglePending = false;
            const auto active = g_ActiveMenu.load();
            if (active == ActiveMenu::KreatE) {
                SKSE::log::info("DirectInput: End released; closing KreatE.");
                CloseActiveModMenu(active);
            } else if (active == ActiveMenu::None &&
                !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                !IsExternalMenuOpen() && !IsENBOpeningTransition() &&
                now >= g_MenuOpenLockUntilMs.load()) {
                SKSE::log::info("DirectInput: End released; opening KreatE.");
                OpenKreatE();
            }
        }
    }

    // ENB is not relocated (its key stays Shift+Enter and it reads it directly when allowed),
    // so bridge it only when its alias is NOT the native Shift+Enter — otherwise we'd double-fire.
    const bool enbAliasIsNative = g_AliasDik[AI_ENB].load() == 0x1C && g_AliasShift[AI_ENB].load() &&
                                  !g_AliasCtrl[AI_ENB].load() && !g_AliasAlt[AI_ENB].load();
    // Several DirectInput consumers poll independent snapshots. While Shift+End is held, one
    // snapshot can briefly omit Shift and make the CS main-menu End alias look pressed. Give the
    // more-specific Editor chord priority using the physical keyboard state shared by all polls.
    const bool csEditorClaimsChord = g_UnblockCSEditor.load() && g_CSConfig.enabled &&
        !AliasEqualsLauncher(AI_CSEditor) && (aliasDown(AI_CSEditor) || physicalAliasDown(AI_CSEditor));
    const std::array<bool, 11> chordDown = {
        g_UnblockOAR.load() && g_OARConfig.enabled && OAROriginalMoved() && !AliasEqualsLauncher(AI_OAR) && aliasDown(AI_OAR),
        g_UnblockIED.load() && g_IEDConfig.enabled &&
            g_AliasDik[AI_IED].load() != g_IEDConfig.toggleDIK && // alias == real key: IED opens on it directly; only bridge a REBOUND key
            !AliasEqualsLauncher(AI_IED) && aliasDown(AI_IED),
        g_UnblockENB.load() && g_ENBConfig.enabled && !enbAliasIsNative && !AliasEqualsLauncher(AI_ENB) && aliasDown(AI_ENB),
        g_UnblockDMenu.load() && g_DMenuConfig.enabled && DMenuOriginalMoved() && !AliasEqualsLauncher(AI_DMenu) && aliasDown(AI_DMenu),
        false, // Improved Camera aliases are handled once at the raw-input make edge.
        g_UnblockFLICK.load() && g_FLICKConfig.enabled && !AliasEqualsLauncher(AI_FLICK) && aliasDown(AI_FLICK),
        g_UnblockDebugMenu.load() && g_DebugMenuConfig.enabled &&
            g_AliasDik[AI_DebugMenu].load() != g_DebugMenuConfig.toggleDIK && // alias == real key: it opens directly, don't also inject
            !AliasEqualsLauncher(AI_DebugMenu) && aliasDown(AI_DebugMenu),
        g_UnblockKreatE.load() && g_KreatEConfig.enabled && KreatEOriginalMoved() && !AliasEqualsLauncher(AI_KreatE) && aliasDown(AI_KreatE),
        false, // Community Shaders aliases are handled once at the raw-input make edge.
        g_UnblockCatMenu.load() && g_CatMenuConfig.enabled && CatMenuOriginalMoved() && !AliasEqualsLauncher(AI_CatMenu) && aliasDown(AI_CatMenu),
        g_UnblockDragonborn.load() && g_DragonbornConfig.enabled && DragonbornOriginalMoved() &&
            !AliasEqualsLauncher(AI_Dragonborn) && aliasDown(AI_Dragonborn)
    };
    const std::array<bool, 11> primaryDown = {
        down(g_AliasDik[AI_OAR].load()), down(g_AliasDik[AI_IED].load()), down(g_AliasDik[AI_ENB].load()),
        down(g_AliasDik[AI_DMenu].load()), physicalKeyDown(g_AliasDik[AI_IC].load()), down(g_AliasDik[AI_FLICK].load()),
        down(g_AliasDik[AI_DebugMenu].load()), down(g_AliasDik[AI_KreatE].load()), physicalKeyDown(g_AliasDik[AI_CS].load()),
        down(g_AliasDik[AI_CatMenu].load()), down(g_AliasDik[AI_Dragonborn].load())
    };

    static std::array<bool, 11> wasPrimaryDown{};
    const std::array<ActiveMenu, 11> targets = {
        ActiveMenu::OAR, ActiveMenu::IED, ActiveMenu::ENB, ActiveMenu::DMenu,
        ActiveMenu::ImprovedCamera, ActiveMenu::FLICK, ActiveMenu::DebugMenu, ActiveMenu::KreatE, ActiveMenu::CS,
        ActiveMenu::CatMenu, ActiveMenu::Dragonborn
    };
    const std::array<const char*, 11> names = {
        "Open Animation Replacer", "IED", "ENB Editor", "dMenu",
        "Improved Camera", "FLICK", "Debug Menu", "KreatE", "Community Shaders", "CatMenu", "Dragonborn's Toolkit"
    };
    const std::array<void(*)(), 11> openActions = {
        OpenAnimationReplacer, OpenImmersiveEquipmentDisplays, OpenENB, OpenDMenu,
        OpenImprovedCamera, OpenFLICK, OpenDebugMenu, OpenKreatE, OpenCommunityShaders,
        OpenCatMenu, OpenDragonbornToolkit
    };

    const int loopAliasMap[11] = {
        AI_OAR, AI_IED, AI_ENB, AI_DMenu, AI_IC, AI_FLICK, AI_DebugMenu, AI_KreatE, AI_CS, AI_CatMenu, AI_Dragonborn
    };

    for (std::size_t i = 0; i < chordDown.size(); ++i) {
        const int idx = loopAliasMap[i];
        const bool pressed = CheckAliasTrigger(idx, chordDown[i]);
        wasPrimaryDown[i] = primaryDown[i];
        if (!pressed) continue;

        const long long now = NowMs();
        const long long last = g_LastOriginalHotkeyMs.exchange(now);
        if (now - last <= 500) continue;

        if (i == 7) {
            kreateTogglePending = true;
            kreateTogglePendingSince = now;
            SKSE::log::info("DirectInput: original KreatE alias waiting for End release.");
            continue;
        }

        const auto active = g_ActiveMenu.load();
        if (active == targets[i]) {
            SKSE::log::info("DirectInput: original {} alias closing active menu.", names[i]);
            CloseActiveModMenu(active);
            continue;
        }
        if (active != ActiveMenu::None ||
            (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) ||
            IsExternalMenuOpen() || IsENBOpeningTransition() ||
            now < g_MenuOpenLockUntilMs.load()) {
            continue;
        }

        SKSE::log::info("DirectInput: original {} alias opening menu.", names[i]);
        openActions[i]();
    }

    // SKSE Menu Framework: not relocated and not in the chordDown loop — bridge its alias here.
    {
        const bool mfChord = g_UnblockMF.load() && !AliasEqualsLauncher(AI_MF) && aliasDown(AI_MF);
        if (CheckAliasTrigger(AI_MF, mfChord)) {
            const long long now = NowMs();
            const long long last = g_LastOriginalHotkeyMs.exchange(now);
            if (now - last > 500) {
                const auto active = g_ActiveMenu.load();
                if (active == ActiveMenu::MF) {
                    CloseActiveModMenu(active);
                } else if (active == ActiveMenu::None &&
                    !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                    !IsExternalMenuOpen() && !IsENBOpeningTransition() &&
                    now >= g_MenuOpenLockUntilMs.load()) {
                    OpenSKSEMenuFramework();
                }
            }
        }
    }

    // Mod Function Menu is relocated to F20 so its native key is free. Its user-facing alias
    // still opens the menu through the same injected-key path as the launcher button.
    {
        const bool mfmChord = g_UnblockModFunctionMenu.load() &&
            g_ModFunctionMenuConfig.enabled &&
            !AliasEqualsLauncher(AI_ModFunctionMenu) &&
            aliasDown(AI_ModFunctionMenu);
        if (CheckAliasTrigger(AI_ModFunctionMenu, mfmChord)) {
            const long long now = NowMs();
            const long long last = g_LastOriginalHotkeyMs.exchange(now);
            if (now - last > 500) {
                const auto active = g_ActiveMenu.load();
                if (active == ActiveMenu::ModFunctionMenu) {
                    CloseActiveModMenu(active);
                } else if (active == ActiveMenu::None &&
                    !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                    !IsExternalMenuOpen() && !IsENBOpeningTransition() &&
                    now >= g_MenuOpenLockUntilMs.load()) {
                    OpenModFunctionMenu();
                }
            }
        }
    }

    // MCM: has no native key of its own, so bridge its alias here to open the Journal/MCM.
    {
        const bool mcmChord = g_UnblockMCM.load() && !g_ExcludeMod[AI_MCM].load() && !AliasEqualsLauncher(AI_MCM) && aliasDown(AI_MCM);
        if (CheckAliasTrigger(AI_MCM, mcmChord)) {
            const long long now = NowMs();
            const long long last = g_LastOriginalHotkeyMs.exchange(now);
            if (now - last > 500) {
                const auto active = g_ActiveMenu.load();
                if (active == ActiveMenu::MCM) {
                    CloseActiveModMenu(active);
                } else if (active == ActiveMenu::None &&
                    !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                    !IsExternalMenuOpen() && now >= g_MenuOpenLockUntilMs.load()) {
                    OpenMCM();
                }
            }
        }
    }

    // QAR: has no native key of its own, so bridge its alias here to open Quick Armor Rebalance via Papyrus.
    {
        const bool qarChord = g_UnblockQAR.load() && !g_ExcludeMod[AI_QAR].load() && !AliasEqualsLauncher(AI_QAR) && aliasDown(AI_QAR);
        if (CheckAliasTrigger(AI_QAR, qarChord)) {
            const long long now = NowMs();
            const long long last = g_LastOriginalHotkeyMs.exchange(now);
            if (now - last > 500) {
                const auto active = g_ActiveMenu.load();
                if (active == ActiveMenu::QAR) {
                    CloseActiveModMenu(active);
                } else if (active == ActiveMenu::None &&
                    !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                    !IsExternalMenuOpen() && now >= g_MenuOpenLockUntilMs.load()) {
                    OpenQAR();
                }
            }
        }
    }

    // Community Shaders sub-toggles (Editor/Overlay/Effect): bridge each rebound alias to open its
    // CS function. Skip when the alias is still the native key (CS reads that directly) to avoid
    // double-firing.
    {
        struct CSSub { int idx; std::atomic<bool>* en; void (*open)(); WORD nativeDik; };
        const CSSub subs[3] = {
            { AI_CSEditor,  &g_UnblockCSEditor,  OpenCSEditor,  g_CSConfig.editorDIK },
            { AI_CSOverlay, &g_UnblockCSOverlay, OpenCSOverlay, g_CSConfig.overlayDIK },
            { AI_CSEffect,  &g_UnblockCSEffect,  OpenCSEffect,  g_CSConfig.effectDIK },
        };
        static bool armed[3] = { true, true, true };
        static long long lastDownSeenMs[3] = { 0, 0, 0 };
        for (int s = 0; s < 3; ++s) {
            const int i = subs[s].idx;
            const WORD nativeModifier = s == 0 ? g_CSConfig.editorModifierDIK : 0;
            const bool nativeMatch = AliasMatchesChord(i, subs[s].nativeDik, nativeModifier);
            const bool nowDown = physicalKeyDown(g_AliasDik[i].load());
            const bool chord = false; // handled by HookedGetRawInputData
            const long long now2 = NowMs();
            if (nowDown) lastDownSeenMs[s] = now2;
            if (chord && armed[s]) {
                armed[s] = false;
                const long long last = g_LastOriginalHotkeyMs.exchange(now2);
                if (now2 - last > 500) {
                    SKSE::log::info("DirectInput: Community Shaders sub-menu {} alias pressed with exact modifiers.", s);
                    subs[s].open();
                }
            }
            // GetDeviceState is called by several consumers with different snapshots.
            // Re-arm only after every caller has reported the key released for a while.
            if (now2 - lastDownSeenMs[s] > 80) armed[s] = true;
        }
    }

    // Skyrim Party Sheet has four actions but needs no reserved keys. Enabled aliases are
    // converted into direct calls to its own input sink; the physical event still reaches Skyrim.
    if (g_PartySheetConfig.enabled) {
        struct PartyAction { int alias; std::atomic<bool>* enabled; void (*open)(); int sub; };
        const PartyAction actions[] = {
            { AI_PartySettings, &g_UnblockPartySettings, OpenPartySettings, 0 },
            { AI_PartySheet, &g_UnblockPartySheet, OpenPartySheet, 1 },
            { AI_PartyInspect, &g_UnblockPartyInspect, OpenPartyInspect, 2 },
            { AI_PartyCharacter, &g_UnblockPartyCharacter, OpenPartyCharacter, 3 }
        };
        static bool wasDown[4]{};
        for (int i = 0; i < 4; ++i) {
            const bool chord = actions[i].enabled->load() && !AliasEqualsLauncher(actions[i].alias) &&
                physicalAliasDown(actions[i].alias);
            const bool pressed = CheckAliasTrigger(actions[i].alias, chord);
            if (!pressed) continue;

            const auto active = g_ActiveMenu.load();
            if (active == ActiveMenu::PartySheet && g_ActivePartySheetSub.load() == actions[i].sub) {
                CloseActiveModMenu(active);
            } else if ((active == ActiveMenu::None || active == ActiveMenu::PartySheet) &&
                !(g_LauncherWindow && g_LauncherWindow->IsOpen.load()) &&
                (active == ActiveMenu::PartySheet || !IsExternalMenuOpen()) &&
                NowMs() >= g_MenuOpenLockUntilMs.load()) {
                actions[i].open();
            }
        }
    }
}

static RE::Actor* GetPartyInspectCrosshairActor() {
    auto* pick = RE::CrosshairPickData::GetSingleton();
    if (!pick) return nullptr;
    auto ref = pick->targetActor.get();
    if (!ref) ref = pick->target.get();
    return ref ? ref->As<RE::Actor>() : nullptr;
}

static void RememberPartyInspectActor() {
    if (!g_PartySheetConfig.enabled || (g_LauncherWindow && g_LauncherWindow->IsOpen.load())) return;
    auto* actor = GetPartyInspectCrosshairActor();
    g_LastPartyInspectActor.store(actor ? actor->GetFormID() : 0);
}

static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(IDirectInputDevice8A* pDevice,
                                                       DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_OrigGetDeviceState(pDevice, cbData, lpvData);
    SubclassGameWindow();
    // Zero each managed ImGui mod's toggle key in the engine's cached keyboard state so
    // their per-frame IsPressed() poll sees "not pressed" (gameplay ButtonEvents unaffected).
    SuppressManagedPollKeys();
    if (SUCCEEDED(hr) && lpvData && cbData == 256) {
        g_LastDIKeyboardPollMs.store(NowMs(), std::memory_order_relaxed);
        auto* state = reinterpret_cast<BYTE*>(lpvData);
        RememberPartyInspectActor();
        PollOriginalHotkeyAliases(state);

        // Sink registration timing varies between plugins. Discover managed sinks from
        // this guaranteed keyboard poll instead of waiting for the launcher to render.
        static std::atomic<long long> lastSinkScanMs{ 0 };
        const long long now = NowMs();
        long long lastScan = lastSinkScanMs.load(std::memory_order_relaxed);
        if (now - lastScan >= 1000 &&
            lastSinkScanMs.compare_exchange_strong(lastScan, now, std::memory_order_relaxed)) {
            TryHookModSinks();
            TryHookKeyboardProcess();
            TryHookCatMenuNativeKey();
            TryManageKreatEHotkey(true);
            TryManageCSHotkey(true);
            TryManageCatMenuHotkey(true);
            TryManageModFunctionMenuHotkey(true);
            TryManageFLICKHotkey(true);
            TryManageOPSHotkey(true);
            TryManageDragonbornHotkey(true);
            SyncDMenuKeyViaApi();  // keep dMenu's key freed (or restored) live via the v2 API
            SyncSearchUIKey();     // keep SearchUI's key freed (F18) or restored, live via the Papyrus VM
        }

        if (g_AllowESCSinkBlock.load() || EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load()) {
            state[0x01] = 0; // zero kEscapeDIK (0x01)
        }
        if (!g_DI8HookFiredOnce.exchange(true))
            SKSE::log::info("DI8 GetDeviceState hook firing — hook is active.");

        // Launcher-hotkey detection at the DirectInput level (this is the path that
        // actually sees in-game key presses). Edge-detected + debounced in ToggleLauncher.
        {
            // GetDeviceState is polled by several callers each frame (engine, Steam overlay, ...),
            // each with its own buffer. If one reports the key released while another reports it
            // held, a naive edge test flip-flops and fires phantom edges (a tap could open then
            // instantly close). So we LATCH "pressed" for a short window (> one frame) across all
            // callers and fire exactly one edge per real press, re-arming only once the key has
            // truly been released. Works on whichever poll actually has the key (needed for Easy
            // Close while a menu is open).
            bool launcherKeyInjection =
                (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) ||
                (g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load());
            long long now = NowMs();
            const bool rawPressed = !IsRebinding() && (state[g_LauncherHotkeyDIK.load()] & 0x80) != 0;

            static long long s_lastPressedSeenMs = 0;
            static bool s_armed = true;
            if (rawPressed) s_lastPressedSeenMs = now;
            const bool pressedLatched = (now - s_lastPressedSeenMs) <= 40; // bridge multi-caller gaps (> 1 frame)

            bool f1JustPressed = false;
            if (pressedLatched) {
                if (s_armed) { f1JustPressed = true; s_armed = false; }
            } else {
                s_armed = true; // key truly released — ready for the next press
            }

            // Ignore the launcher key while a just-opened mod is still appearing (prevents
            // the open-delay desync that left Debug Menu/FLICK un-closable).
            bool menuOpening = IsENBOpeningTransition() || NowMs() < g_MenuOpenLockUntilMs.load();
            if (f1JustPressed && !launcherKeyInjection && !menuOpening) {
                const bool launcherOpen = g_LauncherWindow && g_LauncherWindow->IsOpen.load();
                const bool somethingOpen = launcherOpen || g_ActiveMenu.load() != ActiveMenu::None;
                if (g_LauncherHotkeyEasyClose.load() && somethingOpen) {
                    // Easy Close: a single bare press of the key closes whatever is open —
                    // no modifier or double-tap required (opening still needs the full hotkey).
                    ToggleLauncher();
                } else if (AreLauncherModifiersOk() && !g_LauncherHotkeyHold.load()) {
                    if (g_LauncherHotkeyDoubleTap.load()) {
                        // The two-tap gesture is its own debounce — don't gate it on the 700ms timer.
                        if (CheckLauncherDoubleTap(now)) {
                            ToggleLauncher();
                        }
                    } else if (now - g_LastLauncherToggleMs.load() > 700) {
                        ToggleLauncher();
                    }
                }
            }
        }

        // READ-ONLY master-key detection only. We do NOT modify the key state here (no
        // global zeroing) — that would block the key itself (Requirement #4). This runs
        // every frame from the engine's own poll, so the launcher key works even while
        // another mod's menu is capturing the input-event stream.
        return hr;

    }
    return hr;
}

// ============================================================================
// Hook 1b — IDirectInputDevice8A::GetDeviceData  (vtable slot 10)
//   Skyrim builds the ButtonEvents that other SKSE mods (dMenu, FLICK, DebugMenu…)
//   read from BUFFERED DirectInput data — GetDeviceData — NOT from the GetDeviceState
//   array. Zeroing GetDeviceState therefore can't stop them. Here we drop the buffered
//   events for blocked keys, so the engine never generates their ButtonEvent.
// ============================================================================
static bool ShouldStripDIKFromBuffer(WORD dik) {
    if (dik == 0) return false;
    if (dik == 0x01 && (g_AllowESCSinkBlock.load() || EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load())) {
        SKSE::log::info("ShouldStripDIKFromBuffer: stripping Escape key event.");
        return true;
    }
    // dMenu is managed only through the v2 API, which disables its own key live, so dMenu never
    // reacts to its key no matter what — we don't strip it here (that would only block the key for
    // the game/other mods, exactly what this mod prevents). Stock dMenu (no API) is not supported.
    return false;
}

using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetDeviceData_t g_OrigGetDeviceData = nullptr;
static std::atomic<bool> g_DI8DataHookFiredOnce{ false };

static HRESULT STDMETHODCALLTYPE HookedGetDeviceData(IDirectInputDevice8A* pDevice, DWORD cbObjectData,
        LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    HRESULT hr = g_OrigGetDeviceData(pDevice, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (SUCCEEDED(hr) && rgdod && pdwInOut && *pdwInOut > 0 && cbObjectData == sizeof(DIDEVICEOBJECTDATA)) {
        if (!g_DI8DataHookFiredOnce.exchange(true))
            SKSE::log::info("DI8 GetDeviceData hook firing — buffered input path is active.");

        DWORD count = *pdwInOut;
        DWORD outIdx = 0;
        for (DWORD i = 0; i < count; ++i) {
            WORD dik = static_cast<WORD>(rgdod[i].dwOfs & 0xFF);
            if (ShouldStripDIKFromBuffer(dik)) {
                continue; // drop this buffered event
            }
            if (outIdx != i) rgdod[outIdx] = rgdod[i];
            ++outIdx;
        }
        *pdwInOut = outIdx;
    }
    return hr;
}

// GUIDs inline — avoid needing dxguid.lib
static const GUID s_IID_IDirectInput8A =
    { 0xBF798031, 0x483A, 0x4DA2, {0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00} };
static const GUID s_GUID_SysKeyboard   =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };

// ============================================================================
// Hook 2 — GetAsyncKeyState (User32.dll)
// Hides polling-based menu chords only from the DLL that owns each menu.
// ============================================================================

static SHORT WINAPI HookedGetAsyncKeyState(int vKey) {
    const auto real = [&]() -> SHORT { return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vKey) : ::GetAsyncKeyState(vKey); };
    if (g_WaitingForHotkeyPress.load()) return real();
    if (vKey == VK_ESCAPE && (EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load())) {
        return 0;
    }
    const bool isModifierKey = vKey == VK_CONTROL || vKey == VK_LCONTROL || vKey == VK_RCONTROL ||
        vKey == VK_SHIFT || vKey == VK_LSHIFT || vKey == VK_RSHIFT ||
        vKey == VK_MENU || vKey == VK_LMENU || vKey == VK_RMENU;
    if (g_PartySheetDirectDispatch.load() && g_PartySheetConfig.modifierDIK != 0 &&
        vKey == VKFromDIK(g_PartySheetConfig.modifierDIK) &&
        IsCallerModule(_ReturnAddress(), "SkyrimPartySheet")) {
        return static_cast<SHORT>(0x8000);
    }
    // The user's alias chord (e.g. Shift+F) adds a physical modifier that is NOT part of Party
    // Sheet's own binding. Hide those extra modifiers from Party Sheet during the dispatch so it
    // reads the plain dispatched key instead of "Shift+X" and ignores it. Its own modifier (if any)
    // is force-pressed by the check above and preserved here.
    if (g_PartySheetDirectDispatch.load() && isModifierKey &&
        IsCallerModule(_ReturnAddress(), "SkyrimPartySheet")) {
        const WORD ownMod = g_PartySheetConfig.modifierDIK != 0 ? VKFromDIK(g_PartySheetConfig.modifierDIK) : 0;
        const bool isOwn = ownMod != 0 && (vKey == ownMod ||
            (ownMod == VK_SHIFT   && (vKey == VK_LSHIFT   || vKey == VK_RSHIFT)) ||
            (ownMod == VK_CONTROL && (vKey == VK_LCONTROL || vKey == VK_RCONTROL)) ||
            (ownMod == VK_MENU    && (vKey == VK_LMENU    || vKey == VK_RMENU)));
        if (!isOwn) return 0;
    }
    if (isModifierKey && NowMs() <= g_NeutralizeCSModifiersUntilMs.load() &&
        IsCallerModule(_ReturnAddress(), "CommunityShaders")) {
        return 0;
    }
    const bool isKreatEKey = g_KreatEConfig.enabled && g_KreatEConfig.toggleVK != 0 &&
        vKey == g_KreatEConfig.toggleVK;
    if (isKreatEKey && !g_ExcludeMod[AI_KreatE].load() && !InternalAliasKeyAllowed(AI_KreatE, g_UnblockKreatE, g_KreatEConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "KreatE")) return 0;
    }

    const bool isCSKey = g_CSConfig.enabled && g_CSConfig.toggleVK != 0 &&
        vKey == g_CSConfig.toggleVK;
    if (isCSKey && !g_ExcludeMod[AI_CS].load() && !InternalAliasKeyAllowed(AI_CS, g_UnblockCS, g_CSConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowCSOpen.load() && NowMs() <= g_AllowCSOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "CommunityShaders")) return 0;
    }

    const bool isDragonbornKey = g_DragonbornConfig.enabled && g_DragonbornConfig.toggleVK != 0 &&
        vKey == g_DragonbornConfig.toggleVK;
    if (isDragonbornKey && !g_ExcludeMod[AI_Dragonborn].load() && !InternalAliasKeyAllowed(AI_Dragonborn, g_UnblockDragonborn, g_DragonbornConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "SkyrimCheatMenu")) return 0;
    }

    const bool isIEDKey = g_IEDConfig.enabled && g_IEDConfig.toggleVK != 0 &&
        vKey == g_IEDConfig.toggleVK;
    if (isIEDKey && !g_ExcludeMod[AI_IED].load() && !InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowIEDOpen.load() && NowMs() <= g_AllowIEDOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "ImmersiveEquipmentDisplays")) return 0;
    }

    // Improved Camera polls its menu key. Hide the completed chord only from Improved
    // Camera so Skyrim and other mods can continue using the same gameplay binding.
    const bool isICKey = g_ImprovedCameraConfig.enabled && g_ImprovedCameraConfig.toggleVK != 0 &&
        vKey == g_ImprovedCameraConfig.toggleVK;
    if (isICKey && !g_ExcludeMod[AI_IC].load() && !IsUserTyping()) {
        const bool allowed = g_AllowImprovedCameraOpen.load() &&
            NowMs() <= g_AllowImprovedCameraOpenUntilMs.load();
        if (!allowed && IsImprovedCameraModifierDown() &&
            IsCallerModule(_ReturnAddress(), "ImprovedCameraSE")) return 0;
    }

    const bool isENBKey = g_ENBConfig.enabled &&
        ((g_ENBConfig.editorVK != 0 && vKey == g_ENBConfig.editorVK) ||
         (g_ENBConfig.combinationVK != 0 && vKey == g_ENBConfig.combinationVK));
    if (!isENBKey) return real();

    // ENB editor key: return "not pressed" ONLY when ENB's own d3d11 wrapper is asking,
    // so the game still sees the key normally. Let it through while opening ENB.
    if (!g_ExcludeMod[AI_ENB].load() && !InternalENBKeyAllowed() && !IsUserTyping()) {
        const bool allowed = g_AllowENBOpen.load() && NowMs() <= g_AllowENBOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "d3d11")) return 0;
    }
    return real();

}


// ============================================================================
// Hook 2.5 — GetKeyState (User32.dll)
// Returns 0 for blocked keys so WinAPI GetKeyState polls also see "not pressed".
// ============================================================================
using GetKeyState_t = SHORT(WINAPI*)(int);
static GetKeyState_t g_OrigGetKeyState = nullptr;

static SHORT WINAPI HookedGetKeyState(int vKey) {
    const auto real = [&]() -> SHORT { return g_OrigGetKeyState ? g_OrigGetKeyState(vKey) : ::GetKeyState(vKey); };
    if (g_WaitingForHotkeyPress.load()) return real();
    if (vKey == VK_ESCAPE && (EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load())) {
        return 0;
    }
    const bool isModifierKey = vKey == VK_CONTROL || vKey == VK_LCONTROL || vKey == VK_RCONTROL ||
        vKey == VK_SHIFT || vKey == VK_LSHIFT || vKey == VK_RSHIFT ||
        vKey == VK_MENU || vKey == VK_LMENU || vKey == VK_RMENU;
    if (g_PartySheetDirectDispatch.load() && g_PartySheetConfig.modifierDIK != 0 &&
        vKey == VKFromDIK(g_PartySheetConfig.modifierDIK) &&
        IsCallerModule(_ReturnAddress(), "SkyrimPartySheet")) {
        return static_cast<SHORT>(0x8000);
    }
    // The user's alias chord (e.g. Shift+F) adds a physical modifier that is NOT part of Party
    // Sheet's own binding. Hide those extra modifiers from Party Sheet during the dispatch so it
    // reads the plain dispatched key instead of "Shift+X" and ignores it. Its own modifier (if any)
    // is force-pressed by the check above and preserved here.
    if (g_PartySheetDirectDispatch.load() && isModifierKey &&
        IsCallerModule(_ReturnAddress(), "SkyrimPartySheet")) {
        const WORD ownMod = g_PartySheetConfig.modifierDIK != 0 ? VKFromDIK(g_PartySheetConfig.modifierDIK) : 0;
        const bool isOwn = ownMod != 0 && (vKey == ownMod ||
            (ownMod == VK_SHIFT   && (vKey == VK_LSHIFT   || vKey == VK_RSHIFT)) ||
            (ownMod == VK_CONTROL && (vKey == VK_LCONTROL || vKey == VK_RCONTROL)) ||
            (ownMod == VK_MENU    && (vKey == VK_LMENU    || vKey == VK_RMENU)));
        if (!isOwn) return 0;
    }
    if (isModifierKey && NowMs() <= g_NeutralizeCSModifiersUntilMs.load() &&
        IsCallerModule(_ReturnAddress(), "CommunityShaders")) {
        return 0;
    }
    const bool isKreatEKey = g_KreatEConfig.enabled && g_KreatEConfig.toggleVK != 0 &&
        vKey == g_KreatEConfig.toggleVK;
    if (isKreatEKey && !g_ExcludeMod[AI_KreatE].load() && !InternalAliasKeyAllowed(AI_KreatE, g_UnblockKreatE, g_KreatEConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "KreatE")) return 0;
    }

    const bool isCSKey = g_CSConfig.enabled && g_CSConfig.toggleVK != 0 &&
        vKey == g_CSConfig.toggleVK;
    if (isCSKey && !g_ExcludeMod[AI_CS].load() && !InternalAliasKeyAllowed(AI_CS, g_UnblockCS, g_CSConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowCSOpen.load() && NowMs() <= g_AllowCSOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "CommunityShaders")) return 0;
    }

    const bool isDragonbornKey = g_DragonbornConfig.enabled && g_DragonbornConfig.toggleVK != 0 &&
        vKey == g_DragonbornConfig.toggleVK;
    if (isDragonbornKey && !g_ExcludeMod[AI_Dragonborn].load() && !InternalAliasKeyAllowed(AI_Dragonborn, g_UnblockDragonborn, g_DragonbornConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "SkyrimCheatMenu")) return 0;
    }

    const bool isIEDKey = g_IEDConfig.enabled && g_IEDConfig.toggleVK != 0 &&
        vKey == g_IEDConfig.toggleVK;
    if (isIEDKey && !g_ExcludeMod[AI_IED].load() && !InternalAliasKeyAllowed(AI_IED, g_UnblockIED, g_IEDConfig.toggleDIK) && !IsUserTyping()) {
        const bool allowed = g_AllowIEDOpen.load() && NowMs() <= g_AllowIEDOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "ImmersiveEquipmentDisplays")) return 0;
    }

    const bool isICKey = g_ImprovedCameraConfig.enabled && g_ImprovedCameraConfig.toggleVK != 0 &&
        vKey == g_ImprovedCameraConfig.toggleVK;
    if (isICKey && !g_ExcludeMod[AI_IC].load() && !IsUserTyping()) {
        const bool allowed = g_AllowImprovedCameraOpen.load() &&
            NowMs() <= g_AllowImprovedCameraOpenUntilMs.load();
        if (!allowed && IsImprovedCameraModifierDown() &&
            IsCallerModule(_ReturnAddress(), "ImprovedCameraSE")) return 0;
    }

    const bool isENBKey = g_ENBConfig.enabled &&
        ((g_ENBConfig.editorVK != 0 && vKey == g_ENBConfig.editorVK) ||
         (g_ENBConfig.combinationVK != 0 && vKey == g_ENBConfig.combinationVK));
    if (!isENBKey) return real();
    if (!g_ExcludeMod[AI_ENB].load() && !InternalENBKeyAllowed() && !IsUserTyping()) {
        const bool allowed = g_AllowENBOpen.load() && NowMs() <= g_AllowENBOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "d3d11")) return 0;
    }
    return real();

}


// ============================================================================
// Hook 4 — per-mod InputEvent sink filtering  (THE primary block mechanism)
//   Most managed mods (dMenu, Improved Camera, FLICK, IED, DebugMenu, SKSE Menu
//   Framework) read their toggle from the game's InputEvent system. We hook EACH mod's
//   own sink (BSTEventSink::ProcessEvent, vtable slot 1) and splice that mod's key out
//   of the event list before it sees it — so the mod's menu never opens, but the key is
//   untouched globally (the game and every other sink still get it). When a mod is being
//   opened from our launcher, we let its key through to that mod only. The list is
//   restored after the sink runs, so other sinks are unaffected. No files are edited.
// ============================================================================
using ProcessInputEvent_t = RE::BSEventNotifyControl(*)(
    RE::BSTEventSink<RE::InputEvent*>*, RE::InputEvent* const*, RE::BSTEventSource<RE::InputEvent*>*);

enum class ManagedSink { DMenu, IC, FLICK, IED, DebugMenu, CS, PartySheet, MF };

static WORD SinkBlockDIK(ManagedSink m) {
    switch (m) {
        case ManagedSink::DMenu:     return g_DMenuConfig.toggleDIK;
        case ManagedSink::IC:        return g_ImprovedCameraConfig.toggleDIK;
        case ManagedSink::FLICK:     return g_FLICKConfig.toggleDIK;
        case ManagedSink::IED:       return g_IEDConfig.toggleDIK;
        case ManagedSink::DebugMenu: return g_DebugMenuConfig.toggleDIK;
        case ManagedSink::CS:        return g_CSConfig.toggleDIK;
        case ManagedSink::PartySheet:return 0;
        case ManagedSink::MF:        return g_MFConfig.toggleDIK;
    }
    return 0;
}

// True => let this mod see its key right now (it's being opened from our launcher, or
// the user unblocked it). MF is always filtered (we open MF's window programmatically).
static bool SinkLetThrough(ManagedSink m) {
    const long long now = NowMs();
    switch (m) {
        case ManagedSink::DMenu:     return (g_UnblockDMenu.load() && AliasMatchesChord(AI_DMenu, g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK)) || (g_AllowDMenuOpen.load() && now <= g_AllowDMenuOpenUntilMs.load());
        case ManagedSink::IC:        return g_AllowImprovedCameraOpen.load() && now <= g_AllowImprovedCameraOpenUntilMs.load();
        case ManagedSink::FLICK:     return (g_UnblockFLICK.load() && AliasMatchesChord(AI_FLICK, g_FLICKConfig.toggleDIK)) || (g_AllowFLICKOpen.load() && now <= g_AllowFLICKOpenUntilMs.load());
        case ManagedSink::IED:       return (g_UnblockIED.load() && AliasMatchesChord(AI_IED, g_IEDConfig.toggleDIK)) || (g_AllowIEDOpen.load() && now <= g_AllowIEDOpenUntilMs.load());
        case ManagedSink::DebugMenu: return (g_UnblockDebugMenu.load() && AliasMatchesChord(AI_DebugMenu, g_DebugMenuConfig.toggleDIK)) || (g_AllowDebugMenuOpen.load() && now <= g_AllowDebugMenuOpenUntilMs.load());
        case ManagedSink::CS:        return (g_UnblockCS.load() && AliasMatchesChord(AI_CS, g_CSConfig.toggleDIK)) || (g_AllowCSOpen.load() && now <= g_AllowCSOpenUntilMs.load());
        case ManagedSink::PartySheet:return false;
        case ManagedSink::MF:        return false;
    }
    return false;
}

static bool IsButtonForDIK(const RE::InputEvent* ev, std::uint32_t dik) {
    if (!ev || ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) return false;
    if (ev->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;
    return static_cast<const RE::ButtonEvent*>(ev)->GetIDCode() == dik;
}

// Call orig with all ButtonEvents for `blockDIK` spliced out of the list, then restore.
static RE::BSEventNotifyControl FilterDispatch(RE::BSTEventSink<RE::InputEvent*>* sink, ProcessInputEvent_t orig,
        RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* src, std::uint32_t blockDIK) {
    if (!a_event || !*a_event || blockDIK == 0) return orig(sink, a_event, src);

    static thread_local std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> saved;
    saved.clear();

    RE::InputEvent* head = *a_event;
    while (head && IsButtonForDIK(head, blockDIK)) head = head->next;
    for (RE::InputEvent* cur = head; cur; ) {
        RE::InputEvent* nxt = cur->next;
        while (nxt && IsButtonForDIK(nxt, blockDIK)) nxt = nxt->next;
        if (cur->next != nxt) { saved.emplace_back(cur, cur->next); cur->next = nxt; }
        cur = nxt;
    }

    RE::InputEvent* localHead = head;
    auto result = orig(sink, &localHead, src);
    for (auto& pr : saved) pr.first->next = pr.second; // restore for the next sink
    return result;
}

static RE::BSEventNotifyControl FilterIEDDispatch(RE::BSTEventSink<RE::InputEvent*>* sink,
        ProcessInputEvent_t orig, RE::InputEvent* const* a_event,
        RE::BSTEventSource<RE::InputEvent*>* src) {
    auto* manager = RE::BSInputDeviceManager::GetSingleton();
    auto* keyboard = manager ? manager->GetKeyboard() : nullptr;
    const WORD dik = g_IEDConfig.toggleDIK;
    if (!keyboard || dik == 0 || dik >= 256) {
        return FilterDispatch(sink, orig, a_event, src, dik);
    }

    // IED's ProcessEvent also consults the keyboard's immediate state. Hide only
    // Backspace for the duration of IED's callback, then restore it for everyone else.
    const BYTE cur = keyboard->curState[dik];
    const BYTE prev = keyboard->prevState[dik];
    keyboard->curState[dik] = 0;
    keyboard->prevState[dik] = 0;
    const auto result = FilterDispatch(sink, orig, a_event, src, dik);
    keyboard->curState[dik] = cur;
    keyboard->prevState[dik] = prev;
    return result;
}

static bool ShouldFilterCSButton(const RE::InputEvent* ev) {
    if (!ev || ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
        ev->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;

    const WORD dik = static_cast<const RE::ButtonEvent*>(ev)->GetIDCode();
    bool recognized = false;
    bool directlyAllowed = false;
    int passIndex = -1;
    if (dik == g_CSConfig.toggleDIK) {
        recognized = true;
        passIndex = 0;
    }
    const WORD editorModifierVK = VKFromDIK(g_CSConfig.editorModifierDIK);
    const bool editorModifierDown = editorModifierVK == 0 ||
        ((g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(editorModifierVK) : ::GetAsyncKeyState(editorModifierVK)) & 0x8000) != 0;
    if (dik == g_CSConfig.editorDIK && editorModifierDown) {
        recognized = true;
        passIndex = 1;
    }
    if (dik == g_CSConfig.overlayDIK &&
        !(dik == g_CSConfig.editorDIK && editorModifierDown)) {
        recognized = true;
        passIndex = 2;
    }
    if (dik == g_CSConfig.effectDIK) {
        recognized = true;
        passIndex = 3;
    }
    if (!recognized || directlyAllowed) return false;

    const auto* button = static_cast<const RE::ButtonEvent*>(ev);
    if (g_AllowCSOpen.load() && NowMs() <= g_AllowCSOpenUntilMs.load() &&
        button->IsDown() && passIndex >= 0) {
        int remaining = g_CSKeyPassCount[passIndex].load();
        while (remaining > 0) {
            if (g_CSKeyPassCount[passIndex].compare_exchange_weak(remaining, remaining - 1)) {
                SKSE::log::info("CS input: accepted injected key-down for pass index {} ({}).", passIndex, NameFromDIK(dik));
                return false;
            }
        }
    }
    return true;
}

static void ArmCSKeyPass(int index) {
    for (auto& count : g_CSKeyPassCount) count.store(0);
    if (index >= 0 && index < static_cast<int>(g_CSKeyPassCount.size())) {
        g_CSKeyPassCount[index].store(1);
    }
}

static RE::BSEventNotifyControl FilterDispatchCS(RE::BSTEventSink<RE::InputEvent*>* sink, ProcessInputEvent_t orig,
        RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* src) {
    if (!a_event || !*a_event) return orig(sink, a_event, src);

    static thread_local std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> saved;
    saved.clear();
    RE::InputEvent* head = *a_event;
    while (head && ShouldFilterCSButton(head)) head = head->next;
    for (RE::InputEvent* cur = head; cur; ) {
        RE::InputEvent* nxt = cur->next;
        while (nxt && ShouldFilterCSButton(nxt)) nxt = nxt->next;
        if (cur->next != nxt) { saved.emplace_back(cur, cur->next); cur->next = nxt; }
        cur = nxt;
    }
    RE::InputEvent* localHead = head;
    auto result = orig(sink, &localHead, src);
    for (auto& pr : saved) pr.first->next = pr.second;
    return result;
}

static bool ShouldFilterPartySheetButton(const RE::InputEvent* ev) {
    if (!ev || ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
        ev->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;

    const WORD dik = static_cast<const RE::ButtonEvent*>(ev)->GetIDCode();
    const struct Entry { WORD key; int alias; std::atomic<bool>* enabled; } entries[] = {
        { g_PartySheetConfig.settingsDIK, AI_PartySettings, &g_UnblockPartySettings },
        { g_PartySheetConfig.partyDIK, AI_PartySheet, &g_UnblockPartySheet },
        { g_PartySheetConfig.inspectDIK, AI_PartyInspect, &g_UnblockPartyInspect },
        { g_PartySheetConfig.characterDIK, AI_PartyCharacter, &g_UnblockPartyCharacter }
    };
    for (const auto& entry : entries) {
        if (dik != entry.key) continue;
        // Enabled aliases are bridged by Risa rather than passed through. This keeps menu
        // state deterministic while the physical key remains available to Skyrim.
        return true;
    }
    return false;
}

static RE::BSEventNotifyControl FilterDispatchPartySheet(RE::BSTEventSink<RE::InputEvent*>* sink,
        ProcessInputEvent_t orig, RE::InputEvent* const* a_event,
        RE::BSTEventSource<RE::InputEvent*>* src) {
    if (!a_event || !*a_event) return orig(sink, a_event, src);

    static thread_local std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> saved;
    saved.clear();
    RE::InputEvent* head = *a_event;
    while (head && ShouldFilterPartySheetButton(head)) head = head->next;
    for (RE::InputEvent* cur = head; cur; ) {
        RE::InputEvent* next = cur->next;
        while (next && ShouldFilterPartySheetButton(next)) next = next->next;
        if (cur->next != next) {
            saved.emplace_back(cur, cur->next);
            cur->next = next;
        }
        cur = next;
    }
    RE::InputEvent* localHead = head;
    const auto result = orig(sink, &localHead, src);
    for (auto& item : saved) item.first->next = item.second;
    return result;
}

// Community Shaders does not register a normal InputEvent sink. It branch-hooks Skyrim's
// dispatcher and copies events into Menu::ProcessInputEvents before sink dispatch begins.
// Hook that private queue entry point, hide only CS's managed keys for the duration of its
// call, then restore the list so Skyrim and every other mod still receive the same events.
using CSProcessInputEvents_t = void(*)(void*, RE::InputEvent* const*);
static CSProcessInputEvents_t g_OrigCSProcessInputEvents = nullptr;
static std::atomic<void*> g_CSMenuLast{ nullptr }; // last-seen CS menu ptr, for direct-close injection
static void HookedCSProcessInputEvents(void* menu, RE::InputEvent* const* a_event) {
    g_CSMenuLast.store(menu);
    if (!a_event || !*a_event) {
        g_OrigCSProcessInputEvents(menu, a_event);
        return;
    }

    static thread_local std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> saved;
    saved.clear();
    RE::InputEvent* head = *a_event;
    while (head && ShouldFilterCSButton(head)) head = head->next;
    for (RE::InputEvent* cur = head; cur; ) {
        RE::InputEvent* nxt = cur->next;
        while (nxt && ShouldFilterCSButton(nxt)) nxt = nxt->next;
        if (cur->next != nxt) { saved.emplace_back(cur, cur->next); cur->next = nxt; }
        cur = nxt;
    }
    RE::InputEvent* localHead = head;
    g_OrigCSProcessInputEvents(menu, &localHead);
    for (auto& pr : saved) pr.first->next = pr.second;
}

static void TryHookCSInput() {
    if (g_OrigCSProcessInputEvents || !g_CSConfig.enabled) return;
    HMODULE module = ::GetModuleHandleA("CommunityShaders.dll");
    if (!module) return;

    // Community Shaders 1.5.2 Menu::ProcessInputEvents. Validate a long prologue before
    // touching the private function so an incompatible update fails closed instead of crashing.
    auto* target = reinterpret_cast<std::uint8_t*>(module) + 0x376A60;
    constexpr std::array<std::uint8_t, 14> expected{
        0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC
    };
    if (std::memcmp(target, expected.data(), expected.size()) != 0) {
        SKSE::log::error("TryHookCSInput: unsupported Community Shaders build; input symbol signature changed.");
        return;
    }

    const MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(HookedCSProcessInputEvents),
        reinterpret_cast<void**>(&g_OrigCSProcessInputEvents));
    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(target);
        SKSE::log::info("TryHookCSInput: hooked Community Shaders private input queue at {:p}.", static_cast<void*>(target));
    } else {
        SKSE::log::error("TryHookCSInput: MH_CreateHook failed ({}).", static_cast<int>(status));
    }
}

// Registry of hooked sink functions (keyed by the ProcessEvent function address, so it
// covers multiple sink instances of the same class). Fixed array + atomic count makes it
// safe to append on the render thread while the input thread reads it.
struct SinkHookEntry {
    void* fn;
    ProcessInputEvent_t orig;
    RE::BSTEventSink<RE::InputEvent*>* sink;
    ManagedSink which;
};
static std::array<SinkHookEntry, 16> g_SinkEntries{};
static std::atomic<size_t> g_SinkEntryCount{ 0 };

static bool DispatchManagedSinkToggle(int whichValue) {
    const auto which = static_cast<ManagedSink>(whichValue);
    if (which != ManagedSink::DMenu && which != ManagedSink::IED) return false;

    const WORD dik = SinkBlockDIK(which);
    if (dik == 0) return false;

    const size_t count = g_SinkEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = g_SinkEntries[i];
        if (entry.which != which || !entry.orig || !entry.sink) continue;

        auto* manager = RE::BSInputDeviceManager::GetSingleton();
        auto* source = manager ? static_cast<RE::BSTEventSource<RE::InputEvent*>*>(manager) : nullptr;

        // Deliver a COMPLETE press: key-down then key-up. A down-only event left the mod mid-press
        // (dMenu needed a second press / a stray keypress and would not close). One tap = one toggle.
        const bool withMod = (which == ManagedSink::DMenu && g_DMenuConfig.modifierDIK != 0);
        auto sendPress = [&](float value, float heldSecs) -> bool {
            auto* button = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(), dik, value, heldSecs);
            if (!button) return false;
            RE::ButtonEvent* modifier = nullptr;
            if (withMod) {
                modifier = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(),
                    g_DMenuConfig.modifierDIK, value, heldSecs);
                if (!modifier) { button->~ButtonEvent(); RE::free(button); return false; }
                modifier->next = button;
            }
            RE::InputEvent* head = modifier ? static_cast<RE::InputEvent*>(modifier) : button;
            entry.orig(entry.sink, &head, source);
            if (modifier) { modifier->~ButtonEvent(); RE::free(modifier); }
            button->~ButtonEvent(); RE::free(button);
            return true;
        };
        if (!sendPress(1.0f, 0.0f)) return false;  // key down (IsDown)
        sendPress(0.0f, 0.12f);                    // key up (release)

        SKSE::log::info("DispatchManagedSinkToggle: dispatched full press of {} to {}.",
            NameFromDIK(dik), which == ManagedSink::DMenu ? "dMenu" : "IED");
        return true;
    }
    return false;
}

// Close/toggle Community Shaders by feeding a key straight into its private input processor
// (g_OrigCSProcessInputEvents), using the last-seen CS menu pointer. This is where CS actually reads
// its toggle, and it bypasses the engine input stream — needed while the console is open, since the
// console captures engine input so a normal InjectEngineKey never reaches CS. One tap = down + up.
static bool DispatchKeyToCSSink(WORD dik) {
    if (dik == 0 || !g_OrigCSProcessInputEvents) return false;
    void* menu = g_CSMenuLast.load();
    if (!menu) return false;
    const auto send = [&](float value, float heldSecs) {
        auto* key = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(), dik, value, heldSecs);
        if (!key) return;
        RE::InputEvent* head = key;
        g_OrigCSProcessInputEvents(menu, &head);
        key->~ButtonEvent();
        RE::free(key);
    };
    send(1.0f, 0.0f);   // key down
    send(0.0f, 0.12f);  // key up
    return true;
}

static bool DispatchPartySheetKey(WORD dik) {
    if (!g_PartySheetConfig.enabled || dik == 0) return false;

    const size_t count = g_SinkEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = g_SinkEntries[i];
        if (entry.which != ManagedSink::PartySheet || !entry.orig || !entry.sink) continue;

        auto* manager = RE::BSInputDeviceManager::GetSingleton();
        auto* source = manager ? static_cast<RE::BSTEventSource<RE::InputEvent*>*>(manager) : nullptr;
        const WORD modifier = g_PartySheetConfig.modifierDIK;
        const auto send = [&](float value, float heldSecs) -> bool {
            auto* key = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(), dik, value, heldSecs);
            if (!key) return false;
            RE::ButtonEvent* mod = nullptr;
            if (modifier != 0 && dik != kEscapeDIK) {
                mod = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString(), modifier, value, heldSecs);
                if (!mod) {
                    key->~ButtonEvent();
                    RE::free(key);
                    return false;
                }
                mod->next = key;
            }
            RE::InputEvent* head = mod ? static_cast<RE::InputEvent*>(mod) : key;
            g_PartySheetDirectDispatch.store(true);
            // Hide the user's physically-held modifiers from Party Sheet for this callback. The alias
            // that triggered us may be a modifier chord (e.g. Shift+F), but Party Sheet reads the
            // keyboard's immediate modifier state and would treat the dispatched key as "Shift+X" and
            // ignore it. Zero the modifier bytes for the dispatch, then restore them for everyone else.
            auto* kb = manager ? manager->GetKeyboard() : nullptr;
            static constexpr WORD kMods[] = { 0x2A, 0x36, 0x1D, 0x9D, 0x38, 0xB8 }; // L/R Shift, Ctrl, Alt
            BYTE savedCur[6]{}, savedPrev[6]{};
            if (kb) {
                for (int m = 0; m < 6; ++m) {
                    savedCur[m] = kb->curState[kMods[m]];
                    savedPrev[m] = kb->prevState[kMods[m]];
                    kb->curState[kMods[m]] = 0;
                    kb->prevState[kMods[m]] = 0;
                }
            }
            entry.orig(entry.sink, &head, source);
            if (kb) {
                for (int m = 0; m < 6; ++m) {
                    kb->curState[kMods[m]] = savedCur[m];
                    kb->prevState[kMods[m]] = savedPrev[m];
                }
            }
            g_PartySheetDirectDispatch.store(false);
            if (mod) {
                mod->~ButtonEvent();
                RE::free(mod);
            }
            key->~ButtonEvent();
            RE::free(key);
            return true;
        };
        if (!send(1.0f, 0.0f)) return false;
        send(0.0f, 0.12f);
        SKSE::log::info("DispatchPartySheetKey: dispatched {} directly to Skyrim Party Sheet.", NameFromDIK(dik));
        return true;
    }
    SKSE::log::warn("DispatchPartySheetKey: Skyrim Party Sheet input sink is not available yet.");
    return false;
}

static bool ListHasButtonDown(RE::InputEvent* const* a_event, std::uint32_t dik) {
    if (!a_event) return false;
    for (RE::InputEvent* ev = *a_event; ev; ev = ev->next) {
        if (IsButtonForDIK(ev, dik) && static_cast<const RE::ButtonEvent*>(ev)->value > 0.0f) return true;
    }
    return false;
}

static RE::BSEventNotifyControl GenericSinkHook(RE::BSTEventSink<RE::InputEvent*>* sink,
        RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* src) {
    void* fn = (*reinterpret_cast<void***>(sink))[1];
    size_t n = g_SinkEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (g_SinkEntries[i].fn == fn) {
            auto& e = g_SinkEntries[i];
            // Engage the passCount guard while blocked OR during a simulated open/close window —
            // even when unblocked — so the simulated F2 only toggles DebugMenu once (otherwise the
            // held simulated key spams toggle events and the menu opens/closes repeatedly).
            const bool dbgSimWindow = g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load();
            if (e.which == ManagedSink::DebugMenu &&
                (!InternalAliasKeyAllowed(AI_DebugMenu, g_UnblockDebugMenu, g_DebugMenuConfig.toggleDIK) || dbgSimWindow)) {
                bool hasKey = false;
                for (RE::InputEvent* ev = *a_event; ev; ev = ev->next) {
                    if (IsButtonForDIK(ev, g_DebugMenuConfig.toggleDIK)) { hasKey = true; break; }
                }
                if (hasKey) {
                    // Diag: fires only when the toggle key actually reaches the sink (i.e. our injects),
                    // so we can see whether the engine sink is even reached while the menu is OPEN.
                    SKSE::log::info("GenericSinkHook[DebugMenu]: toggle key hit sink (menuOpen={}, simWindow={}) -> {}",
                        g_ActiveMenu.load() == ActiveMenu::DebugMenu, dbgSimWindow, dbgSimWindow ? "pass" : "filter");
                    // Our injected open-key is let through during the allow-window; a stray one is filtered.
                    if (dbgSimWindow) return e.orig(sink, a_event, src);
                    return FilterDispatch(sink, e.orig, a_event, src, g_DebugMenuConfig.toggleDIK);
                }
            }
            if (e.which == ManagedSink::CS) {
                return FilterDispatchCS(sink, e.orig, a_event, src);
            }
            if (e.which == ManagedSink::PartySheet) {
                return FilterDispatchPartySheet(sink, e.orig, a_event, src);
            }
            if (SinkLetThrough(e.which)) return e.orig(sink, a_event, src);
            if (e.which == ManagedSink::IED) {
                if (ListHasButtonDown(a_event, g_IEDConfig.toggleDIK)) {
                    SKSE::log::info("IED input sink: blocked physical {} press before IED callback.",
                        NameFromDIK(g_IEDConfig.toggleDIK));
                    return RE::BSEventNotifyControl::kContinue;
                }
                return FilterIEDDispatch(sink, e.orig, a_event, src);
            }
            return FilterDispatch(sink, e.orig, a_event, src, SinkBlockDIK(e.which));
        }
    }
    return RE::BSEventNotifyControl::kContinue; // unknown (shouldn't happen)
}

static bool VtableInModule(void* obj, const char* moduleName) {
    HMODULE hMod = ::GetModuleHandleA(moduleName);
    if (!hMod) return false;
    void* vtbl = *reinterpret_cast<void**>(obj);
    HMODULE hOwner = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(vtbl), &hOwner)) {
        return hOwner == hMod;
    }
    return false;
}

static bool FnAlreadyHooked(void* fn) {
    size_t n = g_SinkEntryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) if (g_SinkEntries[i].fn == fn) return true;
    return false;
}

// Scan the input-event sinks and hook each managed mod's ProcessEvent. Safe to call
// repeatedly — only hooks what isn't hooked yet (mods register their sinks at different times).
static void TryHookModSinks() {
    auto* idm = RE::BSInputDeviceManager::GetSingleton();
    if (!idm) return;
    auto* source = static_cast<RE::BSTEventSource<RE::InputEvent*>*>(idm);

    struct Target { const char* module; ManagedSink which; bool enabled; };
    const Target targets[] = {
        { "dmenu",                      ManagedSink::DMenu,     g_DMenuConfig.enabled },
        { "ImprovedCameraSE",           ManagedSink::IC,        g_ImprovedCameraConfig.enabled },
        { "FUCK",                       ManagedSink::FLICK,     g_FLICKConfig.enabled },
        { "ImmersiveEquipmentDisplays", ManagedSink::IED,       g_IEDConfig.enabled },
        { "DebugMenu",                  ManagedSink::DebugMenu, g_DebugMenuConfig.enabled },
        { "CommunityShaders",           ManagedSink::CS,        g_CSConfig.enabled },
        { "SkyrimPartySheet",           ManagedSink::PartySheet,g_PartySheetConfig.enabled },
        { "SKSEMenuFramework",          ManagedSink::MF,        true },
    };

    for (auto* sink : source->sinks) {
        if (!sink) continue;
        void* fn = (*reinterpret_cast<void***>(sink))[1]; // vtable slot 1 = ProcessEvent
        if (FnAlreadyHooked(fn)) continue;

        for (const auto& t : targets) {
            if (!t.enabled || !VtableInModule(sink, t.module)) continue;
            ProcessInputEvent_t orig = nullptr;
            if (MH_CreateHook(fn, reinterpret_cast<void*>(GenericSinkHook),
                              reinterpret_cast<void**>(&orig)) == MH_OK && MH_EnableHook(fn) == MH_OK) {
                size_t idx = g_SinkEntryCount.load(std::memory_order_relaxed);
                if (idx < g_SinkEntries.size()) {
                    g_SinkEntries[idx] = SinkHookEntry{ fn, orig, sink, t.which };
                    g_SinkEntryCount.store(idx + 1, std::memory_order_release);
                    SKSE::log::info("TryHookModSinks: hooked {} input sink at {:p}.", t.module, fn);
                }
            }
            break;
        }
    }
}

// CatMenu's source has one native toggle check:
// ImGui::IsKeyPressed(settings.toggle_key). Intercept only that call when its direct
// caller is catmenu.dll. Every other ImGui caller and every physical key remains untouched.
using ImGuiIsKeyPressed_t = bool(*)(int, bool);
static ImGuiIsKeyPressed_t g_OrigImGuiIsKeyPressed = nullptr;
static std::uintptr_t g_CatMenuModuleBegin = 0;
static std::uintptr_t g_CatMenuModuleEnd = 0;
static std::atomic<bool> g_CatMenuKeyHookInstalled{ false };

static bool HookedImGuiIsKeyPressed(int key, bool repeat) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (g_CatMenuModuleBegin != 0 && caller >= g_CatMenuModuleBegin && caller < g_CatMenuModuleEnd &&
        key == g_CatMenuConfig.toggleImGuiKey) {
        return false;
    }
    return g_OrigImGuiIsKeyPressed ? g_OrigImGuiIsKeyPressed(key, repeat) : false;
}

static void TryHookCatMenuNativeKey() {
    if (!g_CatMenuConfig.enabled || g_CatMenuKeyHookInstalled.load()) return;
    HMODULE catMenu = ::GetModuleHandleA("catmenu.dll");
    HMODULE imgui = ::GetModuleHandleA("imgui.dll");
    if (!catMenu || !imgui) {
        SKSE::log::error("TryHookCatMenuNativeKey: catmenu.dll or imgui.dll is not loaded.");
        return;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(catMenu);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_CatMenuModuleBegin = reinterpret_cast<std::uintptr_t>(base);
    g_CatMenuModuleEnd = g_CatMenuModuleBegin + nt->OptionalHeader.SizeOfImage;

    constexpr const char* kExport = "?IsKeyPressed@ImGui@@YA_NW4ImGuiKey@@_N@Z";
    void* target = reinterpret_cast<void*>(::GetProcAddress(imgui, kExport));
    if (!target) {
        SKSE::log::error("TryHookCatMenuNativeKey: ImGui::IsKeyPressed export was not found.");
        return;
    }

    const MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(HookedImGuiIsKeyPressed),
        reinterpret_cast<void**>(&g_OrigImGuiIsKeyPressed));
    if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(target);
        g_CatMenuKeyHookInstalled.store(true);
        SKSE::log::info("TryHookCatMenuNativeKey: CatMenu native toggle listener disabled; physical keys remain untouched.");
    } else {
        SKSE::log::error("TryHookCatMenuNativeKey: MH_CreateHook failed ({}).", static_cast<int>(status));
    }
}

// ============================================================================
// Hook installation
// ============================================================================
// ============================================================================
// ReShade-proof ESC suppression. ReShade wraps DirectInput, so our GetDeviceState/GetDeviceData
// ESC strip is bypassed when ReShade is installed. This hook sits on the engine's input-event
// dispatch (the SAME call site dMenu NG hooks), which is BELOW ReShade's DI wrapping. While a
// managed ESC-ignoring menu is open (or just closed), strip ESC ButtonEvents here so Skyrim's
// system menu can't open. Non-ReShade users are unaffected (ESC was already dropped from the DI
// buffer, so there's nothing to strip). Chains cleanly with dMenu's identical write_call hook.
// ============================================================================
using InputDispatch_t = void (*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**);
static REL::Relocation<InputDispatch_t> g_OrigInputDispatch;

static void RisaInputDispatch(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events) {
    if (a_events && *a_events && (EscBlockedForActiveMenu() || NowMs() < g_SuppressEscUntilMs.load())) {
        for (auto* ev = *a_events; ev; ev = ev->next) {
            if (IsButtonForDIK(ev, 0x01)) {
                SKSE::log::info("RisaInputDispatch: stripping Escape key event from dispatch list.");
            }
        }
        static thread_local std::vector<std::pair<RE::InputEvent*, RE::InputEvent*>> saved;
        saved.clear();
        RE::InputEvent* head = *a_events;
        while (head && IsButtonForDIK(head, 0x01)) head = head->next; // drop ESC at the head
        for (RE::InputEvent* cur = head; cur; ) {
            RE::InputEvent* nxt = cur->next;
            while (nxt && IsButtonForDIK(nxt, 0x01)) nxt = nxt->next;
            if (cur->next != nxt) { saved.emplace_back(cur, cur->next); cur->next = nxt; }
            cur = nxt;
        }
        RE::InputEvent* localHead = head;
        g_OrigInputDispatch(a_source, &localHead);
        for (auto& pr : saved) pr.first->next = pr.second; // restore the list for everyone else
        return;
    }
    g_OrigInputDispatch(a_source, a_events);
}

static void TryHookInputDispatch() {
    static std::atomic<bool> s_done{ false };
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true)) return;
    SKSE::AllocTrampoline(64);
    auto& trampoline = SKSE::GetTrampoline();
    REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(67315, 68617) };
    g_OrigInputDispatch = trampoline.write_call<5>(caller.address() + 0x7B, RisaInputDispatch);
    SKSE::log::info("TryHookInputDispatch: hooked engine input-event dispatch (ReShade-proof ESC strip).");
}

static void InstallHooks() {
    MH_Initialize();
    TryHookInputDispatch();
    TryHookCSInput();
    TryHookCatMenuNativeKey();

    MH_STATUS stRaw = MH_CreateHook(
        reinterpret_cast<void*>(::GetRawInputData),
        reinterpret_cast<void*>(HookedGetRawInputData),
        reinterpret_cast<void**>(&g_OrigGetRawInputData)
    );
    if (stRaw == MH_OK || stRaw == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(reinterpret_cast<void*>(::GetRawInputData));
        SKSE::log::info("InstallHooks: GetRawInputData hooked successfully (ReShade raw-input F1 close support).");
    } else {
        SKSE::log::error("InstallHooks: GetRawInputData MH_CreateHook failed ({}).", (int)stRaw);
    }

    // --- DI8 GetDeviceData (buffered keyboard) ---
    // Managed hotkeys are never stripped globally. This hook only suppresses ESC during
    // the short Improved Camera close transition so Skyrim does not also open its menu.
    do {
        HMODULE hDI = ::LoadLibraryA("dinput8.dll");
        if (!hDI) { SKSE::log::error("InstallHooks: dinput8.dll not found."); break; }
        using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto pCreate = reinterpret_cast<DI8Create_t>(GetProcAddress(hDI, "DirectInput8Create"));
        if (!pCreate) { SKSE::log::error("InstallHooks: DirectInput8Create missing."); break; }
        IDirectInput8A* pDI = nullptr;
        if (FAILED(pCreate(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                           s_IID_IDirectInput8A, reinterpret_cast<void**>(&pDI), nullptr))) {
            SKSE::log::error("InstallHooks: DirectInput8Create failed."); break;
        }
        IDirectInputDevice8A* pKbd = nullptr;
        HRESULT hr = pDI->CreateDevice(s_GUID_SysKeyboard, &pKbd, nullptr);
        pDI->Release();
        if (FAILED(hr)) { SKSE::log::error("InstallHooks: CreateDevice(SysKeyboard) failed."); break; }
        void** vtbl = *reinterpret_cast<void***>(pKbd);
        void* targetState = vtbl[9];  // GetDeviceState (READ-ONLY master-key detection)
        void* targetData  = vtbl[10]; // GetDeviceData (temporary ESC suppression only)
        pKbd->Release();

        MH_STATUS stState = MH_CreateHook(targetState,
            reinterpret_cast<void*>(HookedGetDeviceState),
            reinterpret_cast<void**>(&g_OrigGetDeviceState));
        if (stState == MH_OK || stState == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(targetState);
            SKSE::log::info("InstallHooks: DI8 GetDeviceState hooked at {:p} (read-only master-key poll).", targetState);
        } else {
            SKSE::log::error("InstallHooks: DI8 GetDeviceState MH_CreateHook failed ({}).", (int)stState);
        }

        MH_STATUS stData = MH_CreateHook(targetData,
            reinterpret_cast<void*>(HookedGetDeviceData),
            reinterpret_cast<void**>(&g_OrigGetDeviceData));
        if (stData == MH_OK || stData == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(targetData);
            SKSE::log::info("InstallHooks: DI8 GetDeviceData hooked at {:p}.", targetData);
        } else {
            SKSE::log::error("InstallHooks: DI8 GetDeviceData MH_CreateHook failed ({}).", (int)stData);
        }
    } while (false);

    // Managed mods are otherwise blocked per-mod: sink-based mods via their own InputEvent
    // sink (Hook 4, TryHookModSinks), and polling mods via the caller-scoped
    // GetAsyncKeyState/GetKeyState hooks below.

    // --- GetAsyncKeyState (caller-scoped polling for ENB and Improved Camera) ---
    {
        MH_STATUS st = MH_CreateHookApi(L"user32", "GetAsyncKeyState",
            reinterpret_cast<void*>(HookedGetAsyncKeyState),
            reinterpret_cast<void**>(&g_OrigGetAsyncKeyState));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(MH_ALL_HOOKS);
            SKSE::log::info("InstallHooks: GetAsyncKeyState hooked (caller-scoped menu polling).");
        } else {
            SKSE::log::error("InstallHooks: GetAsyncKeyState MH_CreateHookApi failed ({}).", (int)st);
        }
    }

    // --- GetKeyState ---
    {
        MH_STATUS st = MH_CreateHookApi(L"user32", "GetKeyState",
            reinterpret_cast<void*>(HookedGetKeyState),
            reinterpret_cast<void**>(&g_OrigGetKeyState));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(MH_ALL_HOOKS);
            SKSE::log::info("InstallHooks: GetKeyState hooked.");
        } else {
            SKSE::log::error("InstallHooks: GetKeyState MH_CreateHookApi failed ({}).", (int)st);
        }
    }
}

// ============================================================================
// Open MF from launcher using its exported main window.
// ============================================================================
// Set while a Settings-tab mod button toggles a menu for re-syncing: the launcher must stay open so
// the user can keep toggling. The mod's Open*() helper still calls CloseLauncher() internally, so we
// swallow that close here.
static std::atomic<bool> g_KeepLauncherOpenForSync{ false };

static void CloseLauncher(bool external) {
    if (g_KeepLauncherOpenForSync.load()) return; // Settings sync button — keep the launcher open
    // Hand freezeTime back to the framework exactly as it set it (frozen), immediately on close, so our
    // "keep running" override never bleeds into the next framework window (e.g. the SKSE MF menu).
    RestoreFreezeOverride();
    if (g_LauncherWindow) {
        g_LauncherWindow->IsOpen.store(false);
        g_WaitForLauncherKeyRelease.store(false);
        // On an EXTERNAL close (the user pressed ESC, not F1) backdate the toggle timestamp so the
        // very next F1 is treated as a fresh press. Stamping "now" here would start a 700ms debounce
        // anchored to the ESC, which swallowed the first F1 after closing (open -> ESC -> F1 did
        // nothing -> F1 opened). F1-initiated closes still stamp normally so a held key can't spam.
        g_LastLauncherToggleMs.store(external ? NowMs() - 1000 : NowMs());
    }
    if (!g_RememberSubView.load()) g_LauncherSubView.store(0); // back to the grid unless persistence is on
}

static void OpenSKSEMenuFramework() {
    if (g_ActiveMenu.load() == ActiveMenu::MF && SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
        ForceCloseSKSEMenuFramework();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::MF);

    auto* mainWindow = SKSEMenuFramework::GetMainWindow();
    if (!mainWindow) {
        SKSE::log::error("OpenSKSEMenuFramework: GetMainWindow returned null.");
        return;
    }

    mainWindow->IsOpen.store(true);
    mainWindow->BlockUserInput.store(true);
    SKSE::log::info("OpenSKSEMenuFramework: opened SKSE Menu Framework main window directly.");
}

// True when SkyUI (which hosts MCM) is in the load order. Use LookupModByName, not
// LookupLoadedModByName: the "loaded" variant only reports a mod once a save is actually loaded, so
// at the main menu it returns null and the MCM button would vanish entirely (inactive buttons are
// filtered out, not greyed). LookupModByName reports load-order presence, which is populated before
// the main menu. Latched so a single successful detection keeps the button for the rest of the session.
static std::atomic<bool> g_HasMCM{ false };
static bool IsMCMAvailable() {
    if (g_HasMCM.load()) return true;
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler && dataHandler->LookupModByName("SkyUI_SE.esp")) {
        g_HasMCM.store(true);
        return true;
    }
    return false;
}

static bool OpenMCMPanel() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;
    auto journal = ui->GetMenu(RE::JournalMenu::MENU_NAME);
    if (!journal || !journal->uiMovie) return false;

    constexpr auto kRestoreSavedSettings = "_root.QuestJournalFader.Menu_mc.RestoreSavedSettings";
    constexpr auto kConfigPanelOpen = "_root.QuestJournalFader.Menu_mc.ConfigPanelOpen";
    std::array<RE::GFxValue, 2> systemTab{ 2.0, false };
    const bool switched = journal->uiMovie->Invoke(
        kRestoreSavedSettings, nullptr, systemTab.data(), static_cast<std::uint32_t>(systemTab.size()));
    const bool opened = journal->uiMovie->Invoke(kConfigPanelOpen, nullptr, nullptr, 0);
    SKSE::log::info("OpenMCMPanel: System tab={}, ConfigPanelOpen={}.", switched, opened);
    return opened;
}

static void OpenMCM() {
    auto* queue = RE::UIMessageQueue::GetSingleton();
    if (!queue) {
        SKSE::log::error("OpenMCM: UIMessageQueue unavailable.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::MCM) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::MCM);
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::MCM);
    g_OpenMCMWhenJournalReady.store(true);
    g_MenuOpenLockUntilMs.store(NowMs() + 500);
    queue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
    SKSE::log::info("OpenMCM: queued Journal Menu; waiting for SkyUI ConfigPanelOpen.");
}

static bool SetOAROpen(bool open) {
    HMODULE module = ::GetModuleHandleA("OpenAnimationReplacer.dll");
    if (!module) return false;

    using GetUIManager_t = void*(*)();
    static GetUIManager_t getUIManager = nullptr;
    if (!getUIManager) {
        auto* base = reinterpret_cast<std::uint8_t*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        static constexpr std::array<int, 32> pattern{
            0x53, 0x48, 0x83, 0xEC, 0x20,
            0x8B, 0x0D, -1, -1, -1, -1,
            0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
            0xBA, 0x34, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x04, 0xC8,
            0x8B, 0x04, 0x02
        };

        std::uint8_t* match = nullptr;
        std::size_t matches = 0;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            if ((section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            auto* begin = base + section[s].VirtualAddress;
            const std::size_t size = section[s].Misc.VirtualSize;
            if (size < pattern.size()) continue;
            for (std::size_t i = 0; i <= size - pattern.size(); ++i) {
                bool same = true;
                for (std::size_t p = 0; p < pattern.size(); ++p) {
                    if (pattern[p] >= 0 && begin[i + p] != static_cast<std::uint8_t>(pattern[p])) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    match = begin + i;
                    ++matches;
                }
            }
        }

        if (matches == 1) {
            getUIManager = reinterpret_cast<GetUIManager_t>(match);
            SKSE::log::info("SetOAROpen: resolved UIManager::GetSingleton at RVA 0x{:X}.",
                static_cast<std::size_t>(match - base));
        } else {
            constexpr DWORD kSupportedTimestamp = 0x6A40292A;
            constexpr DWORD kSupportedImageSize = 0x3B3000;
            if (nt->FileHeader.TimeDateStamp == kSupportedTimestamp &&
                nt->OptionalHeader.SizeOfImage == kSupportedImageSize) {
                getUIManager = reinterpret_cast<GetUIManager_t>(base + 0x109590);
                SKSE::log::warn("SetOAROpen: signature matches={}, using verified PDB RVA for build {:08X}/0x{:X}.",
                    matches, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
            } else {
                SKSE::log::error("SetOAROpen: unsupported OAR build timestamp={:08X}, imageSize=0x{:X}, signature matches={}.",
                    nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, matches);
                return false;
            }
        }
    }

    auto* manager = static_cast<std::uint8_t*>(getUIManager());
    if (!manager) return false;
    manager[0x9] = open ? 1 : 0;
    return true;
}

static bool SetImprovedCameraOpen(bool open) {
    HMODULE module = ::GetModuleHandleA("ImprovedCameraSE.dll");
    if (!module) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    // Improved Camera SE 1.1.2.4228 ships its PDB. Resolve the live UIMenu through
    // Plugin -> Graphics -> UIMenu and gate every private offset to that exact DLL.
    constexpr DWORD kSupportedTimestamp = 0x670AB16D;
    constexpr DWORD kSupportedImageSize = 0x166000;
    if (nt->FileHeader.TimeDateStamp != kSupportedTimestamp ||
        nt->OptionalHeader.SizeOfImage != kSupportedImageSize) {
        SKSE::log::error("SetImprovedCameraOpen: unsupported build timestamp={:08X}, imageSize=0x{:X}.",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }

    constexpr std::size_t kPluginGlobalRVA = 0x153880;
    constexpr std::size_t kGraphicsOffset = 0x88;
    constexpr std::size_t kMenuOffset = 0x50;
    constexpr std::size_t kOpenOffset = 0x08;
    constexpr std::size_t kCloseRVA = 0x60ED0;
    constexpr std::array<std::uint8_t, 10> kClosePrologue{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18
    };

    auto* closeAddress = base + kCloseRVA;
    if (std::memcmp(closeAddress, kClosePrologue.data(), kClosePrologue.size()) != 0) {
        SKSE::log::error("SetImprovedCameraOpen: UIMenu close signature changed.");
        return false;
    }

    auto* plugin = *reinterpret_cast<std::uint8_t**>(base + kPluginGlobalRVA);
    if (!plugin) return false;
    auto* graphics = *reinterpret_cast<std::uint8_t**>(plugin + kGraphicsOffset);
    if (!graphics) return false;
    auto* menu = *reinterpret_cast<std::uint8_t**>(graphics + kMenuOffset);
    if (!menu) return false;

    if (open) {
        menu[kOpenOffset] = 1;
    } else {
        using CloseMenu_t = void(*)(void*);
        reinterpret_cast<CloseMenu_t>(closeAddress)(menu);
    }
    return true;
}

static bool ToggleImprovedCameraWithRegisteredCommand() {
    HMODULE module = ::GetModuleHandleA("ImprovedCameraSE.dll");
    if (!module) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    // Improved Camera SE 2.0.0.1215 (Discord build). Its PDB shows that the registered
    // console handler reads the complete command from Script::text and does not use any
    // other Script state. Keep this path pinned to that exact binary contract.
    constexpr DWORD kSupportedTimestamp = 0x693FDDE7;
    constexpr DWORD kSupportedImageSize = 0x2BA000;
    constexpr std::size_t kCommandHandlerRVA = 0x33420;
    if (nt->FileHeader.TimeDateStamp != kSupportedTimestamp ||
        nt->OptionalHeader.SizeOfImage != kSupportedImageSize) {
        return false;
    }

    RE::SCRIPT_FUNCTION* command = nullptr;
    if (auto* commands = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand()) {
        for (std::uint32_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd; ++i) {
            const auto matches = [](const char* value, const char* expected) {
                return value && _stricmp(value, expected) == 0;
            };
            if (matches(commands[i].shortName, "ic") ||
                matches(commands[i].functionName, "ImprovedCameraSE")) {
                command = &commands[i];
                break;
            }
        }
    }

    if (!command || !command->executeFunction ||
        reinterpret_cast<std::uint8_t*>(command->executeFunction) != base + kCommandHandlerRVA) {
        SKSE::log::error("ToggleImprovedCameraWithRegisteredCommand: verified IC command handler was not registered.");
        return false;
    }

    struct CommandScriptView {
        std::array<std::byte, offsetof(RE::Script, text)> prefix{};
        char* text{ nullptr };
    };
    static_assert(offsetof(CommandScriptView, text) == offsetof(RE::Script, text));

    char commandText[] = "ic menu";
    CommandScriptView scriptView{};
    scriptView.text = commandText;
    RE::SCRIPT_FUNCTION::ScriptData scriptData{};
    double result = 0.0;
    std::uint32_t opcodeOffset = 0;
    command->executeFunction(command->params, &scriptData, nullptr, nullptr,
        reinterpret_cast<RE::Script*>(&scriptView), nullptr, result, opcodeOffset);
    return true;
}

static bool IsImprovedCameraOpen() {
    HMODULE module = ::GetModuleHandleA("ImprovedCameraSE.dll");
    if (!module) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 0x670AB16D ||
        nt->OptionalHeader.SizeOfImage != 0x166000) return false;

    auto* plugin = *reinterpret_cast<std::uint8_t**>(base + 0x153880);
    if (!plugin) return false;
    auto* graphics = *reinterpret_cast<std::uint8_t**>(plugin + 0x88);
    if (!graphics) return false;
    auto* menu = *reinterpret_cast<std::uint8_t**>(graphics + 0x50);
    return menu && menu[0x08] != 0;
}

static void OpenAnimationReplacer() {
    if (!g_OARConfig.enabled) {
        SKSE::log::warn("OpenAnimationReplacer: OAR UI is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::OAR) {
        CloseLauncher();
        if (SetOAROpen(false)) {
            g_ActiveMenu.store(ActiveMenu::None);
            SKSE::log::info("OpenAnimationReplacer: closed OAR directly.");
        }
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    if (SetOAROpen(true)) {
        g_ActiveMenu.store(ActiveMenu::OAR);
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenAnimationReplacer: opened OAR directly.");
    } else {
        g_ActiveMenu.store(ActiveMenu::None);
    }
}

static bool ResolveIEDNativeUI(std::uint8_t*& a_iui) {
    a_iui = nullptr;
    HMODULE module = ::GetModuleHandleA("ImmersiveEquipmentDisplays.dll");
    if (!module) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    // IED 1.7.5b (10 Dec 2023), validated against its source, RTTI, and UI-open thunk.
    constexpr DWORD kSupportedTimestamp = 0x6575B58C;
    constexpr DWORD kSupportedImageSize = 0x581000;
    constexpr std::size_t kIUISubobjectOffset = 0x1638;
    constexpr std::size_t kOpenThunkRVA = 0xEFE00;
    constexpr std::size_t kUIOpenRVA = 0x158470;
    constexpr std::array<std::uint8_t, 11> kOpenThunkPrefix{
        0x48, 0x8B, 0x49, 0x08, 0x48, 0x81, 0xC1, 0x38, 0x16, 0x00, 0x00
    };
    // Real prologue at 0x158470 begins with a REX-prefixed `push rbx` (0x40 0x53). The original
    // pattern dropped that leading 0x40 (disassembler off-by-one), so the check always failed even
    // on the exact matched build. Verified against the on-disk bytes and the open-thunk's jmp target.
    constexpr std::array<std::uint8_t, 11> kUIOpenPrefix{
        0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30
    };

    // Log the exact failing step once per distinct reason (this fn is polled, so avoid spam).
    static std::atomic<int> s_lastFail{ -99 };
    auto fail = [&](int code, const char* why) -> bool {
        if (s_lastFail.exchange(code) != code) SKSE::log::warn("ResolveIEDNativeUI: FAIL[{}] {}", code, why);
        return false;
    };

    if (nt->FileHeader.TimeDateStamp != kSupportedTimestamp) return fail(2, "TimeDateStamp mismatch");
    if (nt->OptionalHeader.SizeOfImage != kSupportedImageSize) return fail(3, "SizeOfImage mismatch");
    if (std::memcmp(base + kOpenThunkRVA, kOpenThunkPrefix.data(), kOpenThunkPrefix.size()) != 0) return fail(4, "open-thunk bytes mismatch");
    if (std::memcmp(base + kUIOpenRVA, kUIOpenPrefix.data(), kUIOpenPrefix.size()) != 0) return fail(5, "UI-open bytes mismatch");

    using GetPluginInterface_t = void*(*)();
    auto getInterface = reinterpret_cast<GetPluginInterface_t>(
        ::GetProcAddress(module, "SKMP_GetPluginInterface"));
    if (!getInterface) return fail(6, "SKMP_GetPluginInterface export not found");

    auto* pluginInterface = static_cast<std::uint8_t*>(getInterface());
    if (!pluginInterface) return fail(7, "GetPluginInterface() returned null");
    if (!VtableInModule(pluginInterface, "ImmersiveEquipmentDisplays.dll")) return fail(8, "pluginInterface vtable not in IED module");
    auto* controller = *reinterpret_cast<std::uint8_t**>(pluginInterface + 0x08);
    if (!controller) return fail(9, "controller ptr null (IED UI not initialized yet?)");
    if (!VtableInModule(controller, "ImmersiveEquipmentDisplays.dll")) return fail(10, "controller vtable not in IED module");

    auto* iui = controller + kIUISubobjectOffset;
    if (!VtableInModule(iui, "ImmersiveEquipmentDisplays.dll")) return fail(11, "UI subobject vtable not in IED module");
    a_iui = iui;
    if (s_lastFail.exchange(0) != 0) SKSE::log::info("ResolveIEDNativeUI: resolved OK (iui at {:p}).", static_cast<void*>(iui));
    return true;
}

static bool IsIEDOpen() {
    std::uint8_t* iui = nullptr;
    if (!ResolveIEDNativeUI(iui)) return false;

    constexpr std::size_t kMainTaskOffset = 0xB0;
    constexpr std::size_t kTaskRunningOffset = 0x20;
    auto* task = *reinterpret_cast<std::uint8_t**>(iui + kMainTaskOffset);
    return task && VtableInModule(task, "ImmersiveEquipmentDisplays.dll") &&
        task[kTaskRunningOffset] != 0;
}

static bool SetIEDOpen(bool open) {
    std::uint8_t* iui = nullptr;
    if (!ResolveIEDNativeUI(iui)) return false;

    constexpr std::size_t kUIOpenRVA = 0x158470;
    constexpr std::size_t kMainTaskOffset = 0xB0;
    constexpr std::size_t kTaskRunningOffset = 0x20;
    constexpr std::size_t kTaskStopOffset = 0x26;
    auto* module = reinterpret_cast<std::uint8_t*>(::GetModuleHandleA("ImmersiveEquipmentDisplays.dll"));
    auto* task = *reinterpret_cast<std::uint8_t**>(iui + kMainTaskOffset);
    const bool running = task && VtableInModule(task, "ImmersiveEquipmentDisplays.dll") &&
        task[kTaskRunningOffset] != 0;

    if (open) {
        if (running) return true;
        using UIOpen_t = int(*)(void*);
        return reinterpret_cast<UIOpen_t>(module + kUIOpenRVA)(iui) == 1;
    }

    if (!running) return true;
    std::atomic_ref<bool>(*reinterpret_cast<bool*>(task + kTaskStopOffset)).store(
        true, std::memory_order_relaxed);
    return true;
}

static void OpenImmersiveEquipmentDisplays() {
    if (!g_IEDConfig.enabled) {
        SKSE::log::warn("OpenImmersiveEquipmentDisplays: IED is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::IED || IsIEDOpen()) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::IED);
        SKSE::log::info("OpenImmersiveEquipmentDisplays: closing IED.");
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    if (SetIEDOpen(true)) {
        g_ActiveMenu.store(ActiveMenu::IED);
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenImmersiveEquipmentDisplays: opened IED through its native render task.");
        return;
    }

    if (g_IEDConfig.toggleDIK == 0) {
        g_ActiveMenu.store(ActiveMenu::None);
        SKSE::log::error("OpenImmersiveEquipmentDisplays: native adapter unavailable and no fallback key is configured.");
        return;
    }

    g_ActiveMenu.store(ActiveMenu::IED);
    g_AllowIEDOpen.store(true);
    g_AllowIEDOpenUntilMs.store(NowMs() + 1000);
    InjectEngineKey(g_IEDConfig.toggleDIK);
    SKSE::log::warn("OpenImmersiveEquipmentDisplays: native adapter unavailable; opening via fallback engine event {}.",
        NameFromDIK(g_IEDConfig.toggleDIK));
}

static void OpenENB() {
    
    if (!g_ENBConfig.enabled) {
        SKSE::log::warn("OpenENB: ENB is not configured or active.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::ENB || IsExternalMenuOpen()) {
        g_ENBOpenRequestedMs.store(0);
        CloseLauncher();
        g_MenuOpenLockUntilMs.store(NowMs() + 1000);
        
        std::thread([]() {
            g_AllowENBOpen.store(true);
            g_AllowENBOpenUntilMs.store(NowMs() + 1000);

            WORD combDIK = DIKFromVK(g_ENBConfig.combinationVK);
            WORD editDIK = DIKFromVK(g_ENBConfig.editorVK);

            std::array<INPUT, 2> downInputs{};
            UINT downCount = 0;
            if (combDIK != 0) AddScanInput(downInputs[downCount++], combDIK, false);
            if (editDIK != 0) AddScanInput(downInputs[downCount++], editDIK, false);
            ::SendInput(downCount, downInputs.data(), sizeof(INPUT));

            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            std::array<INPUT, 2> upInputs{};
            UINT upCount = 0;
            if (editDIK != 0) AddScanInput(upInputs[upCount++], editDIK, true);
            if (combDIK != 0) AddScanInput(upInputs[upCount++], combDIK, true);
            ::SendInput(upCount, upInputs.data(), sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_AllowENBOpen.store(false);
            g_AllowENBOpenUntilMs.store(0);
            
            SKSE::log::info("OpenENB: closed ENB via simulated hotkey.");
        }).detach();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::ENB);
    g_ENBOpenRequestedMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 1800); // ENB editor takes time to load; ignore F1 until then
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        g_AllowENBOpen.store(true);
        g_AllowENBOpenUntilMs.store(NowMs() + 1000);

        WORD combDIK = DIKFromVK(g_ENBConfig.combinationVK);
        WORD editDIK = DIKFromVK(g_ENBConfig.editorVK);

        std::array<INPUT, 2> downInputs{};
        UINT downCount = 0;
        if (combDIK != 0) AddScanInput(downInputs[downCount++], combDIK, false);
        if (editDIK != 0) AddScanInput(downInputs[downCount++], editDIK, false);
        ::SendInput(downCount, downInputs.data(), sizeof(INPUT));

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        std::array<INPUT, 2> upInputs{};
        UINT upCount = 0;
        if (editDIK != 0) AddScanInput(upInputs[upCount++], editDIK, true);
        if (combDIK != 0) AddScanInput(upInputs[upCount++], combDIK, true);
        ::SendInput(upCount, upInputs.data(), sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_AllowENBOpen.store(false);
        g_AllowENBOpenUntilMs.store(0);
        g_ENBOpenRequestedMs.store(0);
        g_MenuOpenLockUntilMs.store(0);
        
        SKSE::log::info("OpenENB: injected hotkey completed; F1 close is now enabled.");
    }).detach();
}

static void OpenFLICK() {
    auto* flick = GetFLICKInterface();
    if (!flick || !flick->SetMenuOpen || !flick->IsMenuOpen) {
        SKSE::log::warn("OpenFLICK: RequestFUCK API is unavailable.");
        return;
    }

    if (flick->IsMenuOpen()) {
        flick->SetMenuOpen(false);
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        SKSE::log::info("OpenFLICK: closed FLICK through its API.");
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::FLICK);
    g_LastLauncherToggleMs.store(NowMs());
    flick->SetMenuOpen(true);
    SKSE::log::info("OpenFLICK: opened FLICK through its API.");
}

static FLICKInterface* GetFLICKInterface() {
    static FLICKInterface* api = nullptr;
    if (api) return api;

    HMODULE module = ::GetModuleHandleA("FUCK.dll");
    if (!module) return nullptr;

    using RequestFUCK_t = void* (*)();
    auto request = reinterpret_cast<RequestFUCK_t>(::GetProcAddress(module, "RequestFUCK"));
    if (!request) return nullptr;

    auto* candidate = static_cast<FLICKInterface*>(request());
    if (!candidate || candidate->version < 1 || !candidate->SetMenuOpen || !candidate->IsMenuOpen) {
        return nullptr;
    }
    api = candidate;
    SKSE::log::info("GetFLICKInterface: connected to FLICK API version {}.", api->version);
    return api;
}

static bool SetFLICKHotkeyEnabled(bool enabled) {
    auto* flick = GetFLICKInterface();
    if (!flick) {
        return false;
    }

    if (flick->version < 3 || !flick->SetHotkeyEnabled) {
        SKSE::log::warn("FLICK API: SetHotkeyEnabled({}) unavailable on API version {}; FLICK hotkey left unchanged.",
            enabled, flick->version);
        return false;
    }

    flick->SetHotkeyEnabled(enabled);
    SKSE::log::info("FLICK API: SetHotkeyEnabled({}) via RequestFUCK.", enabled);
    return true;
}

// Quick Armor Rebalance is an SKSE-only ImGui utility (no ESP). Detect it by its loaded DLL.
static bool IsQARInstalled() {
    return ::GetModuleHandleA("QuickArmorRebalance.dll") != nullptr;
}

// Open Quick Armor Rebalance from the launcher. QAR exposes a Papyrus global native
// `QuickArmorRebalance.Open()` (registered in its plugin) that calls its own Show(true) - so we
// open it straight through the VM, no console command and no key simulation. It opens only; QAR
// closes from its own window X button / Esc, so this is fire-and-forget like SearchUI.
static void OpenQAR() {
    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) { SKSE::log::warn("OpenQAR: no Papyrus VM."); return; }
    auto* args = RE::MakeFunctionArguments();
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
    vm->DispatchStaticCall("QuickArmorRebalance", "Open", args, cb);
    // Track it so the launcher key can close it. QAR has no programmatic close yet, so we simulate
    // Esc (CloseActiveModMenu) - which closes its window IF the user has QAR's "Escape closes windows"
    // setting on. Temporary until the author adds a Close()/SetMenuOpen(false).
    g_ActiveMenu.store(ActiveMenu::QAR);
    g_MenuOpenLockUntilMs.store(NowMs() + 500);
    SKSE::log::info("OpenQAR: opened Quick Armor Rebalance via QuickArmorRebalance.Open() (Papyrus native).");
}

static void OpenModFunctionMenu(const char* source) {
    SKSE::log::info("MFM_TRACE open-request: source={}, active={}, launcherOpen={}, externalMenu={}, lockRemainingMs={}, toggleDIK={}",
        source ? source : "unknown",
        static_cast<int>(g_ActiveMenu.load()),
        g_LauncherWindow && g_LauncherWindow->IsOpen.load(),
        IsExternalMenuOpen(),
        std::max<long long>(0, g_MenuOpenLockUntilMs.load() - NowMs()),
        NameFromDIK(g_ModFunctionMenuConfig.toggleDIK));
    if (!g_ModFunctionMenuConfig.enabled) {
        SKSE::log::warn("OpenModFunctionMenu: Mod Function Menu is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::ModFunctionMenu) {
        SKSE::log::info("MFM_TRACE open-request became close: MFM already active.");
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::ModFunctionMenu);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    g_ActiveMenu.store(ActiveMenu::ModFunctionMenu);
    // Let the launcher release its ImGui focus/capture for one frame before sending MFM's
    // relocated engine-queue toggle. This avoids a visible open-time handoff flicker.
    g_MenuOpenLockUntilMs.store(NowMs() + 350);
    const WORD toggleDIK = g_ModFunctionMenuConfig.toggleDIK;
    std::thread([toggleDIK]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (g_ActiveMenu.load() != ActiveMenu::ModFunctionMenu) {
            SKSE::log::warn("MFM_TRACE delayed-inject cancelled: active menu changed to {}.",
                static_cast<int>(g_ActiveMenu.load()));
            return;
        }
        SKSE::log::info("MFM_TRACE delayed-inject: sending {} after 80ms; launcherOpen={}.",
            NameFromDIK(toggleDIK), g_LauncherWindow && g_LauncherWindow->IsOpen.load());
        // The launcher key can auto-repeat while the user is clicking the MFM button. Refresh the
        // launcher debounce at the actual handoff so that repeat cannot immediately close MFM.
        g_LastLauncherToggleMs.store(NowMs());
        InjectEngineKey(toggleDIK);
        SKSE::log::info("OpenModFunctionMenu: injected {} to open Mod Function Menu after launcher handoff.",
            NameFromDIK(toggleDIK));
    }).detach();
}

static void OpenModFunctionMenu() {
    OpenModFunctionMenu("button");
}

static void OpenOPS() {
    if (!g_OPSConfig.enabled) {
        SKSE::log::warn("OpenOPS: Outfit Preview Selector is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::OPS || IsOPSMenuOpen()) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::OPS);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    // Open via the author's intended SKSE mod event (no VM method call, no Papyrus reach-in). OPS's own
    // plugin handles the open + camera correctly on its side.
    SendOPSModEvent("OPS_NativePreviewOpen");
    g_ActiveMenu.store(ActiveMenu::OPS);
    g_MenuOpenLockUntilMs.store(NowMs() + 500);
    SKSE::log::info("OpenOPS: sent OPS_NativePreviewOpen.");
}

static void OpenDebugMenu() {
    if (!g_DebugMenuConfig.enabled || g_DebugMenuConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenDebugMenu: DebugMenu hotkey is not configured.");
        return;
    }

    ResolveDebugMenuApi("OpenDebugMenu");

    if (g_DebugMenuConfig.setMenuOpen) {
        CloseLauncher();
        g_LastLauncherToggleMs.store(NowMs());

        const bool open = g_DebugMenuConfig.isMenuOpen
            ? g_DebugMenuConfig.isMenuOpen()
            : (g_ActiveMenu.load() == ActiveMenu::DebugMenu);

        if (open) {
            g_DebugMenuConfig.setMenuOpen(false);
            g_ActiveMenu.store(ActiveMenu::None);
            SKSE::log::info("OpenDebugMenu: closed Debug Menu via exported SetMenuOpen(false).");
            return;
        }

        if (g_DebugMenuConfig.setHotkeysEnabled && !g_UnblockDebugMenu.load()) {
            g_DebugMenuConfig.setHotkeysEnabled(false);
            SKSE::log::info("OpenDebugMenu: disabled Debug Menu native hotkey via exported SetHotkeyEnabled(false).");
        }

        g_DebugMenuConfig.setMenuOpen(true);
        g_ActiveMenu.store(ActiveMenu::DebugMenu);
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenDebugMenu: opened Debug Menu via exported SetMenuOpen(true).");
        return;
    }

    // Debug Menu is parked on an UNPRESSABLE F-key (e.g. F17). It reads its toggle two different ways
    // depending on state, so we drive each accordingly:
    //  - CLOSED: it reads the engine ButtonEvent stream -> inject the scan code via SendInput.
    //  - OPEN (its ImGui overlay has focus): it reads raw Win32 messages -> post WM_KEYDOWN with the
    //    virtual key AND a hardcoded scan code (SendInput's auto scan->VK lookup yields 0 for F17,
    //    which ImGui ignores; PostKeyWithScanToGameWindow supplies both halves).
    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowDebugMenuOpen.store(true);
    g_AllowDebugMenuOpenUntilMs.store(NowMs() + 1000);

    if (g_ActiveMenu.load() == ActiveMenu::DebugMenu || IsExternalMenuOpen()) {
        InjectEngineKey(g_DebugMenuConfig.toggleDIK); // same engine path as open; toggles it shut
        g_ActiveMenu.store(ActiveMenu::None);
        SKSE::log::info("OpenDebugMenu: closing Debug Menu via engine injection.");
        return;
    }

    g_ActiveMenu.store(ActiveMenu::DebugMenu);
    g_MenuOpenLockUntilMs.store(NowMs() + 500);
    InjectEngineKey(g_DebugMenuConfig.toggleDIK); // feeds a real F17 ButtonEvent to Debug Menu's sink
    SKSE::log::info("OpenDebugMenu: opening Debug Menu via engine injection.");
}

static void OpenDMenu() {
    // dMenu is supported only through the dMenu NG external-control API -- real menu state, no key
    // simulation. Stock/older dMenu without the API is not opened from the launcher.
    if (!HasDMenuApi()) AcquireDMenuApiFromExport();
    if (const auto* api = g_DMenuApi.load(); api && api->IsMenuOpen && api->OpenMenu && api->CloseMenu) {
        const bool open = api->IsMenuOpen();
        CloseLauncher();
        if (open) { api->CloseMenu(); g_ActiveMenu.store(ActiveMenu::None); }
        else      { api->OpenMenu();  g_ActiveMenu.store(ActiveMenu::DMenu); }
        g_LastLauncherToggleMs.store(NowMs());
        SKSE::log::info("OpenDMenu: {} dMenu via NG API.", open ? "closed" : "opened");
        return;
    }

    SKSE::log::warn("OpenDMenu: dMenu NG v2 API not present; dMenu is API-only now (a non-API dMenu is not opened from the launcher).");
}


// Post a key press/release straight to the game window (window message ONLY — no buffered
// DirectInput event). Used for FLICK, which otherwise double-toggles: SendInput produces
// both a buffer event and a window message, FLICK reads both, and it cancels out.
static void PostKeyToGameWindow(WORD vk) {
    std::thread([vk]() {
        auto* rw = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
        if (!rw || !rw->hWnd) { SKSE::log::warn("PostKeyToGameWindow: no render window."); return; }
        HWND hwnd = reinterpret_cast<HWND>(rw->hWnd);
        UINT scan = ::MapVirtualKeyA(vk, 0 /*MAPVK_VK_TO_VSC*/);
        LPARAM lp = (static_cast<LPARAM>(scan) << 16) | 1;
        switch (vk) {
            case VK_HOME: case VK_END: case VK_INSERT: case VK_DELETE:
            case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
            case VK_UP: case VK_DOWN: lp |= (1LL << 24); break; // extended keys
            default: break;
        }
        ::PostMessageA(hwnd, WM_KEYDOWN, vk, lp);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::PostMessageA(hwnd, WM_KEYUP, vk, lp | (1LL << 30) | (1LL << 31));
    }).detach();
}

// Post WM_KEYDOWN/UP with BOTH the virtual key AND an explicit hardware scan code baked into lParam.
// Needed for UNPRESSABLE keys like F17: Windows can't derive a scan code from VK_F17 (MapVirtualKey
// returns 0), and ImGui/Win32 ignores key messages whose lParam scan code is 0 -- so we build lParam
// ourselves from the DirectInput scan code we already know. This is how the launcher closes an ImGui
// menu (Debug Menu) that is parked on an unpressable F-key while it holds focus.
static void PostKeyWithScanToGameWindow(WORD vk, WORD scan) {
    std::thread([vk, scan]() {
        auto* rw = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
        if (!rw || !rw->hWnd) { SKSE::log::warn("PostKeyWithScanToGameWindow: no render window."); return; }
        HWND hwnd = reinterpret_cast<HWND>(rw->hWnd);
        LPARAM lp = (static_cast<LPARAM>(scan & 0xFF) << 16) | 1;
        if (scan > 0x7F) lp |= (1LL << 24); // extended-key flag for E0-prefixed scan codes
        ::PostMessageA(hwnd, WM_KEYDOWN, vk, lp);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        ::PostMessageA(hwnd, WM_KEYUP, vk, lp | (1LL << 30) | (1LL << 31));
    }).detach();
}

// Simulate a (modifier +) key chord via SendInput. SendInput sets BOTH the window message
// AND the live key state, which Improved Camera needs (it checks that Left Shift is held).
static void SimulateModifiedKey(WORD modifierDIK, WORD keyDIK) {
    std::thread([modifierDIK, keyDIK]() {
        std::array<INPUT, 2> down{}; UINT dc = 0;
        if (modifierDIK) AddScanInput(down[dc++], modifierDIK, false);
        if (keyDIK)      AddScanInput(down[dc++], keyDIK, false);
        if (dc) ::SendInput(dc, down.data(), sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::array<INPUT, 2> up{}; UINT uc = 0;
        if (keyDIK)      AddScanInput(up[uc++], keyDIK, true);
        if (modifierDIK) AddScanInput(up[uc++], modifierDIK, true);
        if (uc) ::SendInput(uc, up.data(), sizeof(INPUT));
    }).detach();
}

static void OpenImprovedCamera() {
    if (!g_ImprovedCameraConfig.enabled) {
        SKSE::log::warn("OpenImprovedCamera: Improved Camera is not available.");
        return;
    }

    const bool trackedOpen = g_ActiveMenu.load() == ActiveMenu::ImprovedCamera;
    if (trackedOpen) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::ImprovedCamera);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    if (ToggleImprovedCameraWithRegisteredCommand()) {
        CloseLauncher();
        g_ActiveMenu.store(ActiveMenu::ImprovedCamera);
        g_LastLauncherToggleMs.store(NowMs());
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenImprovedCamera: opened through the registered 'ic menu' handler.");
        return;
    }

    if (IsImprovedCameraOpen()) {
        CloseLauncher();
        if (SetImprovedCameraOpen(false)) {
            g_ActiveMenu.store(ActiveMenu::None);
            SKSE::log::info("OpenImprovedCamera: closed Improved Camera directly.");
        }
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    if (SetImprovedCameraOpen(true)) {
        g_ActiveMenu.store(ActiveMenu::ImprovedCamera);
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenImprovedCamera: opened Improved Camera directly.");
    } else {
        g_ActiveMenu.store(ActiveMenu::None);
    }
}

static void OpenKreatE() {
    if (!g_KreatEConfig.enabled || g_KreatEConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenKreatE: KreatE hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::KreatE) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::KreatE);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::KreatE);
    g_LastLauncherToggleMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 1000);

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        g_AllowKreatEOpen.store(true);
        g_AllowKreatEOpenUntilMs.store(NowMs() + 1000);
        SimulateModifiedKey(0, g_KreatEConfig.toggleDIK);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        g_AllowKreatEOpen.store(false);
        g_AllowKreatEOpenUntilMs.store(0);
        SKSE::log::info("OpenKreatE: toggled KreatE with {}.", NameFromDIK(g_KreatEConfig.toggleDIK));
    }).detach();
}

static void OpenCommunityShaders() {
    if (!g_CSConfig.enabled || g_CSConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenCommunityShaders: Community Shaders hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::CS) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::CS);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::CS);
    g_ActiveCSSub.store(0); // main menu
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowCSOpen.store(true);
    g_AllowCSOpenUntilMs.store(NowMs() + 1000);
    ArmCSKeyPass(0);
    InjectEngineKey(g_CSConfig.toggleDIK);
    SKSE::log::info("OpenCommunityShaders: queued engine event {}.", NameFromDIK(g_CSConfig.toggleDIK));
}

// ---------------------------------------------------------------------------------------------------
// SearchUI - Get Any Item Instantly integration.
//
// SearchUI has no hotkey API and no INI: its key is a Papyrus property (SearchUIController.SearchHotkey,
// RegisterForKey/OnKeyDown, default F4). We open it the same way we open every other mod - inject its
// key so its own OnKeyDown fires and its native search prompt comes up. Nothing reimplemented here.
//
// Key handling follows the same "original hotkey" toggle as the other mods (alias AI_SearchUI +
// g_UnblockSearchUI):
//   toggle ON  -> SearchUI's key = the user's chosen alias key (pressing it opens SearchUI)
//   toggle OFF -> SearchUI's key parked on the unpressable F18, so the chosen key is free for gameplay
// Either way the launcher button opens SearchUI by injecting its current key. We set the key live
// through the controller's own SetHotkey() via the Papyrus VM - no INI, no script edit, no patch.
// ---------------------------------------------------------------------------------------------------
static constexpr int      kSearchUIFreeKey = 0x69;       // F18 - internal park for the "off" (freed) state
static std::atomic<bool>  g_HasSearchUI{ false };
static std::atomic<int>   g_SearchUIOrigKey{ 0x3E };     // user's real key; default F4 until we read it
static std::atomic<bool>  g_SearchUIOrigCaptured{ false };
std::atomic<int>   g_SearchUIEffectiveDIK{ 0x3E };// key to inject to open (updated by the sync)
static std::atomic<std::uint32_t> g_SearchUIControllerFormID{ 0 };
static std::atomic<bool>  g_SearchUISyncQueued{ false };
static std::atomic<bool>  g_SearchUIExclReleased{ false }; // true once we've handed the key back for an exclusion

// True when SearchUI is in the load order. Latched like MCM so it stays visible from the main menu on.
static bool IsSearchUIAvailable() {
    if (g_HasSearchUI.load()) return true;
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (dh && dh->LookupModByName("SearchUI.esp")) {
        g_HasSearchUI.store(true);
        return true;
    }
    return false;
}

// Find the SearchUIController script object bound to its quest (cached by form id once found).
static bool GetSearchUIController(RE::BSScript::Internal::VirtualMachine* vm,
                                  RE::BSTSmartPointer<RE::BSScript::Object>& out) {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!vm || !dh) return false;
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) return false;
    const auto tryQuest = [&](RE::TESQuest* q) -> bool {
        if (!q) return false;
        const auto handle = policy->GetHandleForObject(q->GetFormType(), q);
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        if (vm->FindBoundObject(handle, "SearchUIController", obj) && obj) { out = obj; return true; }
        return false;
    };
    if (const auto id = g_SearchUIControllerFormID.load(); id != 0) {
        if (auto* q = RE::TESForm::LookupByID<RE::TESQuest>(id)) { if (tryQuest(q)) return true; }
    }
    for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
        if (tryQuest(q)) { g_SearchUIControllerFormID.store(q->GetFormID()); return true; }
    }
    return false;
}

// Keep SearchUI's key where we want it (freed onto F18, or restored to the original). VM work runs on
// the main thread. Called ~once a second and self-heals per save (the key lives in the co-save).
static void SyncSearchUIKey() {
    if (!IsSearchUIAvailable() || !IsGameLoaded()) return;
    if (g_SearchUISyncQueued.exchange(true)) return; // don't pile up tasks
    SKSE::GetTaskInterface()->AddTask([]() {
        auto clear = [] { g_SearchUISyncQueued.store(false); };
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) { clear(); return; }
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        if (!GetSearchUIController(vm, obj)) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true))
                SKSE::log::warn("SyncSearchUIKey: SearchUIController script not found yet.");
            clear(); return;
        }

        const auto* var = obj->GetProperty("SearchHotkey");
        const int cur = var ? var->GetSInt() : -1;
        {
            static std::atomic<bool> logged{ false };
            if (!logged.exchange(true))
                SKSE::log::info("SyncSearchUIKey: found SearchUIController; SearchHotkey={}.", cur);
        }
        if (cur < 0) { clear(); return; }

        // First time we see a real (non-parked) key, remember it as the user's original.
        if (!g_SearchUIOrigCaptured.load() && cur != kSearchUIFreeKey) {
            g_SearchUIOrigKey.store(cur);
            g_SearchUIOrigCaptured.store(true);
        }

        // Excluded: hand SearchUI's key back to its original ONCE, then stop managing it so the mod/user
        // owns it (that's what "release the key back to the game" means). Don't keep forcing it.
        if (g_ExcludeMod[AI_SearchUI].load()) {
            if (!g_SearchUIExclReleased.exchange(true)) {
                const int orig = g_SearchUIOrigKey.load();
                g_SearchUIEffectiveDIK.store(orig);
                if (cur != orig) {
                    auto* args = RE::MakeFunctionArguments(std::int32_t(orig));
                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
                    vm->DispatchMethodCall(obj, "SetHotkey", args, cb);
                    SKSE::log::info("SyncSearchUIKey: excluded - released key back to {} (was {}).", orig, cur);
                }
            }
            clear(); return;
        }
        g_SearchUIExclReleased.store(false); // re-included: resume managing

        // Toggle ON: SearchUI's key = the user's chosen alias key (so that key opens it). Toggle OFF:
        // park it on the unpressable F18 so the chosen key is free; the launcher button still opens it.
        const int aliasKey = g_AliasDik[AI_SearchUI].load();
        const int want = (g_UnblockSearchUI.load() && aliasKey != 0) ? aliasKey : kSearchUIFreeKey;
        g_SearchUIEffectiveDIK.store(want);
        if (cur != want) {
            auto* args = RE::MakeFunctionArguments(std::int32_t(want));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
            vm->DispatchMethodCall(obj, "SetHotkey", args, cb);
            SKSE::log::info("SyncSearchUIKey: SetHotkey({}) [{}], was {}.", want,
                (g_UnblockSearchUI.load() && aliasKey != 0) ? "user key on" : "freed (parked on F18)", cur);
        }
        clear();
    });
}

// Open SearchUI's search from the launcher by injecting its current key (F18 once freed, else F4)
// so its Papyrus OnKeyDown fires. SearchUI has no close API; its UITextEntryMenu closes natively
// with Tab, which is also the path used by Risa for F1/ESC close.
static void OpenSearchUI() {
    if (g_ActiveMenu.load() == ActiveMenu::SearchUI) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::SearchUI);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }
    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    g_ActiveMenu.store(ActiveMenu::SearchUI);
    g_MenuOpenLockUntilMs.store(NowMs() + 350);
    g_AllowSearchUIOpen.store(true);
    g_AllowSearchUIOpenUntilMs.store(NowMs() + 1000);
    const WORD dik = static_cast<WORD>(g_SearchUIEffectiveDIK.load());
    InjectEngineKey(dik);
    SKSE::log::info("OpenSearchUI: injected {} to open SearchUI's search (closes on its own Tab key).", NameFromDIK(dik));
}

// Clicking Community Shaders in the launcher opens its hub (Main/Editor/Overlay/Effect) instead
// of opening the main menu directly.
static void EnterCSHub() { g_LauncherSubView.store(1); }

// Community Shaders has several independent toggles, each on its own key. We open each by
// simulating its key through the engine (CS reads the buffered ButtonEvent stream, same as its
// main End toggle). Allow-window lets the injected key through our CS sink filter.
static void OpenCSEditor() {
    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::CS);
    g_ActiveCSSub.store(1); // editor
    g_LastLauncherToggleMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 1000);
    g_AllowCSOpen.store(true);
    g_AllowCSOpenUntilMs.store(NowMs() + 1000);
    ArmCSKeyPass(1);
    InjectEngineKey(g_CSConfig.editorDIK, g_CSConfig.editorModifierDIK);
    SKSE::log::info("OpenCSEditor: queued engine chord {}.",
        FormatModifiedHotkey(g_CSConfig.editorDIK, g_CSConfig.editorModifierDIK));
}
static void OpenCSOverlay() {
    CloseLauncher();
    // Overlay is a persistent HUD toggle, not a modal menu. F1 should reopen the launcher;
    // clicking this button again emits the same key and disables the overlay.
    g_ActiveMenu.store(ActiveMenu::None);
    g_ActiveCSSub.store(0);
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowCSOpen.store(true);
    g_AllowCSOpenUntilMs.store(NowMs() + 1000);
    ArmCSKeyPass(2);
    InjectEngineKey(g_CSConfig.overlayDIK);
    SKSE::log::info("OpenCSOverlay: queued CS Overlay {}.", FormatHotkey(g_CSConfig.overlayDIK));
}
static void OpenCSEffect() {
    CloseLauncher();
    // Effect is persistent too; only its hub button toggles it again.
    g_ActiveMenu.store(ActiveMenu::None);
    g_ActiveCSSub.store(0);
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowCSOpen.store(true);
    g_AllowCSOpenUntilMs.store(NowMs() + 1000);
    ArmCSKeyPass(3);
    InjectEngineKey(g_CSConfig.effectDIK);
    SKSE::log::info("OpenCSEffect: queued CS Effect {}.", FormatHotkey(g_CSConfig.effectDIK));
}

static void EnterPartySheetHub() { g_LauncherSubView.store(2); }

static bool ShowPartyInspectForCrosshair() {
    HMODULE module = ::GetModuleHandleA("SkyrimPartySheet.dll");
    if (!module) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    constexpr std::uint32_t kTimestamp = 0x6A327815;
    constexpr std::uint32_t kImageSize = 0x2C7000;
    if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize) {
        SKSE::log::warn("ShowPartyInspectForCrosshair: unsupported Skyrim Party Sheet build {:08X}/0x{:X}; using sink fallback.",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }

    auto* showAddress = base + 0x93700;
    constexpr std::array<std::uint8_t, 16> expected{
        0x48, 0x85, 0xC9, 0x0F, 0x84, 0xC5, 0x00, 0x00,
        0x00, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00
    };
    if (std::memcmp(showAddress, expected.data(), expected.size()) != 0) {
        SKSE::log::error("ShowPartyInspectForCrosshair: InspectCard::Show signature mismatch; using sink fallback.");
        return false;
    }

    RE::Actor* actor = GetPartyInspectCrosshairActor();
    if (!actor) {
        const auto formID = g_LastPartyInspectActor.load();
        if (formID != 0) actor = RE::TESForm::LookupByID<RE::Actor>(formID);
    }
    if (!actor) {
        SKSE::log::warn("ShowPartyInspectForCrosshair: no actor was under the crosshair before the launcher opened.");
        return false;
    }

    using Show_t = void(*)(RE::Actor*);
    reinterpret_cast<Show_t>(showAddress)(actor);
    SKSE::log::info("ShowPartyInspectForCrosshair: opened Inspect Card directly for actor {:08X}.", actor->GetFormID());
    return true;
}

static void OpenPartySheetAction(int action, WORD dik, const char* name, bool waitForCrosshair = false) {
    if (!g_PartySheetConfig.enabled) {
        SKSE::log::warn("OpenPartySheetAction: Skyrim Party Sheet is not available.");
        return;
    }
    TryHookModSinks();
    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 500);
    if (waitForCrosshair) {
        const auto request = g_PartyInspectRequest.fetch_add(1) + 1;
        g_ActiveMenu.store(ActiveMenu::PartySheet);
        g_ActivePartySheetSub.store(action);
        std::thread([request, action, dik, name]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(125));
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                if (g_PartyInspectRequest.load() == request) g_ActiveMenu.store(ActiveMenu::None);
                return;
            }
            tasks->AddTask([request, action, dik, name]() {
                if (g_PartyInspectRequest.load() != request ||
                    g_ActiveMenu.load() != ActiveMenu::PartySheet ||
                    g_ActivePartySheetSub.load() != action) return;
                if (ShowPartyInspectForCrosshair() || DispatchPartySheetKey(dik)) {
                    SKSE::log::info("OpenPartySheetAction: opened {} keylessly after restoring the crosshair target.", name);
                } else {
                    g_ActiveMenu.store(ActiveMenu::None);
                    SKSE::log::error("OpenPartySheetAction: delayed {} dispatch failed.", name);
                }
            });
        }).detach();
        return;
    }
    if (DispatchPartySheetKey(dik)) {
        g_ActiveMenu.store(ActiveMenu::PartySheet);
        g_ActivePartySheetSub.store(action);
        SKSE::log::info("OpenPartySheetAction: opened {} keylessly.", name);
    } else {
        g_ActiveMenu.store(ActiveMenu::None);
        SKSE::log::error("OpenPartySheetAction: could not dispatch {} because the mod input sink was unavailable.", name);
    }
}

static void OpenPartySettings() {
    OpenPartySheetAction(0, g_PartySheetConfig.settingsDIK, "Settings");
}
static void OpenPartySheet() {
    OpenPartySheetAction(1, g_PartySheetConfig.partyDIK, "Party Sheet");
}
static void OpenPartyInspect() {
    OpenPartySheetAction(2, g_PartySheetConfig.inspectDIK, "Inspect Card", true);
}
static void OpenPartyCharacter() {
    OpenPartySheetAction(3, g_PartySheetConfig.characterDIK, "Character Sheet");
}

static void OpenCatMenu() {
    if (!g_CatMenuConfig.enabled) {
        SKSE::log::warn("OpenCatMenu: CatMenu is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::CatMenu) {
        CloseLauncher();
        SetCatMenuOpen(false);
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    if (SetCatMenuOpen(true)) {
        g_ActiveMenu.store(ActiveMenu::CatMenu);
        g_LastLauncherToggleMs.store(NowMs());
        g_MenuOpenLockUntilMs.store(NowMs() + 500);
        SKSE::log::info("OpenCatMenu: opened CatMenu directly.");
    } else {
        g_ActiveMenu.store(ActiveMenu::None);
    }
}

static bool SetCatMenuOpen(bool open) {
    HMODULE module = ::GetModuleHandleA("catmenu.dll");
    if (!module) return false;

    // CatMenu 2.0.1 ships its PDB. UI::GetSingleton is RVA 0x152B0 and the
    // first byte of the returned UI object is its open flag. Validate the
    // function prologue so an updated CatMenu cannot turn this into a bad call.
    auto* getSingletonAddress = reinterpret_cast<std::uint8_t*>(module) + 0x152B0;
    constexpr std::array<std::uint8_t, 5> expected{ 0x40, 0x53, 0x48, 0x83, 0xEC };
    if (std::memcmp(getSingletonAddress, expected.data(), expected.size()) != 0) {
        SKSE::log::error("SetCatMenuOpen: unsupported CatMenu build; UI symbol signature changed.");
        return false;
    }

    using GetSingleton_t = void*(*)();
    void* ui = reinterpret_cast<GetSingleton_t>(getSingletonAddress)();
    if (!ui) return false;
    *reinterpret_cast<std::uint8_t*>(ui) = open ? 1 : 0;
    return true;
}

static DBTK_API::IVDBTK1* GetDragonbornToolkitApi() {
    static std::atomic<DBTK_API::IVDBTK1*> api{ nullptr };
    static std::atomic<bool> unavailableLogged{ false };

    if (auto* cached = api.load()) return cached;
    auto* resolved = DBTK_API::GetAPI();
    if (!resolved) {
        if (!unavailableLogged.exchange(true)) {
            SKSE::log::warn("Dragonborn's Toolkit API: RequestPluginAPI v1 unavailable; Dragonborn integration disabled.");
        }
        return nullptr;
    }

    const auto version = resolved->GetVersion();
    if (version < static_cast<std::uint32_t>(DBTK_API::InterfaceVersion::kV1)) {
        SKSE::log::error("Dragonborn's Toolkit API: rejected interface version {} (v1 required).", version);
        return nullptr;
    }

    api.store(resolved);
    SKSE::log::info("Dragonborn's Toolkit API: connected successfully (interface v{}, DLL=SkyrimCheatMenu.dll).", version);
    return resolved;
}

static bool SetDragonbornOpen(bool open) {
    if (auto* api = GetDragonbornToolkitApi()) {
        const bool before = api->IsOpen();
        if (open) {
            api->Open();
        } else {
            api->Close();
        }
        const bool after = api->IsOpen();
        SKSE::log::info(
            "Dragonborn's Toolkit API: requested={}, before={}, after={}, verified={}.",
            open ? "OPEN" : "CLOSE", before, after, after == open);
        return true;
    }
    SKSE::log::warn("Dragonborn's Toolkit API: public open/close API unavailable; refusing private fallback.");
    return false;
}

// Dragonborn's Toolkit v0.5+ exposes a runtime hotkey switch as a plain-C export. Calling it with
// false disables the toolkit's own key listener live - no SkyrimCheatMenu.json edit, no restart -
// so its key is free while the launcher drives open/close through the API. Returns false when the
// export is missing (older build), letting callers fall back to the JSON edit.
static bool SetDragonbornHotkeyEnabled(bool enabled) {
    HMODULE module = ::GetModuleHandleA("SkyrimCheatMenu.dll");
    if (!module) return false;
    using SetHotkeyEnabled_t = void (*)(bool);
    auto* fn = reinterpret_cast<SetHotkeyEnabled_t>(
        ::GetProcAddress(module, "DragonbornsToolkit_SetHotkeyEnabled"));
    if (!fn) return false;
    fn(enabled);
    SKSE::log::info("Dragonborn's Toolkit API: SetHotkeyEnabled({}) via runtime C export.",
        enabled ? "true" : "false");
    return true;
}

static void OpenDragonbornToolkit() {
    if (!g_DragonbornConfig.enabled) {
        SKSE::log::warn("OpenDragonbornToolkit: Dragonborn's Toolkit is not available.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::Dragonborn) {
        CloseLauncher();
        if (SetDragonbornOpen(false)) g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_LastLauncherToggleMs.store(NowMs());
    if (SetDragonbornOpen(true)) {
        g_ActiveMenu.store(ActiveMenu::Dragonborn);
        g_MenuOpenLockUntilMs.store(NowMs() + 1000);
        SKSE::log::info("OpenDragonbornToolkit: opened Dragonborn's Toolkit directly.");
    } else {
        g_ActiveMenu.store(ActiveMenu::None);
    }
}

// Called by the ReShade add-on bridge whenever ReShade reports its overlay opened or closed
// (from any source), so the launcher's active-menu state never desyncs — e.g. if the overlay is
// closed outside our control. Runs on ReShade's thread; only touches atomics.
static void OnReShadeOverlayStateChanged(bool open) {
    if (open) {
        g_ActiveMenu.store(ActiveMenu::ReShade);
    } else if (g_ActiveMenu.load() == ActiveMenu::ReShade) {
        g_ActiveMenu.store(ActiveMenu::None);
    }
}

static void OpenReShade() {
    if (!g_ReShadeConfig.enabled) {
        SKSE::log::warn("OpenReShade: ReShade is not present.");
        return;
    }

    // Only the add-on build is supported: drive ReShade's own overlay through the add-on API.
    // No key, no simulation. Compatibility/non-add-on ReShade is not opened from the launcher.
    if (g_ReShadeAddonActive.load() && RisaReShade::RuntimeReady()) {
        const bool wantOpen = g_ActiveMenu.load() != ActiveMenu::ReShade;
        if (wantOpen) CloseLauncher();
        RisaReShade::SetOverlayOpen(wantOpen);
        g_ActiveMenu.store(wantOpen ? ActiveMenu::ReShade : ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        SKSE::log::info("OpenReShade: {} ReShade overlay via add-on API.", wantOpen ? "opened" : "closed");
        return;
    }

    SKSE::log::warn("OpenReShade: ReShade add-on API not available (non-add-on build); overlay is left on its own hotkey.");
}


static long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void ForceCloseSKSEMenuFramework() {
    if (auto* mainWindow = SKSEMenuFramework::GetMainWindow()) {
        mainWindow->IsOpen.store(false);
    }
}

static void AddScanInput(INPUT& input, WORD scanCode, bool keyUp) {
    input = {};
    input.type = INPUT_KEYBOARD;

    WORD osScan = scanCode;
    // Inject by HARDWARE SCAN CODE (KEYEVENTF_SCANCODE), not virtual key. SendInput with a
    // virtual key only updates the immediate key state (GetDeviceState); it does NOT enter
    // DirectInput's buffered stream (GetDeviceData), which is what Skyrim builds its
    // ButtonEvents from — so VK-injected keys never reach the mods. Scan-code injection lands
    // in the buffered stream, so the engine generates the event and the mod opens.
    DWORD flags = KEYEVENTF_SCANCODE | (keyUp ? KEYEVENTF_KEYUP : 0);

    if (scanCode > 0x7F) {
        osScan = scanCode & 0x7F;
        flags |= KEYEVENTF_EXTENDEDKEY; // 0x0001
    }

    input.ki.wScan = osScan;
    input.ki.wVk = 0;
    input.ki.dwFlags = flags;
}




static bool HandleLauncherHotkeys(RE::InputEvent* ev, const char* source) {
    // Bootstrap the input hooks from here. They normally install from the GetDeviceState
    // per-frame scan, but ReShade wraps DirectInput so that hook never fires — leaving the
    // keyboard Process hook and mod sink hooks uninstalled, which breaks engine-event opens,
    // key suppression and F1 detection. This path DOES run under ReShade (it's how F1 was
    // still detected), so retry the installs here (throttled, idempotent).
    {
        static std::atomic<long long> lastHookScan{ 0 };
        const long long bnow = NowMs();
        long long last = lastHookScan.load(std::memory_order_relaxed);
        if (bnow - last >= 1000 &&
            lastHookScan.compare_exchange_strong(last, bnow, std::memory_order_relaxed)) {
            TryHookKeyboardProcess();
            TryHookModSinks();
            TryHookCatMenuNativeKey();
            // These live key-syncs also normally run from the GetDeviceState scan, which ReShade
            // kills. Mirror them here so they still fire under ReShade. dMenu especially: its
            // one-shot at load runs before dMenu's renderer is ready (IsReady()==false) and returns
            // early, so without this its keyboard toggle is never zeroed on a ReShade setup.
            TryManageKreatEHotkey(true);
            TryManageCSHotkey(true);
            TryManageCatMenuHotkey(true);
            TryManageModFunctionMenuHotkey(true);
            TryManageFLICKHotkey(true);
            TryManageOPSHotkey(true);
            TryManageDragonbornHotkey(true);
            SyncDMenuKeyViaApi();
            SyncSearchUIKey();  // mirror here too so it fires under ReShade (DI poll is dead there)
        }
    }

    if (!ev || ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton)
        return false;
    if (ev->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
        return false;

    if (IsRebinding()) {
        return false; // Let key events pass while binding the launcher hotkey or a mod alias
    }

    auto* btn = static_cast<RE::ButtonEvent*>(ev);
    const auto code = btn->GetIDCode();

    // Prefer the per-frame GetDeviceState poll. ReShade can wrap DirectInput with a device
    // whose vtable is not the one Skyrim ultimately polls, so fall back to SKSE's event only
    // when the normal keyboard poll has gone stale.
    if (code == g_LauncherHotkeyDIK.load()) {
        if (g_ConfigRestartRequired.load() && !g_ExclusionsPopupOpen) {
            return false; // ignore F1 during restart prompt and let it pass to the game
        }
        const long long now = NowMs();
        if ((g_AllowDebugMenuOpen.load() && now <= g_AllowDebugMenuOpenUntilMs.load()) ||
            (g_AllowDragonbornOpen.load() && now <= g_AllowDragonbornOpenUntilMs.load())) {
            return false; // let simulated F1 pass!
        }
        if (IsENBOpeningTransition()) {
            return true;
        }
        if (now < g_MenuOpenLockUntilMs.load()) {
            return false; // don't block: the launcher key stays free for the game
        }

        const bool directInputActive = now - g_LastDIKeyboardPollMs.load(std::memory_order_relaxed) <= 250;
        if (directInputActive) {
            return false;
        }

        if (btn->IsDown()) {
            const long long previous = g_LastLauncherFallbackPressMs.exchange(now);
            if (now - previous > 40) {
                if (!g_LauncherFallbackLogged.exchange(true)) {
                    SKSE::log::info("{}: DirectInput launcher poll is stale; using SKSE input fallback (ReShade compatible).", source);
                }

                const bool launcherOpen = g_LauncherWindow && g_LauncherWindow->IsOpen.load();
                const bool somethingOpen = launcherOpen || g_ActiveMenu.load() != ActiveMenu::None;
                if (g_LauncherHotkeyEasyClose.load() && somethingOpen) {
                    ToggleLauncher();
                } else if (AreLauncherModifiersOk() && !g_LauncherHotkeyHold.load()) {
                    if (g_LauncherHotkeyDoubleTap.load()) {
                        if (CheckLauncherDoubleTap(now)) {
                            ToggleLauncher();
                        }
                    } else if (now - g_LastLauncherToggleMs.load() > 700) {
                        ToggleLauncher();
                    }
                }
            }
        }
        return false; // never block the launcher key - let it reach the game/other mods too
    }

    if (!btn->IsDown())
        return false;

    if (code == kEscapeDIK && g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
        if (g_ConfigRestartRequired.load() && !g_ExclusionsPopupOpen) {
            return false; // ignore ESC during restart prompt and let it pass to the game
        }
        CloseLauncher(true); // external ESC close — arm the next F1 for a fresh open
        SKSE::log::info("{}: launcher closed by ESC.", source);
        return true;
    }

    return false;
}

static bool __stdcall FrameworkInputCallback(RE::InputEvent* ev) {
    return HandleLauncherHotkeys(ev, "MF input");
}

// ============================================================================
// BSTEventSink — fallback F1 toggle + ESC close
// ============================================================================
class RisaInputSink : public RE::BSTEventSink<RE::InputEvent*> {
public:
    RE::BSEventNotifyControl ProcessEvent(
        RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
    {
        if (!a_event || !*a_event) return RE::BSEventNotifyControl::kContinue;
        for (auto* ev = *a_event; ev; ev = ev->next) {
            if (HandleLauncherHotkeys(ev, "BST input"))
                return RE::BSEventNotifyControl::kStop;
        }

        return RE::BSEventNotifyControl::kContinue;
    }
    static RisaInputSink* GetSingleton() { static RisaInputSink s; return &s; }
};

class RisaMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (!event || event->menuName != RE::JournalMenu::MENU_NAME) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (event->opening) {
            if (g_OpenMCMWhenJournalReady.exchange(false) && g_ActiveMenu.load() == ActiveMenu::MCM) {
                if (!OpenMCMPanel()) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddUITask([]() {
                            if (g_ActiveMenu.load() == ActiveMenu::MCM && !OpenMCMPanel()) {
                                SKSE::log::error("RisaMenuSink: SkyUI ConfigPanelOpen failed; MCM may be unavailable.");
                            }
                        });
                    }
                }
            }
        } else if (g_ActiveMenu.load() == ActiveMenu::MCM) {
            g_OpenMCMWhenJournalReady.store(false);
            g_ActiveMenu.store(ActiveMenu::None);
            g_LastLauncherToggleMs.store(NowMs() - 1000);
            SKSE::log::info("RisaMenuSink: Journal/MCM closed; active-menu state synchronized.");
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    static RisaMenuSink* GetSingleton() { static RisaMenuSink sink; return &sink; }
};

static bool IsGameLoaded() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;
    if (ui->IsMenuOpen("MainMenu")) return false;
    if (ui->IsMenuOpen("Loading Menu")) return false;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell()) return false;

    return true;
}

static const char* IniWriteStatus(int result) {
    switch (result) {
        case 1:  return "Changed this launch";
        case 0:  return "Already configured";
        case -1: return "Missing or write failed";
        default: return "Not attempted";
    }
}

static void LogStartupDiagnostics() {
    const std::string launcher = FormatHotkey(g_LauncherHotkeyDIK.load(),
        g_LauncherHotkeyCtrl.load(), g_LauncherHotkeyShift.load(), g_LauncherHotkeyAlt.load());

    SKSE::log::info("========== Risa's Menu startup diagnostics ==========");
    SKSE::log::info("Plugin version: {}", kRisaMenuVersion);
    SKSE::log::info("Skyrim runtime: {} ({})", g_RuntimeVersion, g_RuntimeEdition);
    SKSE::log::info("SKSE version: {}", g_SKSEVersion);
    SKSE::log::info("Launcher hotkey: {}{}", launcher,
        g_LauncherHotkeyDoubleTap.load() ? " [double tap]" : "");
    SKSE::log::info("SKSE Menu Framework: detected={}, key={}, mode={}",
        GetModuleHandleA("SKSEMenuFramework.dll") != nullptr,
        g_MFConfig.toggleKeyName.empty() ? "Unknown" : g_MFConfig.toggleKeyName,
        g_MFConfig.toggleMode.empty() ? "Unknown" : g_MFConfig.toggleMode);
    SKSE::log::info("Mod Configuration Menu: SkyUI detected={}, integration=native Journal/Scaleform.",
        IsMCMAvailable());
    SKSE::log::info("Open Animation Replacer: detected={}, native listener={}, original Shift+O={}",
        g_OARConfig.enabled, g_OARConfig.toggleDIK == 0 ? "disabled (keyless)" : "NOT DISABLED",
        g_UnblockOAR.load() ? "enabled" : "disabled");
    SKSE::log::info("Immersive Equipment Displays: detected={}, key={}, original Backspace={}",
        g_IEDConfig.enabled, g_IEDConfig.enabled ? FormatHotkey(g_IEDConfig.toggleDIK) : "N/A",
        g_UnblockIED.load() ? "enabled" : "disabled");
    SKSE::log::info("ENB Editor: detected={}, original Shift+Enter={}",
        g_ENBConfig.enabled, g_UnblockENB.load() ? "enabled" : "disabled");
    SKSE::log::info("Debug Menu: detected={}, key={}, original F1={}",
        g_DebugMenuConfig.enabled,
        g_DebugMenuConfig.enabled ? FormatHotkey(g_DebugMenuConfig.toggleDIK) : "N/A",
        g_UnblockDebugMenu.load() ? "enabled" : "disabled");
    SKSE::log::info("dMenu: detected={}, key={}, original Home={}",
        g_DMenuConfig.enabled,
        g_DMenuConfig.enabled ? FormatModifiedHotkey(g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK) : "N/A",
        g_UnblockDMenu.load() ? "enabled" : "disabled");
    SKSE::log::info("Improved Camera: detected={}, key={}, original Shift+Home={}",
        g_ImprovedCameraConfig.enabled,
        g_ImprovedCameraConfig.enabled ? FormatModifiedHotkey(g_ImprovedCameraConfig.toggleDIK, g_ImprovedCameraConfig.modifierDIK) : "N/A",
        g_UnblockImprovedCamera.load() ? "enabled" : "disabled");
    SKSE::log::info("FLICK: detected={}, key={}, original F7={}",
        g_FLICKConfig.enabled, g_FLICKConfig.enabled ? FormatHotkey(g_FLICKConfig.toggleDIK) : "N/A",
        g_UnblockFLICK.load() ? "enabled" : "disabled");
    SKSE::log::info("KreatE: detected={}, key={}, original End={}, INI managed={}",
        g_KreatEConfig.enabled, g_KreatEConfig.enabled ? FormatHotkey(g_KreatEConfig.toggleDIK) : "N/A",
        g_UnblockKreatE.load() ? "enabled" : "disabled", g_KreatEIniManaged.load());
    SKSE::log::info("Community Shaders: detected={}, key={}, original End={}",
        g_CSConfig.enabled, g_CSConfig.enabled ? FormatHotkey(g_CSConfig.toggleDIK) : "N/A",
        g_UnblockCS.load() ? "enabled" : "disabled");
    SKSE::log::info("Community Shaders subkeys: editor={}+{} (alias={}), overlay={} (alias={}), effect={} (alias={})",
        FormatHotkey(g_CSConfig.editorModifierDIK), FormatHotkey(g_CSConfig.editorDIK),
        g_UnblockCSEditor.load() ? "enabled" : "disabled", FormatHotkey(g_CSConfig.overlayDIK),
        g_UnblockCSOverlay.load() ? "enabled" : "disabled", FormatHotkey(g_CSConfig.effectDIK),
        g_UnblockCSEffect.load() ? "enabled" : "disabled");
    SKSE::log::info("Skyrim Party Sheet: detected={}, keyless sink=true, settings={} (alias={}), party={} (alias={}), inspect={} (alias={}), character={} (alias={})",
        g_PartySheetConfig.enabled, FormatHotkey(g_PartySheetConfig.settingsDIK),
        g_UnblockPartySettings.load() ? "enabled" : "disabled", FormatHotkey(g_PartySheetConfig.partyDIK),
        g_UnblockPartySheet.load() ? "enabled" : "disabled", FormatHotkey(g_PartySheetConfig.inspectDIK),
        g_UnblockPartyInspect.load() ? "enabled" : "disabled", FormatHotkey(g_PartySheetConfig.characterDIK),
        g_UnblockPartyCharacter.load() ? "enabled" : "disabled");
    SKSE::log::info("CatMenu: detected={}, native listener={}, original F6={}, JSON managed={}",
        g_CatMenuConfig.enabled, g_CatMenuKeyHookInstalled.load() ? "disabled (keyless)" : "NOT DISABLED",
        g_UnblockCatMenu.load() ? "enabled" : "disabled", g_CatMenuIniManaged.load());
    SKSE::log::info("Dragonborn's Toolkit: detected={}, key={}, original F1={}, hotkey managed={}",
        g_DragonbornConfig.enabled, g_DragonbornConfig.enabled ? FormatHotkey(g_DragonbornConfig.toggleDIK) : "N/A",
        g_UnblockDragonborn.load() ? "enabled" : "disabled", g_DragonbornIniManaged.load());
    SKSE::log::info("ReShade: detected={}, key={}, original Home={}", g_ReShadeConfig.enabled,
        g_ReShadeConfig.enabled ? FormatHotkey(g_ReShadeConfig.toggleDIK) : "N/A",
        g_UnblockReShade.load() ? "enabled" : "disabled");
    SKSE::log::info("Hooks: CS input={}, raw input={}, DI state={}, DI buffered={}, GetAsyncKeyState={}, GetKeyState={}, framework callback={}",
        g_OrigCSProcessInputEvents != nullptr, g_OrigGetRawInputData != nullptr,
        g_OrigGetDeviceState != nullptr, g_OrigGetDeviceData != nullptr,
        g_OrigGetAsyncKeyState != nullptr, g_OrigGetKeyState != nullptr,
        g_FrameworkInputEvent != nullptr);
    SKSE::log::info("Privacy: data-relative paths only; no usernames, save names, hardware IDs, or unrelated mod list.");
    SKSE::log::info("=====================================================");
}

static bool IsModInstalled(int aliasIdx);
static WORD GetModNativeKey(int aliasIdx);
static std::string GetModNativeHotkeyString(int aliasIdx);
static const char* GetModDisplayName(int aliasIdx);
static bool ModHasNoNativeKey(int aliasIdx);
static std::string GetBackupKeyString(int aliasIdx, bool forceModDefaults);
static bool RestoreModDefaults(int aliasIdx, bool forceModDefaults);
static void CommitExclusions(bool useUserDefaults);
static std::string GetExclusionDetailString(int aliasIdx);
static void CopyToClipboard(const std::string& text);

// A sliding on/off toggle switch (SKSE Menu Framework has no built-in one, so draw it). Returns true
// when clicked and flips *v. Green = on (excluded), grey = off (managed).
static bool SlideToggle(const char* id, bool* v, bool enabled = true, float layoutScale = 1.0f,
    float heightMult = 0.90f, float minHUnits = 22.0f, float widthMult = 1.85f) {
    ImGuiMCP::ImVec2 p; ImGuiMCP::GetCursorScreenPos(&p);
    // Match the active ImGui frame height so the switch grows with the row's visible controls.
    // Keep a floor based on UI Scale so tiny Font Scale values don't make the switch unreadable.
    float h = ImGuiMCP::GetFrameHeight() * heightMult;
    const float minH = minHUnits * layoutScale;
    if (h < minH) h = minH;
    const float w = h * widthMult;
    const float r = h * 0.5f;
    const bool clicked = enabled && ImGuiMCP::InvisibleButton(id, ImGuiMCP::ImVec2(w, h));
    if (clicked) *v = !*v;
    const bool hov = enabled && ImGuiMCP::IsItemHovered(0);
    auto* dl = ImGuiMCP::GetWindowDrawList();
    ImGuiMCP::ImU32 bg;
    if (enabled) {
        bg = *v
            ? (hov ? IM_COL32(88, 190, 110, 255) : IM_COL32(70, 165, 92, 255))
            : (hov ? IM_COL32(112, 112, 124, 255) : IM_COL32(88, 88, 100, 255));
    } else {
        bg = *v
            ? IM_COL32(70, 165, 92, 100)
            : IM_COL32(88, 88, 100, 100);
    }
    ImGuiMCP::ImDrawListManager::AddRectFilled(dl, p, ImGuiMCP::ImVec2(p.x + w, p.y + h), bg, r, 0);
    const float cx = *v ? (p.x + w - r) : (p.x + r);
    ImGuiMCP::ImDrawListManager::AddCircleFilled(dl, ImGuiMCP::ImVec2(cx, p.y + r), r - 2.0f,
        enabled ? IM_COL32(245, 245, 245, 255) : IM_COL32(200, 200, 200, 100), 20);
    return clicked;
}

static void DrawExclusionsModals() {
    const float baseUiScale = g_LauncherFontScale.load();
    const float uiScale = LanguageFontScale(baseUiScale * g_LauncherTextScale.load());
    const float layoutScale = baseUiScale / 0.9f;

    ImGuiMCP::ImVec2 center;
    if (auto* viewport = ImGuiMCP::GetMainViewport()) {
        center = ImGuiMCP::ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
    }
    ImGuiMCP::SetNextWindowPos(center, ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2(0.5f, 0.5f));
    ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(440.0f * layoutScale, 0.0f));

    // Remove the dim overlay (white screen), center button text, and center the window title
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ModalWindowDimBg, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowTitleAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));

    if (ImGuiMCP::BeginPopupModal(Tr("popup.exclusions.title", "Confirm Exclusions"), nullptr, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        g_ExclusionsPopupOpen = true;
        ImGuiMCP::SetWindowFontScale(uiScale);
        if (g_ExclusionStep == 0 || g_ExclusionStep == 1) {
            if (g_ExclusionsDetailText.empty()) {
                // Only re-includes (un-excludes) this time - nothing to restore, so just confirm.
                ImGuiMCP::TextWrapped("%s", Tr("popup.exclusions.reinclude_body",
                    "These mods will be managed by Risa again (their hotkeys relocated). A restart is recommended afterward so they reload cleanly."));
                ImGuiMCP::Separator();
                if (ButtonFit(Tr("button.apply", "Apply"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                    CommitExclusions(false);
                    g_ExclusionStep = 2;
                }
                if (ButtonFit(Tr("button.cancel", "Cancel"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                    ImGuiMCP::CloseCurrentPopup();
                    g_ExclusionStep = 0;
                    g_ExclusionsPopupOpen = false;
                }
            } else {
            ImGuiMCP::TextWrapped("%s", Tr("popup.exclusions.restore_preview_intro", "Confirm the keys that will be restored for your excluded mods:"));
            ImGuiMCP::Spacing();

            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ChildBg, ImGuiMCP::ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
            ImGuiMCP::BeginChild("##ExclusionsRestorePreview", ImGuiMCP::ImVec2(0.0f, 320.0f * layoutScale), true, ImGuiMCP::ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGuiMCP::SetWindowFontScale(uiScale);

            ImGuiMCP::TextUnformatted(g_ExclusionsDetailText.c_str());

            ImGuiMCP::EndChild();
            ImGuiMCP::PopStyleColor(1);

            ImGuiMCP::Spacing();
            float copyBtnW = 120.0f * layoutScale;
            ImGuiMCP::ImVec2 avail; ImGuiMCP::GetContentRegionAvail(&avail);
            ImGuiMCP::SetCursorPosX(avail.x - copyBtnW);
            std::string copyBtnLabel = (FontAwesome::UnicodeToUtf8(0xf0c5) + " " + Tr("button.copy_info", "Copy Info")) + "##CopyExclusionsText";
            if (ButtonFit(copyBtnLabel.c_str(), ImGuiMCP::ImVec2(copyBtnW, 0.0f), uiScale)) {
                CopyToClipboard(g_ExclusionsDetailText);
            }

            ImGuiMCP::Separator();

            if (ButtonFit(Tr("button.return_user_defaults", "Return to User Defaults"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                CommitExclusions(true); // Restores user-original captured values
                g_ExclusionStep = 2;   // Proceed to Exit Game option
            }
            if (ButtonFit(Tr("button.return_mod_defaults", "Return to Mod Defaults"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                CommitExclusions(false); // Restores mod author defaults
                g_ExclusionStep = 2;    // Proceed to Exit Game option
            }
            if (ButtonFit(Tr("button.cancel", "Cancel"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                ImGuiMCP::CloseCurrentPopup();
                g_ExclusionStep = 0;
                g_ExclusionsPopupOpen = false;
            }
            }
        }
        else if (g_ExclusionStep == 2) {
            ImGuiMCP::TextWrapped("%s", Tr("popup.exclusions.success_body",
                "Changes applied successfully! It is highly recommended to restart Skyrim so the mods reload their native configurations cleanly."));
            ImGuiMCP::Separator();

            float btnW = 150.0f * layoutScale;
            float spacing = 8.0f * layoutScale;
            float totalW = btnW * 2 + spacing;
            ImGuiMCP::ImVec2 avail;
            ImGuiMCP::GetContentRegionAvail(&avail);
            float availW = avail.x;
            ImGuiMCP::SetCursorPosX((availW - totalW) * 0.5f);

            if (ButtonFit(Tr("button.exit_game_now", "Exit Game Now"), ImGuiMCP::ImVec2(btnW, 0.0f), uiScale)) {
                if (auto* consoleLog = RE::ConsoleLog::GetSingleton()) {
                    consoleLog->Print("Exiting game...");
                }
                std::exit(0); // Exit process cleanly
            }
            ImGuiMCP::SameLine(0.0f, spacing);
            if (ButtonFit(Tr("button.continue_playing", "Continue Playing"), ImGuiMCP::ImVec2(btnW, 0.0f), uiScale)) {
                ImGuiMCP::CloseCurrentPopup();
                g_ExclusionStep = 0; // Reset step
                g_ExclusionsPopupOpen = false;
            }
        }
        ImGuiMCP::EndPopup();
    }
    else {
        g_ExclusionsPopupOpen = false;
    }

    ImGuiMCP::PopStyleVar(2);
    ImGuiMCP::PopStyleColor(1);
}

static void DrawExclusionsTabContent() {
    const float baseUiScale = g_LauncherFontScale.load();
    const float uiScale = LanguageFontScale(baseUiScale * g_LauncherTextScale.load());
    const float layoutScale = baseUiScale / 0.9f;

    // Sync the working copy from the committed state each time the tab is (re)entered. g_ExclusionsActive
    // still holds LAST frame's tab state here (it's updated after this draws), so when it's false the tab
    // was just opened -> copy committed -> temp. This also discards any un-applied toggles on leave, and
    // (unlike the old once-per-session latch) reflects committed changes made elsewhere, e.g. a Restore.
    if (!g_ExclusionsActive) {
        for (int i = 0; i < AI_COUNT; ++i) {
            g_TempExcludeState[i] = g_ExcludeMod[i].load();
        }
        g_TempExcludeState[AI_MCM] = false;
    }

    auto SmallTooltip = [&](const std::string& text) {
        if (!text.empty() && ImGuiMCP::IsItemHovered(0)) {
            ImGuiMCP::BeginTooltip();
            ImGuiMCP::SetWindowFontScale(uiScale * 0.8f);
            ImGuiMCP::PushTextWrapPos(280.0f * layoutScale);
            ImGuiMCP::TextUnformatted(text.c_str());
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndTooltip();
        }
    };

    // 1. Top Panel: description + Apply button. Keep the button inside the tab even
    // when translated text or normal menu width leaves no same-line room.
    ImGuiMCP::AlignTextToFramePadding();
    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s",
        Tr("exclusions.description", "Excluding mods releases their keys back to the game."));

    bool hasChanges = false;
    for (int i = 0; i < AI_COUNT; ++i) {
        if (g_TempExcludeState[i] != g_ExcludeMod[i].load()) {
            hasChanges = true;
            break;
        }
    }

    ImGuiMCP::BeginDisabled(!hasChanges);
    if (!hasChanges) {
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text,           ImGuiMCP::ImVec4(0.35f, 0.35f, 0.38f, 0.7f));
    }

    const std::string applyExclusionsLabel = TrLabel("button.apply_changes", "Apply Changes", "##ApplyExclusions");
    ImGuiMCP::ImVec2 exclusionsApplyAvail;
    ImGuiMCP::GetContentRegionAvail(&exclusionsApplyAvail);
    float applyButtonW = 135.0f * layoutScale;
    if (applyButtonW > exclusionsApplyAvail.x) {
        applyButtonW = exclusionsApplyAvail.x;
    }
    if (exclusionsApplyAvail.x > applyButtonW) {
        ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + (exclusionsApplyAvail.x - applyButtonW));
    }
    if (ButtonFit(applyExclusionsLabel.c_str(), ImGuiMCP::ImVec2(applyButtonW, 0.0f), uiScale)) {
        int changeCount = 0;
        g_ExclusionsDetailText = "";
        for (int i = 0; i < AI_COUNT; ++i) {
            if (g_TempExcludeState[i] != g_ExcludeMod[i].load()) changeCount++; // any add OR remove
            // Filter sub-aliases and MCM out of the preview text to keep it identical to the main tab
            if (i == AI_CSEditor || i == AI_CSOverlay || i == AI_CSEffect ||
                i == AI_PartySheet || i == AI_PartyInspect || i == AI_PartyCharacter || i == AI_MCM) {
                continue;
            }
            if (IsModInstalled(i) && g_TempExcludeState[i] && !g_ExcludeMod[i].load()) {
                std::string userKey = GetBackupKeyString(i, false);
                std::string modKey = GetBackupKeyString(i, true);
                g_ExclusionsDetailText += std::format(
                    "{} {} {}\n\n{}:\n  - {}: {}\n  - {}: {}\n\n",
                    Tr("exclusions.detail.decorator", "==="), GetModDisplayName(i), Tr("exclusions.detail.decorator", "==="),
                    Tr("exclusions.detail.restoring_keybinds", "Restoring Keybinds"),
                    Tr("exclusions.detail.user_original", "User Original"), userKey.empty() ? Tr("exclusions.detail.none_captured", "(none captured)") : userKey,
                    Tr("exclusions.detail.mod_default", "Mod Default"), modKey.empty() ? Tr("exclusions.detail.unknown", "(unknown)") : modKey);
            }
        }
        // Always route through the confirmation popup - never commit here. Committing in the button
        // triggered the "restart required" notice, which took over the window before the popup could run.
        if (changeCount > 0) {
            g_ExclusionStep = 1;
            ImGuiMCP::OpenPopup(Tr("popup.exclusions.title", "Confirm Exclusions"));
        }
    }

    ImGuiMCP::EndDisabled();
    if (!hasChanges) {
        ImGuiMCP::PopStyleColor(4);
    }

    // 2. Render confirmation modal
    DrawExclusionsModals();

    ImGuiMCP::Separator();

    float exclusionsChildH;
    // Hold the restart-required takeover while the exclusions confirmation popup is open, so it can't
    // replace the window mid-process (the popup's final step already prompts for the restart).
    const bool restartPrompt = g_ConfigRestartRequired.load() && !g_ExclusionsPopupOpen;
    const bool settingsActive = !restartPrompt && (g_SettingsActive || g_ExclusionsActive);
    if (settingsActive) {
        ImGuiMCP::ImVec2 avail; ImGuiMCP::GetContentRegionAvail(&avail);
        exclusionsChildH = avail.y;
    } else {
        exclusionsChildH = g_SettingsWindowHeight.load() - 70.0f; // 1-frame transition estimate
    }
    if (exclusionsChildH < 120.0f) exclusionsChildH = 120.0f;

    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Separator, ImGuiMCP::ImVec4(0.40f, 0.52f, 0.78f, 1.0f));

    ImGuiMCP::BeginChild("##ExclusionsScroll", ImGuiMCP::ImVec2(0.0f, exclusionsChildH), 0,
        ImGuiMCP::ImGuiWindowFlags_AlwaysVerticalScrollbar);

    ImGuiMCP::SetWindowFontScale(uiScale);

    // 5 columns (Mod | spacer | Current Hotkey | spacer | Exclude) to match the Settings tab's spacing.
    if (ImGuiMCP::BeginTable("ExclusionsTbl", 5, ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
        ImGuiMCP::TableSetupColumn(Tr("table.mod", "Mod"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 200.0f * layoutScale);
        ImGuiMCP::TableSetupColumn("##esp1", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGuiMCP::TableSetupColumn(Tr("table.current_hotkey", "Current Hotkey"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 150.0f * layoutScale);
        ImGuiMCP::TableSetupColumn("##esp2", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGuiMCP::TableSetupColumn(Tr("table.exclude", "Exclude"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 72.0f * layoutScale);

        // Manual header row placed in the SAME columns as the data (0/2/4) so the labels line up with the
        // values below - TableHeadersRow let the narrow "Exclude" header overflow off the right edge.
        ImGuiMCP::TableNextRow(ImGuiMCP::ImGuiTableRowFlags_Headers, 0.0f);
        ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::TableHeader(Tr("table.mod", "Mod"));
        ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::TableHeader(Tr("table.current_hotkey", "Current Hotkey"));
        ImGuiMCP::TableSetColumnIndex(4); ImGuiMCP::TableHeader(Tr("table.exclude", "Exclude"));

        // Build the visible list (installed primary mods only), then show it alphabetically by name.
        std::vector<int> exclOrder;
        for (int i = 0; i < AI_COUNT; ++i) {
            // Filter sub-aliases and MCM out of the main exclusions tab - only show primary mods
            if (i == AI_CSEditor || i == AI_CSOverlay || i == AI_CSEffect ||
                i == AI_PartySheet || i == AI_PartyInspect || i == AI_PartyCharacter ||
                i == AI_MCM) {
                continue;
            }
            if (!IsModInstalled(i)) continue;
            exclOrder.push_back(i);
        }
        std::sort(exclOrder.begin(), exclOrder.end(), [](int a, int b) {
            return _stricmp(GetModDisplayName(a), GetModDisplayName(b)) < 0;
        });

        for (int i : exclOrder) {
            ImGuiMCP::TableNextRow(0, 32.0f * layoutScale);

            // Column 0: Mod Name (friendly)
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::AlignTextToFramePadding();
            ImGuiMCP::Text("%s", GetModDisplayName(i));

            // Column 2: Read-Only Current Hotkey (shows user-configured key if managed, native if excluded)
            ImGuiMCP::TableSetColumnIndex(2);
            ImGuiMCP::AlignTextToFramePadding();
            if (ModHasNoNativeKey(i)) {
                ImGuiMCP::TextDisabled("%s", Tr("hotkey.none", "None"));
            } else {
                std::string currentKeyStr;
                if (g_ExcludeMod[i].load()) {
                    currentKeyStr = GetModNativeHotkeyString(i);
                    ImGuiMCP::TextDisabled("%s", currentKeyStr.c_str());
                } else {
                    std::string prefix = "";
                    if (g_AliasCtrl[i].load()) prefix += "Ctrl + ";
                    if (g_AliasShift[i].load()) prefix += "Shift + ";
                    if (g_AliasAlt[i].load()) prefix += "Alt + ";
                    currentKeyStr = prefix + NameFromDIK(g_AliasDik[i].load());
                    if (currentKeyStr.empty() || currentKeyStr == "None") {
                        currentKeyStr = Tr("hotkey.none", "None");
                        ImGuiMCP::TextDisabled("%s", currentKeyStr.c_str());
                    } else {
                        ImGuiMCP::Text("%s", currentKeyStr.c_str());
                    }
                }
            }

            // Column 4: Exclude slide toggle, right-aligned so it stays at the row's right edge.
            ImGuiMCP::TableSetColumnIndex(4);
            bool excluded = g_TempExcludeState[i];
            float exclusionsToggleH = ImGuiMCP::GetFrameHeight() * 0.90f;
            const float exclusionsMinToggleH = 22.0f * layoutScale;
            if (exclusionsToggleH < exclusionsMinToggleH) exclusionsToggleH = exclusionsMinToggleH;
            const float toggleW = exclusionsToggleH * 1.85f;
            ImGuiMCP::ImVec2 colAvail; ImGuiMCP::GetContentRegionAvail(&colAvail);
            if (colAvail.x > toggleW + 4.0f * layoutScale) {
                ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + (colAvail.x - toggleW - 4.0f * layoutScale));
            }
            std::string toggleId = std::string("##exclude_") + g_AliasIds[i];
            if (SlideToggle(toggleId.c_str(), &excluded, true, layoutScale)) {
                g_TempExcludeState[i] = excluded;

                // If this is a parent mod like CS or PartySettings, cascade the temp state to its child aliases
                if (i == AI_CS) {
                    g_TempExcludeState[AI_CSEditor] = excluded;
                    g_TempExcludeState[AI_CSOverlay] = excluded;
                    g_TempExcludeState[AI_CSEffect] = excluded;
                } else if (i == AI_PartySettings) {
                    g_TempExcludeState[AI_PartySheet] = excluded;
                    g_TempExcludeState[AI_PartyInspect] = excluded;
                    g_TempExcludeState[AI_PartyCharacter] = excluded;
                }
            }
        }
        ImGuiMCP::EndTable();
    }
    ImGuiMCP::PopStyleColor(); // separator color
    ImGuiMCP::EndChild();
}

// ============================================================================
// Launcher render
// ============================================================================
static void __stdcall RenderLauncher() {
    UpdateTimeFreeze(); // clear the framework's freeze here (runs every frame it draws us, even with input blocked)

    // Handle capture timeout (3 seconds)
    if (g_CapturingAlias.load() != -1 || g_WaitingForHotkeyPress.load()) {
        if (NowMs() - g_KeyCaptureStartedMs.load() > 3000) {
            g_CapturingAlias.store(-1);
            g_WaitingForHotkeyPress.store(false);
            SKSE::log::info("RenderLauncher: key capture timed out after 3 seconds.");
        }
    }

    if (g_ConfigRestartRequired.load()) {
        long long started = g_RestartNoticeStartedMs.load();
        if (started == 0) {
            started = RestartNoticeNowMs();
            g_RestartNoticeStartedMs.store(started);
        }
        if (RestartNoticeNowMs() - started >= 10000) {
            g_ConfigRestartRequired.store(false);
            g_RestartNoticeStartedMs.store(0);
            CloseLauncher(true);
            SKSE::log::info("Configuration restart notice closed after 10 seconds.");
            return;
        }
    }

    const bool restartPrompt = g_ConfigRestartRequired.load() && !g_ExclusionsPopupOpen;
    if (restartPrompt) {
        float fade = 1.0f;
        const long long started = g_RestartNoticeStartedMs.load();
        if (started > 0) {
            const long long elapsed = RestartNoticeNowMs() - started;
            if (elapsed > 8500) {
                fade = 1.0f - static_cast<float>(elapsed - 8500) / 1500.0f;
                if (fade < 0.0f) fade = 0.0f;
            }
        }

        ImGuiMCP::SetNextWindowBgAlpha(0.78f * fade);
        ImGuiMCP::PushStyleColor(2,  ImGuiMCP::ImVec4(0.10f, 0.11f, 0.16f, 1.0f * fade)); // WindowBg
        ImGuiMCP::PushStyleColor(10, ImGuiMCP::ImVec4(0.08f, 0.09f, 0.13f, 1.0f * fade)); // TitleBg
        ImGuiMCP::PushStyleColor(11, ImGuiMCP::ImVec4(0.13f, 0.21f, 0.38f, 1.0f * fade)); // TitleBgActive
        ImGuiMCP::PushStyleColor(5,  ImGuiMCP::ImVec4(0.22f, 0.38f, 0.65f, 1.0f * fade)); // Border
        ImGuiMCP::PushStyleVar(10, 6.0f); // WindowRounding
        ImGuiMCP::PushStyleVar(13, 4.0f); // FrameRounding

        if (auto* io = ImGuiMCP::GetIO()) {
            // Centered in the middle of the screen
            ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f),
                ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2(0.5f, 0.5f));
        }

        int flags = ImGuiMCP::ImGuiWindowFlags_NoCollapse |
                    ImGuiMCP::ImGuiWindowFlags_NoDecoration |
                    ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiMCP::ImGuiWindowFlags_NoInputs |
                    ImGuiMCP::ImGuiWindowFlags_NoScrollbar |
                    ImGuiMCP::ImGuiWindowFlags_NoSavedSettings;

        const std::string notificationTitle = TrLabel("notification.window_title", "Risa's Notification", "###RisaMenuNotification");
        if (ImGuiMCP::Begin(notificationTitle.c_str(), nullptr, flags)) {
            const float baseUiScale = g_LauncherFontScale.load();
            const float uiScale = LanguageFontScale(baseUiScale * g_LauncherTextScale.load());
            const float layoutScale = baseUiScale / 0.9f;
            ImGuiMCP::SetWindowFontScale(uiScale);

            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.0f, 0.5f));
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding,
                ImGuiMCP::ImVec2(12.0f * layoutScale, 6.0f * layoutScale));
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing,
                ImGuiMCP::ImVec2(8.0f * layoutScale, 8.0f * layoutScale));

            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4(0.90f, 0.90f, 0.90f, 1.0f * fade));
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Separator, ImGuiMCP::ImVec4(0.40f, 0.52f, 0.78f, 1.0f * fade));

            // Mod name header so players know which mod this notification is from.
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.36f, 0.58f, 0.86f, 1.0f * fade),
                "%s", Tr("menu.mod_name", "Risa's All In One Menu"));
            ImGuiMCP::Separator();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f * fade),
                "%s", Tr("notification.hotkeys_updated", "Mod hotkey configuration updated"));
            ImGuiMCP::Text("%s", Tr("notification.restart_required", "Restart Skyrim for the changes to take effect."));

            ImGuiMCP::PopStyleColor(2);
            ImGuiMCP::PopStyleVar(3);
        }
        ImGuiMCP::End();
        ImGuiMCP::PopStyleVar(2);
        ImGuiMCP::PopStyleColor(4);

        if (NowMs() >= g_LanguageComboActiveUntilMs.load()) {
            if (auto* w = ImGuiMCP::FindWindowByName("###RisaMenuNotification"))
                ImGuiMCP::BringWindowToDisplayFront(w);
        }

        return;
    }
    if (const auto amHeal = g_ActiveMenu.load();
        amHeal != ActiveMenu::None && g_LauncherWindow && g_LauncherWindow->IsOpen.load() && !MenuIgnoresOSCursor(amHeal) && !IsCursorShowing()) {
        g_ActiveMenu.store(ActiveMenu::None);
        SKSE::log::info("RenderLauncher: Self-healed active menu state to None (cursor hidden).");
    }


    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {


        if (!g_ConfigRestartRequired.load() && ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
            CloseLauncher(true); // external ESC close — arm the next F1 for a fresh open
            SKSE::log::info("ImGui input: launcher closed by ESC.");
            return;
        }
    }

    // Regular launcher is opaque; the temporary restart notice is translucent.
    ImGuiMCP::SetNextWindowBgAlpha(1.0f);
    ImGuiMCP::PushStyleColor(2,  ImGuiMCP::ImVec4(0.10f, 0.11f, 0.16f, 1.0f)); // WindowBg
    ImGuiMCP::PushStyleColor(10, ImGuiMCP::ImVec4(0.08f, 0.09f, 0.13f, 1.0f)); // TitleBg
    ImGuiMCP::PushStyleColor(11, ImGuiMCP::ImVec4(0.13f, 0.21f, 0.38f, 1.0f)); // TitleBgActive
    ImGuiMCP::PushStyleColor(5,  ImGuiMCP::ImVec4(0.22f, 0.38f, 0.65f, 1.0f)); // Border
    ImGuiMCP::PushStyleVar(10, 6.0f); // WindowRounding
    ImGuiMCP::PushStyleVar(13, 4.0f); // FrameRounding

    // Per-tab window behavior:
    //   Launcher tab — auto-sizes to its buttons (as before).
    //   Settings tab — can get long, so it's manually resizable VERTICALLY only (width is
    //   locked via equal min/max width) and shows a vertical scrollbar. The active tab is
    //   known one frame late, which is fine.
    static bool  s_prevSettingsActive = false;
    static float s_baseMenuWidth = 0.0f;         // unscaled width captured on the Launcher tab

    // Hold the restart-required takeover while the exclusions confirmation popup is open, so it can't
    // replace the window mid-process (the popup's final step already prompts for the restart).
    const bool settingsActive = !restartPrompt && (g_SettingsActive || g_ExclusionsActive);
    const bool justSwitchedToSettings = settingsActive && !s_prevSettingsActive;
    s_prevSettingsActive = settingsActive;

    int kFlags = ImGuiMCP::ImGuiWindowFlags_NoCollapse;
    if (restartPrompt) {
        kFlags |= ImGuiMCP::ImGuiWindowFlags_NoDecoration |
                  ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiMCP::ImGuiWindowFlags_NoInputs |
                  ImGuiMCP::ImGuiWindowFlags_NoScrollbar;
    } else if (settingsActive) {
        const float layoutScaleVal = g_LauncherFontScale.load() / 0.9f;
        float w = (s_baseMenuWidth > 50.0f) ? (s_baseMenuWidth * layoutScaleVal) : (612.0f * layoutScaleVal);
        if (w < 612.0f * layoutScaleVal) w = 612.0f * layoutScaleVal;

        float maxH = 800.0f;
        if (auto* io = ImGuiMCP::GetIO()) maxH = io->DisplaySize.y * 0.92f;

        float minH = 520.0f * layoutScaleVal;
        if (minH > maxH) minH = maxH;
        if (minH < 220.0f) minH = 220.0f;

        // min.x == max.x locks the width, so dragging only resizes the height (bottom edge).
        ImGuiMCP::SetNextWindowSizeConstraints(ImGuiMCP::ImVec2(w, minH), ImGuiMCP::ImVec2(w, maxH));
        kFlags |= ImGuiMCP::ImGuiWindowFlags_NoScrollbar; // content scrolls in a child so the tab bar stays pinned
        if (justSwitchedToSettings) {
            float h = g_SettingsWindowHeight.load();
            if (h < minH) h = minH;
            if (h > maxH) h = maxH;
            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(w, h), ImGuiMCP::ImGuiCond_Always);
        }
    } else {
        kFlags |= ImGuiMCP::ImGuiWindowFlags_NoResize |
                  ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
                  ImGuiMCP::ImGuiWindowFlags_NoScrollbar;
    }

    if (restartPrompt) {
        if (auto* io = ImGuiMCP::GetIO()) {
            // Middle-left: left edge, vertically centered (pivot 0,0.5) so it clears the ENB overlay.
            ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(24.0f, io->DisplaySize.y * 0.5f),
                ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2(0.0f, 0.5f));
        }
    } else if (g_WindowPosX.load() != -1.0f && g_WindowPosY.load() != -1.0f) {
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(g_WindowPosX.load(), g_WindowPosY.load()), ImGuiMCP::ImGuiCond_FirstUseEver, ImGuiMCP::ImVec2(0.0f, 0.0f));
    } else if (auto* io = ImGuiMCP::GetIO()) {
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f), ImGuiMCP::ImGuiCond_FirstUseEver, ImGuiMCP::ImVec2(0.5f, 0.5f));
    }

    bool open = true;
    // Title reflects the current view so players know which mod a sub-hub belongs to. The "###" keeps
    // a STABLE window id ("RisaMenuLauncher") regardless of the visible label, so position/size persist.
    const int titleSub = restartPrompt ? 0 : g_LauncherSubView.load();
    const char* titleText = (titleSub == 1 && g_CSConfig.enabled)        ? Tr("mod.community_shaders", "Community Shaders")
                          : (titleSub == 2 && g_PartySheetConfig.enabled) ? Tr("mod.skyrim_party_sheet", "Skyrim Party Sheet")
                          :                                                  Tr("menu.launcher_title", "Risa's Menu Launcher");
    const std::string windowTitle = std::string(titleText) + "###RisaMenuLauncher";
    // Center the window title. WindowTitleAlign is read during Begin, so scope the push tightly
    // around it and pop right after (keeps the outer style-var bookkeeping unchanged).
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowTitleAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));
    const bool beginOpen = ImGuiMCP::Begin(windowTitle.c_str(), restartPrompt ? nullptr : &open, kFlags);
    ImGuiMCP::PopStyleVar(1);
    if (!beginOpen) {
        ImGuiMCP::PopStyleVar(2); ImGuiMCP::PopStyleColor(4); ImGuiMCP::End(); return;
    }

    if (!restartPrompt) {
        ImGuiMCP::ImVec2 mousePos;
        ImGuiMCP::ImVec2 windowPos;
        ImGuiMCP::GetMousePos(&mousePos);
        ImGuiMCP::GetWindowPos(&windowPos);
        ImGuiMCP::ImVec2 windowSize; ImGuiMCP::GetWindowSize(&windowSize);
        const float frameHeight = ImGuiMCP::GetFrameHeight();
        const float layoutScale = g_LauncherFontScale.load() / 0.9f;

        // Draw the info circle icon (\uf05a) at the top-left of the title bar. Use the DEFAULT merged
        // font (not PushSolid) - the solid-only font doesn't carry this glyph, so it drew invisibly.
        auto* iconFont = ImGuiMCP::GetFont();
        const float fontSize = ImGuiMCP::GetFontSize();
        ImGuiMCP::ImVec2 iconSize;
        ImGuiMCP::CalcTextSize(&iconSize, FontAwesome::UnicodeToUtf8(0xf05a).c_str(), nullptr, false, -1.0f);
        const float iconX = windowPos.x + 12.0f * layoutScale;
        const float iconY = windowPos.y + (frameHeight - iconSize.y) * 0.5f;
        auto* drawList = ImGuiMCP::GetForegroundDrawList(); // foreground so it's never hidden behind the window

        // Clickable region over the icon (left corner of the title bar).
        const bool overHelp = ImGuiMCP::IsWindowHovered(0) &&
                              mousePos.x >= windowPos.x && mousePos.x <= windowPos.x + 40.0f * layoutScale &&
                              mousePos.y >= windowPos.y && mousePos.y <= windowPos.y + frameHeight;
        const auto iconColor = overHelp ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 210, 255);
        ImGuiMCP::ImDrawListManager::AddText(drawList, iconFont, fontSize, ImGuiMCP::ImVec2(iconX, iconY), iconColor, FontAwesome::UnicodeToUtf8(0xf05a).c_str());

        if (overHelp && ImGuiMCP::IsMouseClicked(0, false)) {
            ImGuiMCP::OpenPopup("RisaHelpPopup");
        }
        ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(460.0f * layoutScale, 0.0f));
        // Pin the help popup to the launcher's RIGHT edge (outside it) so it can't hide behind the window.
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(windowPos.x + windowSize.x - 1.0f, windowPos.y),
            ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2(0.0f, 0.0f));
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_PopupBg, ImGuiMCP::ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
        if (ImGuiMCP::BeginPopup("RisaHelpPopup")) {
            ImGuiMCP::PushTextWrapPos(440.0f * layoutScale);
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 0.62f, 1.00f, 1.0f), "%s", Tr("help.title", "RISA'S ALL IN ONE MENU"));
            ImGuiMCP::Separator();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f), "%s", Tr("help.dragging.title", "Window"));
            ImGuiMCP::Text("%s", Tr("help.dragging.move", "- Drag the title bar to move the launcher."));
            ImGuiMCP::Text("%s", Tr("help.dragging.resize", "- Resize Settings and Exclusions from the bottom edge."));
            ImGuiMCP::Spacing();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f), "%s", Tr("help.remap.title", "Hotkeys"));
            ImGuiMCP::Text("%s", Tr("help.remap.intro", "Risa keeps mod hotkeys from fighting each other. Managed mods may use a parked internal key while Risa's selected key opens the mod through its normal input or API path."));
            ImGuiMCP::Spacing();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f), "%s", Tr("help.advanced.title", "Advanced bindings"));
            ImGuiMCP::Text("%s", Tr("help.advanced.intro", "Use the keyboard button beside a hotkey to configure modifiers, double-press, hold, or reset."));
            ImGuiMCP::Text("%s", Tr("help.advanced.status", "A green keyboard button means that hotkey has custom advanced settings. Gray means it uses the default single-press behavior."));
            ImGuiMCP::Spacing();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f), "%s", Tr("help.exclusions.title", "Exclusions"));
            ImGuiMCP::Text("%s", Tr("help.exclusions.body", "Excluding a mod stops Risa from managing it and restores its saved native settings."));
            ImGuiMCP::Spacing();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f), "%s", Tr("help.backup.title", "Maintenance"));
            ImGuiMCP::Text("%s", Tr("help.backup.body", "Restore Mod Defaults returns managed settings to the captured pre-Risa configuration."));
            ImGuiMCP::PopTextWrapPos();
            ImGuiMCP::EndPopup();
        }
        ImGuiMCP::PopStyleColor(1);
    }

    // Remember the natural width (Launcher tab) and the user-chosen Settings height. The height
    // is persisted to the ini (debounced) so a resize survives restarts / new games.
    if (settingsActive) {
        const float h = ImGuiMCP::GetWindowHeight();
        static long long s_heightChangedMs = 0;
        const float dh = h - g_SettingsWindowHeight.load();
        if (dh > 1.0f || dh < -1.0f) {
            g_SettingsWindowHeight.store(h);
            s_heightChangedMs = NowMs();
        }
        if (s_heightChangedMs != 0 && NowMs() - s_heightChangedMs > 1000) {
            SaveButtonOrder();
            s_heightChangedMs = 0;
        }
    } else if (!restartPrompt) {
        const float layoutScaleVal = g_LauncherFontScale.load() / 0.9f;
        s_baseMenuWidth = ImGuiMCP::GetWindowWidth() / layoutScaleVal;
    }

    if (!restartPrompt) {
        ImGuiMCP::ImVec2 currentPos;
        ImGuiMCP::GetWindowPos(&currentPos);
        
        static long long lastMoveTime = 0;
        static float lastSavedX = -9999.0f;
        static float lastSavedY = -9999.0f;
        
        if (lastSavedX == -9999.0f) {
            lastSavedX = g_WindowPosX.load();
            lastSavedY = g_WindowPosY.load();
        }
        
        if (currentPos.x != lastSavedX || currentPos.y != lastSavedY) {
            lastMoveTime = NowMs();
            lastSavedX = currentPos.x;
            lastSavedY = currentPos.y;
        }
        
        if (lastMoveTime != 0 && NowMs() - lastMoveTime > 1000) {
            g_WindowPosX.store(currentPos.x);
            g_WindowPosY.store(currentPos.y);
            SaveButtonOrder();
            lastMoveTime = 0;
        }
    }

    if (!open) {
        CloseLauncher();
        ImGuiMCP::PopStyleVar(2); ImGuiMCP::PopStyleColor(4); ImGuiMCP::End(); return;
    }

    const float baseUiScale = g_LauncherFontScale.load();
    const float uiScale = LanguageFontScale(baseUiScale * g_LauncherTextScale.load());
    const float layoutScale = baseUiScale / 0.9f; // 0.9 preserves the launcher's original dimensions
    const ImGuiMCP::ImVec2 launcherButtonSize(280.0f * layoutScale, 38.0f * layoutScale);
    ImGuiMCP::SetWindowFontScale(uiScale);

    const bool hasMF = IsPluginPresent("SKSEMenuFramework");
    const bool hasOAR = g_OARConfig.enabled;
    const bool hasIED = g_IEDConfig.enabled;
    const bool hasENB = g_ENBConfig.enabled;
    const bool hasDebugMenu = g_DebugMenuConfig.enabled;
    const bool hasDMenu = g_DMenuConfig.enabled;
    const bool hasImprovedCamera = g_ImprovedCameraConfig.enabled;
    const bool hasFLICK = g_FLICKConfig.enabled;
    const bool hasKreatE = g_KreatEConfig.enabled;
    // Community Shaders is incompatible with ENB and disables itself when ENB is installed, so
    // hide the CS button entirely when ENB is detected (the button filter below drops inactive ones).
    const bool hasCS = g_CSConfig.enabled && !g_ENBConfig.enabled;
    const bool hasPartySheet = g_PartySheetConfig.enabled;
    const bool hasCatMenu = g_CatMenuConfig.enabled;
    const bool hasDragonborn = g_DragonbornConfig.enabled;
    const bool hasReShade = g_ReShadeConfig.enabled;
    const bool hasMCM = IsMCMAvailable();
    const bool hasSearchUI = IsSearchUIAvailable();
    const bool hasQAR = IsQARInstalled();
    const bool hasModFunctionMenu = g_ModFunctionMenuConfig.enabled;
    const bool hasOPS = g_OPSConfig.enabled || IsOPSInstalled();
    if (hasOPS && !g_OPSConfig.enabled) {
        LoadOPSConfig();
    }

    struct LauncherButton {
        std::string id;
        std::string icon;
        std::string name;
        void (*action)();
        bool active;
    };

    std::vector<LauncherButton> allButtons;
    allButtons.push_back({ "MF", FontAwesome::UnicodeToUtf8(0xf013), Tr("mod.skse_menu_framework", "SKSE Menu Framework"), OpenSKSEMenuFramework, hasMF });
    allButtons.push_back({ "MCM", FontAwesome::UnicodeToUtf8(0xf1de), Tr("mod.mod_configuration_menu", "Mod Configuration Menu"), OpenMCM, hasMCM });
    allButtons.push_back({ "SearchUI", FontAwesome::UnicodeToUtf8(0xf002), Tr("mod.search_ui", "SearchUI"), OpenSearchUI, hasSearchUI });
    allButtons.push_back({ "OAR", FontAwesome::UnicodeToUtf8(0xf144), Tr("mod.open_animation_replacer", "Open Animation Replacer"), OpenAnimationReplacer, hasOAR });
    allButtons.push_back({ "IED", FontAwesome::UnicodeToUtf8(0xf132), Tr("mod.immersive_equipment_displays", "Immersive Equipment Displays"), OpenImmersiveEquipmentDisplays, hasIED });
    allButtons.push_back({ "DebugMenu", FontAwesome::UnicodeToUtf8(0xf188), Tr("mod.debug_menu", "Debug Menu"), OpenDebugMenu, hasDebugMenu });
    allButtons.push_back({ "dMenu", FontAwesome::UnicodeToUtf8(0xf520), Tr("mod.dmenu", "dMenu"), OpenDMenu, hasDMenu });
    allButtons.push_back({ "ImprovedCamera", FontAwesome::UnicodeToUtf8(0xf030), Tr("mod.improved_camera", "Improved Camera SE"), OpenImprovedCamera, hasImprovedCamera });
    allButtons.push_back({ "ENB", FontAwesome::UnicodeToUtf8(0xf53f), Tr("mod.enb_editor", "ENB Editor"), OpenENB, hasENB });
    allButtons.push_back({ "FLICK", FontAwesome::UnicodeToUtf8(0xf1b3), Tr("mod.flick", "FLICK"), OpenFLICK, hasFLICK });
    allButtons.push_back({ "KreatE", FontAwesome::UnicodeToUtf8(0xf6c3), Tr("mod.kreate", "KreatE"), OpenKreatE, hasKreatE });
    allButtons.push_back({ "CS", FontAwesome::UnicodeToUtf8(0xf043), Tr("mod.community_shaders", "Community Shaders"), EnterCSHub, hasCS });
    allButtons.push_back({ "PartySheet", FontAwesome::UnicodeToUtf8(0xf0c0), Tr("mod.skyrim_party_sheet", "Skyrim Party Sheet"), EnterPartySheetHub, hasPartySheet });
    allButtons.push_back({ "CatMenu", FontAwesome::UnicodeToUtf8(0xf6be), Tr("mod.cat_menu", "CatMenu"), OpenCatMenu, hasCatMenu });
    allButtons.push_back({ "Dragonborn", FontAwesome::UnicodeToUtf8(0xf6d5), Tr("mod.dragonborn_toolkit", "Dragonborn's Toolkit"), OpenDragonbornToolkit, hasDragonborn });
    allButtons.push_back({ "ReShade", FontAwesome::UnicodeToUtf8(0xf5aa), Tr("mod.reshade", "ReShade"), OpenReShade, hasReShade });
    allButtons.push_back({ "QAR", FontAwesome::UnicodeToUtf8(0xf553), Tr("mod.quick_armor_rebalance", "Quick Armor Rebalance"), OpenQAR, hasQAR });
    allButtons.push_back({ "ModFunctionMenu", FontAwesome::UnicodeToUtf8(0xf0ae), Tr("mod.mod_function_menu", "Mod Function Menu"), OpenModFunctionMenu, hasModFunctionMenu });
    allButtons.push_back({ "OPS", FontAwesome::UnicodeToUtf8(0xf508), Tr("mod.outfit_preview_selector", "Outfit Preview Selector"), OpenOPS, hasOPS });

    // Filter to active buttons sorted by g_ButtonOrder
    std::vector<LauncherButton> buttons;
    const bool hideExcluded = g_HideExcludedInLauncher.load();
    for (const auto& id : g_ButtonOrder) {
        for (const auto& btn : allButtons) {
            if (btn.id == id && btn.active) {
                if (hideExcluded) {
                    int aliasIdx = -1;
                    for (int j = 0; j < AI_COUNT; ++j) {
                        if (g_AliasIds[j] == btn.id) {
                            aliasIdx = j;
                            break;
                        }
                    }
                    if (aliasIdx >= 0 && g_ExcludeMod[aliasIdx].load()) {
                        continue; // Skip rendering this button in the grid
                    }
                }
                buttons.push_back(btn);
                break;
            }
        }
    }

    ImGuiMCP::PushStyleColor(21, ImGuiMCP::ImVec4(0.14f, 0.28f, 0.52f, 1.0f));
    ImGuiMCP::PushStyleColor(22, ImGuiMCP::ImVec4(0.22f, 0.42f, 0.74f, 1.0f));
    ImGuiMCP::PushStyleColor(23, ImGuiMCP::ImVec4(0.09f, 0.18f, 0.36f, 1.0f));

    // Align button text/icons to the left, and set a clean 12px frame padding to prevent border bleed
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.0f, 0.5f));
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding,
        ImGuiMCP::ImVec2(12.0f * layoutScale, 6.0f * layoutScale));
    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing,
        ImGuiMCP::ImVec2(8.0f * layoutScale, 8.0f * layoutScale));

    if (restartPrompt) {
        // Mod name header so players know which mod this notification is from.
        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.36f, 0.58f, 0.86f, 1.0f),
            "%s", Tr("menu.mod_name", "Risa's All In One Menu"));
        ImGuiMCP::Separator();
        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f),
            "%s", Tr("notification.hotkeys_updated", "Mod hotkey configuration updated"));
        ImGuiMCP::Text("%s", Tr("notification.restart_required", "Restart Skyrim for the changes to take effect."));

        ImGuiMCP::PopStyleVar(3);
        ImGuiMCP::PopStyleColor(3);
        ImGuiMCP::End();
        ImGuiMCP::PopStyleVar(2);
        ImGuiMCP::PopStyleColor(4);
        if (NowMs() >= g_LanguageComboActiveUntilMs.load()) {
            if (auto* w = ImGuiMCP::FindWindowByName("###RisaMenuLauncher"))
                ImGuiMCP::BringWindowToDisplayFront(w);
        }
        return;
    }

    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Tab, ImGuiMCP::ImVec4(0.12f, 0.16f, 0.22f, 1.0f));
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_TabHovered, ImGuiMCP::ImVec4(0.18f, 0.45f, 0.45f, 1.0f));
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_TabActive, ImGuiMCP::ImVec4(0.16f, 0.34f, 0.58f, 1.0f));
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_TabUnfocused, ImGuiMCP::ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_TabUnfocusedActive, ImGuiMCP::ImVec4(0.13f, 0.24f, 0.38f, 1.0f));
    if (ImGuiMCP::BeginTabBar("LauncherTabs", 0)) {
        if (ImGuiMCP::BeginTabItem((FontAwesome::UnicodeToUtf8(0xf0e4) + "  " + Tr("tab.launcher", "Launcher")).c_str(), nullptr, 0)) {
            const int subView = g_LauncherSubView.load();
            if (subView == 1 && hasCS) {
                // Community Shaders hub. Full grid-width buttons so the window keeps its size,
                // centered labels, and a distinct (amber) Back button.
                const float fullW = launcherButtonSize.x * 2.0f + 8.0f * layoutScale;
                const ImGuiMCP::ImVec2 subBtn(fullW, launcherButtonSize.y);
                ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));

                ImGuiMCP::PushStyleColor(21, ImGuiMCP::ImVec4(0.45f, 0.33f, 0.12f, 1.0f));
                ImGuiMCP::PushStyleColor(22, ImGuiMCP::ImVec4(0.60f, 0.45f, 0.18f, 1.0f));
                ImGuiMCP::PushStyleColor(23, ImGuiMCP::ImVec4(0.34f, 0.24f, 0.08f, 1.0f));
                if (ButtonFit(TrLabel("button.back", "<  Back", "##cssub").c_str(), subBtn, uiScale)) g_LauncherSubView.store(0);
                ImGuiMCP::PopStyleColor(3);

                if (ButtonFit(TrLabel("button.main_menu", "Main Menu", "##cssub").c_str(), subBtn, uiScale)) OpenCommunityShaders();
                if (ButtonFit(TrLabel("button.editor_toggle", "Editor Toggle", "##cssub").c_str(), subBtn, uiScale)) OpenCSEditor();
                if (ButtonFit(TrLabel("button.overlay_toggle", "Overlay Toggle", "##cssub").c_str(), subBtn, uiScale)) OpenCSOverlay();
                if (ButtonFit(TrLabel("button.effect_toggle", "Effect Toggle", "##cssub").c_str(), subBtn, uiScale)) OpenCSEffect();
                ImGuiMCP::PopStyleVar(1);

                // Pad the height so the window stays about the same size as the button grid.
                const float rowH = launcherButtonSize.y + 8.0f * layoutScale;
                const float pad = static_cast<float>((buttons.size() + 1) / 2) * rowH - 5.0f * rowH;
                if (pad > 0.0f) ImGuiMCP::Dummy(ImGuiMCP::ImVec2(1.0f, pad));
            }
            if (subView == 2 && hasPartySheet) {
                const float fullW = launcherButtonSize.x * 2.0f + 8.0f * layoutScale;
                const ImGuiMCP::ImVec2 subBtn(fullW, launcherButtonSize.y);
                ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));

                ImGuiMCP::PushStyleColor(21, ImGuiMCP::ImVec4(0.45f, 0.33f, 0.12f, 1.0f));
                ImGuiMCP::PushStyleColor(22, ImGuiMCP::ImVec4(0.60f, 0.45f, 0.18f, 1.0f));
                ImGuiMCP::PushStyleColor(23, ImGuiMCP::ImVec4(0.34f, 0.24f, 0.08f, 1.0f));
                if (ButtonFit(TrLabel("button.back", "<  Back", "##partysub").c_str(), subBtn, uiScale)) g_LauncherSubView.store(0);
                ImGuiMCP::PopStyleColor(3);

                if (ButtonFit(TrLabel("button.settings", "Settings", "##partysub").c_str(), subBtn, uiScale)) OpenPartySettings();
                if (ButtonFit(TrLabel("button.party_sheet", "Party Sheet", "##partysub").c_str(), subBtn, uiScale)) OpenPartySheet();
                if (ButtonFit(TrLabel("button.inspect_card", "Inspect Card", "##partysub").c_str(), subBtn, uiScale)) OpenPartyInspect();
                if (ButtonFit(TrLabel("button.character_sheet", "Character Sheet", "##partysub").c_str(), subBtn, uiScale)) OpenPartyCharacter();
                ImGuiMCP::PopStyleVar(1);

                const float rowH = launcherButtonSize.y + 8.0f * layoutScale;
                const float pad = static_cast<float>((buttons.size() + 1) / 2) * rowH - 5.0f * rowH;
                if (pad > 0.0f) ImGuiMCP::Dummy(ImGuiMCP::ImVec2(1.0f, pad));
            }
            if (subView == 0 && !buttons.empty()) {
                size_t N = buttons.size();
                size_t leftCount = (N + 1) / 2;
                size_t rightCount = N - leftCount;

                auto DrawButtonContents = [&](const LauncherButton& button) {
                    ImGuiMCP::ImVec2 itemMin;
                    ImGuiMCP::ImVec2 itemMax;
                    ImGuiMCP::GetItemRectMin(&itemMin);
                    ImGuiMCP::GetItemRectMax(&itemMax);

                    const float fontSize = ImGuiMCP::GetFontSize();
                    auto* textFont = ImGuiMCP::GetFont();
                    ImGuiMCP::ImVec2 nameSize;
                    ImGuiMCP::CalcTextSize(&nameSize, button.name.c_str(), nullptr, false, -1.0f);

                    FontAwesome::PushSolid();
                    auto* iconFont = ImGuiMCP::GetFont();
                    ImGuiMCP::ImVec2 iconSize;
                    ImGuiMCP::CalcTextSize(&iconSize, button.icon.c_str(), nullptr, false, -1.0f);
                    FontAwesome::Pop();

                    const float left = itemMin.x + 12.0f * layoutScale;
                    const float iconSlotWidth = 24.0f * layoutScale;
                    const float textLeft = left + iconSlotWidth + 8.0f * layoutScale;
                    const float textAvailW = itemMax.x - textLeft - 10.0f * layoutScale;
                    float textFit = 1.0f;
                    if (textAvailW > 12.0f && nameSize.x > textAvailW) {
                        textFit = textAvailW / nameSize.x;
                        if (textFit < 0.72f) textFit = 0.72f;
                    }
                    const float centerY = (itemMin.y + itemMax.y) * 0.5f;
                    const auto color = ImGuiMCP::GetColorU32(static_cast<ImGuiMCP::ImGuiCol>(0));
                    auto* drawList = ImGuiMCP::GetWindowDrawList();
                    ImGuiMCP::ImDrawListManager::AddText(drawList, iconFont, fontSize,
                        ImGuiMCP::ImVec2(left + (iconSlotWidth - iconSize.x) * 0.5f, centerY - iconSize.y * 0.5f),
                        color, button.icon.c_str());
                    ImGuiMCP::ImDrawListManager::AddText(drawList, textFont, fontSize * textFit,
                        ImGuiMCP::ImVec2(textLeft, centerY - (nameSize.y * textFit) * 0.5f),
                        color, button.name.c_str());
                };

                const bool consoleOpen = IsConsoleOpen();
                auto DrawButton = [&](size_t i) {
                    bool disabled = (buttons[i].id == "MCM" ||
                                     buttons[i].id == "SearchUI" ||
                                     buttons[i].id == "DebugMenu" ||
                                     buttons[i].id == "IED" ||
                                     buttons[i].id == "ENB" ||
                                     buttons[i].id == "QAR" ||
                                     buttons[i].id == "ModFunctionMenu" ||
                                     buttons[i].id == "OPS" ||
                                     buttons[i].id == "PartySheet") && !IsGameLoaded();
                    // These menus can't be driven while the console is open, so grey them out then.
                    if (consoleOpen && (buttons[i].id == "MCM" || buttons[i].id == "IED" || buttons[i].id == "DebugMenu" ||
                                        buttons[i].id == "ImprovedCamera" || buttons[i].id == "KreatE" ||
                                        buttons[i].id == "PartySheet" || buttons[i].id == "CatMenu" ||
                                        buttons[i].id == "QAR" || buttons[i].id == "ModFunctionMenu" ||
                                        buttons[i].id == "OPS" || buttons[i].id == "Dragonborn")) {
                        disabled = true;
                    }

                    if (disabled) {
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_Alpha, 0.4f);
                    }

                    bool isBeingDragged = false;
                    if (const auto* payload = ImGuiMCP::GetDragDropPayload()) {
                        if (strcmp(payload->DataType, "BUTTON_ORDER") == 0) {
                            std::string draggedId = (const char*)payload->Data;
                            if (draggedId == buttons[i].id) {
                                isBeingDragged = true;
                            }
                        }
                    }

                    if (isBeingDragged) {
                        // Completely hide the dragged button slot (make it invisible)
                        ImGuiMCP::PushStyleColor(0, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Text
                        ImGuiMCP::PushStyleColor(21, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Button Bg
                        ImGuiMCP::PushStyleColor(22, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Button Hovered Bg
                        ImGuiMCP::PushStyleColor(23, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Button Active Bg
                        ImGuiMCP::PushStyleColor(5,  ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Border Color
                    }

                    const std::string buttonId = "##LauncherButton_" + buttons[i].id;
                    if (ImGuiMCP::Button(buttonId.c_str(), launcherButtonSize)) {
                        if (!disabled && !isBeingDragged) {
                            buttons[i].action();
                        }
                    }
                    DrawButtonContents(buttons[i]);

                    if (isBeingDragged) {
                        ImGuiMCP::PopStyleColor(5);
                    }

                    // Interactive reordering checks (run immediately after button submission)
                    if (const auto* payload = ImGuiMCP::GetDragDropPayload()) {
                        if (strcmp(payload->DataType, "BUTTON_ORDER") == 0) {
                            std::string draggedId = (const char*)payload->Data;
                            if (draggedId != buttons[i].id) {
                                // Check hover during drag by passing AllowWhenBlockedByActiveItem flag
                                if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                                    auto srcIt = std::find(g_ButtonOrder.begin(), g_ButtonOrder.end(), draggedId);
                                    auto dstIt = std::find(g_ButtonOrder.begin(), g_ButtonOrder.end(), buttons[i].id);
                                    if (srcIt != g_ButtonOrder.end() && dstIt != g_ButtonOrder.end()) {
                                        bool insertAfter = (srcIt < dstIt);
                                        g_ButtonOrder.erase(srcIt);
                                        dstIt = std::find(g_ButtonOrder.begin(), g_ButtonOrder.end(), buttons[i].id);
                                        if (insertAfter) {
                                            g_ButtonOrder.insert(dstIt + 1, draggedId);
                                        } else {
                                            g_ButtonOrder.insert(dstIt, draggedId);
                                        }
                                        SaveButtonOrder();
                                    }
                                }
                            }
                        }
                    }

                    // Drag source to allow user to hold and move the button
                    static ImGuiMCP::ImVec2 dragOffset(0.0f, 0.0f);
                    if (ImGuiMCP::IsItemHovered() && ImGuiMCP::IsMouseClicked(0)) {
                        ImGuiMCP::ImVec2 mousePos;
                        ImGuiMCP::GetMousePos(&mousePos);
                        ImGuiMCP::ImVec2 itemMin;
                        ImGuiMCP::GetItemRectMin(&itemMin);
                        dragOffset.x = mousePos.x - itemMin.x;
                        dragOffset.y = mousePos.y - itemMin.y;
                    }

                    if (ImGuiMCP::BeginDragDropSource(ImGuiMCP::ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                        ImGuiMCP::SetDragDropPayload("BUTTON_ORDER", buttons[i].id.c_str(), buttons[i].id.size() + 1);

                        ImGuiMCP::ImVec2 mousePos;
                        ImGuiMCP::GetMousePos(&mousePos);
                        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(mousePos.x - dragOffset.x, mousePos.y - dragOffset.y), ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2(0.0f, 0.0f));

                        // Push window styles before Begin to draw the menu background and border around the dragged widget
                        ImGuiMCP::PushStyleColor(2, ImGuiMCP::ImVec4(0.10f, 0.11f, 0.16f, 1.0f));  // WindowBg
                        ImGuiMCP::PushStyleColor(5, ImGuiMCP::ImVec4(0.22f, 0.38f, 0.65f, 1.0f));  // Border
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowRounding, 6.0f);
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowBorderSize, 1.0f);
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowPadding, ImGuiMCP::ImVec2(0.0f, 0.0f));

                        ImGuiMCP::Begin("##drag_preview", nullptr, ImGuiMCP::ImGuiWindowFlags_NoTitleBar | ImGuiMCP::ImGuiWindowFlags_NoResize | ImGuiMCP::ImGuiWindowFlags_NoMove | ImGuiMCP::ImGuiWindowFlags_NoSavedSettings | ImGuiMCP::ImGuiWindowFlags_Tooltip);
                        ImGuiMCP::SetWindowFontScale(uiScale);

                        // Push button styles
                        ImGuiMCP::PushStyleColor(21, ImGuiMCP::ImVec4(0.14f, 0.28f, 0.52f, 1.0f)); // Button Bg
                        ImGuiMCP::PushStyleColor(22, ImGuiMCP::ImVec4(0.22f, 0.42f, 0.74f, 1.0f)); // Button Hovered Bg
                        ImGuiMCP::PushStyleColor(23, ImGuiMCP::ImVec4(0.09f, 0.18f, 0.36f, 1.0f)); // Button Active Bg
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.0f, 0.5f));
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, ImGuiMCP::ImVec2(12.0f * layoutScale, 6.0f * layoutScale));
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameRounding, 4.0f);

                        const std::string previewId = "##LauncherButtonPreview_" + buttons[i].id;
                        ImGuiMCP::Button(previewId.c_str(), launcherButtonSize);
                        DrawButtonContents(buttons[i]);

                        // Pop button styles
                        ImGuiMCP::PopStyleVar(3);
                        ImGuiMCP::PopStyleColor(3);

                        ImGuiMCP::End();

                        // Pop window styles
                        ImGuiMCP::PopStyleVar(3);
                        ImGuiMCP::PopStyleColor(2);

                        ImGuiMCP::EndDragDropSource();
                    }

                    if (disabled) {
                        ImGuiMCP::PopStyleVar(1);
                    }
                };

                // Draw Left Column
                ImGuiMCP::BeginGroup();
                for (size_t leftIdx = 0; leftIdx < leftCount; ++leftIdx) {
                    DrawButton(leftIdx);
                }
                ImGuiMCP::EndGroup();

                if (rightCount > 0) {
                    ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);

                    // Draw Right Column
                    ImGuiMCP::BeginGroup();
                    for (size_t rightIdx = 0; rightIdx < rightCount; ++rightIdx) {
                        DrawButton(leftCount + rightIdx);
                    }
                    ImGuiMCP::EndGroup();
                }
            }
            ImGuiMCP::EndTabItem();
        }

        const bool settingsTabOpen = ImGuiMCP::BeginTabItem((FontAwesome::UnicodeToUtf8(0xf013) + "  " + Tr("tab.settings", "Settings")).c_str(), nullptr, 0);
        g_SettingsActive = settingsTabOpen; // drives the per-tab resize/scroll behavior next frame
        if (settingsTabOpen) {
            // Scroll the Settings content inside a child window so the tab bar stays pinned at the top.
            float settingsChildH;
            if (settingsActive) {
                ImGuiMCP::ImVec2 avail; ImGuiMCP::GetContentRegionAvail(&avail);
                settingsChildH = avail.y;
            } else {
                settingsChildH = g_SettingsWindowHeight.load() - 70.0f; // 1-frame transition estimate
            }
            if (settingsChildH < 120.0f) settingsChildH = 120.0f;
            ImGuiMCP::BeginChild("##SettingsScroll", ImGuiMCP::ImVec2(0.0f, settingsChildH), 0,
                ImGuiMCP::ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGuiMCP::SetWindowFontScale(uiScale); // a child has its own font scale — match the UI scale

            // Make section dividers clearly visible against the dark window background.
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Separator, ImGuiMCP::ImVec4(0.40f, 0.52f, 0.78f, 1.0f));

            // Bold-ish (larger) blue section header followed by a divider.
            auto SectionHeader = [&](const char* label) {
                ImGuiMCP::SetWindowFontScale(uiScale * 1.20f);
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 0.62f, 1.00f, 1.0f), "%s", label);
                ImGuiMCP::SetWindowFontScale(uiScale);
                ImGuiMCP::Separator();
            };

            auto SmallTooltip = [&](const std::string& text) {
                if (text.empty()) return;
                if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled) && ImGuiMCP::BeginTooltip()) {
                    ImGuiMCP::SetWindowFontScale(uiScale * 0.8f);
                    ImGuiMCP::PushTextWrapPos(280.0f * layoutScale);
                    ImGuiMCP::TextUnformatted(text.c_str());
                    ImGuiMCP::PopTextWrapPos();
                    ImGuiMCP::EndTooltip();
                }
            };

            SectionHeader(Tr("settings.section.menu", "Menu"));
            ImGuiMCP::Text("%s", Tr("settings.language", "Language:"));
            {
                const std::vector<LanguageOption> availableLanguages = GetAvailableLanguageOptions();
                int currentLanguageIndex = 0;
                const std::string currentLanguageCode = SanitizeLanguageCode(g_Language);
                for (int i = 0; i < static_cast<int>(availableLanguages.size()); ++i) {
                    if (currentLanguageCode == availableLanguages[i].code) {
                        currentLanguageIndex = i;
                        break;
                    }
                }

                ImGuiMCP::PushItemWidth(-1.0f);
                if (ImGuiMCP::BeginCombo("##LanguageCombo", availableLanguages[currentLanguageIndex].englishName, 0)) {
                    ImGuiMCP::SetWindowFontScale(uiScale);
                    g_LanguageComboActiveUntilMs.store(NowMs() + 2500);
                    for (int i = 0; i < static_cast<int>(availableLanguages.size()); ++i) {
                        const bool selected = (i == currentLanguageIndex);
                        std::string itemLabel = std::string(availableLanguages[i].englishName) + "##LanguageComboItem" + std::to_string(i);
                        if (ImGuiMCP::Selectable(itemLabel.c_str(), selected, 0, ImGuiMCP::ImVec2(0.0f, 0.0f))) {
                            currentLanguageIndex = i;
                            g_Language = availableLanguages[currentLanguageIndex].code;
                            LoadTranslations();
                            SaveButtonOrder();
                            g_LanguageComboActiveUntilMs.store(NowMs() + 1000);
                        }
                        if (selected) {
                            ImGuiMCP::SetItemDefaultFocus();
                        }
                    }
                    ImGuiMCP::EndCombo();
                }
                if (ImGuiMCP::IsItemClicked()) {
                    g_LanguageComboActiveUntilMs.store(NowMs() + 2500);
                }
                ImGuiMCP::PopItemWidth();
            }
            SmallTooltip(Tr("tooltip.language", "Choose the language JSON Risa should load. Changes apply immediately."));

            ImGuiMCP::Text("%s", Tr("settings.ui_scale", "UI Scale:"));
            float scale = g_LauncherFontScale.load();
            if (ImGuiMCP::SliderFloat("##FontScaleSlider", &scale, 0.6f, 1.4f, "%.2f", 0)) {
                g_LauncherFontScale.store(scale);
                SaveButtonOrder();
            }
            ImGuiMCP::Text("%s", Tr("settings.font_scale", "Font Scale:"));
            float textScale = g_LauncherTextScale.load();
            if (ImGuiMCP::SliderFloat("##TextScaleSlider", &textScale, 0.6f, 1.4f, "%.2f", 0)) {
                g_LauncherTextScale.store(textScale);
                SaveButtonOrder();
            }
            SmallTooltip(Tr("tooltip.font_scale", "Scales only menu text. UI Scale still changes both layout and text."));

            ImGuiMCP::Text("%s", Tr("settings.launcher_toggle_hotkey", "Launcher Toggle Hotkey:"));
            const bool waitingForKey = g_WaitingForHotkeyPress.load();
            std::string hotkeyBtnLabel;
            if (waitingForKey) {
                hotkeyBtnLabel = TrLabel("button.press_any_key", "[ Press any key... ]", "##LauncherHotkeyBtn");
            } else {
                std::string prefix = "";
                if (g_LauncherHotkeyCtrl.load()) prefix += "Ctrl + ";
                if (g_LauncherHotkeyShift.load()) prefix += "Shift + ";
                if (g_LauncherHotkeyAlt.load()) prefix += "Alt + ";
                hotkeyBtnLabel = prefix + NameFromDIK(g_LauncherHotkeyDIK.load()) + "##LauncherHotkeyBtn";
            }
            float settingsControlH = ImGuiMCP::GetFrameHeight() * 1.08f;
            const float minSettingsControlH = 30.0f * layoutScale;
            if (settingsControlH < minSettingsControlH) settingsControlH = minSettingsControlH;
            const float launcherAdvButtonW = settingsControlH * 1.55f;
            const float launcherHotkeyGap = 8.0f * layoutScale;
            float launcherIconScale = uiScale;
            if (launcherIconScale < baseUiScale) launcherIconScale = baseUiScale;
            const ImGuiMCP::ImVec2 launcherHotkeyButtonSize(210.0f * layoutScale, settingsControlH);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.24f, 0.24f, 0.30f, 1.0f));
            if (ButtonFit(hotkeyBtnLabel.c_str(), launcherHotkeyButtonSize, uiScale)) {
                if (!g_WaitingForHotkeyPress.load()) {
                    g_KeyCaptureStartedMs.store(NowMs());
                    g_WaitingForHotkeyPress.store(true);
                }
            }
            ImGuiMCP::PopStyleColor(3);
            ImGuiMCP::SameLine(0.0f, launcherHotkeyGap);
            const bool hasLauncherAdvEdit = g_LauncherHotkeyCtrl.load() || g_LauncherHotkeyShift.load() ||
                                            g_LauncherHotkeyAlt.load() || g_LauncherHotkeyDoubleTap.load() ||
                                            g_LauncherHotkeyHold.load();
            if (hasLauncherAdvEdit) {
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.20f, 0.45f, 0.28f, 1.0f));
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.26f, 0.56f, 0.35f, 1.0f));
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.16f, 0.38f, 0.24f, 1.0f));
            }
            ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.58f));
            ImGuiMCP::SetWindowFontScale(launcherIconScale);
            if (IconButtonCentered("##launcherAdv", FontAwesome::UnicodeToUtf8(0xf11c),
                    ImGuiMCP::ImVec2(launcherAdvButtonW, settingsControlH), 1.5f * layoutScale, 1.0f * layoutScale)) {
                ImGuiMCP::OpenPopup("Launcher Hotkey Options");
            }
            ImGuiMCP::SetWindowFontScale(uiScale);
            ImGuiMCP::PopStyleVar(1);
            if (hasLauncherAdvEdit) ImGuiMCP::PopStyleColor(3);
            ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.trigger_mode", "Trigger mode (single / double / hold) and hold time"));
            ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
            ImGuiMCP::AlignTextToFramePadding();
            ImGuiMCP::TextDisabled("%s", Tr("settings.click_to_change", "(Click to change)"));
            // Launcher trigger-mode popup - slides out from the LEFT (right edge pinned to the launcher's left).
            if (auto* lw = ImGuiMCP::FindWindowByName("###RisaMenuLauncher")) {
                ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(lw->Pos.x + 1.0f, lw->Pos.y),
                    ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2(1.0f, 0.0f));
            }
            if (ImGuiMCP::BeginPopup("Launcher Hotkey Options")) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 0.62f, 1.00f, 1.0f), "%s", Tr("popup.launcher_hotkey_options.title", "Launcher Hotkey Options"));
                ImGuiMCP::Separator();
                ImGuiMCP::Text("%s", Tr("settings.trigger_mode", "Trigger Mode:"));
                int lmode = g_LauncherHotkeyHold.load() ? 2 : (g_LauncherHotkeyDoubleTap.load() ? 1 : 0);
                bool lmChanged = false;
                if (ImGuiMCP::RadioButton(TrLabel("trigger.single_press_default", "Single Press (default)", "##lm").c_str(), lmode == 0)) { lmode = 0; lmChanged = true; }
                if (ImGuiMCP::RadioButton(TrLabel("trigger.double_press", "Double Press (tap twice)", "##lm").c_str(), lmode == 1)) { lmode = 1; lmChanged = true; }
                if (ImGuiMCP::RadioButton(TrLabel("trigger.hold_to_trigger", "Hold to Trigger", "##lm").c_str(), lmode == 2)) { lmode = 2; lmChanged = true; }
                if (lmChanged) {
                    g_LauncherHotkeyDoubleTap.store(lmode == 1);
                    g_LauncherHotkeyHold.store(lmode == 2);
                    SaveButtonOrder();
                }
                if (lmode == 2) {
                    int hms = g_LauncherHoldDurationMs.load();
                    ImGuiMCP::PushItemWidth(-1.0f);
                    if (ImGuiMCP::SliderInt("##launcherHoldMs", &hms, 100, 2000, Tr("slider.hold_ms_format", "Hold %d ms"))) {
                        g_LauncherHoldDurationMs.store(hms);
                        SaveButtonOrder();
                    }
                    ImGuiMCP::PopItemWidth();
                }
                ImGuiMCP::EndPopup();
            }
            if (g_WaitingForHotkeyPress.load()) {
                // Intercept key presses
                for (int vk = 8; vk < 256; ++vk) {
                    if (vk == 1 || vk == 2 || vk == 4) continue; // Skip mouse clicks
                    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                        vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
                        vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) continue; // Skip modifier keys
                    if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
                        WORD dik = DIKFromVK(vk);
                        if (dik != 0) {
                            g_LauncherHotkeyDIK.store(dik);
                            g_LauncherHotkeyVK.store(vk);
                            g_WaitingForHotkeyPress.store(false);
                            g_WaitForLauncherKeyRelease.store(true);
                            g_LastLauncherToggleMs.store(NowMs());
                            SaveButtonOrder();
                            break;
                        }
                    }
                }
            }

            bool ctrlVal = g_LauncherHotkeyCtrl.load();
            bool shiftVal = g_LauncherHotkeyShift.load();
            bool altVal = g_LauncherHotkeyAlt.load();
            bool doubleTapVal = g_LauncherHotkeyDoubleTap.load();
            bool easyCloseVal = g_LauncherHotkeyEasyClose.load();
            bool hotkeyOptChanged = false;

            if (ImGuiMCP::Checkbox("Ctrl", &ctrlVal)) { g_LauncherHotkeyCtrl.store(ctrlVal); hotkeyOptChanged = true; }
            ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
            if (ImGuiMCP::Checkbox("Shift", &shiftVal)) { g_LauncherHotkeyShift.store(shiftVal); hotkeyOptChanged = true; }
            ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
            if (ImGuiMCP::Checkbox("Alt", &altVal)) { g_LauncherHotkeyAlt.store(altVal); hotkeyOptChanged = true; }
            // (Double Tap / Hold moved into the keyboard button's popup next to the hotkey.)

            if (ImGuiMCP::Checkbox(Tr("settings.easy_close", "Easy Close (single key)"), &easyCloseVal)) { g_LauncherHotkeyEasyClose.store(easyCloseVal); hotkeyOptChanged = true; }
            SmallTooltip(Tr("tooltip.easy_close",
                "By default, if modifiers, double-tap, or hold are set to open, they are also required to close. Enabling this makes closing always a single press of the key under all settings."));

            if (hotkeyOptChanged) {
                SaveButtonOrder();
            }

            SectionHeader(Tr("settings.section.hotkey_control", "Hotkey Control"));

            // Each category is a collapsing header with its own table. catTableOpen tracks the
            // table for whichever category is currently being drawn.
            bool catTableOpen = false;
            auto BeginCategory = [&](const char* label) -> bool {
                const bool open = ImGuiMCP::CollapsingHeader(label, ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen);
                catTableOpen = false;
                if (open) {
                    const std::string tblId = std::string("HKTbl_") + label;
                    ImGuiMCP::ImVec2 catAvail; ImGuiMCP::GetContentRegionAvail(&catAvail);
                    float tableW = catAvail.x - g_HotkeyLayoutTableRightMargin.load() * layoutScale;
                    if (tableW < 200.0f * layoutScale) tableW = 200.0f * layoutScale;
                    catTableOpen = ImGuiMCP::BeginTable(tblId.c_str(), 6, ImGuiMCP::ImGuiTableFlags_SizingStretchProp,
                        ImGuiMCP::ImVec2(tableW, 0.0f)); // small right margin off the window edge
                    if (catTableOpen) {
                        // name | expand arrow | key (centered) | spacer | advanced (keyboard) | toggle
                        ImGuiMCP::TableSetupColumn(Tr("table.mod", "Mod"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, g_HotkeyLayoutNameColumnW.load() * layoutScale);
                        ImGuiMCP::TableSetupColumn("##expcol", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, g_HotkeyLayoutExpandColumnW.load() * layoutScale);
                        ImGuiMCP::TableSetupColumn(Tr("table.key", "Key"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, g_HotkeyLayoutKeyColumnW.load() * layoutScale);
                        ImGuiMCP::TableSetupColumn("##sp2", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, g_HotkeyLayoutSpacerWeight.load());
                        ImGuiMCP::TableSetupColumn(Tr("table.advanced_short", "Adv"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, g_HotkeyLayoutAdvColumnW.load() * layoutScale);
                        ImGuiMCP::TableSetupColumn(Tr("table.enabled", "Enabled"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, g_HotkeyLayoutToggleColumnW.load() * layoutScale);
                    }
                }
                return open;
            };
            auto EndCategory = [&]() {
                if (catTableOpen) { ImGuiMCP::EndTable(); catTableOpen = false; }
            };

            auto aliasEq = [&](int a, int b) {
                return g_AliasDik[a].load() == g_AliasDik[b].load() && g_AliasCtrl[a].load() == g_AliasCtrl[b].load() &&
                       g_AliasShift[a].load() == g_AliasShift[b].load() && g_AliasAlt[a].load() == g_AliasAlt[b].load();
            };
            auto aliasIsLauncher = [&](int i) {
                return g_AliasDik[i].load() == g_LauncherHotkeyDIK.load() && g_AliasCtrl[i].load() == g_LauncherHotkeyCtrl.load() &&
                       g_AliasShift[i].load() == g_LauncherHotkeyShift.load() && g_AliasAlt[i].load() == g_LauncherHotkeyAlt.load();
            };
            // Returns a warning string if this alias clashes with the launcher key or another
            // ENABLED mod's alias (free choice is allowed — this only warns).
            auto AliasConflict = [&](int idx) -> std::string {
                if (idx < 0) return "";
                if (aliasIsLauncher(idx)) return Tr("conflict.launcher_hotkey", "Clashes with the launcher hotkey.");
                for (int j = 0; j < AI_COUNT; ++j)
                    if (j != idx && g_AliasUnblock[j]->load() && aliasEq(idx, j))
                        return std::string(Tr("conflict.clashes_with", "Clashes with ")) + g_AliasIds[j] + Tr("conflict.enabled_suffix", " (enabled).");
                return "";
            };

            auto DrawOriginalHotkeyRow = [&](const char* id, const char* modName, const char* defaultHotkey,
                    const std::string& tooltip, const std::string& disabledReason, int aliasIdx,
                    std::atomic<bool>& setting, bool installed, bool originalAvailable,
                    bool* expand, bool isSub, auto simulateAction) {
                if (installed && catTableOpen) {
                    float hotkeyRowH = ImGuiMCP::GetFrameHeight();
                    const float advRowH = settingsControlH * g_HotkeyLayoutAdvButtonHMult.load();
                    if (hotkeyRowH < advRowH) hotkeyRowH = advRowH;
                    float toggleRowH = ImGuiMCP::GetFrameHeight() * g_HotkeyLayoutToggleHeightMult.load();
                    const float minToggleRowH = g_HotkeyLayoutToggleMinH.load() * layoutScale;
                    if (toggleRowH < minToggleRowH) toggleRowH = minToggleRowH;
                    if (hotkeyRowH < toggleRowH) hotkeyRowH = toggleRowH;
                    hotkeyRowH += 3.0f * layoutScale;
                    ImGuiMCP::TableNextRow(0, hotkeyRowH);

                    // Column 0: mod name button. Sub-rows are indented + darker blue + a bit smaller.
                    ImGuiMCP::TableSetColumnIndex(0);
                    if (isSub) {
                        ImGuiMCP::Indent(18.0f * layoutScale);
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.10f, 0.20f, 0.38f, 1.0f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.16f, 0.30f, 0.52f, 1.0f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.07f, 0.14f, 0.28f, 1.0f));
                    }
                    const std::string nameBtnLabel = std::string(modName) + "##ModBtn_" + id;
                    if (ButtonFit(nameBtnLabel.c_str(), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                        // These Settings buttons toggle the mod to re-sync a menu that got out of
                        // state — keep Risa's launcher OPEN (unlike the main grid buttons, which
                        // dismiss it). The mod's Open*() helper calls CloseLauncher() internally, so
                        // suppress that for the duration of this click.
                        g_KeepLauncherOpenForSync.store(true);
                        simulateAction();
                        g_KeepLauncherOpenForSync.store(false);
                    }
                    if (isSub) { ImGuiMCP::PopStyleColor(3); ImGuiMCP::Unindent(18.0f * layoutScale); }

                    // Column 1: expand arrow, for rows that have sub-rows (sits between name and key).
                    if (expand) {
                        ImGuiMCP::TableSetColumnIndex(1);
                        ImGuiMCP::AlignTextToFramePadding();
                        const std::string arrowId = std::string(*expand ? "v" : ">") + "##exp_" + id;
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, ImGuiMCP::ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        if (ImGuiMCP::Button(arrowId.c_str(), ImGuiMCP::ImVec2(0.0f, 0.0f))) *expand = !*expand;
                        ImGuiMCP::PopStyleColor(1);
                    }

                    const bool excluded = (aliasIdx >= 0) && g_ExcludeMod[aliasIdx].load();
                    ImGuiMCP::BeginDisabled(excluded);
                    if (excluded) {
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_FrameBg,        ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_FrameBgHovered, ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_FrameBgActive,  ImGuiMCP::ImVec4(0.04f, 0.04f, 0.05f, 0.8f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_CheckMark,      ImGuiMCP::ImVec4(0.10f, 0.10f, 0.12f, 0.4f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text,           ImGuiMCP::ImVec4(0.35f, 0.35f, 0.38f, 0.7f));
                    }

                    // Column 2 (centered): editable hotkey field.
                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::AlignTextToFramePadding();
                    if (aliasIdx < 0) {
                        ImGuiMCP::Text("%s", defaultHotkey);
                        SmallTooltip(tooltip);
                    } else {
                        const bool capturing = g_CapturingAlias.load() == aliasIdx;
                        std::string lbl;
                        if (capturing) {
                            lbl = TrLabel("button.press_any_key", "[ Press any key... ]", (std::string("##alias") + id).c_str());
                        } else {
                            std::string p;
                            if (g_AliasCtrl[aliasIdx].load()) p += "Ctrl + ";
                            if (g_AliasShift[aliasIdx].load()) p += "Shift + ";
                            if (g_AliasAlt[aliasIdx].load()) p += "Alt + ";
                            lbl = p + NameFromDIK(g_AliasDik[aliasIdx].load()) + "##alias" + id;
                        }
                        // Darker button so the editable key field stands out as clickable.
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
                        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.24f, 0.24f, 0.30f, 1.0f));
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.5f)); // center the key text
                        if (ImGuiMCP::Button(lbl.c_str(), ImGuiMCP::ImVec2(-1.0f, 0.0f))) {
                            if (!capturing) {
                                g_KeyCaptureStartedMs.store(NowMs());
                            }
                            g_CapturingAlias.store(capturing ? -1 : aliasIdx); // click to start/cancel capture
                        }
                        ImGuiMCP::PopStyleVar(1);
                        ImGuiMCP::PopStyleColor(3);
                        SmallTooltip(tooltip); // tooltip always shows the mod's original key
                        if (capturing) {
                            for (int vk = 8; vk < 256; ++vk) {
                                if (vk == 1 || vk == 2 || vk == 4) continue; // mouse
                                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                                    vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
                                    vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) continue; // modifiers
                                if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
                                    const WORD dik = DIKFromVK(static_cast<WORD>(vk));
                                    if (dik != 0) {
                                        g_AliasDik[aliasIdx].store(dik);
                                        g_AliasCtrl[aliasIdx].store((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                                        g_AliasShift[aliasIdx].store((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                                        g_AliasAlt[aliasIdx].store((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                                        // Rebinding no longer auto-enables the key - the user toggles it on.
                                        g_CapturingAlias.store(-1);
                                        g_SuppressAliasUntilMs.store(NowMs() + 1000); // don't let this keypress fire the mod
                                        SaveButtonOrder();
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    // Column 4: Advanced settings button (⌨ icon)
                    ImGuiMCP::TableSetColumnIndex(4);
                    if (aliasIdx >= 0) {
                        const std::string advId = std::string("##adv_") + id;
                        // Tint the button when this key has a non-default advanced setting (any modifier,
                        // or a trigger mode other than Single Press) so an edit is visible at a glance.
                        const bool hasAdvEdit = g_AliasCtrl[aliasIdx].load() || g_AliasShift[aliasIdx].load() ||
                                                g_AliasAlt[aliasIdx].load() || g_AliasTriggerMode[aliasIdx].load() != 0;
                        if (hasAdvEdit) {
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.20f, 0.45f, 0.28f, 1.0f));
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.26f, 0.56f, 0.35f, 1.0f));
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.16f, 0.38f, 0.24f, 1.0f));
                        }
                        const float advButtonH = settingsControlH * g_HotkeyLayoutAdvButtonHMult.load();
                        const float advButtonW = settingsControlH * g_HotkeyLayoutAdvButtonWMult.load();
                        float advIconScale = launcherIconScale;
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.58f));
                        ImGuiMCP::SetWindowFontScale(advIconScale);
                        if (IconButtonCentered(advId.c_str(), FontAwesome::UnicodeToUtf8(0xf11c),
                                ImGuiMCP::ImVec2(advButtonW, advButtonH),
                                g_HotkeyLayoutAdvIconYNudge.load() * layoutScale,
                                g_HotkeyLayoutAdvIconXNudge.load() * layoutScale)) {
                            g_ActiveHotkeyOptionsAlias = aliasIdx;
                            g_OpenHotkeyOptionsPopup.store(true);
                        }
                        ImGuiMCP::SetWindowFontScale(uiScale);
                        ImGuiMCP::PopStyleVar(1);
                        if (hasAdvEdit) ImGuiMCP::PopStyleColor(3);
                        SmallTooltip(hasAdvEdit ? Tr("tooltip.advanced_bindings_edited", "Advanced Bindings (edited: modifiers or trigger mode set)")
                                                : Tr("tooltip.advanced_bindings", "Advanced Bindings (modifiers, double-press, hold, reset)"));
                    }

                    // Column 6 (right edge): the enable toggle — OR, when the key clashes, the
                    // warning marker replaces the toggle entirely.
                    ImGuiMCP::TableSetColumnIndex(5);
                    const std::string conflict = (aliasIdx >= 0) ? AliasConflict(aliasIdx) : std::string();
                    float toggleH = ImGuiMCP::GetFrameHeight() * g_HotkeyLayoutToggleHeightMult.load();
                    const float minToggleH = g_HotkeyLayoutToggleMinH.load() * layoutScale;
                    if (toggleH < minToggleH) toggleH = minToggleH;
                    const float toggleW = toggleH * g_HotkeyLayoutToggleWidthMult.load();
                    const float toggleColumnW = ImGuiMCP::GetColumnWidth(-1);
                    const float toggleRightPad = g_HotkeyLayoutToggleRightPad.load() * layoutScale;
                    const float toggleSlotX = (toggleColumnW > toggleW + toggleRightPad)
                        ? (toggleColumnW - toggleW - toggleRightPad)
                        : 0.0f;
                    if (!conflict.empty()) {
                        const std::string warnIcon = FontAwesome::UnicodeToUtf8(0xf071);
                        ImGuiMCP::ImVec2 warnSize;
                        ImGuiMCP::CalcTextSize(&warnSize, warnIcon.c_str(), nullptr, false, -1.0f);
                        ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + toggleSlotX + (toggleW - warnSize.x) * 0.5f);
                        if (toggleH > warnSize.y) {
                            ImGuiMCP::SetCursorPosY(ImGuiMCP::GetCursorPosY() + (toggleH - warnSize.y) * 0.5f);
                        }
                        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s",
                            warnIcon.c_str());
                        SmallTooltip(conflict);
                    } else {
                        bool val = originalAvailable && setting.load();
                        ImGuiMCP::BeginDisabled(!originalAvailable);
                        const std::string toggleId = "##EnableOriginal_" + std::string(id);
                        ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + toggleSlotX);

                        if (SlideToggle(toggleId.c_str(), &val, originalAvailable, layoutScale,
                                g_HotkeyLayoutToggleHeightMult.load(),
                                g_HotkeyLayoutToggleMinH.load(),
                                g_HotkeyLayoutToggleWidthMult.load()) && originalAvailable) {
                            setting.store(val);
                            SKSE::log::info("Settings: alias {} set to {} ({}, conflict='{}').",
                                id, val, FormatHotkey(g_AliasDik[aliasIdx].load(), g_AliasCtrl[aliasIdx].load(),
                                    g_AliasShift[aliasIdx].load(), g_AliasAlt[aliasIdx].load()),
                                AliasConflict(aliasIdx));
                            SaveButtonOrder();
                        }
                        ImGuiMCP::EndDisabled();
                        if (!originalAvailable) SmallTooltip(disabledReason);
                    }
                    ImGuiMCP::EndDisabled();
                    if (excluded) {
                        ImGuiMCP::PopStyleColor(8);
                    }
                }
            };

            // Show both the supported mod's documented default and the binding Risa actually
            // detected this launch. This makes parked keys such as dMenu's F14 visible at a glance.
            auto MakeTooltip = [](const std::string& defaultHotkey, const std::string& currentHotkey) {
                return std::string(Tr("tooltip.default_key", "Default key: ")) + defaultHotkey + "\n" +
                    Tr("tooltip.current_key", "Current key: ") +
                    (currentHotkey.empty() ? std::string(Tr("hotkey.unknown", "Unknown")) : currentHotkey);
            };

            // "Current key" reflects what physically happened to each mod's native key:
            //   - relocated mods show the unpressable F-key they now sit on (e.g. F18),
            //   - API-driven mods say their key is off and the launcher opens them through the API,
            //   - intercepted mods say so.
            // Excluded mods are left untouched, so show their real key with a note instead.
            auto keyOrExcluded = [](int aliasIdx, WORD dik, const std::string& managedDesc) -> std::string {
                if (aliasIdx >= 0 && g_ExcludeMod[aliasIdx].load())
                    return FormatHotkey(dik) + Tr("hotkey.excluded_left_alone_suffix", " (excluded, left alone)");
                return managedDesc;
            };
            const std::string kViaApi = Tr("hotkey.via_api", "Run through the mod's API (native key off)");
            auto movedKey = [&](int idx, WORD dik) { return keyOrExcluded(idx, dik, NameFromDIK(dik)); };
            auto apiKey   = [&](int idx, WORD dik) { return keyOrExcluded(idx, dik, kViaApi); };

            const std::string oarTooltip = MakeTooltip("SHIFT + O", movedKey(AI_OAR, g_OARConfig.toggleDIK));
            const std::string iedTooltip = MakeTooltip("BACKSPACE", movedKey(AI_IED, g_IEDConfig.toggleDIK));
            const std::string qarTooltip = MakeTooltip("None",
                FormatHotkey(g_AliasDik[AI_QAR].load(), g_AliasCtrl[AI_QAR].load(), g_AliasShift[AI_QAR].load(), g_AliasAlt[AI_QAR].load()));
            const std::string mfmTooltip = MakeTooltip("F1", movedKey(AI_ModFunctionMenu, g_ModFunctionMenuConfig.toggleDIK));
            // OPS: key freed by disabling its MCM Hotkey property (opened via mod event). Excluded = left alone.
            const std::string opsTooltip = MakeTooltip("Numpad 1",
                "Numpad 1 (kept - opens from the launcher too; its key doesn't clash)");

            std::string enbHotkey;
            if (g_ENBConfig.combinationVK != 0) {
                enbHotkey = FormatHotkey(DIKFromVK(static_cast<WORD>(g_ENBConfig.combinationVK))) + " + ";
            }
            enbHotkey += FormatHotkey(DIKFromVK(static_cast<WORD>(g_ENBConfig.editorVK)));
            const std::string enbTooltip = MakeTooltip("SHIFT + ENTER", enbHotkey);

            const std::string debugMenuTooltip = MakeTooltip("F1", movedKey(AI_DebugMenu, g_DebugMenuConfig.toggleDIK));
            const std::string dMenuTooltip = MakeTooltip("HOME",
                HasDMenuV2Api() ? apiKey(AI_DMenu, g_DMenuConfig.toggleDIK)
                                : keyOrExcluded(AI_DMenu, g_DMenuConfig.toggleDIK,
                                    FormatModifiedHotkey(g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK)));
            const std::string icTooltip = MakeTooltip("SHIFT + HOME",
                FormatModifiedHotkey(g_ImprovedCameraConfig.toggleDIK, g_ImprovedCameraConfig.modifierDIK));
            const std::string flickTooltip = MakeTooltip("F7", apiKey(AI_FLICK, g_FLICKConfig.toggleDIK));
            const std::string kreateTooltip = MakeTooltip("END", movedKey(AI_KreatE, g_KreatEConfig.toggleDIK));
            const std::string csTooltip = MakeTooltip("END", movedKey(AI_CS, g_CSConfig.toggleDIK));
            const std::string catMenuTooltip = MakeTooltip("F6",
                keyOrExcluded(AI_CatMenu, g_CatMenuConfig.toggleDIK, Tr("hotkey.opened_directly_intercepted", "Opened directly (native key intercepted)")));
            const std::string dragonbornTooltip = MakeTooltip("F1", apiKey(AI_Dragonborn, g_DragonbornConfig.toggleDIK));
            const std::string reshadeTooltip = MakeTooltip("HOME",
                g_ReShadeAddonActive.load() ? apiKey(AI_ReShade, g_ReShadeConfig.toggleDIK)
                                            : keyOrExcluded(AI_ReShade, g_ReShadeConfig.toggleDIK, FormatHotkey(g_ReShadeConfig.toggleDIK)));
            const std::string mfTooltip = MakeTooltip("F1", apiKey(AI_MF, g_MFConfig.toggleDIK));
            const std::string csEditorTip = MakeTooltip("SHIFT + END", movedKey(AI_CSEditor, g_CSConfig.editorDIK));
            const std::string csOverlayTip = MakeTooltip("F10", movedKey(AI_CSOverlay, g_CSConfig.overlayDIK));
            const std::string csEffectTip = MakeTooltip("NUMPAD *", movedKey(AI_CSEffect, g_CSConfig.effectDIK));
            const std::string partySettingsTip = MakeTooltip("X", FormatHotkey(g_PartySheetConfig.settingsDIK));
            const std::string partySheetTip = MakeTooltip("F6", FormatHotkey(g_PartySheetConfig.partyDIK));
            const std::string partyInspectTip = MakeTooltip("Y", FormatHotkey(g_PartySheetConfig.inspectDIK));
            const std::string partyCharacterTip = MakeTooltip("U", FormatHotkey(g_PartySheetConfig.characterDIK));
            static bool s_csExpanded = false; // Community Shaders sub-key rows expanded?
            static bool s_partySheetExpanded = false;

            // A mod's toggle is only blocked if its (rebindable) alias key equals the launcher
            // hotkey — clicking the key to rebind it elsewhere frees the toggle.
            auto aliasEqualsLauncher = [&](int idx) {
                return g_AliasDik[idx].load() == g_LauncherHotkeyDIK.load() &&
                       g_AliasCtrl[idx].load() == g_LauncherHotkeyCtrl.load() &&
                       g_AliasShift[idx].load() == g_LauncherHotkeyShift.load() &&
                       g_AliasAlt[idx].load() == g_LauncherHotkeyAlt.load();
            };
            const std::string launcherClashReason =
                Tr("tooltip.launcher_clash_reason",
                    "Can't enable: this key is the launcher hotkey.\nClick the key to rebind it to something else.");

            const bool debugOriginalAvailable = !aliasEqualsLauncher(AI_DebugMenu);
            const std::string& debugOriginalTooltip = debugMenuTooltip;
            const std::string& debugDisabledReason = launcherClashReason;

            const bool dragonbornOriginalAvailable = !aliasEqualsLauncher(AI_Dragonborn);
            const std::string& dragonbornOriginalTooltip = dragonbornTooltip;
            const std::string& dragonbornDisabledReason = launcherClashReason;

            const bool anyMenusTools = hasMF || hasDMenu || hasFLICK || hasCatMenu || hasDragonborn || hasDebugMenu || hasSearchUI || hasMCM || hasModFunctionMenu;
            const bool anyAnimGear   = hasOAR || hasIED || hasPartySheet || hasQAR || hasOPS;
            const bool anyGraphics   = hasENB || hasCS || hasImprovedCamera || hasReShade || hasKreatE;

            if (anyMenusTools && BeginCategory(Tr("settings.category.menus_tools", "Menus & Tools"))) {
                const std::string mcmTip =
                    Tr("tooltip.mcm_hotkey", "Sets a hotkey to open the MCM directly. Turn ON to use it, or OFF to free the key for normal gameplay (you can still open MCM using the launcher button).");
                DrawOriginalHotkeyRow("MCM", "MCM", "None", mcmTip, "", AI_MCM, g_UnblockMCM, hasMCM, true, nullptr, false, []() {
                    OpenMCM();
                });
                DrawOriginalHotkeyRow("MF", Tr("mod.skse_menu_framework", "SKSE Menu Framework"), "F1", mfTooltip, "", AI_MF, g_UnblockMF, hasMF, true, nullptr, false, []() {
                    OpenSKSEMenuFramework();
                });
                DrawOriginalHotkeyRow("dMenu", "dMenu", "Home", dMenuTooltip, "", AI_DMenu, g_UnblockDMenu, hasDMenu, true, nullptr, false, []() {
                    OpenDMenu();
                });
                DrawOriginalHotkeyRow("FLICK", "FLICK", "F7", flickTooltip, "", AI_FLICK, g_UnblockFLICK, hasFLICK, true, nullptr, false, []() {
                    OpenFLICK();
                });
                DrawOriginalHotkeyRow("CatMenu", "CatMenu", "F6", catMenuTooltip, "", AI_CatMenu, g_UnblockCatMenu, hasCatMenu, true, nullptr, false, []() {
                    OpenCatMenu();
                });
                DrawOriginalHotkeyRow("Dragonborn", Tr("mod.dragonborn_toolkit", "Dragonborn's Toolkit"), "F1", dragonbornOriginalTooltip, dragonbornDisabledReason,
                    AI_Dragonborn, g_UnblockDragonborn, hasDragonborn, dragonbornOriginalAvailable, nullptr, false, []() {
                    OpenDragonbornToolkit();
                });
                DrawOriginalHotkeyRow("DebugMenu", Tr("mod.debug_menu", "Debug Menu"), "F1", debugOriginalTooltip, debugDisabledReason,
                    AI_DebugMenu, g_UnblockDebugMenu, hasDebugMenu, debugOriginalAvailable, nullptr, false, []() {
                    OpenDebugMenu();
                });
                // ON = the user's key opens SearchUI; OFF = key parked on the unpressable F18 (freed).
                const WORD searchDik = static_cast<WORD>(g_SearchUIEffectiveDIK.load());
                const std::string searchUITip = MakeTooltip("F4",
                    keyOrExcluded(AI_SearchUI, searchDik,
                        g_UnblockSearchUI.load() ? FormatHotkey(searchDik) : NameFromDIK(searchDik)));
                DrawOriginalHotkeyRow("SearchUI", "SearchUI", "F4", searchUITip, "", AI_SearchUI, g_UnblockSearchUI, hasSearchUI, true, nullptr, false, []() {
                    OpenSearchUI();
                });
                DrawOriginalHotkeyRow("ModFunctionMenu", Tr("mod.mod_function_menu", "Mod Function Menu"), "F1", mfmTooltip, "", AI_ModFunctionMenu, g_UnblockModFunctionMenu, hasModFunctionMenu, true, nullptr, false, []() {
                    OpenModFunctionMenu("alias");
                });
            }
            EndCategory();

            if (anyAnimGear && BeginCategory(Tr("settings.category.animation_gear", "Animation & Gear"))) {
                DrawOriginalHotkeyRow("OAR", Tr("mod.open_animation_replacer", "Open Animation Replacer"), "Shift + O", oarTooltip, "", AI_OAR, g_UnblockOAR, hasOAR, true, nullptr, false, []() {
                    OpenAnimationReplacer();
                });
                DrawOriginalHotkeyRow("IED", "IED", "Backspace", iedTooltip, "", AI_IED, g_UnblockIED, hasIED, true, nullptr, false, []() {
                    OpenImmersiveEquipmentDisplays();
                });
                DrawOriginalHotkeyRow("QAR", Tr("mod.quick_armor_rebalance", "Quick Armor Rebalance"), "None", qarTooltip, "", AI_QAR, g_UnblockQAR, hasQAR, true, nullptr, false, []() {
                    OpenQAR();
                });
                DrawOriginalHotkeyRow("OPS", Tr("mod.outfit_preview_selector", "Outfit Preview Selector"), "Numpad 1", opsTooltip, "", AI_OPS, g_UnblockOPS, hasOPS, true, nullptr, false, []() {
                    OpenOPS();
                });
                DrawOriginalHotkeyRow("PartySettings", Tr("mod.skyrim_party_sheet", "Skyrim Party Sheet"), "X", partySettingsTip, "",
                    AI_PartySettings, g_UnblockPartySettings, hasPartySheet, true, &s_partySheetExpanded, false, []() {
                    OpenPartySettings();
                });
                if (s_partySheetExpanded) {
                    DrawOriginalHotkeyRow("PartySheetAction", Tr("button.party_sheet", "Party Sheet"), "F6", partySheetTip, "",
                        AI_PartySheet, g_UnblockPartySheet, hasPartySheet, true, nullptr, true, []() {
                        OpenPartySheet();
                    });
                    DrawOriginalHotkeyRow("PartyInspect", Tr("button.inspect_card", "Inspect Card"), "Y", partyInspectTip, "",
                        AI_PartyInspect, g_UnblockPartyInspect, hasPartySheet, true, nullptr, true, []() {
                        OpenPartyInspect();
                    });
                    DrawOriginalHotkeyRow("PartyCharacter", Tr("button.character_sheet", "Character Sheet"), "U", partyCharacterTip, "",
                        AI_PartyCharacter, g_UnblockPartyCharacter, hasPartySheet, true, nullptr, true, []() {
                        OpenPartyCharacter();
                    });
                }
            }
            EndCategory();

            if (anyGraphics && BeginCategory(Tr("settings.category.visual_lighting", "Visual and Lighting"))) {
                DrawOriginalHotkeyRow("ENB", Tr("mod.enb_editor", "ENB Editor"), "Shift + Enter", enbTooltip, "", AI_ENB, g_UnblockENB, hasENB, true, nullptr, false, []() {
                    OpenENB();
                });
                DrawOriginalHotkeyRow("CS", Tr("mod.community_shaders", "Community Shaders"), "End", csTooltip, "", AI_CS, g_UnblockCS, hasCS, true, &s_csExpanded, false, []() {
                    OpenCommunityShaders();
                });
                if (s_csExpanded) {
                    DrawOriginalHotkeyRow("CSEditor", Tr("button.editor_toggle", "Editor"), "Shift + End", csEditorTip, "", AI_CSEditor, g_UnblockCSEditor, hasCS, true, nullptr, true, []() {
                        OpenCSEditor();
                    });
                    DrawOriginalHotkeyRow("CSOverlay", Tr("button.overlay_toggle", "Overlay"), "F10", csOverlayTip, "", AI_CSOverlay, g_UnblockCSOverlay, hasCS, true, nullptr, true, []() {
                        OpenCSOverlay();
                    });
                    DrawOriginalHotkeyRow("CSEffect", Tr("button.effect_toggle", "Effect"), "Numpad *", csEffectTip, "", AI_CSEffect, g_UnblockCSEffect, hasCS, true, nullptr, true, []() {
                        OpenCSEffect();
                    });
                }
                DrawOriginalHotkeyRow("ImprovedCamera", Tr("mod.improved_camera", "Improved Camera"), "Shift + Home", icTooltip, "",
                    AI_IC, g_UnblockImprovedCamera, hasImprovedCamera, true, nullptr, false, []() {
                    OpenImprovedCamera();
                });
                DrawOriginalHotkeyRow("ReShade", "ReShade", "Home", reshadeTooltip, "", AI_ReShade, g_UnblockReShade, hasReShade, true, nullptr, false, []() {
                    OpenReShade();
                });
                DrawOriginalHotkeyRow("KreatE", "KreatE", "End", kreateTooltip, "", AI_KreatE, g_UnblockKreatE, hasKreatE, true, nullptr, false, []() {
                    OpenKreatE();
                });
            }
            EndCategory();

            SectionHeader(Tr("settings.section.mod_options", "Mod Options"));
            {
                bool remember = g_RememberSubView.load();
                if (ImGuiMCP::Checkbox(Tr("settings.remember_sub_menu", "Remember the open mod sub-menu"), &remember)) {
                    g_RememberSubView.store(remember);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.remember_sub_menu", "When on, a mod's sub-menu (e.g. Community Shaders) stays open after you\nclose and reopen the launcher, until you press Back."));

                bool consoleVal = g_AllowOpenInConsole.load();
                if (ImGuiMCP::Checkbox(Tr("settings.open_in_console", "Can open menu while console is open"), &consoleVal)) {
                    g_AllowOpenInConsole.store(consoleVal);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.open_in_console", "Open the launcher while the console is up. Use the mouse to navigate."));

                bool keepRunningVal = g_KeepGameRunning.load();
                if (ImGuiMCP::Checkbox(Tr("settings.keep_game_running", "Keep the game running while menu is open"), &keepRunningVal)) {
                    g_KeepGameRunning.store(keepRunningVal);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.keep_game_running", "Keep the world moving (animations, fire) instead of pausing."));

                bool showExclusions = g_ShowExclusionsTab.load();
                if (ImGuiMCP::Checkbox(Tr("settings.show_exclusions_tab", "Show Exclusions Tab"), &showExclusions)) {
                    g_ShowExclusionsTab.store(showExclusions);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.show_exclusions_tab", "Displays a dedicated 'Exclusions' tab where you can opt-out specific mods from being managed by this menu."));

                bool hideExcluded = g_HideExcludedInLauncher.load();
                if (ImGuiMCP::Checkbox(Tr("settings.hide_excluded_launcher", "Excluded mods don't appear in launcher menu"), &hideExcluded)) {
                    g_HideExcludedInLauncher.store(hideExcluded);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.hide_excluded_launcher", "When enabled, mods marked as 'Excluded' will be hidden from the launcher's main button grid."));
            }

            SectionHeader(Tr("settings.section.maintenance", "Maintenance"));
            {
                bool logVal = g_LoggingEnabled.load();
                if (ImGuiMCP::Checkbox(Tr("settings.enable_logging", "Enable logging"), &logVal)) {
                    g_LoggingEnabled.store(logVal);
                    ApplyLogLevel();
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.enable_logging", "The full troubleshooting log.\nDocuments\\My Games\\Skyrim Special Edition\\SKSE\\RisaAllInOneMenu.log"));
                ImGuiMCP::SameLine(0.0f, 16.0f * layoutScale);
                bool fileLogVal = g_FileChangeLogEnabled.load();
                if (ImGuiMCP::Checkbox(Tr("settings.log_file_changes", "Log file changes"), &fileLogVal)) {
                    g_FileChangeLogEnabled.store(fileLogVal);
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.log_file_changes", "A separate audit of exactly which config files this mod\nedits (EDIT) or reverts (REVERT) - nothing else.\nDocuments\\My Games\\Skyrim Special Edition\\SKSE\\RisaAllInOneMenu_FileChanges.log"));

                static bool s_confirmRestore = false;
                static bool s_restoreDone = false;
                static bool s_restoreFailed = false;
                static bool s_forceModDefaults = false;
                static int s_scrollToBottomFrames = 0;
                static bool s_exitDismissed = false;
                if (s_restoreDone) {
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 1.00f, 0.55f, 1.0f),
                        s_forceModDefaults ? Tr("restore.defaults_done", "All supported mod settings restored to their defaults.")
                                           : Tr("restore.originals_done", "All mod settings restored to their captured originals."));
                    ImGuiMCP::Text("%s", Tr("restore.backup_removed", "The completed originals backup was removed so a future install starts fresh."));
                    ImGuiMCP::Text("%s", Tr("restore.quit_then_disable", "Quit Skyrim, then disable this mod before the next launch."));
                    if (!s_exitDismissed) {
                        ImGuiMCP::Spacing();
                        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f),
                            "%s", Tr("restore.exit_prompt", "Do you want to exit the game now?"));
                        if (ButtonFit(Tr("restore.yes_quit_now", "Yes, quit to desktop now"), ImGuiMCP::ImVec2(0.0f, 0.0f), uiScale)) {
                            SKSE::log::info("Restore UI: user chose to exit now; posting a deferred window-close request.");
                            RequestGameExit();
                        }
                        ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
                        if (ButtonFit(Tr("restore.no_exit_later", "No, I'll exit later"), ImGuiMCP::ImVec2(0.0f, 0.0f), uiScale)) {
                            s_exitDismissed = true;
                        }
                    }
                } else if (s_confirmRestore) {
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f),
                        "%s", s_forceModDefaults
                            ? Tr("restore.confirm_defaults", "Apply the supported mods' upstream defaults and ignore captured custom values?")
                            : Tr("restore.confirm_originals", "Restore the settings captured before Risa changed them?"));
                    if (ButtonFit(Tr("restore.yes_restore_uninstall", "Yes, restore for uninstall"), ImGuiMCP::ImVec2(0.0f, 0.0f), uiScale)) {
                        const bool restored = RestoreAllModDefaults(s_forceModDefaults);
                        s_confirmRestore = false;
                        s_restoreDone = restored;
                        s_restoreFailed = !restored;
                        s_scrollToBottomFrames = 3;
                    }
                    ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
                    if (ButtonFit(Tr("button.cancel", "Cancel"), ImGuiMCP::ImVec2(0.0f, 0.0f), uiScale)) {
                        s_confirmRestore = false;
                    }
                    // Confirm buttons just appeared below the fold — scroll them into view.
                } else {
                    if (s_restoreFailed) {
                        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.35f, 0.35f, 1.0f),
                            "%s", Tr("restore.incomplete", "Restore was incomplete. The originals backup was kept; check the logs and retry."));
                    }
                    if (ButtonFit(Tr("restore.my_original_settings", "Restore My Original Settings"), launcherButtonSize, uiScale)) {
                        s_forceModDefaults = false;
                        s_confirmRestore = true;
                        s_scrollToBottomFrames = 3;
                    }
                    ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.restore_originals", "Restore the values captured before Risa changed them.\nMissing captured values use safe supported-mod defaults."));
                    ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
                    if (ButtonFit(Tr("restore.supported_mod_defaults", "Restore Supported Mod Defaults"), launcherButtonSize, uiScale)) {
                        s_forceModDefaults = true;
                        s_confirmRestore = true;
                        s_scrollToBottomFrames = 3;
                    }
                    ImGuiMCP::SetItemTooltip("%s", Tr("tooltip.restore_defaults", "Ignore captured custom values and restore every managed setting\nto the supported mod's upstream default."));
                }

                // Keep pushing to the true bottom while ImGui recalculates the changed child height.
                if (s_scrollToBottomFrames > 0) {
                    ImGuiMCP::SetScrollY(ImGuiMCP::GetScrollMaxY());
                    --s_scrollToBottomFrames;
                }
            }

            ImGuiMCP::PopStyleColor(); // separator color
            ImGuiMCP::EndChild();

            if (g_OpenHotkeyOptionsPopup.exchange(false)) {
                ImGuiMCP::OpenPopup("Hotkey Options");
            }
            // Slide the Advanced Bindings popup out from the launcher's right edge so it reads as an
            // attached panel instead of floating over the menu.
            if (auto* lw = ImGuiMCP::FindWindowByName("###RisaMenuLauncher")) {
                ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(lw->Pos.x + lw->Size.x - 1.0f, lw->Pos.y),
                    ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2(0.0f, 0.0f));
            }
            if (ImGuiMCP::BeginPopup("Hotkey Options")) {
                int idx = g_ActiveHotkeyOptionsAlias;
                if (idx >= 0 && idx < AI_COUNT) {
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 0.62f, 1.00f, 1.0f), "%s %s",
                        Tr("advanced.title_prefix", "Advanced Bindings:"), GetModDisplayName(idx));
                    ImGuiMCP::Separator();
                    ImGuiMCP::Text("%s", Tr("advanced.key_bind", "Key Bind:"));
                    const bool capturing = g_CapturingAlias.load() == idx;
                    std::string lbl;
                    if (capturing) {
                        lbl = TrLabel("button.press_any_key", "[ Press any key... ]", "##popup_alias");
                    } else {
                        std::string p;
                        if (g_AliasCtrl[idx].load()) p += "Ctrl + ";
                        if (g_AliasShift[idx].load()) p += "Shift + ";
                        if (g_AliasAlt[idx].load()) p += "Alt + ";
                        lbl = p + NameFromDIK(g_AliasDik[idx].load()) + "##popup_alias";
                    }

                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.24f, 0.24f, 0.30f, 1.0f));
                    ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ButtonTextAlign, ImGuiMCP::ImVec2(0.5f, 0.5f));
                    if (ImGuiMCP::Button(lbl.c_str(), ImGuiMCP::ImVec2(-1.0f, 0.0f))) {
                        if (!capturing) {
                            g_KeyCaptureStartedMs.store(NowMs());
                        }
                        g_CapturingAlias.store(capturing ? -1 : idx); // click to start/cancel capture
                    }
                    ImGuiMCP::PopStyleVar(1);
                    ImGuiMCP::PopStyleColor(3);

                    if (capturing) {
                        for (int vk = 8; vk < 256; ++vk) {
                            if (vk == 1 || vk == 2 || vk == 4) continue; // mouse
                            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                                vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
                                vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) continue; // modifiers
                            if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
                                const WORD dik = DIKFromVK(static_cast<WORD>(vk));
                                if (dik != 0) {
                                    g_AliasDik[idx].store(dik);
                                    g_AliasCtrl[idx].store((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                                    g_AliasShift[idx].store((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                                    g_AliasAlt[idx].store((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                                    // Rebinding no longer auto-enables the key - the user toggles it on themselves.
                                    g_CapturingAlias.store(-1);
                                    g_SuppressAliasUntilMs.store(NowMs() + 1000); // don't let this keypress fire the mod
                                    SaveButtonOrder();
                                    break;
                                }
                            }
                        }
                    }

                    ImGuiMCP::Separator();
                    ImGuiMCP::Text("%s", Tr("advanced.modifiers", "Modifiers:"));
                    bool ctrl = g_AliasCtrl[idx].load();
                    bool shift = g_AliasShift[idx].load();
                    bool alt = g_AliasAlt[idx].load();
                    bool modChanged = false;
                    if (ImGuiMCP::Checkbox("Ctrl", &ctrl)) { g_AliasCtrl[idx].store(ctrl); modChanged = true; }
                    ImGuiMCP::SameLine(0.0f, 12.0f * layoutScale);
                    if (ImGuiMCP::Checkbox("Shift", &shift)) { g_AliasShift[idx].store(shift); modChanged = true; }
                    ImGuiMCP::SameLine(0.0f, 12.0f * layoutScale);
                    if (ImGuiMCP::Checkbox("Alt", &alt)) { g_AliasAlt[idx].store(alt); modChanged = true; }
                    if (modChanged) {
                        SaveButtonOrder();
                    }

                    ImGuiMCP::Separator();
                    ImGuiMCP::Text("%s", Tr("settings.trigger_mode", "Trigger Mode:"));
                    int mode = g_AliasTriggerMode[idx].load();
                    bool modeChanged = false;
                    if (ImGuiMCP::RadioButton(TrLabel("trigger.single_press_default", "Single Press (default)", "##alias_popup").c_str(), mode == 0)) { mode = 0; modeChanged = true; }
                    if (ImGuiMCP::RadioButton(TrLabel("trigger.double_press", "Double Press (tap twice)", "##alias_popup").c_str(), mode == 1)) { mode = 1; modeChanged = true; }
                    if (ImGuiMCP::RadioButton(TrLabel("trigger.hold_to_trigger", "Hold to Trigger", "##alias_popup").c_str(), mode == 2)) { mode = 2; modeChanged = true; }
                    if (modeChanged) {
                        g_AliasTriggerMode[idx].store(mode);
                        SaveButtonOrder();
                    }
                    if (mode == 2) {
                        int holdMs = g_AliasHoldDurationMs[idx].load();
                        ImGuiMCP::PushItemWidth(-1.0f);
                        if (ImGuiMCP::SliderInt("##aliasHoldMs", &holdMs, 100, 2000, Tr("slider.hold_ms_format", "Hold %d ms"))) {
                            g_AliasHoldDurationMs[idx].store(holdMs);
                            SaveButtonOrder();
                        }
                        ImGuiMCP::PopItemWidth();
                    }

                    ImGuiMCP::Separator();
                    // Reset to default key button
                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.70f, 0.40f, 0.15f, 1.0f));
                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.85f, 0.50f, 0.20f, 1.0f));
                    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.95f, 0.60f, 0.25f, 1.0f));
                    if (ButtonFit(Tr("advanced.reset_default_key", "Reset to Default Key"), ImGuiMCP::ImVec2(-1.0f, 0.0f), uiScale)) {
                        RestoreAliasToDefault(idx);
                        SaveButtonOrder();
                        ImGuiMCP::CloseCurrentPopup();
                    }
                    ImGuiMCP::PopStyleColor(3);
                }
                ImGuiMCP::EndPopup();
            }

            ImGuiMCP::EndTabItem(); // Settings tab was missing this - left the tab stack unbalanced so
                                    // the Exclusions tab couldn't take over (it showed Settings instead).
        }

        bool exclusionsTabOpen = false;
        if (g_ShowExclusionsTab.load()) {
            exclusionsTabOpen = ImGuiMCP::BeginTabItem(
                (FontAwesome::UnicodeToUtf8(0xf05e) + "  " + Tr("tab.exclusions", "Exclusions")).c_str(), nullptr, 0); // fa-ban
            if (exclusionsTabOpen) {
                DrawExclusionsTabContent();
                ImGuiMCP::EndTabItem();
            }
        }
        g_ExclusionsActive = exclusionsTabOpen;

        ImGuiMCP::EndTabBar();
    }
    ImGuiMCP::PopStyleColor(5);

    ImGuiMCP::PopStyleVar(3);
    ImGuiMCP::PopStyleColor(3);

    ImGuiMCP::End();
    ImGuiMCP::PopStyleVar(2);
    ImGuiMCP::PopStyleColor(4);

    // Keep on top
    if (!g_ExclusionsPopupOpen && NowMs() >= g_LanguageComboActiveUntilMs.load()) {
        if (auto* w = ImGuiMCP::FindWindowByName("###RisaMenuLauncher")) {
            ImGuiMCP::BringWindowToDisplayFront(w);
        }
    }
}

// ============================================================================
// Logging
// ============================================================================
static void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) return;
    auto logPath = *logsFolder / std::format("{}.log",
        SKSE::PluginDeclaration::GetSingleton()->GetName());
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("log", std::move(sink)));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    ApplyLogLevel(); // LoadButtonOrder re-applies the saved toggle after settings load

    // Second, dedicated log: ONLY the config files this mod edits/reverts. Kept separate and plain
    // so anyone can see, at a glance, exactly what was touched (and reverted) on disk.
    auto fcPath = *logsFolder / "RisaAllInOneMenu_FileChanges.log";
    try {
        auto fcSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(fcPath.string(), true);
        g_FileChangeLog = std::make_shared<spdlog::logger>("filechanges", std::move(fcSink));
        g_FileChangeLog->set_pattern("[%Y-%m-%d %H:%M:%S] %v");
        g_FileChangeLog->set_level(spdlog::level::info);
        g_FileChangeLog->info("=== Risa's All In One Menu - config file change audit ===");
        g_FileChangeLog->info("Lists ONLY config files this mod edits (EDIT) or reverts (REVERT). Nothing else is touched.");
        g_FileChangeLog->flush();
    } catch (...) {}
}

// If the launcher hotkey collides with a managed mod's key (e.g. default F1 == Debug
// Menu), reassign it to a free key. This only ever changes OUR OWN ini — no external file.
static void EnsureLauncherKeyNoConflict() {
    auto conflicts = [](WORD dik) -> bool {
        if (dik == 0) return false;
        if (g_DMenuConfig.enabled          && dik == g_DMenuConfig.toggleDIK)          return true;
        if (g_ImprovedCameraConfig.enabled && dik == g_ImprovedCameraConfig.toggleDIK) return true;
        if (g_FLICKConfig.enabled          && dik == g_FLICKConfig.toggleDIK)          return true;
        if (g_DebugMenuConfig.enabled      && dik == g_DebugMenuConfig.toggleDIK)      return true;
        if (g_IEDConfig.enabled            && dik == g_IEDConfig.toggleDIK)            return true;
        if (g_KreatEConfig.enabled         && dik == g_KreatEConfig.toggleDIK)         return true;
        if (g_MFConfig.toggleDIK != 0      && dik == g_MFConfig.toggleDIK)             return true;
        return false;
    };
    const WORD cur = g_LauncherHotkeyDIK.load();
    if (!conflicts(cur)) return;

    const WORD candidates[] = { 0x58/*F12*/, 0x57/*F11*/, 0x44/*F10*/, 0x43/*F9*/, 0x42/*F8*/,
                                0xD2/*Insert*/, 0xCF/*End*/, 0xC9/*PageUp*/, 0xD1/*PageDown*/ };
    for (WORD c : candidates) {
        if (!conflicts(c)) {
            g_LauncherHotkeyDIK.store(c);
            g_LauncherHotkeyVK.store(VKFromDIK(c));
            SaveButtonOrder();
            SKSE::log::info("EnsureLauncherKeyNoConflict: launcher key 0x{:02X} collided with a managed mod; auto-reassigned to {} (0x{:02X}).",
                cur, NameFromDIK(c), c);
            return;
        }
    }
    SKSE::log::warn("EnsureLauncherKeyNoConflict: launcher key 0x{:02X} collides but no free candidate was available.", cur);
}

// dMenu and Improved Camera read the IDENTICAL engine event, so if they share a key they
// can't be separated in software. dMenu reads its key from its own file (CSimpleIni), so
// the only way to give it a distinct key is a managed write to dmenu.ini. We do this only
// when they actually collide, pick a free key, preserve the rest of the file, and it takes
// effect on the next launch (dMenu reads its ini before we run). Writes once, then the
// collision is gone so we never touch it again.
// Debug Menu's hotkey defaults to F1 — the same as our launcher key — which can't be told
// apart when simulating. If they collide, move Debug Menu to a free function key by editing
// its MCM config (uOpenMenuHotkey), the same managed-write approach used for dMenu.
static nlohmann::json GetOriginal(const std::string& id);
static void ManageDebugMenuKey() {
    if (g_ExcludeMod[AI_DebugMenu].load()) return;
    if (g_ManagementSuspended.load()) return; // after a full Restore: leave keys where they were put back
    if (!g_DebugMenuConfig.enabled) return;
    const WORD cur = g_DebugMenuConfig.toggleDIK;
    if (cur == 0) return;
    const WORD launcher = g_LauncherHotkeyDIK.load();
    // Debug Menu has no open API, so the launcher opens it by SIMULATING its key. Relocate when it
    // collides with the launcher key, OR when it is sitting on a key WE assigned in an older build we
    // no longer use: the legacy pressable F2-F8 keys (they clash with popular mods) or Scroll Lock
    // (0x46, a real pressable key). We move it onto the UNPRESSABLE F17 - opening uses engine scan-code
    // injection and closing uses a WM post with a hardcoded scan code, so F17 works for both.
    const bool legacyReloc = (cur == 0x3C || cur == 0x3D || cur == 0x3E || cur == 0x40 || cur == 0x42 || cur == 0x46);
    const nlohmann::json origJ = GetOriginal("DebugMenu.uOpenMenuHotkey");
    const int origKey = origJ.is_number_integer() ? origJ.get<int>() : -1;
    const bool ourReloc = legacyReloc && static_cast<int>(cur) != origKey; // a key WE assigned
    if (cur != launcher && !ourReloc) return; // no launcher collision and not one of our old keys

    // Match LoadDebugMenuConfig's precedence so we edit the file Debug Menu will consume.
    std::filesystem::path settingsIni = "Data/MCM/Settings/DebugMenu.ini";
    if (!std::filesystem::exists(settingsIni)) {
        settingsIni = "Data/MCM/Config/DebugMenu/settings.ini";
    }
    if (!std::filesystem::exists(settingsIni)) { SKSE::log::warn("ManageDebugMenuKey: DebugMenu settings.ini not found."); return; }

    auto used = [&](WORD dik) -> bool {
        if (dik == 0) return true;
        if (dik == g_LauncherHotkeyDIK.load())                               return true;
        if (g_DMenuConfig.enabled          && dik == g_DMenuConfig.toggleDIK)          return true;
        if (g_ImprovedCameraConfig.enabled && dik == g_ImprovedCameraConfig.toggleDIK) return true;
        if (g_FLICKConfig.enabled          && dik == g_FLICKConfig.toggleDIK)          return true;
        if (g_IEDConfig.enabled            && dik == g_IEDConfig.toggleDIK)            return true;
        if (g_KreatEConfig.enabled         && dik == g_KreatEConfig.toggleDIK)         return true;
        if (g_MFConfig.toggleDIK != 0      && dik == g_MFConfig.toggleDIK)             return true;
        return false;
    };
    // Park on the UNPRESSABLE F17 (no physical key can hit it, so Debug Menu's key is truly freed);
    // F20 as a backup if F17 is somehow taken. Both are driven via the engine (open) + WM post (close).
    const WORD candidates[] = { 0x68 /*F17*/, 0x6B /*F20*/ };
    WORD freeKey = 0;
    for (WORD c : candidates) if (!used(c)) { freeKey = c; break; }
    if (freeKey == 0) { SKSE::log::warn("ManageDebugMenuKey: no free key available."); return; }

    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream f(settingsIni);
        std::string line;
        while (std::getline(f, line)) {
            std::string trimmed = TrimStr(line);
            auto eq = trimmed.find('=');
            if (eq != std::string::npos && ToUpper(TrimStr(trimmed.substr(0, eq))) == "UOPENMENUHOTKEY") {
                line = std::format("uOpenMenuHotkey = {}", static_cast<int>(freeKey));
                replaced = true;
            }
            lines.push_back(line);
        }
    }
    if (!replaced) { SKSE::log::warn("ManageDebugMenuKey: uOpenMenuHotkey line not found."); return; }

    std::ofstream out(settingsIni, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("ManageDebugMenuKey: failed to open {} for writing.", settingsIni.string()); return; }
    for (const auto& l : lines) out << l << "\n";
    out.close();

    MarkConfigRestartRequired("Debug Menu hotkey");
    SKSE::log::info("ManageDebugMenuKey: DebugMenu shared the launcher key (0x{:02X}); set {} uOpenMenuHotkey = {} ({}). "
                    "The change was written during plugin startup.",
        g_DebugMenuConfig.toggleDIK, settingsIni.string(), static_cast<int>(freeKey), NameFromDIK(freeKey));
}

// Rewrite a "Key = Value" line in an ini, preserving the rest of the file and the original
// key spelling. Idempotent. Returns 1=written, 0=already had the value, -1=missing/error.
static int SetIniValue(const std::filesystem::path& ini, const std::string& keyName, int newValue, bool hex = false) {
    if (!std::filesystem::exists(ini)) { SKSE::log::warn("SetIniValue: {} not found.", ini.string()); return -1; }
    std::vector<std::string> lines;
    bool found = false, changed = false;
    const std::string KEY = ToUpper(keyName);
    {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            std::string trimmed = TrimStr(line);
            auto eq = trimmed.find('=');
            if (eq != std::string::npos && ToUpper(TrimStr(trimmed.substr(0, eq))) == KEY) {
                found = true;
                int cur = 0x7FFFFFFF; // sentinel: if unparseable, treat as "needs write"
                try { cur = static_cast<int>(std::stoul(TrimStr(trimmed.substr(eq + 1)), nullptr, 0)); } catch (...) {}
                if (cur != newValue) {
                    line = hex ? std::format("{} = 0x{:X}", keyName, newValue)
                               : std::format("{} = {}", keyName, newValue);
                    changed = true;
                }
            }
            lines.push_back(line);
        }
    }
    if (!found) { SKSE::log::warn("SetIniValue: key '{}' not found in {}.", keyName, ini.string()); return -1; }
    if (!changed) return 0;
    std::ofstream out(ini, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("SetIniValue: cannot write {}.", ini.string()); return -2; }
    for (const auto& l : lines) out << l << "\n";
    LogFileChange(ini.string(), std::format("{} = {}{}", keyName, hex ? "0x" : "", hex ? std::format("{:X}", newValue) : std::format("{}", newValue)));
    return 1;
}

static void TryManageKreatEHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_KreatE].load()) return;
    if (g_KreatEIniManaged.load() || !IsPluginPresent("KreatE")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastKreatEIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    const std::filesystem::path ini = "Data/KreatE/UserSettings.ini";
    if (!std::filesystem::exists(ini)) return;
    // KreatE reads its toggle through the window-message / ImGui channel (its only user32 input
    // import is GetKeyState, used by ImGui for modifiers), NOT a GetKeyState/GetAsyncKeyState call
    // we can answer caller-scoped. So the scoped-synthetic approach can't drive it — instead we
    // relocate its listener to unpressable F18 (frees End) and open it with a SendInput F18 tap,
    // which reaches KreatE through the same window-message path a real key would.
    const int result = SetIniValue(ini, "GUIToggleKeys", VK_F18, true);
    if (result < 0) return;

    g_KreatEIniManaged.store(true);
    if (result == 1) {
        MarkConfigRestartRequired("KreatE hotkey");
        SKSE::log::info("TryManageKreatEHotkey: KreatE GUIToggleKeys -> F18 (was End).{}",
            lateRetry ? " Restart Skyrim once so KreatE reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageKreatEHotkey: KreatE GUIToggleKeys is already F18.");
    }
}

static int SetJsonKeyValue(const std::filesystem::path& path, const std::string& section, const std::string& key,
        const nlohmann::json& newValue) {
    if (!std::filesystem::exists(path)) return -1;
    try {
        nlohmann::json j;
        {
            std::ifstream f(path);
            if (!f.is_open()) return -1;
            f >> j;
        }
        if (j.contains(section) && j[section].contains(key)) {
            if (j[section][key] == newValue) return 0;
            j[section][key] = newValue;
        } else {
            if (j.contains(section)) {
                j[section][key] = newValue;
            } else {
                return -1;
            }
        }
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) return -2;
        out << j.dump(4);
        LogFileChange(path.string(), std::format("{}.{} = {}", section, key, newValue.dump()));
        return 1;
    } catch (...) {
        return -1;
    }
}

static int SetJsonRootValue(const std::filesystem::path& path, const std::string& key,
        const nlohmann::json& newValue) {
    if (!std::filesystem::exists(path)) return -1;
    try {
        nlohmann::json j;
        {
            std::ifstream f(path);
            if (!f.is_open()) return -1;
            f >> j;
        }
        if (j.contains(key) && j[key] == newValue) return 0;
        j[key] = newValue;
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) return -2;
        out << j.dump(4) << '\n';
        LogFileChange(path.string(), std::format("{} = {}", key, newValue.dump()));
        return 1;
    } catch (...) {
        return -1;
    }
}

static std::atomic<bool> g_CSIniManaged{ false };
static std::atomic<long long> g_LastCSIniAttemptMs{ 0 };

static void TryManageCSHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_CS].load()) return;
    if (g_CSIniManaged.load() || !IsPluginPresent("CommunityShaders")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastCSIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    bool any = false;
    const std::filesystem::path userJson = "Data/SKSE/Plugins/CommunityShaders/SettingsUser.json";
    const std::filesystem::path defaultJson = "Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json";

    // Keep every CS listener on a distinct hidden key. Editor must not share F23 with
    // Overlay: releasing Shift made the held F23 transition into the Overlay chord.
    const auto manage = [&](const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) return false;
        bool changed = false;
        changed |= SetJsonKeyValue(path, "Menu", "ToggleKey", VK_F19) == 1;
        changed |= SetJsonKeyValue(path, "Menu", "CSEditorToggleKey",
            nlohmann::json::array({ 0, VK_F15 })) == 1;
        changed |= SetJsonKeyValue(path, "Menu", "OverlayToggleKey", VK_F23) == 1;
        changed |= SetJsonKeyValue(path, "Menu", "EffectToggleKey", VK_F24) == 1;
        return changed;
    };
    any |= manage(userJson);
    any |= manage(defaultJson);

    if (any) {
        g_CSIniManaged.store(true);
        MarkConfigRestartRequired("Community Shaders hotkeys");
        SKSE::log::info("TryManageCSHotkey: Community Shaders keys -> F19 / F15 / F23 / F24. Restart required.");
    } else if (std::filesystem::exists(userJson) || std::filesystem::exists(defaultJson)) {
        g_CSIniManaged.store(true);
        SKSE::log::info("TryManageCSHotkey: Community Shaders keys are already F19 / F15 / F23 / F24.");
    }
}

// Rewrite iHotkey under [Controls.Keyboard] in Mod Function Menu's TOML, preserving every other line
// (including the gamepad iHotkey). Returns 1 if changed, 0 if already the target, negative on error.
// Set an integer key under [Controls.Keyboard] in Mod Function Menu's TOML (iHotkey or iExtraExit).
static int SetModFunctionMenuKey(const std::filesystem::path& toml, const char* keyName, int newValue) {
    if (!std::filesystem::exists(toml)) {
        SKSE::log::warn("SetModFunctionMenuKey: {} not found.", toml.string());
        return -1;
    }
    const std::string wantKey = ToUpper(keyName);
    std::vector<std::string> lines;
    std::string section;
    bool found = false, changed = false;
    {
        std::ifstream in(toml);
        std::string line;
        while (std::getline(in, line)) {
            const std::string t = TrimStr(line);
            if (!t.empty() && t.front() == '[') {
                section = ToUpper(t);
            } else if (section == "[CONTROLS.KEYBOARD]" && !found) {
                const auto eq = t.find('=');
                if (eq != std::string::npos && ToUpper(TrimStr(t.substr(0, eq))) == wantKey) {
                    found = true;
                    std::string cur = TrimStr(t.substr(eq + 1));
                    const auto c = cur.find('#'); if (c != std::string::npos) cur = TrimStr(cur.substr(0, c));
                    int curVal = -1; try { curVal = std::stoi(cur); } catch (...) {}
                    if (curVal != newValue) { line = std::string(keyName) + " = " + std::to_string(newValue); changed = true; }
                }
            }
            lines.push_back(line);
        }
    }
    if (!found) {
        SKSE::log::warn("SetModFunctionMenuKey: {} not found under [Controls.Keyboard] in {}.", keyName, toml.string());
        return -1;
    }
    if (!changed) return 0;
    std::ofstream out(toml, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("SetModFunctionMenuKey: cannot write {}.", toml.string()); return -2; }
    for (const auto& l : lines) out << l << "\n";
    LogFileChange(toml.string(), std::string(keyName) + " = " + std::to_string(newValue));
    return 1;
}
static int SetModFunctionMenuHotkey(const std::filesystem::path& toml, int newValue) {
    return SetModFunctionMenuKey(toml, "iHotkey", newValue);
}

static void TryManageModFunctionMenuHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_ModFunctionMenu].load()) return;
    if (g_ModFunctionMenuIniManaged.load() || !IsPluginPresent("ccld_ModFunctionMenu")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastModFunctionMenuIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    // Mod Function Menu defaults to F1 (= our launcher key) and reads the engine input queue. Relocate
    // its hotkey onto the unpressable F20 so the physical key is freed; the launcher opens it by
    // injecting F20. Takes effect after a restart (it reads the TOML at plugin load).
    constexpr int kParkKey = 0x6B; // F20 (DirectInput scancode)
    const std::filesystem::path tomlPath = "Data/SKSE/Plugins/ccld_ModFunctionMenu.toml";
    const int result = SetModFunctionMenuHotkey(tomlPath, kParkKey);
    if (result < 0) return;

    g_ModFunctionMenuConfig.toggleDIK = static_cast<WORD>(kParkKey);
    g_ModFunctionMenuConfig.toggleVK  = VKFromDIK(static_cast<WORD>(kParkKey));
    g_ModFunctionMenuIniManaged.store(true);
    if (result == 1) {
        MarkConfigRestartRequired("Mod Function Menu hotkey");
        SKSE::log::info("TryManageModFunctionMenuHotkey: Mod Function Menu hotkey -> F20 (was F1).{}",
            lateRetry ? " Restart Skyrim once so Mod Function Menu reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageModFunctionMenuHotkey: Mod Function Menu hotkey is already F20.");
    }
}

static void TryManageFLICKHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_FLICK].load()) {
        if (g_FLICKHotkeyManaged.exchange(false)) {
            SetFLICKHotkeyEnabled(true);
            SKSE::log::info("TryManageFLICKHotkey: FLICK excluded; native hotkey re-enabled via API.");
        }
        return;
    }
    if (!IsPluginPresent("FUCK")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastFLICKHotkeyAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    if (SetFLICKHotkeyEnabled(false)) {
        // The API disables FLICK's own native hotkey processing. Risa's separately configured alias
        // remains user-controlled and opens FLICK through the API.
                g_FLICKHotkeyManaged.store(true);
        const long long until = now + 15000;
        long long oldUntil = g_FLICKHotkeyReassertUntilMs.load();
        while (oldUntil < until && !g_FLICKHotkeyReassertUntilMs.compare_exchange_weak(oldUntil, until)) {}
        SKSE::log::info("TryManageFLICKHotkey: disabled FLICK native hotkey via API.");
    }
}

static void StartFLICKHotkeyMonitor() {
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return;

    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!::GetModuleHandleW(L"FUCK.dll")) continue;
            if (SetFLICKHotkeyEnabled(false)) {
                g_FLICKHotkeyManaged.store(true);
            }
        }
    }).detach();
    SKSE::log::info("StartFLICKHotkeyMonitor: periodic API reassertion started.");
}

static void TryManageOPSHotkey(bool lateRetry) {
    // We no longer FREE OPS's key: rewriting its live Papyrus key registration froze input and fought
    // the user's own rebinds. OPS's default key (Numpad 1) doesn't clash with the launcher, so we leave
    // it alone and just provide the launcher button. But an earlier build forced Hotkey=0 into existing
    // saves, killing the key - so if we find it disabled, restore Numpad 1 once. Otherwise never touch it.
    if (g_OPSKeyManaged.load() || !IsOPSInstalled()) return;
    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastOPSKeyAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    RE::BSTSmartPointer<RE::BSScript::Object> obj;
    if (!GetOPSObject(vm, obj)) return; // quest not bound yet - retry later
    auto* prop = obj->GetProperty("Hotkey");
    if (prop && prop->GetSInt() == 0) {
        SetOPSHotkey(79); // Numpad 1 - undo the earlier build's disable
        SKSE::log::info("TryManageOPSHotkey: restored Outfit Preview Selector hotkey to Numpad 1 (an earlier build had disabled it).");
    }
    g_OPSConfig.toggleDIK = 0x4F;
    g_OPSConfig.toggleVK = VK_NUMPAD1;
    g_OPSKeyManaged.store(true);
}

static void TryManageCatMenuHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_CatMenu].load()) return;
    if (g_CatMenuIniManaged.load() || !IsPluginPresent("catmenu")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastCatMenuIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/catmenu/settings.json";
    // Keep a valid ImGui key in CatMenu's settings (ImGuiKey_None is unsafe in its
    // unguarded IsKeyPressed call). Our caller-scoped ImGui hook neutralizes it.
    constexpr int kParkingImGuiKey = 592; // ImGuiKey_F21; not reserved
    const int result = SetJsonRootValue(jsonPath, "toggle_key", kParkingImGuiKey);
    if (result < 0) return;

    // We load settings before performing the managed write. Keep this launch in sync
    // with the value CatMenu will read later during its own plugin initialization.
    g_CatMenuConfig.toggleImGuiKey = kParkingImGuiKey;
    g_CatMenuConfig.toggleVK = VK_F21;
    g_CatMenuConfig.toggleDIK = 0x6C;
    g_CatMenuIniManaged.store(true);
    if (result == 1) {
        MarkConfigRestartRequired("CatMenu hotkey");
        SKSE::log::info("TryManageCatMenuHotkey: CatMenu parking key -> F21; native listener is intercepted.{}",
            lateRetry ? " Restart Skyrim once so CatMenu reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageCatMenuHotkey: CatMenu parking key is F21; native listener is intercepted.");
    }
}

static void TryManageDragonbornHotkey(bool lateRetry) {
    if (g_ExcludeMod[AI_Dragonborn].load()) return;
    if (g_DragonbornIniManaged.load() || !IsPluginPresent("SkyrimCheatMenu")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastDragonbornIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    // Preferred path (Dragonborn's Toolkit v0.5+): disable its native hotkey listener at RUNTIME via
    // the exported API. No SkyrimCheatMenu.json edit, no restart. The launcher opens/closes via API.
    // Re-applied on every launch (the toolkit re-enables its listener each session), so it's idempotent.
    if (SetDragonbornHotkeyEnabled(false)) {
        g_DragonbornConfig.toggleKeyName = "UNASSIGNED";
        g_DragonbornConfig.toggleDIK = 0;
        g_DragonbornConfig.toggleVK = 0;
        g_DragonbornIniManaged.store(true);
        SKSE::log::info("TryManageDragonbornHotkey: disabled Dragonborn's Toolkit native hotkey via runtime API (no file edit, no restart).");
        return;
    }

    // Fallback for older builds without the runtime hotkey export: edit the JSON (takes effect next restart).
    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/SkyrimCheatMenu.json";
    const int result = SetJsonRootValue(jsonPath, "toggleKey", 0);
    if (result < 0) return;

    g_DragonbornConfig.toggleKeyName = "UNASSIGNED";
    g_DragonbornConfig.toggleDIK = 0;
    g_DragonbornConfig.toggleVK = 0;
    g_DragonbornIniManaged.store(true);
    if (result == 1) {
        MarkConfigRestartRequired("Dragonborn's Toolkit hotkey");
        SKSE::log::info("TryManageDragonbornHotkey: disabled Dragonborn's Toolkit native hotkey.{}",
            lateRetry ? " Restart Skyrim once so Dragonborn's Toolkit reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageDragonbornHotkey: Dragonborn's Toolkit native hotkey is already disabled.");
    }
}

// Rewrite a text-valued INI setting while preserving all unrelated lines.
static int SetIniTextValue(const std::filesystem::path& ini, const std::string& keyName,
        const std::string& newValue) {
    if (!std::filesystem::exists(ini)) {
        SKSE::log::warn("SetIniTextValue: {} not found.", ini.string());
        return -1;
    }

    std::vector<std::string> lines;
    bool found = false;
    bool changed = false;
    const std::string key = ToUpper(keyName);
    const std::string expected = ToUpper(newValue);
    {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = TrimStr(line);
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos && ToUpper(TrimStr(trimmed.substr(0, eq))) == key) {
                found = true;
                std::string current = TrimStr(trimmed.substr(eq + 1));
                const auto comment = current.find(';');
                if (comment != std::string::npos) current = TrimStr(current.substr(0, comment));
                if (ToUpper(current) != expected) {
                    line = std::format("{} = {}", keyName, newValue);
                    changed = true;
                }
            }
            lines.push_back(line);
        }
    }

    if (!found) {
        SKSE::log::warn("SetIniTextValue: key '{}' not found in {}.", keyName, ini.string());
        return -1;
    }
    if (!changed) return 0;

    std::ofstream out(ini, std::ios::trunc);
    if (!out.is_open()) {
        SKSE::log::error("SetIniTextValue: cannot write {}.", ini.string());
        return -2;
    }
    for (const auto& line : lines) out << line << "\n";
    LogFileChange(ini.string(), std::format("{} = {}", keyName, newValue));
    return 1;
}

static void RestoreLegacyKreatEBlocking() {
    const int original = g_LegacyKreatEOriginalBlocking.exchange(-1);
    if (original < 0) return;

    const std::filesystem::path ini = "Data/KreatE/UserSettings.ini";
    const int result = SetIniTextValue(ini, "Blocking", original != 0 ? "true" : "false");
    if (result >= 0) {
        if (result == 1) MarkConfigRestartRequired("KreatE legacy Blocking setting");
        SKSE::log::info("RestoreLegacyKreatEBlocking: restored KreatE Input.Blocking to {} and removed the legacy keep-open option.", original != 0);
        SaveButtonOrder();
    }
}

// Return the first candidate path that exists (handles MO2 mounting a mod's files under
// different virtual locations than expected).
static std::filesystem::path FindModFile(std::initializer_list<const char*> candidates) {
    for (auto* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}
// Prefer FLICK's keybinds_user.ini (the file it actually reads/writes once the user has changed any
// setting) and fall back to keybinds.ini. Returns empty only if neither exists.
static std::filesystem::path FindFLICKKeybinds() {
    if (auto user = FindModFile({ "Data/FUCKs/FUCK/keybinds_user.ini", "FUCKs/FUCK/keybinds_user.ini",
                                  "Data/SKSE/Plugins/FUCKs/FUCK/keybinds_user.ini" }); !user.empty())
        return user;
    return FindModFile({ "Data/FUCKs/FUCK/keybinds.ini", "FUCKs/FUCK/keybinds.ini",
                         "Data/SKSE/Plugins/FUCKs/FUCK/keybinds.ini" });
}

// Point the mods that can't be blocked at the input layer (they read the shared engine event
// stream) at "unpressable" hotkeys, freeing their original key for vanilla. The launcher opens
// each by emitting that key through the engine. FLICK is opened via its API, so its key just
// needs to be moved off F7. Idempotent: writes only when a value isn't already set, so it runs
// at most once; takes effect on the next launch (mods read their config before us).
// Disable ReShade's overlay key (KeyOverlay=0) so Home is free; the add-on API opens the overlay
// from the launcher. Only the add-on build is supported - non-add-on ReShade is left untouched.
// ReShade.ini KeyOverlay format is "VK,ctrl,shift,alt". Idempotent; takes effect next launch.
// NOTE: ReShade rewrites ReShade.ini itself, so this can get clobbered — if Home keeps coming
// back, set the Overlay key inside ReShade's own Settings > Input instead (that sticks).
static void ManageReShadeHotkey() {
    if (g_ManagementSuspended.load()) return; // after a full Restore: leave ReShade.ini where it was put back
    if (g_ExcludeMod[AI_ReShade].load()) return;
    if (!g_ReShadeConfig.enabled) return;
    // Only the add-on build is supported: we open the overlay through its API and DISABLE its
    // keyboard hotkey entirely (KeyOverlay=0), which frees both Home and F22. If the add-on API
    // isn't present (compatibility/non-add-on ReShade), we leave ReShade.ini alone - no F22
    // relocation - so nothing is managed and the user keeps their own overlay key.
    if (!g_ReShadeAddonActive.load()) return;
    const std::filesystem::path ini = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" });
    if (ini.empty()) { SKSE::log::warn("ManageReShadeHotkey: ReShade.ini not found."); return; }
    const int targetVK = 0; // disabled; the add-on API drives the overlay

    std::vector<std::string> lines;
    bool found = false, changed = false;
    {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos && ToUpper(TrimStr(line.substr(0, eq))) == "KEYOVERLAY") {
                found = true;
                int curVK = 0;
                try { curVK = std::stoi(TrimStr(line.substr(eq + 1))); } catch (...) {}
                if (curVK != targetVK) { line = std::format("KeyOverlay={},0,0,0", targetVK); changed = true; }
            }
            lines.push_back(line);
        }
    }
    if (!found) { SKSE::log::warn("ManageReShadeHotkey: KeyOverlay line not found in {}.", ini.string()); return; }
    if (!changed) return;
    std::ofstream out(ini, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("ManageReShadeHotkey: cannot write {}.", ini.string()); return; }
    for (const auto& l : lines) out << l << "\n";
    MarkConfigRestartRequired("ReShade overlay hotkey");
    SKSE::log::info("ManageReShadeHotkey: ReShade KeyOverlay -> DISABLED (add-on API drives the overlay; Home + F22 freed) at {}. RESTART once.",
        ini.string());
}

// ============================================================================
// Original-hotkey backup. Before the plugin relocates any mod's key, we record that
// mod's ORIGINAL value here so Restore can put back exactly what the user had, instead
// of a hard-coded guess. Stored as JSON (handles int keys, chord arrays, and strings).
// Capture is once-only and skips values that are our own F-key relocations (so an
// already-relocated install doesn't record a bogus "original").
// ============================================================================
static const char* kHotkeyBackupPath = "Data/SKSE/Plugins/RisaAllInOneMenu_OriginalHotkeys.json";
static nlohmann::json g_HotkeyBackup;
static bool g_HotkeyBackupLoaded = false;

static void SaveHotkeyBackup() {
    std::ofstream out(kHotkeyBackupPath, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("HotkeyBackup: CANNOT WRITE {}.", kHotkeyBackupPath); return; }
    out << g_HotkeyBackup.dump(2);
}
// Self-heal older/poisoned backups: an earlier build's legacy-detection heuristic could record OUR
// OWN relocation value as if it were the user's original (e.g. IED.ToggleKeys=103 / F16, MF
// ToggleMode=OFF, IED OverrideToggleKeys=true). Those are never real originals, so a "restore" would
// just re-apply the relocation. Purge any entry that equals a known current relocation sentinel;
// restore then correctly falls back to the upstream default. Returns true if anything was removed.
static bool SanitizeHotkeyBackup() {
    // id -> the managed/relocation value that must never be treated as an original.
    static const std::vector<std::pair<std::string, nlohmann::json>> kSentinels = {
        { "IED.ToggleKeys",            nlohmann::json(103) },    // F16
        { "IC.MenuKey",                nlohmann::json(126) },    // F15
        { "OAR.uToggleUIKey",          nlohmann::json(100) },    // F13
        { "dMenu.key_toggle_dmenu",    nlohmann::json(101) },    // F14
        { "dMenu.key_toggle_dmenu_mkb",nlohmann::json(101) },    // F14 (newer dMenu keyboard field)
        { "KreatE.GUIToggleKeys",      nlohmann::json(129) },    // F18
        { "CatMenu.toggle_key",        nlohmann::json(592) },    // ImGuiKey_F21
        { "CS.User.ToggleKey",         nlohmann::json(VK_F19) },
        { "CS.Default.ToggleKey",      nlohmann::json(VK_F19) },
    };
    bool changed = false;
    for (const auto& [id, sentinel] : kSentinels) {
        if (!g_HotkeyBackup.contains(id)) continue;
        const nlohmann::json& v = g_HotkeyBackup[id];
        bool bad = (v == sentinel);
        if (!bad && v.is_string() && sentinel.is_string())
            bad = ToUpper(v.get<std::string>()) == ToUpper(sentinel.get<std::string>());
        if (bad) {
            SKSE::log::warn("HotkeyBackup: PURGED poisoned entry {} = {} (our relocation value, not a real original); "
                "restore will use the upstream default.", id, v.dump());
            g_HotkeyBackup.erase(id);
            changed = true;
        }
    }
    return changed;
}
static void LoadHotkeyBackup() {
    g_HotkeyBackup = nlohmann::json::object();
    std::ifstream f(kHotkeyBackupPath);
    if (f.is_open()) { try { f >> g_HotkeyBackup; } catch (...) { g_HotkeyBackup = nlohmann::json::object(); } }
    if (!g_HotkeyBackup.is_object()) g_HotkeyBackup = nlohmann::json::object();
    g_HotkeyBackupLoaded = true;
    if (SanitizeHotkeyBackup()) SaveHotkeyBackup(); // rewrite the cleaned file so the fix persists
    SKSE::log::info("HotkeyBackup: loaded {} saved original(s) from {}.", g_HotkeyBackup.size(), kHotkeyBackupPath);
}
// Capture once. If value == skipReloc it's our own relocation, not a real original, so skip.
static void CaptureOriginal(const std::string& id, const nlohmann::json& value, const nlohmann::json& skipReloc = nullptr) {
    if (!g_HotkeyBackupLoaded) LoadHotkeyBackup();
    if (g_HotkeyBackup.contains(id)) { SKSE::log::info("HotkeyBackup: {} already saved ({}) - keeping.", id, g_HotkeyBackup[id].dump()); return; }
    if (value.is_null()) { SKSE::log::info("HotkeyBackup: {} not found on disk - nothing to capture.", id); return; }
    if (!skipReloc.is_null() && value == skipReloc) {
        SKSE::log::warn("HotkeyBackup: SKIP {} = {} (matches our relocation value - not a real original; likely an already-relocated install).", id, value.dump());
        return;
    }
    g_HotkeyBackup[id] = value;
    SaveHotkeyBackup();
    SKSE::log::info("HotkeyBackup: CAPTURED {} = {} (original).", id, value.dump());
}
static nlohmann::json GetOriginal(const std::string& id) {
    if (!g_HotkeyBackupLoaded) LoadHotkeyBackup();
    return g_HotkeyBackup.contains(id) ? g_HotkeyBackup[id] : nlohmann::json(nullptr);
}
// Read an int-valued ini key (decimal or 0xHEX); null if missing/unparseable.
static nlohmann::json ReadIniInt(const std::filesystem::path& ini, const std::string& keyName) {
    if (!std::filesystem::exists(ini)) return nullptr;
    std::ifstream f(ini); std::string line; const std::string KEY = ToUpper(keyName);
    while (std::getline(f, line)) {
        std::string t = TrimStr(line);
        auto eq = t.find('=');
        if (eq != std::string::npos && ToUpper(TrimStr(t.substr(0, eq))) == KEY) {
            try { return static_cast<int>(std::stoul(TrimStr(t.substr(eq + 1)), nullptr, 0)); } catch (...) { return nullptr; }
        }
    }
    return nullptr;
}
// Read a text-valued ini key while discarding an inline comment; null if missing.
// The original spelling/case of the value is retained for lossless restoration.
static nlohmann::json ReadIniText(const std::filesystem::path& ini, const std::string& keyName) {
    if (!std::filesystem::exists(ini)) return nullptr;
    std::ifstream f(ini); std::string line; const std::string KEY = ToUpper(keyName);
    while (std::getline(f, line)) {
        std::string t = TrimStr(line);
        const auto eq = t.find('=');
        if (eq == std::string::npos || ToUpper(TrimStr(t.substr(0, eq))) != KEY) continue;
        std::string value = TrimStr(t.substr(eq + 1));
        const auto comment = value.find(';');
        if (comment != std::string::npos) value = TrimStr(value.substr(0, comment));
        return value;
    }
    return nullptr;
}
static nlohmann::json ReadJsonValue(const std::filesystem::path& path, const std::string& section, const std::string& key) {
    if (!std::filesystem::exists(path)) return nullptr;
    try { nlohmann::json j; std::ifstream f(path); f >> j;
        if (!section.empty()) return (j.contains(section) && j[section].contains(key)) ? j[section][key] : nlohmann::json(nullptr);
        return j.contains(key) ? j[key] : nlohmann::json(nullptr);
    } catch (...) { return nullptr; }
}
// Capture the raw "KeyOverlay=..." value string from ReShade.ini (e.g. "36,0,0,0").
static nlohmann::json ReadReShadeOverlay() {
    auto rs = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" });
    if (rs.empty()) return nullptr;
    std::ifstream f(rs); std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos && ToUpper(TrimStr(line.substr(0, eq))) == "KEYOVERLAY")
            return TrimStr(line.substr(eq + 1));
    }
    return nullptr;
}

// Detect a configuration that was already managed by an older Risa release before the
// originals-backup feature existed (or after that backup was lost). Requiring multiple
// matching relocations avoids mistaking one user-selected F-key for a legacy installation.
static bool DetectLegacyRisaRelocations() {
    int matches = 0;
    const auto matchIni = [&](const std::filesystem::path& path, const char* key, int value) {
        if (ReadIniInt(path, key) == nlohmann::json(value)) ++matches;
    };

    matchIni("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKey", 100);             // F13
    {
        const std::filesystem::path dmenu = "Data/SKSE/Plugins/dmenu/dmenu.ini";
        const auto mkb = ReadIniInt(dmenu, "key_toggle_dmenu_mkb");
        if ((!mkb.is_null() && mkb == nlohmann::json(101)) ||
            (mkb.is_null() && ReadIniInt(dmenu, "key_toggle_dmenu") == nlohmann::json(101)))
            ++matches; // F14 in either the active MKB field or the legacy field
    }
    matchIni("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", 126);     // F15
    matchIni("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys", 103);          // F16
    matchIni("Data/KreatE/UserSettings.ini", "GUIToggleKeys", 129);                           // F18

    if (auto flick = FindFLICKKeybinds(); !flick.empty()) {
        matchIni(flick, "iToggleFUCK_Key", 104);                                               // F17
    }

    const bool detected = matches >= 2;
    if (detected) {
        SKSE::log::warn("HotkeyBackup: detected a pre-backup Risa relocation signature ({} matches); "
                        "historical parking keys will use upstream defaults on restore.", matches);
    }
    return detected;
}

// Record each managed mod's ORIGINAL hotkey before we ever relocate it. Reads straight from disk,
// so it must run before any TryManage*/ManageModHotkeys/ManageReShadeHotkey call.
static void CaptureOriginalHotkeys() {
    LoadHotkeyBackup();
    const bool legacyRelocations = g_HotkeyBackup.empty() && DetectLegacyRisaRelocations();

    // These are not key codes, but Risa changes them as part of hotkey management and must
    // therefore restore their exact original values too. A value that already equals OUR managed
    // value is never a real original (the install was already relocated when the backup was first
    // written), so we NEVER record it — restore then falls back to the correct upstream default.
    // This is unconditional: the old legacy-detection heuristic could miss an already-relocated
    // install and poison the backup with our own sentinels (e.g. IED.ToggleKeys=103).
    if (auto overrideKeys = ReadIniText("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "OverrideToggleKeys");
        overrideKeys.is_string()) {
        if (!legacyRelocations || ToUpper(overrideKeys.get<std::string>()) != "TRUE")
            CaptureOriginal("IED.OverrideToggleKeys", overrideKeys);
        else
            SKSE::log::warn("HotkeyBackup: SKIP IED.OverrideToggleKeys = true (matches our managed value - not a real original).");
    }

    const std::string oar = "Data/SKSE/Plugins/OpenAnimationReplacer.ini";
    // OAR relocates uToggleUIKey to 100 (F13). Guard the whole chord on the main key.
    if (ReadIniInt(oar, "uToggleUIKey") != nlohmann::json(100)) {
        CaptureOriginal("OAR.uToggleUIKey",      ReadIniInt(oar, "uToggleUIKey"));
        CaptureOriginal("OAR.uToggleUIKeyShift", ReadIniInt(oar, "uToggleUIKeyShift"));
        CaptureOriginal("OAR.uToggleUIKeyCtrl",  ReadIniInt(oar, "uToggleUIKeyCtrl"));
        CaptureOriginal("OAR.uToggleUIKeyAlt",   ReadIniInt(oar, "uToggleUIKeyAlt"));
    } else SKSE::log::warn("HotkeyBackup: OAR already relocated (uToggleUIKey=100); not capturing.");

    // dMenu is managed only through the v2 API (keyless, live); Risa never edits dmenu.ini, so there
    // is nothing to capture/restore for it here.

    CaptureOriginal("IC.MenuKey",
        ReadIniInt("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey"), 126);   // 126 = F15

    CaptureOriginal("IED.ToggleKeys",
        ReadIniInt("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys"), 103);        // 103 = F16

    CaptureOriginal("KreatE.GUIToggleKeys",   ReadIniInt("Data/KreatE/UserSettings.ini", "GUIToggleKeys"), 129);            // 129 = F18

    if (auto flick = FindFLICKKeybinds(); !flick.empty()) {
        const auto flickKey = ReadIniInt(flick, "iToggleFUCK_Key");
        if (legacyRelocations && flickKey == nlohmann::json(104))
            SKSE::log::warn("HotkeyBackup: SKIP FLICK.iToggleFUCK_Key = 104 (historical Risa F17 relocation).");
        else
            CaptureOriginal("FLICK.iToggleFUCK_Key", flickKey, 0);                                          // 0 = disabled
    }

    // Community Shaders (relocates ToggleKey to VK_F19=0x88). Guard on the main key.
    for (const char* p : { "Data/SKSE/Plugins/CommunityShaders/SettingsUser.json",
                           "Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json" }) {
        if (!std::filesystem::exists(p)) continue;
        const std::string tag = std::string("CS.") + (std::string(p).find("User") != std::string::npos ? "User." : "Default.");
        if (ReadJsonValue(p, "Menu", "ToggleKey") != nlohmann::json(VK_F19)) {
            CaptureOriginal(tag + "ToggleKey",         ReadJsonValue(p, "Menu", "ToggleKey"));
            CaptureOriginal(tag + "CSEditorToggleKey", ReadJsonValue(p, "Menu", "CSEditorToggleKey"));
            CaptureOriginal(tag + "OverlayToggleKey",  ReadJsonValue(p, "Menu", "OverlayToggleKey"));
            CaptureOriginal(tag + "EffectToggleKey",   ReadJsonValue(p, "Menu", "EffectToggleKey"));
        } else SKSE::log::warn("HotkeyBackup: CS already relocated in {}; not capturing.", p);
    }

    // ReShade: full "VK,ctrl,shift,alt" string. Skip our relocations (F22 = "133,0,0,0" or disabled "0,0,0,0").
    if (auto ov = ReadReShadeOverlay(); !ov.is_null()) {
        const std::string s = ov.get<std::string>();
        if (s.rfind("133", 0) == 0 || s.rfind("0,", 0) == 0 || s == "0")
            SKSE::log::warn("HotkeyBackup: SKIP ReShade.KeyOverlay = {} (our relocation/disabled value).", s);
        else CaptureOriginal("ReShade.KeyOverlay", ov);
    }
    // Debug Menu (uOpenMenuHotkey, DIK). Relocated on a launcher-key collision to a pressable
    // Scroll Lock (0x46) or legacy F17/F20 (older builds used a pressable F2-F8). A value equal to
    // any of our relocation keys is never a real user setting, so never record it as the original.
    {
        std::filesystem::path dm = "Data/MCM/Settings/DebugMenu.ini";
        if (!std::filesystem::exists(dm)) dm = "Data/MCM/Config/DebugMenu/settings.ini";
        if (auto v = ReadIniInt(dm, "uOpenMenuHotkey"); v.is_number_integer()) {
            const int iv = v.get<int>();
            if ((legacyRelocations && (iv == 0x3C || iv == 0x3D || iv == 0x3E || iv == 0x40 || iv == 0x42)) || iv == 0x68 || iv == 0x6B || iv == 0x46)
                SKSE::log::warn("HotkeyBackup: SKIP DebugMenu.uOpenMenuHotkey = {} (our relocation key, not a real original).", iv);
            else CaptureOriginal("DebugMenu.uOpenMenuHotkey", v);
        }
    }
    // CatMenu (JSON root toggle_key). Relocated to 592 (ImGuiKey_F21).
    const auto catMenuKey = ReadJsonValue("Data/SKSE/Plugins/catmenu/settings.json", "", "toggle_key");
    if (legacyRelocations && catMenuKey == nlohmann::json(591))
        SKSE::log::warn("HotkeyBackup: SKIP CatMenu.toggle_key = 591 (historical Risa F20 relocation).");
    else
        CaptureOriginal("CatMenu.toggle_key", catMenuKey, 592);
    // Mod Function Menu (TOML [Controls.Keyboard] iHotkey, a DIK). Relocated to 107 (F20); skip that.
    {
        const int mfmKey = ReadModFunctionMenuHotkey();
        if (mfmKey == 0x6B)
            SKSE::log::warn("HotkeyBackup: SKIP ModFunctionMenu.iHotkey = 107 (our F20 relocation, not a real original).");
        else
            CaptureOriginal("ModFunctionMenu.iHotkey", nlohmann::json(mfmKey), 59);
    }
    // Dragonborn's Toolkit (JSON root toggleKey, normally a string like "F1"). Disabled = int 0.
    const auto dragonbornKey = ReadJsonValue("Data/SKSE/Plugins/SkyrimCheatMenu.json", "", "toggleKey");
    if (legacyRelocations && dragonbornKey == nlohmann::json(108))
        SKSE::log::warn("HotkeyBackup: SKIP Dragonborn.toggleKey = 108 (historical Risa F21 relocation).");
    else
        CaptureOriginal("Dragonborn.toggleKey", dragonbornKey, 0);

    SKSE::log::info("HotkeyBackup: capture pass complete ({} entries total).", g_HotkeyBackup.size());
}

// True once we've taken dMenu's toggle key over through the v2 API (so we must NOT also touch/
// restore dmenu.ini for it — the ini was never modified).
static std::atomic<bool> g_DMenuManagedViaApi{ false };
// The user's own dMenu keyboard key, captured live the first time we sync (before we zero it),
// so "enable original hotkey" restores exactly what they had rather than a guessed default.
static std::atomic<std::uint32_t> g_DMenuOrigKeyMkb{ 199 };  // 199 = Home
static std::atomic<bool> g_DMenuOrigKeyCaptured{ false };

// Drive dMenu's keyboard toggle key live via the API v2: 0 while blocked (frees Home), or the
// user's captured key when they enable the original hotkey. No ini edit, no restart. Safe to call
// every frame/second — it only writes when the value actually needs to change.
static void SyncDMenuKeyViaApi() {
    if (!HasDMenuV2Api()) return;
    const auto* api = g_DMenuApi.load();
    if (api->IsReady && !api->IsReady()) return;  // wait until dMenu has loaded its own settings

    if (!g_DMenuOrigKeyCaptured.exchange(true)) {
        const std::uint32_t cur = api->GetToggleKeyMkb();
        if (cur != 0) g_DMenuOrigKeyMkb.store(cur);  // remember the real configured key
    }

    const std::uint32_t want = g_UnblockDMenu.load() ? g_DMenuOrigKeyMkb.load() : 0u;
    if (api->GetToggleKeyMkb() != want) {
        api->SetToggleKeyMkb(want);
        SKSE::log::info("SyncDMenuKeyViaApi: set dMenu keyboard toggle to {} ({}).",
            want, want == 0 ? "disabled — Home free" : "restored user key");
    }
    g_DMenuManagedViaApi.store(true);
}

static void ManageModHotkeys() {
    bool any = false;
    if (g_OARConfig.enabled && !g_ExcludeMod[AI_OAR].load()) {
        const auto ini = std::filesystem::path("Data/SKSE/Plugins/OpenAnimationReplacer.ini");
        // Park OAR's native key on the unpressable F13, NOT 0 — scancode/IDCode 0 is the LEFT MOUSE
        // BUTTON, so uToggleUIKey=0 made OAR open on every left-click. OAR itself is opened directly
        // via SetOAROpen, so this key just needs to be something no physical input can ever produce.
        const int key = SetIniValue(ini, "uToggleUIKey", 0x64); // F13
        const int ctrl = SetIniValue(ini, "uToggleUIKeyCtrl", 0);
        const int shift = SetIniValue(ini, "uToggleUIKeyShift", 0);
        const int alt = SetIniValue(ini, "uToggleUIKeyAlt", 0);
        if (key >= 0 && ctrl >= 0 && shift >= 0 && alt >= 0) {
            g_OARConfig.toggleDIK = 0x64;
            g_OARConfig.toggleVK = VKFromDIK(0x64);
            g_OARConfig.ctrl = false;
            g_OARConfig.shift = false;
            g_OARConfig.alt = false;
        }
        if (key == 1 || ctrl == 1 || shift == 1 || alt == 1) {
            any = true;
            SKSE::log::info("ManageModHotkeys: disabled OAR's native UI chord; opened directly through UIManager.");
        }
    }
    if (g_DMenuConfig.enabled && !g_ExcludeMod[AI_DMenu].load()) {
        // Preferred path: dMenu NG API v2 lets us free (or restore) dMenu's keyboard toggle key
        // LIVE — no dmenu.ini edit and no restart. SyncDMenuKeyViaApi() (driven every second and
        // gated on dMenu being ready) sets the key to 0 while blocked and back to the user's own
        // key when they enable the original hotkey. Because we never touch the ini, there is
        // nothing to undo on uninstall.
        // dMenu is supported ONLY through the dMenu NG v2 API (SetToggleKeyMkb) - keyless, live, no ini
        // edit, no restart. Stock/older dMenu without the API is no longer supported (we don't fall back
        // to editing dmenu.ini). If the API isn't present, we simply leave dMenu alone.
        AcquireDMenuApiFromExport();
        if (HasDMenuV2Api()) {
            SyncDMenuKeyViaApi();
            SKSE::log::info("ManageModHotkeys: dMenu key managed live via API v2 (Home freed, no ini edit, no restart).");
        } else {
            SKSE::log::info("ManageModHotkeys: dMenu NG v2 API not present; dMenu left unmanaged (stock dMenu ini fallback dropped).");
        }
    }
    if (g_ImprovedCameraConfig.enabled && !g_ExcludeMod[AI_IC].load()) {
        if (SetIniValue("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", 0x24, true) == 1) { // Home
            any = true; SKSE::log::info("ManageModHotkeys: Improved Camera restored to Home; launcher opens its UIMenu directly.");
        }
    }
    if (g_IEDConfig.enabled && !g_ExcludeMod[AI_IED].load()) {
        // IED ignores its ini ToggleKeys by default (OverrideToggleKeys=false) and uses the key
        // set through its own UI — which no input hook can intercept, so physical Backspace kept
        // opening it. Force IED to honor the ini and park its listener on the unpressable F16
        // (DIK 0x67). Backspace is then fully freed; the launcher still opens IED via its native
        // render task, and the optional Backspace alias is bridged by this plugin. Takes effect on
        // the next launch (IED reads its ini before we run). Reverted to Backspace +
        // OverrideToggleKeys=false by RestoreAllModDefaults on uninstall.
        const std::filesystem::path ied = "Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini";
        const int keyR = SetIniValue(ied, "ToggleKeys", 0x67, true);            // F16
        const int ovrR = SetIniTextValue(ied, "OverrideToggleKeys", "true");
        if (keyR >= 0) {
            g_IEDConfig.toggleDIK = 0x67;
            g_IEDConfig.toggleVK = VKFromDIK(0x67);
        }
        if (keyR == 1 || ovrR == 1) {
            any = true;
            SKSE::log::info("ManageModHotkeys: parked IED on F16 (0x67) with OverrideToggleKeys=true; Backspace freed. Launcher opens it via the native render task. RESTART once.");
        }
    }
    if (g_FLICKConfig.enabled && !g_ExcludeMod[AI_FLICK].load()) {
        TryManageFLICKHotkey(false);
    }
    if (any) {
        MarkConfigRestartRequired("supported mod hotkeys");
        SKSE::log::info("ManageModHotkeys: mod hotkeys changed — RESTART Skyrim once for them to take effect.");
    }
}

// Post the close request instead of running "qqq" from the ImGui render callback. Executing
// qqq synchronously can tear game state down before SKSE Menu Framework finishes that callback.
static void RequestGameExit() {
    HWND hwnd = g_GameHWND.load();
    if (!hwnd) {
        if (auto* renderWindow = RE::BSGraphics::Renderer::GetCurrentRenderWindow()) {
            hwnd = reinterpret_cast<HWND>(renderWindow->hWnd);
        }
    }
    if (!hwnd || !::IsWindow(hwnd)) {
        SKSE::log::error("RequestGameExit: could not find Skyrim's game window.");
        return;
    }
    if (!::PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
        SKSE::log::error("RequestGameExit: PostMessageW(WM_CLOSE) failed with error {}.", ::GetLastError());
        return;
    }
    SKSE::log::info("RequestGameExit: deferred WM_CLOSE posted successfully.");
}

static bool IsModInstalled(int aliasIdx) {
    switch (aliasIdx) {
        case AI_MF:        return IsPluginPresent("SKSEMenuFramework");
        case AI_MCM:       return IsMCMAvailable();
        case AI_SearchUI:  return IsSearchUIAvailable();
        case AI_OAR:       return g_OARConfig.enabled;
        case AI_IED:       return g_IEDConfig.enabled;
        case AI_DebugMenu: return g_DebugMenuConfig.enabled;
        case AI_DMenu:     return g_DMenuConfig.enabled;
        case AI_IC:        return g_ImprovedCameraConfig.enabled;
        case AI_ENB:       return g_ENBConfig.enabled;
        case AI_FLICK:     return g_FLICKConfig.enabled;
        case AI_KreatE:    return g_KreatEConfig.enabled;
        case AI_CS:        return g_CSConfig.enabled;
        case AI_CSEditor:  return g_CSConfig.enabled;
        case AI_CSOverlay: return g_CSConfig.enabled;
        case AI_CSEffect:  return g_CSConfig.enabled;
        case AI_PartySettings: return g_PartySheetConfig.enabled;
        case AI_PartySheet: return g_PartySheetConfig.enabled;
        case AI_PartyInspect: return g_PartySheetConfig.enabled;
        case AI_PartyCharacter: return g_PartySheetConfig.enabled;
        case AI_CatMenu:   return g_CatMenuConfig.enabled;
        case AI_Dragonborn: return g_DragonbornConfig.enabled;
        case AI_ReShade:   return g_ReShadeConfig.enabled;
        case AI_QAR:       return IsQARInstalled();
        case AI_ModFunctionMenu: return g_ModFunctionMenuConfig.enabled;
        case AI_OPS:       return g_OPSConfig.enabled;
        default:           return false;
    }
}

// Friendly names for the exclusions/settings lists (g_AliasIds are terse internal ids).
static const char* GetModDisplayName(int aliasIdx) {
    switch (aliasIdx) {
        case AI_MF:            return "SKSE Menu Framework";
        case AI_MCM:           return "Mod Configuration Menu";
        case AI_SearchUI:      return "SearchUI";
        case AI_OAR:           return "Open Animation Replacer";
        case AI_IED:           return "Immersive Equipment Displays";
        case AI_DebugMenu:     return "Debug Menu";
        case AI_DMenu:         return "dMenu";
        case AI_IC:            return "Improved Camera";
        case AI_ENB:           return "ENB Editor";
        case AI_FLICK:         return "FLICK";
        case AI_KreatE:        return "KreatE";
        case AI_CS:            return "Community Shaders";
        case AI_CatMenu:       return "CatMenu";
        case AI_Dragonborn:    return "Dragonborn's Toolkit";
        case AI_ReShade:       return "ReShade";
        case AI_PartySettings: return "Skyrim Party Sheet";
        case AI_QAR:           return "Quick Armor Rebalance";
        case AI_ModFunctionMenu: return "Mod Function Menu";
        case AI_OPS:           return "Outfit Preview Selector";
        default:               return "";
    }
}

// True for mods that have no native game hotkey of their own (opened only through Risa / a shared menu),
// so the exclusions "Native Hotkey" column should read "None" rather than a scanned key. MCM opens
// through SkyUI's Journal menu - it has no hotkey of its own.
static bool ModHasNoNativeKey(int aliasIdx) {
    return aliasIdx == AI_MCM || aliasIdx == AI_QAR;
}

static std::string GetModNativeHotkeyString(int aliasIdx) {
    switch (aliasIdx) {
        case AI_OAR:
            return FormatHotkey(g_OARConfig.toggleDIK, g_OARConfig.ctrl, g_OARConfig.shift, g_OARConfig.alt);
        case AI_IED:
            return FormatHotkey(g_IEDConfig.toggleDIK);
        case AI_DMenu:
            return FormatModifiedHotkey(g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK);
        case AI_IC:
            {
                bool ctrl = g_ImprovedCameraConfig.modifierVK == VK_CONTROL;
                bool shift = g_ImprovedCameraConfig.modifierVK == VK_SHIFT;
                bool alt = g_ImprovedCameraConfig.modifierVK == VK_MENU;
                return FormatHotkey(g_ImprovedCameraConfig.toggleDIK, ctrl, shift, alt);
            }
        case AI_FLICK:
            return FormatHotkey(g_FLICKConfig.toggleDIK);
        case AI_DebugMenu:
            return FormatHotkey(g_DebugMenuConfig.toggleDIK);
        case AI_KreatE:
            return FormatHotkey(g_KreatEConfig.toggleDIK);
        case AI_CatMenu:
            return FormatHotkey(g_CatMenuConfig.toggleDIK);
        case AI_ModFunctionMenu:
            return FormatHotkey(g_ModFunctionMenuConfig.toggleDIK);
        case AI_OPS:
            return FormatHotkey(g_OPSConfig.toggleDIK);
        case AI_Dragonborn:
            return FormatHotkey(g_DragonbornConfig.toggleDIK);
        case AI_CS:
            return FormatHotkey(g_CSConfig.toggleDIK);
        case AI_CSEditor:
            return FormatModifiedHotkey(g_CSConfig.editorDIK, g_CSConfig.editorModifierDIK);
        case AI_CSOverlay:
            return FormatHotkey(g_CSConfig.overlayDIK);
        case AI_CSEffect:
            return FormatHotkey(g_CSConfig.effectDIK);
        case AI_ReShade:
            return FormatModifiedHotkey(g_ReShadeConfig.toggleDIK, g_ReShadeConfig.modifierDIK);
        case AI_SearchUI:
            return FormatHotkey(static_cast<WORD>(g_SearchUIOrigKey.load()));
        case AI_PartySettings:
            return FormatModifiedHotkey(g_PartySheetConfig.settingsDIK, g_PartySheetConfig.modifierDIK);
        case AI_MF:
            return "F1";
        default:
            return "None";
    }
}

static std::string GetExclusionDetailString(int aliasIdx) {
    switch (aliasIdx) {
        case AI_MF:
            return "Path: None (uses live SKSE Menu Framework API)\n"
                   "Default key: F1\n"
                   "Changed to: API SetHotkeyEnabled(false) called (disables the framework's own hotkey live, no INI edit at all)\n";
        case AI_OAR:
            return "Path: SKSE\\Plugins\\OpenAnimationReplacer.ini\n"
                   "Default key: Shift + O\n"
                   "Default values: uToggleUIKey = 24 | uToggleUIKeyShift = 1 | uToggleUIKeyCtrl = 0 | uToggleUIKeyAlt = 0\n"
                   "Changed to: uToggleUIKey = F13 (unpressable)\n";
        case AI_DMenu:
            return "Path: None (uses the dMenu NG v2 API)\n"
                   "Default key: Home\n"
                   "Changed to: API SetToggleKeyMkb(0) called (frees the key live, no INI edit at all). Requires the API-enabled dMenu build; stock/older dMenu without the API is not managed.\n";
        case AI_IC:
            return "Path: SKSE\\Plugins\\ImprovedCameraSE\\ImprovedCameraSE.ini\n"
                   "Default key: Left Shift + Home\n"
                   "Default value: MenuKey = 0x24\n"
                   "Changed to: Not relocated, the key stays Home and is hidden only from Improved Camera, so Shift + Home is free for everything else.\n";
        case AI_IED:
            return "Path: SKSE\\Plugins\\ImmersiveEquipmentDisplays.ini\n"
                   "Default key: Backspace\n"
                   "Default values: ToggleKeys = 0x0E | OverrideToggleKeys = false\n"
                   "Changed to: ToggleKeys = F16 (unpressable) | OverrideToggleKeys = true\n";
        case AI_FLICK:
            return "Path: RequestFUCK API SetHotkeyEnabled(false)\n"
                   "Default key: F7\n"
                   "Changed to: native hotkey disabled live; keybind files are not edited\n";
        case AI_DebugMenu:
            return "Path: MCM\\Settings\\DebugMenu.ini (or MCM\\Config\\DebugMenu\\settings.ini)\n"
                   "Default key: F1\n"
                   "Default value: uOpenMenuHotkey = 59\n"
                   "Changed to: uOpenMenuHotkey = F17 (unpressable)\n";
        case AI_KreatE:
            return "Path: KreatE\\UserSettings.ini\n"
                   "Default key: End\n"
                   "Default value: GUIToggleKeys = 0x23\n"
                   "Changed to: GUIToggleKeys = F18 (unpressable)\n";
        case AI_CS:
            return "Path: SKSE\\Plugins\\CommunityShaders\\SettingsUser.json (or SettingsDefault.json)\n"
                   "Default keys: End / Shift + End / F10 / Numpad *\n"
                   "Default values under \"Menu\": \"ToggleKey\": 35 | \"CSEditorToggleKey\": [16, 35] | \"OverlayToggleKey\": 121 | \"EffectToggleKey\": 106\n"
                   "Changed to: ToggleKey = F19 | CSEditorToggleKey = F15 | OverlayToggleKey = F23 | EffectToggleKey = F24 (all unpressable)\n";
        case AI_CatMenu:
            return "Path: SKSE\\Plugins\\catmenu\\settings.json\n"
                   "Default key: F6\n"
                   "Default value: \"toggle_key\": 577\n"
                   "Changed to: \"toggle_key\": F21\n";
        case AI_Dragonborn:
            return "Path: SKSE\\Plugins\\SkyrimCheatMenu.json\n"
                   "Default key: F1\n"
                   "Default value: \"toggleKey\": \"F1\"\n"
                   "Changed to: \"toggleKey\": 0\n";
        case AI_ReShade:
            return "Path: ReShade.ini in the game folder\n"
                   "Default key: Home\n"
                   "Default value: KeyOverlay=36,0,0,0\n"
                   "Modifiers: Ctrl = 0 | Shift = 0 | Alt = 0\n"
                   "Changed to: overlay key disabled (KeyOverlay=0); the add-on API opens the overlay, so Home stays free. Requires the ReShade add-on build; the non-add-on build is left unmanaged.\n";
        case AI_QAR:
            return "Path: None (opened through Papyrus call)\n"
                   "Default key: None\n"
                   "Changed to: Not managed (hotkey is unmapped by default in its MCM)\n";
        case AI_ModFunctionMenu:
            return "Path: SKSE\\Plugins\\ccld_ModFunctionMenu.toml\n"
                   "Default key: F1\n"
                   "Default value: [Controls.Keyboard] iHotkey = 59\n"
                   "Changed to: iHotkey = F20 (unpressable)\n";
        case AI_OPS:
            return "Path: None (MCM Hotkey property on the OutfitPreviewSe script, in the save)\n"
                   "Default key: Numpad 1\n"
                   "Default value: Hotkey = 79\n"
                   "Changed to: Hotkey = 0 (disabled). Opened through the mod's SKSE mod events; no file edit.\n";
        default:
            return "";
    }
}

static void CopyToClipboard(const std::string& text) {
    if (::OpenClipboard(nullptr)) {
        ::EmptyClipboard();
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hg) {
            char* ptr = static_cast<char*>(::GlobalLock(hg));
            if (ptr) {
                std::memcpy(ptr, text.c_str(), text.size() + 1);
                ::GlobalUnlock(hg);
                ::SetClipboardData(CF_TEXT, hg);
            }
        }
        ::CloseClipboard();
    }
}

static WORD GetModNativeKey(int aliasIdx) {
    switch (aliasIdx) {
        case AI_OAR:       return g_OARConfig.toggleDIK;
        case AI_IED:       return g_IEDConfig.toggleDIK;
        case AI_DMenu:     return g_DMenuConfig.toggleDIK;
        case AI_IC:        return g_ImprovedCameraConfig.toggleDIK;
        case AI_FLICK:     return g_FLICKConfig.toggleDIK;
        case AI_DebugMenu: return g_DebugMenuConfig.toggleDIK;
        case AI_KreatE:    return g_KreatEConfig.toggleDIK;
        case AI_CatMenu:   return g_CatMenuConfig.toggleDIK;
        case AI_ModFunctionMenu: return g_ModFunctionMenuConfig.toggleDIK;
        case AI_OPS:       return g_OPSConfig.toggleDIK;
        case AI_Dragonborn:return g_DragonbornConfig.toggleDIK;
        case AI_CS:        return g_CSConfig.toggleDIK;
        case AI_CSEditor:  return g_CSConfig.editorDIK;
        case AI_CSOverlay: return g_CSConfig.overlayDIK;
        case AI_CSEffect:  return g_CSConfig.effectDIK;
        case AI_ReShade:   return g_ReShadeConfig.toggleDIK;
        case AI_SearchUI:  return static_cast<WORD>(g_SearchUIOrigKey.load());
        case AI_PartySettings: return g_PartySheetConfig.settingsDIK;
        case AI_MF:        return 0x3B; // Default F1
        default:           return 0;
    }
}

static std::string GetBackupKeyString(int aliasIdx, bool forceModDefaults) {
    if (!g_HotkeyBackupLoaded) LoadHotkeyBackup();

    std::string backupId = "";
    nlohmann::json modDefaultVal = nullptr;

    switch (aliasIdx) {
        case AI_IED:       backupId = "IED.ToggleKeys";            modDefaultVal = 0x0E; break; // Backspace
        case AI_OAR:       backupId = "OAR.uToggleUIKey";          modDefaultVal = 24;   break; // Shift + O
        case AI_DMenu:     backupId = "dMenu.key_toggle_dmenu";    modDefaultVal = 199;  break; // Home
        case AI_IC:        backupId = "IC.MenuKey";                modDefaultVal = 0x24; break; // Shift + Home
        case AI_FLICK:     backupId = "FLICK.iToggleFUCK_Key";     modDefaultVal = 65;   break; // F7
        case AI_DebugMenu: backupId = "DebugMenu.uOpenMenuHotkey"; modDefaultVal = 59;   break; // F1
        case AI_KreatE:    backupId = "KreatE.GUIToggleKeys";      modDefaultVal = VK_END; break;
        case AI_CatMenu:   backupId = "CatMenu.toggle_key";        modDefaultVal = 577;  break; // ImGui F6
        case AI_ModFunctionMenu: backupId = "ModFunctionMenu.iHotkey"; modDefaultVal = 59; break; // F1
        case AI_OPS:       backupId = "OPS.Hotkey";                modDefaultVal = 79;   break; // Numpad 1
        case AI_Dragonborn:backupId = "Dragonborn.toggleKey";      modDefaultVal = "F1"; break;
        case AI_ReShade:   backupId = "ReShade.KeyOverlay";        modDefaultVal = "36,0,0,0"; break; // Home
        case AI_SearchUI:  return NameFromDIK(0x3E); // SearchUI uses F4
        case AI_MF:        return "F1";
        case AI_PartySettings: return "X";
        case AI_MCM:       return "None";
        case AI_QAR:       return "None";
    }

    nlohmann::json val = modDefaultVal;
    if (!forceModDefaults && !backupId.empty() && g_HotkeyBackup.contains(backupId)) {
        val = g_HotkeyBackup[backupId];
    }

    if (val.is_null()) {
        val = modDefaultVal;
    }

    if (val.is_null()) return "Default";

    if (backupId == "ReShade.KeyOverlay" && val.is_string()) {
        auto parts = SplitStr(val.get<std::string>(), ',');
        if (!parts.empty()) return NameFromDIK(DIKFromVK(std::stoi(parts[0])));
    }
    if (val.is_number_integer()) {
        int code = val.get<int>();
        if (backupId == "IC.MenuKey" || backupId == "KreatE.GUIToggleKeys" || backupId.find("CS.User") == 0) {
            return NameFromDIK(DIKFromVK(code));
        }
        if (backupId == "CatMenu.toggle_key") {
            if (code == 577) return "F6";
            if (code == 592) return "F21";
            return "Key " + std::to_string(code);
        }
        return NameFromDIK(code);
    }
    if (val.is_string()) return val.get<std::string>();

    return "Default";
}

static bool RestoreModDefaults(int aliasIdx, bool forceModDefaults) {
    if (!g_HotkeyBackupLoaded) LoadHotkeyBackup();
    bool allWritesSucceeded = true;
    g_InRestore.store(true);

    const auto track = [&](const std::filesystem::path& path, int result) {
        if (result <= -2) {
            allWritesSucceeded = false;
        }
    };
    auto ini = [&](const char* path, const char* key, const char* id, int def, bool hex = false) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : def;
        const int r = SetIniValue(path, key, v, hex);
        track(path, r);
    };
    auto jkey = [&](const char* path, const char* section, const char* key, const char* id, const nlohmann::json& def) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && !o.is_null();
        const nlohmann::json& v = fromBak ? o : def;
        const int r = SetJsonKeyValue(path, section, key, v);
        track(path, r);
    };
    auto text = [&](const char* path, const char* key, const char* id, const char* def) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && o.is_string();
        const std::string v = fromBak ? o.get<std::string>() : std::string(def);
        const int r = SetIniTextValue(path, key, v);
        track(path, r);
    };

    switch (aliasIdx) {
        case AI_MF:
            SKSEMenuFramework::SetHotkeyEnabled(true);
            SKSE::log::info("RestoreModDefaults: enabled SKSE Menu Framework hotkey via API.");
            break;
        case AI_OAR:
            ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKey",      "OAR.uToggleUIKey",      24);
            ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyShift", "OAR.uToggleUIKeyShift", 1);
            ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyCtrl",  "OAR.uToggleUIKeyCtrl",  0);
            ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyAlt",   "OAR.uToggleUIKeyAlt",   0);
            break;
        case AI_DMenu:
            // dMenu is managed only through the v2 API now; Risa never edits dmenu.ini, so there is
            // nothing in the ini to restore. Just push the key back through the API (keyless, live):
            // the user's captured original for "restore my settings", or Home (DIK 199) for defaults.
            if (HasDMenuV2Api()) {
                if (const auto* api = g_DMenuApi.load())
                    api->SetToggleKeyMkb((g_DMenuManagedViaApi.load() && !forceModDefaults) ? g_DMenuOrigKeyMkb.load() : 199);
            }
            break;
        case AI_IC:
            ini("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", "IC.MenuKey", 0x24, true);
            break;
        case AI_IED:
            ini("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys", "IED.ToggleKeys", 0x0E, true);
            text("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "OverrideToggleKeys", "IED.OverrideToggleKeys", "false");
            break;
        case AI_FLICK:
            SetFLICKHotkeyEnabled(true);
            break;
        case AI_DebugMenu:
            {
                std::filesystem::path dm = "Data/MCM/Settings/DebugMenu.ini";
                if (!std::filesystem::exists(dm)) dm = "Data/MCM/Config/DebugMenu/settings.ini";
                nlohmann::json o = GetOriginal("DebugMenu.uOpenMenuHotkey");
                const bool fromBak = !forceModDefaults && o.is_number_integer();
                const int v = fromBak ? o.get<int>() : 59;
                const int r = SetIniValue(dm, "uOpenMenuHotkey", v);
                track(dm, r);
            }
            break;
        case AI_KreatE:
            if (std::filesystem::exists("Data/KreatE/UserSettings.ini")) {
                nlohmann::json o = GetOriginal("KreatE.GUIToggleKeys");
                const bool fromBak = !forceModDefaults && o.is_number_integer();
                const int v = fromBak ? o.get<int>() : VK_END;
                const int r = SetIniValue("Data/KreatE/UserSettings.ini", "GUIToggleKeys", v, true);
                track("Data/KreatE/UserSettings.ini", r);
            }
            break;
        case AI_CS:
        case AI_CSEditor:
        case AI_CSOverlay:
        case AI_CSEffect:
            if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json")) {
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "ToggleKey",         "CS.User.ToggleKey", 35);
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "CSEditorToggleKey", "CS.User.CSEditorToggleKey", nlohmann::json::array({ VK_SHIFT, VK_END }));
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "OverlayToggleKey",  "CS.User.OverlayToggleKey", VK_F10);
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "EffectToggleKey",   "CS.User.EffectToggleKey", VK_MULTIPLY);
            }
            if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json")) {
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "ToggleKey",         "CS.Default.ToggleKey", 35);
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "CSEditorToggleKey", "CS.Default.CSEditorToggleKey", nlohmann::json::array({ VK_SHIFT, VK_END }));
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "OverlayToggleKey",  "CS.Default.OverlayToggleKey", VK_F10);
                jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "EffectToggleKey",   "CS.Default.EffectToggleKey", VK_MULTIPLY);
            }
            break;
        case AI_CatMenu:
            {
                nlohmann::json o = GetOriginal("CatMenu.toggle_key");
                const bool fromBak = !forceModDefaults && o.is_number_integer();
                const nlohmann::json v = fromBak ? o : nlohmann::json(577);
                const int r = SetJsonRootValue("Data/SKSE/Plugins/catmenu/settings.json", "toggle_key", v);
                track("Data/SKSE/Plugins/catmenu/settings.json", r);
            }
            break;
        case AI_ModFunctionMenu:
            {
                nlohmann::json o = GetOriginal("ModFunctionMenu.iHotkey");
                const bool fromBak = !forceModDefaults && o.is_number_integer();
                const int v = fromBak ? o.get<int>() : 59; // F1
                const int r = SetModFunctionMenuHotkey("Data/SKSE/Plugins/ccld_ModFunctionMenu.toml", v);
                track("Data/SKSE/Plugins/ccld_ModFunctionMenu.toml", r);
            }
            break;
        case AI_OPS:
            // OPS's hotkey lives in the save (MCM Hotkey property), not a file. Hand the key back by
            // setting the property to the saved value (or Numpad 1). No-op if the quest isn't bound yet.
            {
                nlohmann::json o = GetOriginal("OPS.Hotkey");
                const bool fromBak = !forceModDefaults && o.is_number_integer();
                const int v = fromBak ? o.get<int>() : 79; // Numpad 1
                if (SetOPSHotkey(v))
                    SKSE::log::info("RestoreModDefaults: restored Outfit Preview Selector Hotkey = {}.", v);
            }
            break;
        case AI_Dragonborn:
            // Dragonborn's Toolkit v0.5+ is managed only through its runtime API - Risa never edits
            // SkyrimCheatMenu.json, so there is nothing in the file to restore. Just hand the key back
            // live by re-enabling its native listener. Only older builds fall back to the JSON write.
            if (SetDragonbornHotkeyEnabled(true)) {
                SKSE::log::info("RestoreModDefaults: re-enabled Dragonborn's Toolkit native hotkey via runtime API.");
            } else {
                nlohmann::json o = GetOriginal("Dragonborn.toggleKey");
                const bool fromBak = !forceModDefaults && !o.is_null();
                const nlohmann::json v = fromBak ? o : nlohmann::json("F1");
                const int r = SetJsonRootValue("Data/SKSE/Plugins/SkyrimCheatMenu.json", "toggleKey", v);
                track("Data/SKSE/Plugins/SkyrimCheatMenu.json", r);
            }
            break;
        case AI_ReShade:
            if (const auto rs = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" }); !rs.empty()) {
                nlohmann::json o = GetOriginal("ReShade.KeyOverlay");
                const bool fromBak = !forceModDefaults && o.is_string();
                const std::string val = fromBak ? o.get<std::string>() : std::string("36,0,0,0");
                std::vector<std::string> lines;
                bool found = false;
                {
                    std::ifstream f(rs);
                    std::string line;
                    while (std::getline(f, line)) {
                        const auto eq = line.find('=');
                        if (eq != std::string::npos && ToUpper(TrimStr(line.substr(0, eq))) == "KEYOVERLAY") {
                            line = "KeyOverlay=" + val; found = true;
                        }
                        lines.push_back(line);
                    }
                }
                if (!found) lines.push_back("KeyOverlay=" + val);
                std::ofstream f(rs, std::ios::trunc);
                if (f.is_open()) {
                    for (const auto& l : lines) f << l << "\n";
                } else {
                    allWritesSucceeded = false;
                }
            }
            break;
        case AI_SearchUI:
            break;
        case AI_QAR:
            break;
    }

    g_InRestore.store(false);
    return allWritesSucceeded;
}

static void CommitExclusions(bool useUserDefaults) {
    g_TempExcludeState[AI_MCM] = false;
    for (int i = 0; i < AI_COUNT; ++i) {
        if (i == AI_MCM) {
            g_ExcludeMod[i].store(false);
            continue;
        }
        const bool oldExcluded = g_ExcludeMod[i].load();
        const bool newExcluded = g_TempExcludeState[i];

        g_ExcludeMod[i].store(newExcluded);

        if (newExcluded && !oldExcluded) {
            RestoreModDefaults(i, !useUserDefaults);
        }
        if (i == AI_MF) {
            SKSEMenuFramework::SetHotkeyEnabled(newExcluded);
            SKSE::log::info("CommitExclusions: set SKSE Menu Framework hotkey enabled to {} via API.", newExcluded);
        }
    }
    SaveButtonOrder();
}

// Revert every managed mod's hotkey back to its original default — undoes all the edits this
// mod made. Intended as "uninstall prep": run it, restart once, then it's safe to disable the
// mod and every other mod behaves as if Risa's menu had never touched it. Files that don't
// exist are skipped automatically.
static bool RestoreAllModDefaults(bool forceModDefaults) {
    if (!g_HotkeyBackupLoaded) LoadHotkeyBackup();
    bool allWritesSucceeded = true;
    g_InRestore.store(true); // audit-log everything written below as REVERT (cleared at the end)
    g_ManagementSuspended.store(true); // stop re-managing so restored keys (Debug Menu -> F1) stay put
    SKSE::log::info("=== RestoreAllModDefaults START (mode: {}) ===",
        forceModDefaults ? "supported mod defaults" : "captured originals with default fallbacks");
    if (g_FileChangeLog) {
        g_FileChangeLog->info("--- RestoreAllModDefaults: mode={} ---",
            forceModDefaults ? "SUPPORTED MOD DEFAULTS" : "CAPTURED ORIGINALS");
        g_FileChangeLog->flush();
    }

    // Only a genuine write failure (-2 = the file exists but could not be opened for writing) counts
    // as an incomplete restore. -1 means "file or key not present" — i.e. there was simply nothing to
    // revert (e.g. an OpenAnimationReplacer.ini that never had uToggleUIKeyCtrl/Alt), which is normal
    // and must NOT be reported as a failure.
    const auto track = [&](const std::filesystem::path& path, int result) {
        if (result <= -2) {
            allWritesSucceeded = false;
            SKSE::log::error("Restore: failed to WRITE existing file {} (revert incomplete).", path.string());
        }
    };

    // Restore an int ini key from backup if we captured it, else from the given default.
    auto ini = [&](const char* path, const char* key, const char* id, int def, bool hex = false) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : def;
        const int r = SetIniValue(path, key, v, hex);
        track(path, r);
        SKSE::log::info("Restore[{}] {} = {} (result {}) <- {}", fromBak ? "BACKUP" : "default", key, v, r, id);
    };
    // Restore a JSON key from backup if captured, else default.
    auto jkey = [&](const char* path, const char* section, const char* key, const char* id, const nlohmann::json& def) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && !o.is_null();
        const nlohmann::json& v = fromBak ? o : def;
        const int r = SetJsonKeyValue(path, section, key, v);
        track(path, r);
        SKSE::log::info("Restore[{}] {}.{} = {} (result {}) <- {}", fromBak ? "BACKUP" : "default", section, key, v.dump(), r, id);
    };
    // Restore a text ini key from backup if captured, otherwise use the upstream default.
    auto text = [&](const char* path, const char* key, const char* id, const char* def) {
        nlohmann::json o = GetOriginal(id);
        const bool fromBak = !forceModDefaults && o.is_string();
        const std::string v = fromBak ? o.get<std::string>() : std::string(def);
        const int r = SetIniTextValue(path, key, v);
        track(path, r);
        SKSE::log::info("Restore[{}] {} = {} (result {}) <- {}", fromBak ? "BACKUP" : "default", key, v, r, id);
    };

    SKSEMenuFramework::SetHotkeyEnabled(true);
    SKSE::log::info("Restore[API] Enabled SKSE Menu Framework hotkey via API.");
    ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKey",      "OAR.uToggleUIKey",      24);
    ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyShift", "OAR.uToggleUIKeyShift", 1);
    ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyCtrl",  "OAR.uToggleUIKeyCtrl",  0);
    ini("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyAlt",   "OAR.uToggleUIKeyAlt",   0);
    // dMenu is managed only through the v2 API; dmenu.ini is never touched, so there is nothing to
    // undo. Just push the key back through the API (keyless, live): the user's captured original for
    // "restore my settings", or Home (DIK 199) for defaults.
    if (HasDMenuV2Api()) {
        if (const auto* api = g_DMenuApi.load())
            api->SetToggleKeyMkb((g_DMenuManagedViaApi.load() && !forceModDefaults) ? g_DMenuOrigKeyMkb.load() : 199);
        SKSE::log::info("Restore[API] dMenu key managed live; dmenu.ini left untouched.");
    }
    ini("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", "IC.MenuKey", 0x24, true);
    ini("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys", "IED.ToggleKeys", 0x0E, true);
    text("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "OverrideToggleKeys", "IED.OverrideToggleKeys", "false");

    SetFLICKHotkeyEnabled(true);

    {
        std::filesystem::path dm = "Data/MCM/Settings/DebugMenu.ini";
        if (!std::filesystem::exists(dm)) dm = "Data/MCM/Config/DebugMenu/settings.ini";
        nlohmann::json o = GetOriginal("DebugMenu.uOpenMenuHotkey");
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : 59; // F1
        const int r = SetIniValue(dm, "uOpenMenuHotkey", v);
        track(dm, r);
        SKSE::log::info("Restore[{}] DebugMenu uOpenMenuHotkey = {}", fromBak ? "BACKUP" : "default", v);
    }

    if (std::filesystem::exists("Data/KreatE/UserSettings.ini")) {
        nlohmann::json o = GetOriginal("KreatE.GUIToggleKeys");
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : VK_END;
        const int r = SetIniValue("Data/KreatE/UserSettings.ini", "GUIToggleKeys", v, true);
        track("Data/KreatE/UserSettings.ini", r);
        SKSE::log::info("Restore[{}] KreatE GUIToggleKeys = {}", fromBak ? "BACKUP" : "default", v);
    }

    if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json")) {
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "ToggleKey",         "CS.User.ToggleKey", 35);
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "CSEditorToggleKey", "CS.User.CSEditorToggleKey", nlohmann::json::array({ VK_SHIFT, VK_END }));
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "OverlayToggleKey",  "CS.User.OverlayToggleKey", VK_F10);
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "EffectToggleKey",   "CS.User.EffectToggleKey", VK_MULTIPLY);
    }
    if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json")) {
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "ToggleKey",         "CS.Default.ToggleKey", 35);
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "CSEditorToggleKey", "CS.Default.CSEditorToggleKey", nlohmann::json::array({ VK_SHIFT, VK_END }));
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "OverlayToggleKey",  "CS.Default.OverlayToggleKey", VK_F10);
        jkey("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "EffectToggleKey",   "CS.Default.EffectToggleKey", VK_MULTIPLY);
    }
    {
        nlohmann::json o = GetOriginal("CatMenu.toggle_key");
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const nlohmann::json v = fromBak ? o : nlohmann::json(577); // ImGuiKey_F6
        const int r = SetJsonRootValue("Data/SKSE/Plugins/catmenu/settings.json", "toggle_key", v);
        track("Data/SKSE/Plugins/catmenu/settings.json", r);
        SKSE::log::info("Restore[{}] CatMenu toggle_key = {}", fromBak ? "BACKUP" : "default", v.dump());
    }
    {
        nlohmann::json o = GetOriginal("ModFunctionMenu.iHotkey");
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : 59; // F1
        const int r = SetModFunctionMenuHotkey("Data/SKSE/Plugins/ccld_ModFunctionMenu.toml", v);
        track("Data/SKSE/Plugins/ccld_ModFunctionMenu.toml", r);
        SKSE::log::info("Restore[{}] ModFunctionMenu iHotkey = {}", fromBak ? "BACKUP" : "default", v);
    }
    {
        nlohmann::json o = GetOriginal("OPS.Hotkey");
        const bool fromBak = !forceModDefaults && o.is_number_integer();
        const int v = fromBak ? o.get<int>() : 79; // Numpad 1
        if (SetOPSHotkey(v))
            SKSE::log::info("Restore[{}] OPS Hotkey = {}", fromBak ? "BACKUP" : "default", v);
    }
    // Dragonborn's Toolkit v0.5+ is managed only through its runtime API - Risa never edits
    // SkyrimCheatMenu.json, so nothing in the file needs restoring. Hand the key back live by
    // re-enabling its native listener; only older builds fall back to the JSON write.
    if (SetDragonbornHotkeyEnabled(true)) {
        SKSE::log::info("Restore[API] Dragonborn native hotkey re-enabled via runtime API.");
    } else {
        nlohmann::json o = GetOriginal("Dragonborn.toggleKey");
        const bool fromBak = !forceModDefaults && !o.is_null();
        const nlohmann::json v = fromBak ? o : nlohmann::json("F1");
        const int r = SetJsonRootValue("Data/SKSE/Plugins/SkyrimCheatMenu.json", "toggleKey", v);
        track("Data/SKSE/Plugins/SkyrimCheatMenu.json", r);
        SKSE::log::info("Restore[{}] Dragonborn toggleKey = {}", fromBak ? "BACKUP" : "default", v.dump());
    }

    // ReShade: restore the captured "VK,ctrl,shift,alt" string if we have it, else Home (36,0,0,0).
    if (const auto rs = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" }); !rs.empty()) {
        nlohmann::json o = GetOriginal("ReShade.KeyOverlay");
        const bool fromBak = !forceModDefaults && o.is_string();
        const std::string val = fromBak ? o.get<std::string>() : std::string("36,0,0,0");
        std::vector<std::string> lines;
        bool found = false;
        {
            std::ifstream f(rs);
            std::string line;
            while (std::getline(f, line)) {
                const auto eq = line.find('=');
                if (eq != std::string::npos && ToUpper(TrimStr(line.substr(0, eq))) == "KEYOVERLAY") {
                    line = "KeyOverlay=" + val; found = true;
                }
                lines.push_back(line);
            }
        }
        if (found) {
            std::ofstream out(rs, std::ios::trunc);
            if (!out.is_open()) {
                allWritesSucceeded = false;
                SKSE::log::error("Restore: failed to open {} for writing.", rs.string());
            } else {
                for (const auto& l : lines) out << l << "\n";
                if (!out.good()) {
                    allWritesSucceeded = false;
                    SKSE::log::error("Restore: failed while writing {}.", rs.string());
                } else {
                    LogFileChange(rs.string(), std::format("KeyOverlay={}", val));
                }
            }
        }
        SKSE::log::info("Restore[{}] ReShade KeyOverlay = {} (found line: {})", fromBak ? "BACKUP" : "default", val, found);
    }

    // SearchUI's key is a live Papyrus property, not a file. Put it back to the user's original key and
    // let the live sync push it (SetHotkey via the VM). Point the alias at the original and enable it so
    // "want" resolves to the original key. Nothing on disk to revert.
    if (IsSearchUIAvailable()) {
        g_AliasDik[AI_SearchUI].store(static_cast<WORD>(g_SearchUIOrigKey.load()));
        g_UnblockSearchUI.store(true);
        SyncSearchUIKey();
        SKSE::log::info("Restore: SearchUI hotkey restored to original key {}.", g_SearchUIOrigKey.load());
    }

    SaveButtonOrder();

    if (allWritesSucceeded) {
        std::error_code ec;
        const bool removed = std::filesystem::remove(kHotkeyBackupPath, ec);
        if (ec || (!removed && std::filesystem::exists(kHotkeyBackupPath))) {
            allWritesSucceeded = false;
            SKSE::log::error("Restore: settings were restored, but could not remove {}: {}",
                kHotkeyBackupPath, ec ? ec.message() : "unknown error");
        } else {
            g_HotkeyBackup = nlohmann::json::object();
            g_HotkeyBackupLoaded = true;
            SKSE::log::info("Restore: removed completed originals backup {}; the next install will capture fresh settings.", kHotkeyBackupPath);
        }
    }
    if (g_FileChangeLog) {
        g_FileChangeLog->info("--- RestoreAllModDefaults: {}. {} ---",
            allWritesSucceeded ? "COMPLETE; originals backup removed" : "INCOMPLETE; originals backup kept for retry",
            allWritesSucceeded ? "Restart Skyrim once, then it is safe to remove the mod." : "Check the main log and retry before uninstalling.");
        g_FileChangeLog->flush();
    }
    g_InRestore.store(false);
    SKSE::log::info("=== RestoreAllModDefaults {} ===", allWritesSucceeded ? "DONE" : "INCOMPLETE");
    return allWritesSucceeded;
}

// ============================================================================
// SKSE messaging
// ============================================================================
static void MessageHandler(SKSE::MessagingInterface::Message* msg) {
    if (!msg) return;
    if (msg->type == dmenu_api::kMessageInterface) {
        if (msg->sender && (std::string_view(msg->sender) == "dmenu" || std::string_view(msg->sender) == "Simple Wheeler Menu")) {
            DMenuApiMessageHandler(msg);
        }
        return;
    }
    switch (msg->type) {

    case SKSE::MessagingInterface::kPostLoad: {
        LoadButtonOrder();
        LoadMFConfig();

        LoadOARConfig();
        LoadIEDConfig();
        LoadENBConfig();
        LoadDebugMenuConfig();
        LoadDMenuConfig();
        LoadImprovedCameraConfig();
        LoadFLICKConfig();
        LoadKreatEConfig();
        LoadCSConfig();
        LoadPartySheetConfig();
        LoadCatMenuConfig();
        LoadModFunctionMenuConfig();
        LoadOPSConfig();
        LoadDragonbornConfig();
        LoadReShadeConfig();
        TryManageKreatEHotkey();
        TryManageCSHotkey();
        TryManageCatMenuHotkey();
        TryManageModFunctionMenuHotkey();
        TryManageFLICKHotkey();
        TryManageOPSHotkey();
        TryManageDragonbornHotkey();
        // Retry add-on registration in case ReShade's add-on API wasn't ready at plugin load.
        // Still early enough (before the renderer/swapchain) to catch init_effect_runtime.
        if (!g_ReShadeAddonActive.load() && g_ReShadeConfig.enabled && RisaReShade::RegisterAddon()) {
            g_ReShadeAddonActive.store(true);
            SKSE::log::info("kPostLoad: registered as a ReShade add-on (late); overlay driven keylessly.");
        }
        ManageReShadeHotkey();         // disable ReShade's overlay key via the add-on API (non-add-on left unmanaged)
        ManageModHotkeys();            // disable OAR/FLICK native keys; relocate dMenu/IC/IED
        TryManageFLICKHotkey(true);     // FLICK enables its hotkeys at DataLoaded; reassert after that pass.
        StartFLICKHotkeyMonitor();
        // Debug Menu's collision was handled at the earliest plugin-load point.
        // NOTE: launcher key is intentionally NOT auto-reassigned — the user wants to keep F1.
        // F1 also being Debug Menu's key is fine: a PHYSICAL F1 toggles our launcher (Debug
        // Menu's sink filters it), while our SIMULATED F1 to open Debug Menu is let through to
        // Debug Menu only (its allow-window) and ignored by our launcher detection.
        if (!GetModuleHandle("SKSEMenuFramework")) {
            SKSE::log::error("kPostLoad: SKSEMenuFramework.dll not loaded."); return;
        }
        g_LauncherWindow = SKSEMenuFramework::AddWindow(RenderLauncher, false);

        if (!g_LauncherWindow) { SKSE::log::error("kPostLoad: AddWindow null."); return; }
        SKSE::log::info("kPostLoad: Launcher window registered.");

        const bool mfExcluded = g_ExcludeMod[AI_MF].load();
        SKSEMenuFramework::SetHotkeyEnabled(mfExcluded);
        SKSE::log::info("kPostLoad: set SKSE Menu Framework hotkey enabled to {} via API.", mfExcluded);
        if (g_ConfigRestartRequired.load()) {
            g_LauncherWindow->IsOpen.store(true);
            g_LauncherWindow->BlockUserInput.store(false);
            SKSE::log::info("kPostLoad: configuration changes detected; 10-second restart notice opened.");
        }
        g_FrameworkInputEvent = SKSEMenuFramework::AddInputEvent(FrameworkInputCallback);
        SKSE::log::info("kPostLoad: SKSE Menu Framework input callback registered.");
        InstallHooks();
        LogStartupDiagnostics();
        break;
    }

    case SKSE::MessagingInterface::kInputLoaded: {
        LoadOPSConfig();
        TryManageFLICKHotkey(true);
        TryManageOPSHotkey(true);
        auto* mgr = RE::BSInputDeviceManager::GetSingleton();
        if (mgr) {
            mgr->AddEventSink(RisaInputSink::GetSingleton());
            SKSE::log::info("kInputLoaded: BSTEventSink registered.");
            TryHookModSinks();
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(RisaMenuSink::GetSingleton());
            SKSE::log::info("kInputLoaded: Journal/MCM menu event sink registered.");
        }
        // Blocking is per-mod: sink-based mods are filtered at their own InputEvent sink
        // (TryHookModSinks, retried from the keyboard poll), and ENB via caller-scoped
        // GetAsyncKeyState/GetKeyState hooks.
        break;
    }

    default: break;
    }
}

// ============================================================================
// Plugin entry point
// ============================================================================
SKSEPluginInfo(
    .Version              = REL::Version{ 1, 5, 5, 0 },
    .Name                 = "RisaAllInOneMenu",
    .Author               = "Risa",
    .SupportEmail         = "",
    .StructCompatibility  = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion   = 0
);

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    g_RuntimeVersion = skse->RuntimeVersion().string(".");
    g_SKSEVersion = REL::Version::unpack(skse->SKSEVersion()).string(".");
    switch (REL::Module::GetRuntime()) {
        case REL::Module::Runtime::SE: g_RuntimeEdition = "SE"; break;
        case REL::Module::Runtime::AE: g_RuntimeEdition = "AE"; break;
        case REL::Module::Runtime::VR: g_RuntimeEdition = "VR"; break;
        default:                       g_RuntimeEdition = "Unknown"; break;
    }
    SKSE::log::info("RisaAllInOneMenu loaded.");
    SKSE::log::info("Diagnostics: plugin={}, Skyrim={} ({}), SKSE={}.",
        kRisaMenuVersion, g_RuntimeVersion, g_RuntimeEdition, g_SKSEVersion);

    // Register as a ReShade add-on as early as possible so we catch its init_effect_runtime event
    // (fired when ReShade creates its overlay runtime on swapchain init). Safe no-op if ReShade or
    // its add-on API is absent — the launcher then falls back to key relocation for ReShade.
    RisaReShade::SetOverlayStateCallback(&OnReShadeOverlayStateChanged);
    if (RisaReShade::RegisterAddon()) {
        g_ReShadeAddonActive.store(true);
        SKSE::log::info("RisaAllInOneMenu: registered as a ReShade add-on; overlay driven keylessly (Home + F22 freed).");
    }

    // Run before the managed plugins load so they observe collision-safe settings during
    // this launch. The normal full configuration pass still runs at kPostLoad.
    LoadButtonOrder();
    CaptureOriginalHotkeys(); // record each mod's ORIGINAL hotkey before we relocate anything
    RestoreLegacyKreatEBlocking();
    TryManageKreatEHotkey();
    TryManageCSHotkey();
    TryManageCatMenuHotkey();
    TryManageModFunctionMenuHotkey();
    TryManageFLICKHotkey();
    TryManageOPSHotkey();
    TryManageDragonbornHotkey();
    LoadDebugMenuConfig();
    ManageDebugMenuKey();

    auto* msg = SKSE::GetMessagingInterface();
    if (!msg || !msg->RegisterListener(MessageHandler)) {
        SKSE::log::error("Failed to register messaging listener."); return false;
    }
    // dMenu NG broadcasts its external-control API (sender "dmenu") at kPostLoad — handled via our general MessageHandler.
    return true;
}
