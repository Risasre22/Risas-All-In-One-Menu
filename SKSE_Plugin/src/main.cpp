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

// ============================================================================
// DIK (hardware scan code) + VK (virtual key) lookup table
// ============================================================================
struct KeyEntry { const char* name; WORD dik; WORD vk; };
static constexpr KeyEntry kKeyTable[] = {
    { "F1",  0x3B, VK_F1  }, { "F2",  0x3C, VK_F2  }, { "F3",  0x3D, VK_F3  },
    { "F4",  0x3E, VK_F4  }, { "F5",  0x3F, VK_F5  }, { "F6",  0x40, VK_F6  },
    { "F7",  0x41, VK_F7  }, { "F8",  0x42, VK_F8  }, { "F9",  0x43, VK_F9  },
    { "F10", 0x44, VK_F10 }, { "F11", 0x57, VK_F11 }, { "F12", 0x58, VK_F12 },
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
};

// ============================================================================
// MF INI config
// ============================================================================
using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
static GetAsyncKeyState_t g_OrigGetAsyncKeyState = nullptr;

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

// Minimal prefix of FLICK's official packed C API (FUCK_API.h). Keeping only the
// fields through SetMenuOpen/IsMenuOpen avoids depending on FLICK's ImGui types.
#pragma pack(push, 1)
struct FLICKInterface {
    std::uint32_t version;
    void* functionsBeforeSetMenuOpen[12];
    void (*SetMenuOpen)(bool);
    bool (*IsMenuOpen)();
};
#pragma pack(pop)

struct DurabilityConfig {
    bool enabled = false;
    int toggleVK = -1;
};
static DurabilityConfig g_DurabilityConfig;

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
    Durability
};
static std::atomic<ActiveMenu> g_ActiveMenu{ ActiveMenu::None };

static std::string TrimStr(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
static std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
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

static std::string NameFromDIK(WORD dik) {
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

static bool ParseBoolInt(const std::string& value) {
    const auto upper = ToUpper(TrimStr(value));
    return upper == "1" || upper == "TRUE" || upper == "YES" || upper == "ON";
}

static void LoadOARConfig() {
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

static WORD DIKFromVK(int vk) {
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return 0x2A;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return 0x1D;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return 0x38; // Alt
    
    UINT sc = ::MapVirtualKeyA(vk, 0); // MAPVK_VK_TO_VSC = 0
    return static_cast<WORD>(sc);
}

static bool IsUserTyping() {
    auto* ui = RE::UI::GetSingleton();
    return ui && (ui->IsMenuOpen("Console") || ui->IsMenuOpen("TextEntryMenu"));
}

static SKSEMenuFramework::Model::WindowInterface* g_LauncherWindow = nullptr;

static bool IsExternalMenuOpen() {
    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
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
    const std::filesystem::path ini = "Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini";
    if (!std::filesystem::exists(ini)) {
        SKSE::log::warn("LoadIEDConfig: ini not found.");
        return;
    }

    g_IEDConfig.enabled = true;

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

static void LoadDebugMenuConfig() {
    g_DebugMenuConfig.enabled = true; // default to true if dll is found

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
    g_DMenuConfig.enabled = true; // default to true if the dll is present

    // Default values
    g_DMenuConfig.toggleDIK = 199; // Home default (0xC7)
    g_DMenuConfig.toggleVK = VK_HOME;
    g_DMenuConfig.modifierDIK = 0;
    g_DMenuConfig.modifierVK = 0;

    const std::filesystem::path ini = "Data/SKSE/Plugins/dmenu/dmenu.ini";
    if (std::filesystem::exists(ini)) {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
            auto eq = line.find('='); if (eq == std::string::npos) continue;
            std::string key = ToUpper(TrimStr(line.substr(0, eq)));
            std::string val = TrimStr(line.substr(eq + 1));

            try {
                if (key == "KEY_TOGGLE_DMENU") {
                    g_DMenuConfig.toggleDIK = static_cast<WORD>(std::stoul(val));
                }
                if (key == "KEY_TOGGLE_MODIFIER") {
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
    g_ImprovedCameraConfig.enabled = true; // default to true if the dll is present

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
    g_FLICKConfig.enabled = (GetModuleHandleA("FUCK") != nullptr);
    if (!g_FLICKConfig.enabled) return;

    // Load key from FUCKs/FUCK/keybinds.ini
    const std::filesystem::path iniPath = "FUCKs/FUCK/keybinds.ini";
    if (std::filesystem::exists(iniPath)) {
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
    }
    g_FLICKConfig.toggleVK = VKFromDIK(g_FLICKConfig.toggleDIK);
    SKSE::log::info("LoadFLICKConfig: enabled={}, toggleDIK={:#x}, toggleVK={:#x}",
        g_FLICKConfig.enabled, g_FLICKConfig.toggleDIK, g_FLICKConfig.toggleVK);
}

static void LoadDurabilityConfig() {
    g_DurabilityConfig.enabled = (GetModuleHandleA("EquipmentDurabilitySystem-NG") != nullptr);
    if (!g_DurabilityConfig.enabled) return;

    const std::filesystem::path iniPath = "Data/SKSE/Plugins/EquipmentDurability/Settings.ini";
    if (std::filesystem::exists(iniPath)) {
        std::ifstream infile(iniPath);
        if (infile.is_open()) {
            std::string line;
            while (std::getline(infile, line)) {
                auto sc = line.find(';'); if (sc != std::string::npos) line = line.substr(0, sc);
                auto eq = line.find('='); if (eq == std::string::npos) continue;
                std::string key = ToUpper(TrimStr(line.substr(0, eq)));
                std::string val = TrimStr(line.substr(eq + 1));
                if (key == "TOGGLEKEYCODE") {
                    try {
                        g_DurabilityConfig.toggleVK = std::stoi(val);
                    } catch (...) {}
                }
            }
            infile.close();
        }
    }
    SKSE::log::info("LoadDurabilityConfig: enabled={}, toggleVK={}", g_DurabilityConfig.enabled, g_DurabilityConfig.toggleVK);
}

static bool IsImprovedCameraModifierDown() {
    if (!g_ImprovedCameraConfig.enabled || g_ImprovedCameraConfig.modifierVK == 0) return true;
    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };
    return (keyState(g_ImprovedCameraConfig.modifierVK) & 0x8000) != 0;
}

static std::atomic<float> g_WindowPosX{ -1.0f };
static std::atomic<float> g_WindowPosY{ -1.0f };
static std::atomic<float> g_LauncherFontScale{ 0.9f };
static std::atomic<WORD> g_LauncherHotkeyDIK{ 0x3B };  // Default F1 (0x3B)
static std::atomic<WORD> g_LauncherHotkeyVK{ 0x70 };   // Default VK_F1 (0x70)

static std::atomic<bool> g_UnblockOAR{ false };
static std::atomic<bool> g_UnblockIED{ false };
static std::atomic<bool> g_UnblockENB{ false };
static std::atomic<bool> g_UnblockDebugMenu{ false };
static std::atomic<bool> g_UnblockDMenu{ false };
static std::atomic<bool> g_UnblockImprovedCamera{ false };
static std::atomic<bool> g_UnblockFLICK{ false };
static std::atomic<bool> g_UnblockDurability{ false };

static std::atomic<bool> g_WaitingForHotkeyPress{ false };

static std::vector<std::string> g_ButtonOrder = {
    "MF", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "Durability"
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
        outfile << "FontScale = " << g_LauncherFontScale.load() << "\n";
        outfile << "WindowPosX = " << g_WindowPosX.load() << "\n";
        outfile << "WindowPosY = " << g_WindowPosY.load() << "\n";
        outfile << "HotkeyDIK = " << g_LauncherHotkeyDIK.load() << "\n";
        outfile << "UnblockOAR = " << (g_UnblockOAR.load() ? 1 : 0) << "\n";
        outfile << "UnblockIED = " << (g_UnblockIED.load() ? 1 : 0) << "\n";
        outfile << "UnblockENB = " << (g_UnblockENB.load() ? 1 : 0) << "\n";
        outfile << "UnblockDebugMenu = " << (g_UnblockDebugMenu.load() ? 1 : 0) << "\n";
        outfile << "UnblockDMenu = " << (g_UnblockDMenu.load() ? 1 : 0) << "\n";
        outfile << "UnblockImprovedCamera = " << (g_UnblockImprovedCamera.load() ? 1 : 0) << "\n";
        outfile << "UnblockFLICK = " << (g_UnblockFLICK.load() ? 1 : 0) << "\n";
        outfile << "UnblockDurability = " << (g_UnblockDurability.load() ? 1 : 0) << "\n";
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
                } else if (key == "FONTSCALE") {
                    try {
                        g_LauncherFontScale.store(std::stof(val));
                    } catch (...) {}
                } else if (key == "WINDOWPOSX") {
                    try { g_WindowPosX.store(std::stof(val)); } catch (...) {}
                } else if (key == "WINDOWPOSY") {
                    try { g_WindowPosY.store(std::stof(val)); } catch (...) {}
                } else if (key == "HOTKEYDIK") {
                    try {
                        WORD dik = (WORD)std::stoi(val);
                        g_LauncherHotkeyDIK.store(dik);
                        g_LauncherHotkeyVK.store(VKFromDIK(dik));
                    } catch (...) {}
                } else if (key == "UNBLOCKOAR") {
                    try { g_UnblockOAR.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKIED") {
                    try { g_UnblockIED.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKENB") {
                    try { g_UnblockENB.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKDEBUGMENU") {
                    try { g_UnblockDebugMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKDMENU") {
                    try { g_UnblockDMenu.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKIMPROVEDCAMERA") {
                    try { g_UnblockImprovedCamera.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKFLICK") {
                    try { g_UnblockFLICK.store(std::stoi(val) != 0); } catch (...) {}
                } else if (key == "UNBLOCKDURABILITY") {
                    try { g_UnblockDurability.store(std::stoi(val) != 0); } catch (...) {}
                }
            }
            infile.close();
        }
    }

    std::vector<std::string> defaultOrder = {
        "MF", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "Durability"
    };

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
    SKSE::log::info("LoadButtonOrder: Loaded button order successfully.");
}

// ============================================================================
// State
// ============================================================================
static SKSEMenuFramework::Model::InputEvent* g_FrameworkInputEvent = nullptr;
static std::atomic<bool> g_WaitForLauncherKeyRelease{ false };
static std::atomic<long long> g_LastLauncherToggleMs{ 0 };
static std::atomic<bool> g_AllowOAROpen{ false };
static std::atomic<long long> g_AllowOAROpenUntilMs{ 0 };

static std::atomic<bool> g_AllowIEDOpen{ false };
static std::atomic<int> g_AllowIEDOpenPolls{ 0 };
static std::atomic<long long> g_AllowIEDOpenUntilMs{ 0 };

static std::atomic<bool> g_AllowENBOpen{ false };
static std::atomic<long long> g_AllowENBOpenUntilMs{ 0 };
static std::atomic<long long> g_ENBOpenRequestedMs{ 0 };

static std::atomic<bool> g_AllowDebugMenuOpen{ false };
static std::atomic<long long> g_AllowDebugMenuOpenUntilMs{ 0 };
static std::atomic<int> g_DebugMenuPassCount{ 0 };
static std::atomic<bool> g_AllowESCSinkBlock{ false };

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

static constexpr WORD kEscapeDIK   = 0x01;  // ESC scan code

static long long NowMs();
static void SendOARHotkey();
static void AddScanInput(INPUT& input, WORD scanCode, bool keyUp);
static void CloseLauncher();
static void ForceCloseSKSEMenuFramework();
static void ToggleLauncher();
static void SimulateModifiedKey(WORD modifierDIK, WORD keyDIK);
static void PostKeyToGameWindow(WORD vk);
static void TryHookModSinks();
static FLICKInterface* GetFLICKInterface();
static bool IsENBOpeningTransition();

// ----------------------------------------------------------------------------
// Low-level keyboard hook state.
//   The mods we block (dMenu, FLICK, DebugMenu) read their toggle through the game
//   engine's InputEvent/ButtonEvent system, NOT through WM / GetAsyncKeyState / the
//   DI8 GetDeviceState array — so none of those hooks can suppress them. A
//   WH_KEYBOARD_LL hook sits beneath ALL of those channels and is the only place we
//   can reliably stop a physical key from reaching them. Injected (SendInput) keys
//   carry LLKHF_INJECTED, so our menu-button "open" simulations still pass through.
static HHOOK g_KbdLLHook = nullptr;
static std::atomic<bool> g_LLHookActive{ false };
static std::atomic<HWND>  g_GameHWND{ nullptr };
// Improved Camera shares plain Home with dMenu but reads it via GetAsyncKeyState. We
// open IC by synthesizing its key read for this short window (no real key is injected,
// so dMenu — which reads the engine ButtonEvent — never sees it).
static std::atomic<long long> g_ICSynthPressedUntilMs{ 0 };

// WndProc subclass state.
//   g_OrigWndProc  — the proc our FRONT instance chains down to (updated when we
//                    re-assert to the front after another mod chained on top of us).
//   g_GameWndProc  — the ORIGINAL game proc, captured once. Used as a fixed
//                    loop-breaker: when our proc is re-entered via the chain (a mod
//                    chained on top of our old proc), we jump straight here instead
//                    of looping back through the chain.
//   g_WndReentry   — per-thread re-entrancy depth guard.
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
    if (active == ActiveMenu::IED || active == ActiveMenu::OAR) {
        std::thread([]() {
            // NOTE: do NOT set g_AllowESCSinkBlock here — IED/OAR read ESC from the buffered
            // input, and that block strips ESC, preventing them from closing. They consume
            // ESC themselves while open, so the game won't pause.
            INPUT downInput{};
            AddScanInput(downInput, kEscapeDIK, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT upInput{};
            AddScanInput(upInput, kEscapeDIK, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            SKSE::log::info("CloseActiveModMenu: Closed IED/OAR via simulated ESC.");
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
        if (g_LauncherHotkeyVK.load() == g_DebugMenuConfig.toggleVK) {
            // Launcher key IS Debug Menu's key: the physical F1 that triggered this close
            // passes through Debug Menu's sink during this short window and closes it — do
            // NOT also simulate F1 (that would double-toggle and reopen it).
            g_AllowDebugMenuOpen.store(true);
            g_AllowDebugMenuOpenUntilMs.store(NowMs() + 200);
            g_DebugMenuPassCount.store(2);
            SKSE::log::info("CloseActiveModMenu: Closing DebugMenu via the physical hotkey (short window).");
        } else {
            g_AllowDebugMenuOpen.store(true);
            g_AllowDebugMenuOpenUntilMs.store(NowMs() + 1000);
            g_DebugMenuPassCount.store(2);
            
            std::thread([]() {
                INPUT downInput{};
                AddScanInput(downInput, g_DebugMenuConfig.toggleDIK, false);
                ::SendInput(1, &downInput, sizeof(INPUT));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                INPUT upInput{};
                AddScanInput(upInput, g_DebugMenuConfig.toggleDIK, true);
                ::SendInput(1, &upInput, sizeof(INPUT));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                g_AllowDebugMenuOpen.store(false);
                g_AllowDebugMenuOpenUntilMs.store(0);
                
                SKSE::log::info("CloseActiveModMenu: Closed DebugMenu via simulated key.");
            }).detach();
        }
    } else if (active == ActiveMenu::DMenu) {
        
        std::thread([]() {
            g_AllowDMenuOpen.store(true);
            g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);

            WORD combDIK = g_DMenuConfig.modifierDIK;
            WORD editDIK = g_DMenuConfig.toggleDIK;

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

            SKSE::log::info("CloseActiveModMenu: Closed dMenu.");
            
        }).detach();
    } else if (active == ActiveMenu::ImprovedCamera) {
        // IC's menu closes on ESC (not its open chord); send ESC while it's the active menu.
        g_AllowESCSinkBlock.store(true);
        SimulateModifiedKey(0, kEscapeDIK);
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            g_AllowESCSinkBlock.store(false);
        }).detach();
        SKSE::log::info("CloseActiveModMenu: Closed ImprovedCamera via simulated ESC.");
    } else if (active == ActiveMenu::FLICK) {
        if (auto* flick = GetFLICKInterface(); flick && flick->SetMenuOpen) {
            flick->SetMenuOpen(false);
            SKSE::log::info("CloseActiveModMenu: Closed FLICK through its API.");
        }
    } else if (active == ActiveMenu::Durability) {
        
        std::thread([]() {
            WORD dik = DIKFromVK(g_DurabilityConfig.toggleVK);
            if (dik != 0) {
                INPUT downInput{};
                AddScanInput(downInput, dik, false);
                ::SendInput(1, &downInput, sizeof(INPUT));
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                INPUT upInput{};
                AddScanInput(upInput, dik, true);
                ::SendInput(1, &upInput, sizeof(INPUT));
                SKSE::log::info("CloseActiveModMenu: Closed Durability via simulated key.");
            }
        }).detach();
    } else if (active == ActiveMenu::MF) {
        ForceCloseSKSEMenuFramework();
        SKSE::log::info("CloseActiveModMenu: Closed SKSE Menu Framework.");
    }
    g_ActiveMenu.store(ActiveMenu::None);
}

// Toggle Risa's launcher (or close the active mod menu). Debounced. Called from the
// low-level keyboard hook on a physical launcher-hotkey press, and as a fallback from
// the DI8 GetDeviceState path when the LL hook isn't active.
static void ToggleLauncher() {
    if (IsUserTyping()) return;
    if (IsENBOpeningTransition()) {
        return;
    }
    if (NowMs() < g_MenuOpenLockUntilMs.load()) {
        SKSE::log::info("ToggleLauncher: ignored because g_MenuOpenLockUntilMs is active.");
        return;
    }
    long long now = NowMs();
    if (now - g_LastLauncherToggleMs.load() <= 700) return;
    g_LastLauncherToggleMs.store(now);

    auto active = g_ActiveMenu.load();
    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
        CloseLauncher();
        SKSE::log::info("ToggleLauncher: Launcher closed.");
    } else if (active != ActiveMenu::None) {
        CloseActiveModMenu(active);
        SKSE::log::info("ToggleLauncher: closed active mod menu.");
    } else if (g_LauncherWindow) {
        ForceCloseSKSEMenuFramework();
        g_LauncherWindow->IsOpen.store(true);
        g_LauncherWindow->BlockUserInput.store(true);
        SKSE::log::info("ToggleLauncher: Launcher opened.");
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

    if (g_WaitingForHotkeyPress.load()) {
        return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
    }
    if (!IsUserTyping()) {
        // 1. Block SKSE Menu Framework's hotkey completely
        if (g_MFConfig.toggleVK != 0 && g_MFConfig.toggleMode != "OFF" && wParam == static_cast<WPARAM>(g_MFConfig.toggleVK)) {
            if (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) {
                return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
            }
            SKSE::log::info("WndProc: BLOCKED SKSE Menu Framework key 0x{:02X}", wParam);
            return 0;
        }

        // 2. Check for Risa's menu hotkey and ENB/OAR/IED window close
        if (!g_WaitingForHotkeyPress.load() && wParam == static_cast<WPARAM>(g_LauncherHotkeyVK.load())) {
            // If we are simulating Debug Menu (which defaults to F1/hotkey), let it pass to the game!
            if (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) {
                return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
            }

            // Do not let F1 clear ENB's tracked state while the delayed Shift+Enter
            // request is still waiting for the editor to become visible.
            if (IsENBOpeningTransition()) {
                return 0;
            }

            if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
                long long now = NowMs();
                if (now - g_LastLauncherToggleMs.load() > 400) {
                    g_LastLauncherToggleMs.store(now);
                    auto active = g_ActiveMenu.load();
                    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
                        CloseLauncher();
                        SKSE::log::info("WndProc: Launcher closed.");
                    } else if (active != ActiveMenu::None) {
                        CloseActiveModMenu(active);
                    } else {
                        if (g_LauncherWindow) {
                            ForceCloseSKSEMenuFramework();
                            g_LauncherWindow->IsOpen.store(true);
                            g_LauncherWindow->BlockUserInput.store(true);
                            SKSE::log::info("WndProc: Launcher opened.");
                        }
                    }
                }
            }
            return 0; // block launcher hotkey completely from reaching other WndProc hooks
        }





        // 3. ENB Shift+Enter editor key handling
        if (g_ENBConfig.enabled && wParam == static_cast<WPARAM>(g_ENBConfig.editorVK) && !g_UnblockENB.load()) {
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
        if (g_IEDConfig.enabled && wParam == static_cast<WPARAM>(g_IEDConfig.toggleVK) && !g_UnblockIED.load()) {
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
        if (g_DebugMenuConfig.enabled && wParam == static_cast<WPARAM>(g_DebugMenuConfig.toggleVK) && !g_UnblockDebugMenu.load()) {
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
        if (g_FLICKConfig.enabled && wParam == static_cast<WPARAM>(g_FLICKConfig.toggleVK) && !g_UnblockFLICK.load()) {
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
                bool passToDMenu  = dMenuAllowed || (isDMenuKey && g_UnblockDMenu.load());
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

static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(IDirectInputDevice8A* pDevice,
                                                       DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_OrigGetDeviceState(pDevice, cbData, lpvData);
    SubclassGameWindow();
    if (SUCCEEDED(hr) && lpvData && cbData == 256) {
        auto* state = reinterpret_cast<BYTE*>(lpvData);

        // Sink registration timing varies between plugins. Discover managed sinks from
        // this guaranteed keyboard poll instead of waiting for the launcher to render.
        static std::atomic<long long> lastSinkScanMs{ 0 };
        const long long now = NowMs();
        long long lastScan = lastSinkScanMs.load(std::memory_order_relaxed);
        if (now - lastScan >= 1000 &&
            lastSinkScanMs.compare_exchange_strong(lastScan, now, std::memory_order_relaxed)) {
            TryHookModSinks();
        }

        if (g_AllowESCSinkBlock.load()) {
            state[0x01] = 0; // zero kEscapeDIK (0x01)
        }
        if (!g_DI8HookFiredOnce.exchange(true))
            SKSE::log::info("DI8 GetDeviceState hook firing — hook is active.");

        // Launcher-hotkey detection at the DirectInput level (this is the path that
        // actually sees in-game key presses). Edge-detected + debounced in ToggleLauncher.
        {
            // If DebugMenu (which may share this key) is mid-open via simulated input,
            // don't treat that injected press as a launcher toggle.
            bool debugMenuOpening = g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load();
            static bool lastF1State = false;
            bool f1Pressed = !g_WaitingForHotkeyPress.load() && (state[g_LauncherHotkeyDIK.load()] & 0x80) != 0;
            bool f1JustPressed = f1Pressed && !lastF1State;
            lastF1State = f1Pressed;
            // Ignore the launcher key while a just-opened mod is still appearing (prevents
            // the open-delay desync that left Debug Menu/FLICK un-closable).
            bool menuOpening = IsENBOpeningTransition() || NowMs() < g_MenuOpenLockUntilMs.load();
            if (f1JustPressed && !debugMenuOpening && !menuOpening) {
                ToggleLauncher();
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
    if (dik == 0 || IsUserTyping()) return false;
    if (g_AllowESCSinkBlock.load() && dik == 0x01) return true;
    const long long now = NowMs();

    bool matches = false;    // dik belongs to at least one managed mod
    bool letThrough = false; // ...and at least one of those mods wants it right now

    auto consider = [&](bool enabled, WORD tdik, bool unblock, bool allow) {
        if (!enabled || tdik == 0 || dik != tdik) return;
        matches = true;
        if (unblock || allow) letThrough = true;
    };

    // Only the mods that DON'T expose a hookable input sink go through this global buffer
    // strip (dMenu, Improved Camera, FLICK). Their keys (Home, F7) have no in-game
    // function, so a global strip is equivalent to a per-mod block here. IED, Debug Menu
    // and MF are handled per-mod by their sink hooks (Hook 4) so their keys stay usable.
    consider(g_DMenuConfig.enabled, g_DMenuConfig.toggleDIK, g_UnblockDMenu.load(),
             g_AllowDMenuOpen.load() && now <= g_AllowDMenuOpenUntilMs.load());
    consider(g_ImprovedCameraConfig.enabled, g_ImprovedCameraConfig.toggleDIK, g_UnblockImprovedCamera.load(),
             g_AllowImprovedCameraOpen.load() && now <= g_AllowImprovedCameraOpenUntilMs.load());
    consider(g_FLICKConfig.enabled, g_FLICKConfig.toggleDIK, g_UnblockFLICK.load(),
             g_AllowFLICKOpen.load() && now <= g_AllowFLICKOpenUntilMs.load());

    // Strip only if it's a managed key AND no owning mod currently wants it. This lets a
    // simulated open through (its allow-window sets letThrough) for the buffered path.
    return matches && !letThrough;
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
// Returns 0 for MF's virtual key so WinAPI-based polls also see "not pressed".
// ============================================================================

static bool IsOARModifierDown() {
    if (!g_OARConfig.enabled)
        return false;

    const auto keyState = [](int vk) {
        return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
    };

    const bool ctrlDown = (keyState(VK_CONTROL) & 0x8000) != 0 ||
                          (keyState(VK_LCONTROL) & 0x8000) != 0 ||
                          (keyState(VK_RCONTROL) & 0x8000) != 0;
    const bool shiftDown = (keyState(VK_SHIFT) & 0x8000) != 0 ||
                           (keyState(VK_LSHIFT) & 0x8000) != 0 ||
                           (keyState(VK_RSHIFT) & 0x8000) != 0;
    const bool altDown = (keyState(VK_MENU) & 0x8000) != 0 ||
                         (keyState(VK_LMENU) & 0x8000) != 0 ||
                         (keyState(VK_RMENU) & 0x8000) != 0;

    return (!g_OARConfig.ctrl || ctrlDown) &&
           (!g_OARConfig.shift || shiftDown) &&
           (!g_OARConfig.alt || altDown);
}

static SHORT WINAPI HookedGetAsyncKeyState(int vKey) {
    const auto real = [&]() -> SHORT { return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vKey) : ::GetAsyncKeyState(vKey); };
    if (g_WaitingForHotkeyPress.load()) return real();

    // OAR polls its configured key through User32. Hide only the toggle key and only
    // when the call originates from OAR, leaving the same key intact everywhere else.
    const bool isOARKey = g_OARConfig.enabled && g_OARConfig.toggleVK != 0 &&
        vKey == g_OARConfig.toggleVK;
    if (isOARKey && !g_UnblockOAR.load() && !IsUserTyping()) {
        const bool allowed = g_AllowOAROpen.load() && NowMs() <= g_AllowOAROpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "OpenAnimationReplacer")) return 0;
    }

    const bool isENBKey = g_ENBConfig.enabled &&
        ((g_ENBConfig.editorVK != 0 && vKey == g_ENBConfig.editorVK) ||
         (g_ENBConfig.combinationVK != 0 && vKey == g_ENBConfig.combinationVK));
    if (!isENBKey) return real();

    // ENB editor key: return "not pressed" ONLY when ENB's own d3d11 wrapper is asking,
    // so the game still sees the key normally. Let it through while opening ENB.
    if (!g_UnblockENB.load() && !IsUserTyping()) {
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

    const bool isOARKey = g_OARConfig.enabled && g_OARConfig.toggleVK != 0 &&
        vKey == g_OARConfig.toggleVK;
    if (isOARKey && !g_UnblockOAR.load() && !IsUserTyping()) {
        const bool allowed = g_AllowOAROpen.load() && NowMs() <= g_AllowOAROpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "OpenAnimationReplacer")) return 0;
    }

    const bool isENBKey = g_ENBConfig.enabled &&
        ((g_ENBConfig.editorVK != 0 && vKey == g_ENBConfig.editorVK) ||
         (g_ENBConfig.combinationVK != 0 && vKey == g_ENBConfig.combinationVK));
    if (!isENBKey) return real();
    if (!g_UnblockENB.load() && !IsUserTyping()) {
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

enum class ManagedSink { DMenu, IC, FLICK, IED, DebugMenu, MF };

static WORD SinkBlockDIK(ManagedSink m) {
    switch (m) {
        case ManagedSink::DMenu:     return g_DMenuConfig.toggleDIK;
        case ManagedSink::IC:        return g_ImprovedCameraConfig.toggleDIK;
        case ManagedSink::FLICK:     return g_FLICKConfig.toggleDIK;
        case ManagedSink::IED:       return g_IEDConfig.toggleDIK;
        case ManagedSink::DebugMenu: return g_DebugMenuConfig.toggleDIK;
        case ManagedSink::MF:        return g_MFConfig.toggleDIK;
    }
    return 0;
}

// True => let this mod see its key right now (it's being opened from our launcher, or
// the user unblocked it). MF is always filtered (we open MF's window programmatically).
static bool SinkLetThrough(ManagedSink m) {
    const long long now = NowMs();
    switch (m) {
        case ManagedSink::DMenu:     return g_UnblockDMenu.load()          || (g_AllowDMenuOpen.load()          && now <= g_AllowDMenuOpenUntilMs.load());
        case ManagedSink::IC:        return g_UnblockImprovedCamera.load() || (g_AllowImprovedCameraOpen.load() && now <= g_AllowImprovedCameraOpenUntilMs.load());
        case ManagedSink::FLICK:     return g_UnblockFLICK.load()          || (g_AllowFLICKOpen.load()          && now <= g_AllowFLICKOpenUntilMs.load());
        case ManagedSink::IED:       return g_UnblockIED.load()            || (g_AllowIEDOpen.load()            && now <= g_AllowIEDOpenUntilMs.load());
        case ManagedSink::DebugMenu: return g_UnblockDebugMenu.load()      || (g_AllowDebugMenuOpen.load()      && now <= g_AllowDebugMenuOpenUntilMs.load());
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

// Registry of hooked sink functions (keyed by the ProcessEvent function address, so it
// covers multiple sink instances of the same class). Fixed array + atomic count makes it
// safe to append on the render thread while the input thread reads it.
struct SinkHookEntry { void* fn; ProcessInputEvent_t orig; ManagedSink which; };
static std::array<SinkHookEntry, 16> g_SinkEntries{};
static std::atomic<size_t> g_SinkEntryCount{ 0 };

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
            if (e.which == ManagedSink::DebugMenu && !g_UnblockDebugMenu.load()) {
                bool hasF1 = false;
                for (RE::InputEvent* ev = *a_event; ev; ev = ev->next) {
                    if (IsButtonForDIK(ev, g_DebugMenuConfig.toggleDIK)) {
                        hasF1 = true;
                        break;
                    }
                }
                if (hasF1) {
                    int passes = g_DebugMenuPassCount.load();
                    if (passes > 0) {
                        g_DebugMenuPassCount.store(passes - 1);
                        return e.orig(sink, a_event, src);
                    } else {
                        return FilterDispatch(sink, e.orig, a_event, src, g_DebugMenuConfig.toggleDIK);
                    }
                }
            }
            if (SinkLetThrough(e.which)) return e.orig(sink, a_event, src);
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
                    g_SinkEntries[idx] = SinkHookEntry{ fn, orig, t.which };
                    g_SinkEntryCount.store(idx + 1, std::memory_order_release);
                    SKSE::log::info("TryHookModSinks: hooked {} input sink at {:p}.", t.module, fn);
                }
            }
            break;
        }
    }
}

// ============================================================================
// Hook installation
// ============================================================================
static void InstallHooks() {
    MH_Initialize();

    // --- DI8 GetDeviceData (buffered keyboard) ---
    // Used ONLY to strip the keys of mods that don't expose a hookable input sink
    // (dMenu, Improved Camera, FLICK). Those keys (Home/F7) have no in-game function, so
    // stripping the buffered event is equivalent to a per-mod block. IED/DebugMenu/MF are
    // handled per-mod via their sinks; ENB via the caller-scoped GetAsyncKeyState below.
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
        void* targetData  = vtbl[10]; // GetDeviceData (buffered strip for no-sink mods)
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
    // sink (Hook 4, TryHookModSinks), and the only poller (ENB) via the caller-scoped
    // GetAsyncKeyState/GetKeyState hooks below.

    // --- GetAsyncKeyState (used only to block ENB's editor key for ENB's own polls) ---
    {
        MH_STATUS st = MH_CreateHookApi(L"user32", "GetAsyncKeyState",
            reinterpret_cast<void*>(HookedGetAsyncKeyState),
            reinterpret_cast<void**>(&g_OrigGetAsyncKeyState));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(MH_ALL_HOOKS);
            SKSE::log::info("InstallHooks: GetAsyncKeyState hooked (ENB editor key, caller-scoped).");
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
static void CloseLauncher() {
    if (g_LauncherWindow) {
        g_LauncherWindow->IsOpen.store(false);
        g_WaitForLauncherKeyRelease.store(false);
        g_LastLauncherToggleMs.store(NowMs());
    }
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

static void OpenAnimationReplacer() {
    if (!g_OARConfig.enabled || g_OARConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenAnimationReplacer: OAR UI hotkey is not configured.");
        return;
    }

    if ((g_ActiveMenu.load() == ActiveMenu::OAR && IsExternalMenuOpen()) || (IsExternalMenuOpen() && g_ActiveMenu.load() == ActiveMenu::None)) {
        CloseLauncher();
        std::thread([]() {
            INPUT downInput{};
            AddScanInput(downInput, kEscapeDIK, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT upInput{};
            AddScanInput(upInput, kEscapeDIK, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            SKSE::log::info("OpenAnimationReplacer: closed OAR via simulated ESC.");
        }).detach();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::OAR);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        g_AllowOAROpen.store(true);
        g_AllowOAROpenUntilMs.store(NowMs() + 800);
        SendOARHotkey();
        SKSE::log::info("OpenAnimationReplacer: opening via configured hotkey {}{}{}{}.",
            g_OARConfig.ctrl ? "Ctrl+" : "",
            g_OARConfig.shift ? "Shift+" : "",
            g_OARConfig.alt ? "Alt+" : "",
            NameFromDIK(g_OARConfig.toggleDIK));
    }).detach();
}

static void OpenImmersiveEquipmentDisplays() {
    if (!g_IEDConfig.enabled || g_IEDConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenImmersiveEquipmentDisplays: IED UI hotkey is not configured.");
        return;
    }

    if ((g_ActiveMenu.load() == ActiveMenu::IED && IsExternalMenuOpen()) || (IsExternalMenuOpen() && g_ActiveMenu.load() == ActiveMenu::None)) {
        CloseLauncher();
        std::thread([]() {
            INPUT downInput{};
            AddScanInput(downInput, kEscapeDIK, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT upInput{};
            AddScanInput(upInput, kEscapeDIK, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            SKSE::log::info("OpenImmersiveEquipmentDisplays: closed IED via simulated ESC.");
        }).detach();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::IED);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        g_AllowIEDOpen.store(true);
        g_AllowIEDOpenPolls.store(20);
        g_AllowIEDOpenUntilMs.store(NowMs() + 800);

        INPUT downInput{};
        AddScanInput(downInput, g_IEDConfig.toggleDIK, false);
        ::SendInput(1, &downInput, sizeof(INPUT));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        INPUT upInput{};
        AddScanInput(upInput, g_IEDConfig.toggleDIK, true);
        ::SendInput(1, &upInput, sizeof(INPUT));

        SKSE::log::info("OpenImmersiveEquipmentDisplays: opening via configured hotkey (split 100ms).");
    }).detach();
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

static void OpenDurability() {
    
    if (!g_DurabilityConfig.enabled) {
        SKSE::log::warn("OpenDurability: Equipment Durability mod is not active.");
        return;
    }
    if (g_DurabilityConfig.toggleVK <= 0) {
        SKSE::log::warn("OpenDurability: No toggle key code configured in Settings.ini.");
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::Durability);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        WORD dik = DIKFromVK(g_DurabilityConfig.toggleVK);
        if (dik != 0) {
            INPUT downInput{};
            AddScanInput(downInput, dik, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            
            INPUT upInput{};
            AddScanInput(upInput, dik, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            SKSE::log::info("OpenDurability: toggled durability widget via simulated key.");
        }
        
    }).detach();
}


static void OpenDebugMenu() {
    
    if (!g_DebugMenuConfig.enabled || g_DebugMenuConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenDebugMenu: DebugMenu hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::DebugMenu || IsExternalMenuOpen()) {
        CloseLauncher();
        g_AllowDebugMenuOpen.store(true);
        g_AllowDebugMenuOpenUntilMs.store(NowMs() + 1000);
        g_DebugMenuPassCount.store(2);
        
        std::thread([]() {
            INPUT downInput{};
            AddScanInput(downInput, g_DebugMenuConfig.toggleDIK, false);
            ::SendInput(1, &downInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT upInput{};
            AddScanInput(upInput, g_DebugMenuConfig.toggleDIK, true);
            ::SendInput(1, &upInput, sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_AllowDebugMenuOpen.store(false);
            g_AllowDebugMenuOpenUntilMs.store(0);
            
            SKSE::log::info("OpenDebugMenu: closed DebugMenu via simulated hotkey (one-shot).");
        }).detach();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::DebugMenu);
    g_MenuOpenLockUntilMs.store(NowMs() + 1500);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        g_AllowDebugMenuOpen.store(true);
        g_AllowDebugMenuOpenUntilMs.store(NowMs() + 1000);
        g_DebugMenuPassCount.store(2);

        INPUT downInput{};
        AddScanInput(downInput, g_DebugMenuConfig.toggleDIK, false);
        ::SendInput(1, &downInput, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        INPUT upInput{};
        AddScanInput(upInput, g_DebugMenuConfig.toggleDIK, true);
        ::SendInput(1, &upInput, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_AllowDebugMenuOpen.store(false);
        g_AllowDebugMenuOpenUntilMs.store(0);
        

        SKSE::log::info("OpenDebugMenu: opening via configured hotkey (one-shot).");
    }).detach();
}

static void OpenDMenu() {
    
    if (!g_DMenuConfig.enabled || g_DMenuConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenDMenu: dMenu hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::DMenu || IsExternalMenuOpen()) {
        CloseLauncher();
        
        std::thread([]() {
            g_AllowDMenuOpen.store(true);
            g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);

            WORD combDIK = g_DMenuConfig.modifierDIK;
            WORD editDIK = g_DMenuConfig.toggleDIK;

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

            SKSE::log::info("OpenDMenu: closed dMenu via simulated hotkey.");
            
        }).detach();
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::DMenu);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        g_AllowDMenuOpen.store(true);
        g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);

        WORD combDIK = g_DMenuConfig.modifierDIK;
        WORD editDIK = g_DMenuConfig.toggleDIK;

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

        SKSE::log::info("OpenDMenu: opened dMenu via simulated hotkey.");
        
    }).detach();
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
    
    if (!g_ImprovedCameraConfig.enabled || g_ImprovedCameraConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenImprovedCamera: ImprovedCamera hotkey is not configured.");
        return;
    }

    if ((g_ActiveMenu.load() == ActiveMenu::ImprovedCamera && IsExternalMenuOpen()) || (IsExternalMenuOpen() && g_ActiveMenu.load() == ActiveMenu::None)) {
        CloseLauncher();
        SimulateModifiedKey(0, kEscapeDIK); // IC closes on ESC
        SKSE::log::info("OpenImprovedCamera: closed ImprovedCamera via simulated ESC.");
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::ImprovedCamera);
    g_LastLauncherToggleMs.store(NowMs());
    

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        // IC's menu is Left Shift + MenuKey. SendInput sets both the key message and the
        // live Shift state IC checks. dMenu (now on a different key) is unaffected.
        SimulateModifiedKey(g_ImprovedCameraConfig.modifierDIK, g_ImprovedCameraConfig.toggleDIK);
        SKSE::log::info("OpenImprovedCamera: opened ImprovedCamera via simulated Shift+MenuKey (0x{:02X}+0x{:02X}).",
            g_ImprovedCameraConfig.modifierDIK, g_ImprovedCameraConfig.toggleDIK);
    }).detach();
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
    DWORD flags = (keyUp ? KEYEVENTF_KEYUP : 0);
    
    if (scanCode > 0x7F) {
        osScan = scanCode & 0x7F;
        flags |= KEYEVENTF_EXTENDEDKEY; // 0x0001
    }
    
    input.ki.wScan = osScan;
    WORD vk = VKFromDIK(scanCode);
    if (vk == 0) {
        vk = static_cast<WORD>(::MapVirtualKeyA(osScan, 1)); // MAPVK_VSC_TO_VK = 1
    }
    input.ki.wVk = vk;
    input.ki.dwFlags = flags;
}




static void SendOARHotkey() {
    std::array<INPUT, 4> downInputs{};
    UINT downCount = 0;

    if (g_OARConfig.ctrl) AddScanInput(downInputs[downCount++], 0x1D, false);
    if (g_OARConfig.shift) AddScanInput(downInputs[downCount++], 0x2A, false);
    if (g_OARConfig.alt) AddScanInput(downInputs[downCount++], 0x38, false);
    AddScanInput(downInputs[downCount++], g_OARConfig.toggleDIK, false);

    const UINT downSent = ::SendInput(downCount, downInputs.data(), sizeof(INPUT));
    if (downSent != downCount) {
        SKSE::log::error("SendOARHotkey: sent {}/{} key-down events (GetLastError={}).",
            downSent, downCount, ::GetLastError());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::array<INPUT, 4> upInputs{};
    UINT upCount = 0;
    AddScanInput(upInputs[upCount++], g_OARConfig.toggleDIK, true);
    if (g_OARConfig.alt) AddScanInput(upInputs[upCount++], 0x38, true);
    if (g_OARConfig.shift) AddScanInput(upInputs[upCount++], 0x2A, true);
    if (g_OARConfig.ctrl) AddScanInput(upInputs[upCount++], 0x1D, true);

    const UINT upSent = ::SendInput(upCount, upInputs.data(), sizeof(INPUT));
    if (upSent != upCount) {
        SKSE::log::error("SendOARHotkey: sent {}/{} key-up events (GetLastError={}).",
            upSent, upCount, ::GetLastError());
    }
}

static bool HandleLauncherHotkeys(RE::InputEvent* ev, const char* source) {
    if (!ev || ev->GetEventType() != RE::INPUT_EVENT_TYPE::kButton)
        return false;
    if (ev->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
        return false;

    if (g_WaitingForHotkeyPress.load()) {
        return false; // Let key events pass to game/ImGui so we can bind the new hotkey
    }

    auto* btn = static_cast<RE::ButtonEvent*>(ev);
    const auto code = btn->GetIDCode();

    // The launcher key is handled SOLELY by the per-frame GetDeviceState poll (which works
    // even while a mod's menu is capturing input). Don't also handle it here, or the two
    // paths double-toggle (caused Debug Menu to reopen on a fast close). Let it pass.
    if (code == g_LauncherHotkeyDIK.load()) {
        if (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) {
            return false; // let simulated F1 pass!
        }
        if (IsENBOpeningTransition()) {
            return true;
        }
        if (NowMs() < g_MenuOpenLockUntilMs.load()) {
            return true; // block physical hotkey!
        }
        return false;
    }

    if (!btn->IsDown())
        return false;

    if (g_MFConfig.toggleDIK != 0 && code == g_MFConfig.toggleDIK) {
        if (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) {
            return false; // let simulated F1 pass!
        }
        ForceCloseSKSEMenuFramework();
        if (btn->IsDown())
            SKSE::log::info("{}: blocked SKSE Menu Framework hotkey {}.", source, g_MFConfig.toggleKeyName);
        return true;
    }

    if (g_OARConfig.enabled && g_OARConfig.toggleDIK != 0 && code == g_OARConfig.toggleDIK && IsOARModifierDown() && !IsUserTyping()) {
        if (g_UnblockOAR.load()) return false;
        if (g_AllowOAROpen.load() && NowMs() <= g_AllowOAROpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked Open Animation Replacer hotkey {}.", source, NameFromDIK(g_OARConfig.toggleDIK));
        return true;
    }

    if (g_IEDConfig.enabled && g_IEDConfig.toggleDIK != 0 && code == g_IEDConfig.toggleDIK && !IsUserTyping()) {
        if (g_UnblockIED.load()) return false;
        if (g_AllowIEDOpen.load() && NowMs() <= g_AllowIEDOpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked Immersive Equipment Displays hotkey.", source);
        return true;
    }

    if (g_DebugMenuConfig.enabled && g_DebugMenuConfig.toggleDIK != 0 && code == g_DebugMenuConfig.toggleDIK && !IsUserTyping()) {
        if (g_UnblockDebugMenu.load()) return false;
        if (g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked DebugMenu hotkey.", source);
        return true;
    }

    if (g_DMenuConfig.enabled && g_DMenuConfig.toggleDIK != 0 && code == g_DMenuConfig.toggleDIK && IsDMenuModifierDown() && !IsUserTyping()) {
        if (g_UnblockDMenu.load()) return false;
        if (g_AllowDMenuOpen.load() && NowMs() <= g_AllowDMenuOpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked dMenu hotkey.", source);
        return true;
    }

    if (g_ImprovedCameraConfig.enabled && g_ImprovedCameraConfig.toggleDIK != 0 && code == g_ImprovedCameraConfig.toggleDIK && IsImprovedCameraModifierDown() && !IsUserTyping()) {
        if (g_UnblockImprovedCamera.load()) return false;
        if (g_AllowImprovedCameraOpen.load() && NowMs() <= g_AllowImprovedCameraOpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked ImprovedCamera hotkey.", source);
        return true;
    }

    if (g_FLICKConfig.enabled && g_FLICKConfig.toggleDIK != 0 && code == g_FLICKConfig.toggleDIK && !IsUserTyping()) {
        if (g_UnblockFLICK.load()) return false;
        if (g_AllowFLICKOpen.load() && NowMs() <= g_AllowFLICKOpenUntilMs.load()) {
            return false; // let it pass
        }
        if (btn->IsDown())
            SKSE::log::info("{}: blocked FLICK hotkey.", source);
        return true;
    }

    if (g_DurabilityConfig.enabled && g_DurabilityConfig.toggleVK > 0 && !IsUserTyping()) {
        WORD durDIK = DIKFromVK(g_DurabilityConfig.toggleVK);
        if (code == durDIK && durDIK != 0) {
            if (g_UnblockDurability.load()) return false;
            if (btn->IsDown())
                SKSE::log::info("{}: blocked Equipment Durability hotkey.", source);
            return true;
        }
    }

    if (code == kEscapeDIK && g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {
        CloseLauncher();
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

static bool IsGameLoaded() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;
    if (ui->IsMenuOpen("MainMenu")) return false;
    if (ui->IsMenuOpen("Loading Menu")) return false;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell()) return false;

    return true;
}

// ============================================================================
// Launcher render
// ============================================================================
static void __stdcall RenderLauncher() {
    if (g_ActiveMenu.load() != ActiveMenu::None && !IsCursorShowing()) {
        g_ActiveMenu.store(ActiveMenu::None);
        SKSE::log::info("RenderLauncher: Self-healed active menu state to None (cursor hidden).");
    }


    if (g_LauncherWindow && g_LauncherWindow->IsOpen.load()) {


        if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
            CloseLauncher();
            SKSE::log::info("ImGui input: launcher closed by ESC.");
            return;
        }
    }

    // Solid opaque window — no transparency from MF style
    ImGuiMCP::SetNextWindowBgAlpha(1.0f);
    ImGuiMCP::PushStyleColor(2,  ImGuiMCP::ImVec4(0.10f, 0.11f, 0.16f, 1.0f)); // WindowBg
    ImGuiMCP::PushStyleColor(10, ImGuiMCP::ImVec4(0.08f, 0.09f, 0.13f, 1.0f)); // TitleBg
    ImGuiMCP::PushStyleColor(11, ImGuiMCP::ImVec4(0.13f, 0.21f, 0.38f, 1.0f)); // TitleBgActive
    ImGuiMCP::PushStyleColor(5,  ImGuiMCP::ImVec4(0.22f, 0.38f, 0.65f, 1.0f)); // Border
    ImGuiMCP::PushStyleVar(10, 6.0f); // WindowRounding
    ImGuiMCP::PushStyleVar(13, 4.0f); // FrameRounding

    constexpr int kFlags =
        ImGuiMCP::ImGuiWindowFlags_NoResize |
        ImGuiMCP::ImGuiWindowFlags_NoCollapse |
        ImGuiMCP::ImGuiWindowFlags_NoScrollbar |
        ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize;

    if (g_WindowPosX.load() != -1.0f && g_WindowPosY.load() != -1.0f) {
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(g_WindowPosX.load(), g_WindowPosY.load()), ImGuiMCP::ImGuiCond_FirstUseEver, ImGuiMCP::ImVec2(0.0f, 0.0f));
    }

    bool open = true;
    if (!ImGuiMCP::Begin("Risa's Menu Launcher", &open, kFlags)) {
        ImGuiMCP::PopStyleVar(2); ImGuiMCP::PopStyleColor(4); ImGuiMCP::End(); return;
    }

    {
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

    const float uiScale = g_LauncherFontScale.load();
    const float layoutScale = uiScale / 0.9f; // 0.9 preserves the launcher's original dimensions
    const ImGuiMCP::ImVec2 launcherButtonSize(280.0f * layoutScale, 38.0f * layoutScale);
    ImGuiMCP::SetWindowFontScale(uiScale);

    static const bool hasMF = GetModuleHandleA("SKSEMenuFramework") != nullptr;
    static const bool hasOAR = GetModuleHandleA("OpenAnimationReplacer") != nullptr;
    static const bool hasIED = GetModuleHandleA("ImmersiveEquipmentDisplays") != nullptr;
    static const bool hasENB = std::filesystem::exists("enblocal.ini");
    static const bool hasDebugMenu = GetModuleHandleA("DebugMenu") != nullptr;
    static const bool hasDMenu = GetModuleHandleA("dmenu") != nullptr;
    static const bool hasImprovedCamera = GetModuleHandleA("ImprovedCameraSE") != nullptr;
    static const bool hasFLICK = GetModuleHandleA("FUCK") != nullptr;
    static const bool hasDurability = GetModuleHandleA("EquipmentDurabilitySystem-NG") != nullptr;

    struct LauncherButton {
        std::string id;
        std::string label;
        void (*action)();
        bool active;
    };

    std::vector<LauncherButton> allButtons;
    allButtons.push_back({ "MF", FontAwesome::UnicodeToUtf8(0xf013) + "  SKSE Menu Framework", OpenSKSEMenuFramework, hasMF });
    allButtons.push_back({ "OAR", FontAwesome::UnicodeToUtf8(0xf144) + "  Open Animation Replacer", OpenAnimationReplacer, hasOAR });
    allButtons.push_back({ "IED", FontAwesome::UnicodeToUtf8(0xf132) + "  Immersive Equipment Displays", OpenImmersiveEquipmentDisplays, hasIED });
    allButtons.push_back({ "DebugMenu", FontAwesome::UnicodeToUtf8(0xf188) + "  Debug Menu", OpenDebugMenu, hasDebugMenu });
    allButtons.push_back({ "dMenu", FontAwesome::UnicodeToUtf8(0xf0c9) + "  dMenu", OpenDMenu, hasDMenu });
    allButtons.push_back({ "ImprovedCamera", FontAwesome::UnicodeToUtf8(0xf030) + "  Improved Camera SE", OpenImprovedCamera, hasImprovedCamera });
    allButtons.push_back({ "ENB", FontAwesome::UnicodeToUtf8(0xf53f) + "  ENB Editor", OpenENB, hasENB });
    allButtons.push_back({ "FLICK", FontAwesome::UnicodeToUtf8(0xf1b3) + "  FLICK", OpenFLICK, hasFLICK });
    allButtons.push_back({ "Durability", FontAwesome::UnicodeToUtf8(0xf0ad) + "  Equipment Durability", OpenDurability, hasDurability });

    // Filter to active buttons sorted by g_ButtonOrder
    std::vector<LauncherButton> buttons;
    for (const auto& id : g_ButtonOrder) {
        for (const auto& btn : allButtons) {
            if (btn.id == id && btn.active) {
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

    if (ImGuiMCP::BeginTabBar("LauncherTabs", 0)) {
        if (ImGuiMCP::BeginTabItem((FontAwesome::UnicodeToUtf8(0xf0e4) + "  Launcher").c_str(), nullptr, 0)) {
            if (!buttons.empty()) {
                size_t N = buttons.size();
                size_t leftCount = (N + 1) / 2;
                size_t rightCount = N - leftCount;

                auto DrawButton = [&](size_t i) {
                    bool disabled = (buttons[i].id == "DebugMenu" || buttons[i].id == "ImprovedCamera" ||
                                     buttons[i].id == "IED" || buttons[i].id == "ENB" ||
                                     buttons[i].id == "Durability") && !IsGameLoaded();

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
                    }

                    FontAwesome::PushSolid();
                    if (ImGuiMCP::Button(buttons[i].label.c_str(), launcherButtonSize)) {
                        if (!disabled && !isBeingDragged) {
                            buttons[i].action();
                        }
                    }
                    FontAwesome::Pop();

                    if (isBeingDragged) {
                        ImGuiMCP::PopStyleColor(4);
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

                        // Temporarily disable window padding for tooltip to ensure precise alignment
                        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_WindowPadding, ImGuiMCP::ImVec2(0.0f, 0.0f));
                        ImGuiMCP::Begin("##drag_preview", nullptr, ImGuiMCP::ImGuiWindowFlags_NoTitleBar | ImGuiMCP::ImGuiWindowFlags_NoResize | ImGuiMCP::ImGuiWindowFlags_NoMove | ImGuiMCP::ImGuiWindowFlags_NoSavedSettings | ImGuiMCP::ImGuiWindowFlags_Tooltip | ImGuiMCP::ImGuiWindowFlags_NoBackground);
                        FontAwesome::PushSolid();
                        ImGuiMCP::Button(buttons[i].label.c_str(), launcherButtonSize);
                        FontAwesome::Pop();
                        ImGuiMCP::End();
                        ImGuiMCP::PopStyleVar(1);

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

        if (ImGuiMCP::BeginTabItem((FontAwesome::UnicodeToUtf8(0xf013) + "  Settings").c_str(), nullptr, 0)) {
            // UI Scale slider
            ImGuiMCP::Text("UI Scale:");
            float scale = g_LauncherFontScale.load();
            if (ImGuiMCP::SliderFloat("##FontScaleSlider", &scale, 0.6f, 1.4f, "%.2f", 0)) {
                g_LauncherFontScale.store(scale);
                SaveButtonOrder();
            }

            ImGuiMCP::Text("Launcher Toggle Hotkey:");
            std::string hotkeyBtnLabel;
            if (g_WaitingForHotkeyPress.load()) {
                hotkeyBtnLabel = "[ Press any key... ]";
            } else {
                hotkeyBtnLabel = NameFromDIK(g_LauncherHotkeyDIK.load()) + "  (Click to change)";
            }
            if (ImGuiMCP::Button(hotkeyBtnLabel.c_str(), launcherButtonSize)) {
                if (!g_WaitingForHotkeyPress.load()) {
                    g_WaitingForHotkeyPress.store(true);
                }
            }
            if (g_WaitingForHotkeyPress.load()) {
                // Intercept key presses
                for (int vk = 8; vk < 256; ++vk) {
                    if (vk == 1 || vk == 2 || vk == 4) continue; // Skip mouse clicks
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

            ImGuiMCP::Text("Unblock Mod Hotkeys:");

            auto DrawUnblockCheckbox = [](const char* label, const char* tooltip, std::atomic<bool>& setting, bool installed) {
                if (installed) {
                    bool val = setting.load();
                    if (ImGuiMCP::Checkbox(label, &val)) {
                        setting.store(val);
                        SaveButtonOrder();
                    }
                    ImGuiMCP::SetItemTooltip(tooltip);
                }
            };

            std::string debugMenuTooltip = "Default hotkey: F1";
            if (g_DebugMenuConfig.toggleDIK != 0x3B) {
                debugMenuTooltip += "\nChanged by Risa's Menu: " + NameFromDIK(g_DebugMenuConfig.toggleDIK);
            }

            std::string dMenuTooltip = "Default hotkey: HOME";
            if (g_DMenuConfig.toggleDIK != 0xC7) {
                dMenuTooltip += "\nChanged by Risa's Menu: " + NameFromDIK(g_DMenuConfig.toggleDIK);
            }

            std::string durabilityTooltip = "Default hotkey: Unassigned";
            if (g_DurabilityConfig.toggleVK > 0) {
                durabilityTooltip = "Default hotkey: " + NameFromDIK(DIKFromVK(g_DurabilityConfig.toggleVK));
            }

            DrawUnblockCheckbox("Allow Open Animation Replacer Key", "Default hotkey: SHIFT + O", g_UnblockOAR, hasOAR);
            DrawUnblockCheckbox("Allow Immersive Equipment Displays Key", "Default hotkey: BACKSPACE", g_UnblockIED, hasIED);
            DrawUnblockCheckbox("Allow ENB Editor Key", "Default hotkey: SHIFT + ENTER", g_UnblockENB, hasENB);
            DrawUnblockCheckbox("Allow Debug Menu Key", debugMenuTooltip.c_str(), g_UnblockDebugMenu, hasDebugMenu);
            DrawUnblockCheckbox("Allow dMenu Key", dMenuTooltip.c_str(), g_UnblockDMenu, hasDMenu);
            DrawUnblockCheckbox("Allow Improved Camera Key", "Default hotkey: SHIFT + HOME", g_UnblockImprovedCamera, hasImprovedCamera);
            DrawUnblockCheckbox("Allow FLICK Key", "Default hotkey: F7", g_UnblockFLICK, hasFLICK);
            DrawUnblockCheckbox("Allow Equipment Durability Key", durabilityTooltip.c_str(), g_UnblockDurability, hasDurability);

            ImGuiMCP::EndTabItem();
        }
        ImGuiMCP::EndTabBar();
    }

    ImGuiMCP::PopStyleVar(3);
    ImGuiMCP::PopStyleColor(3);

    ImGuiMCP::End();
    ImGuiMCP::PopStyleVar(2);
    ImGuiMCP::PopStyleColor(4);

    // Keep on top
    if (auto* w = ImGuiMCP::FindWindowByName("Risa's Menu Launcher"))
        ImGuiMCP::BringWindowToDisplayFront(w);
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
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
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
static void ManageDMenuKey() {
    if (!g_DMenuConfig.enabled || !g_ImprovedCameraConfig.enabled) return;
    if (g_DMenuConfig.toggleDIK == 0 || g_DMenuConfig.toggleDIK != g_ImprovedCameraConfig.toggleDIK) return; // no collision

    const std::filesystem::path ini = "Data/SKSE/Plugins/dmenu/dmenu.ini";
    if (!std::filesystem::exists(ini)) { SKSE::log::warn("ManageDMenuKey: dmenu.ini not found."); return; }

    auto used = [&](WORD dik) -> bool {
        if (dik == 0) return true;
        if (dik == g_ImprovedCameraConfig.toggleDIK)                         return true;
        if (g_FLICKConfig.enabled     && dik == g_FLICKConfig.toggleDIK)     return true;
        if (g_DebugMenuConfig.enabled && dik == g_DebugMenuConfig.toggleDIK) return true;
        if (g_IEDConfig.enabled       && dik == g_IEDConfig.toggleDIK)       return true;
        if (g_MFConfig.toggleDIK != 0 && dik == g_MFConfig.toggleDIK)        return true;
        if (dik == g_LauncherHotkeyDIK.load())                               return true;
        return false;
    };
    const WORD candidates[] = { 0xCF /*End*/, 0xD2 /*Insert*/, 0xC9 /*PageUp*/, 0xD1 /*PageDown*/, 0xD3 /*Delete*/ };
    WORD freeKey = 0;
    for (WORD c : candidates) if (!used(c)) { freeKey = c; break; }
    if (freeKey == 0) { SKSE::log::warn("ManageDMenuKey: no free key available."); return; }

    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream f(ini);
        std::string line;
        while (std::getline(f, line)) {
            std::string trimmed = TrimStr(line);
            auto eq = trimmed.find('=');
            if (eq != std::string::npos && ToUpper(TrimStr(trimmed.substr(0, eq))) == "KEY_TOGGLE_DMENU") {
                line = std::format("key_toggle_dmenu = {}", static_cast<int>(freeKey));
                replaced = true;
            }
            lines.push_back(line);
        }
    }
    if (!replaced) { SKSE::log::warn("ManageDMenuKey: key_toggle_dmenu line not found in dmenu.ini."); return; }

    std::ofstream out(ini, std::ios::trunc);
    if (!out.is_open()) { SKSE::log::error("ManageDMenuKey: failed to open dmenu.ini for writing."); return; }
    for (const auto& l : lines) out << l << "\n";
    out.close();

    SKSE::log::info("ManageDMenuKey: dMenu shared its key (0x{:02X}) with Improved Camera; set dmenu.ini key_toggle_dmenu = {} ({}). "
                    "Original was 0x{:02X} (199 = Home). RESTART Skyrim once for dMenu to use the new key.",
        g_DMenuConfig.toggleDIK, static_cast<int>(freeKey), NameFromDIK(freeKey), g_DMenuConfig.toggleDIK);
}

// Debug Menu's hotkey defaults to F1 — the same as our launcher key — which can't be told
// apart when simulating. If they collide, move Debug Menu to a free function key by editing
// its MCM config (uOpenMenuHotkey), the same managed-write approach used for dMenu.
static void ManageDebugMenuKey() {
    if (!g_DebugMenuConfig.enabled) return;
    if (g_DebugMenuConfig.toggleDIK == 0 || g_DebugMenuConfig.toggleDIK != g_LauncherHotkeyDIK.load()) return; // no collision with launcher

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
        if (g_MFConfig.toggleDIK != 0      && dik == g_MFConfig.toggleDIK)             return true;
        return false;
    };
    // Avoid F5 (quicksave) and F9 (quickload); F7 is FLICK.
    const WORD candidates[] = { 0x3C /*F2*/, 0x3D /*F3*/, 0x3E /*F4*/, 0x40 /*F6*/, 0x42 /*F8*/ };
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

    SKSE::log::info("ManageDebugMenuKey: DebugMenu shared the launcher key (0x{:02X}); set {} uOpenMenuHotkey = {} ({}). "
                    "The change was written during plugin startup.",
        g_DebugMenuConfig.toggleDIK, settingsIni.string(), static_cast<int>(freeKey), NameFromDIK(freeKey));
}

// ============================================================================
// SKSE messaging
// ============================================================================
static void MessageHandler(SKSE::MessagingInterface::Message* msg) {
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
        LoadDurabilityConfig();
        ManageDMenuKey();              // give dMenu a distinct key if it collides with Improved Camera
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
        g_FrameworkInputEvent = SKSEMenuFramework::AddInputEvent(FrameworkInputCallback);
        SKSE::log::info("kPostLoad: SKSE Menu Framework input callback registered.");
        InstallHooks();
        break;
    }

    case SKSE::MessagingInterface::kInputLoaded: {
        auto* mgr = RE::BSInputDeviceManager::GetSingleton();
        if (mgr) {
            mgr->AddEventSink(RisaInputSink::GetSingleton());
            SKSE::log::info("kInputLoaded: BSTEventSink registered.");
        }
        // Blocking is per-mod: sink-based mods are filtered at their own InputEvent sink
        // (TryHookModSinks, retried from the keyboard poll), and ENB/OAR via caller-scoped
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
    .Version              = 1,
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
    SKSE::log::info("RisaAllInOneMenu loaded.");

    // Run before kPostLoad so Debug Menu can observe the reassigned collision key during
    // this launch. Load only the state required by the collision manager here; the normal
    // full configuration pass still runs at kPostLoad.
    LoadButtonOrder();
    LoadDebugMenuConfig();
    ManageDebugMenuKey();

    // MF's own config remains untouched; its hotkey is silenced at runtime via its input
    // sink. Debug Menu is the one startup-managed exception when its key collides with F1.
    auto* msg = SKSE::GetMessagingInterface();
    if (!msg || !msg->RegisterListener(MessageHandler)) {
        SKSE::log::error("Failed to register messaging listener."); return false;
    }
    return true;
}
