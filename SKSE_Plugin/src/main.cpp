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
#include <set>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <cstring>
#include <initializer_list>

static constexpr std::string_view kRisaMenuVersion = "1.2.1";
static std::string g_RuntimeVersion = "Unknown";
static std::string g_SKSEVersion = "Unknown";
static std::string g_RuntimeEdition = "Unknown";
static std::atomic<int> g_MFIniWriteResult{ -2 };

// ============================================================================
// [DIAG] TEMPORARY channel-identification logging. Remove once the input channel
// of each leaking mod (OAR / dMenu / FLICK / IED) is confirmed. Logs ONCE per
// (channel, key) so it never floods.
// ============================================================================
static std::string DiagModuleName(void* addr) {
    HMODULE h = nullptr;
    if (addr && ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(addr), &h) && h) {
        char buf[MAX_PATH];
        if (::GetModuleFileNameA(h, buf, MAX_PATH)) {
            std::string s(buf);
            auto pos = s.find_last_of("\\/");
            return pos == std::string::npos ? s : s.substr(pos + 1);
        }
    }
    return "?";
}
// Master logging switch. OFF by default (quiet for end users); the Settings toggle turns it on
// for troubleshooting. Also gates the expensive stack-walking diagnostics below so they cost
// nothing when off.
static std::atomic<bool> g_LoggingEnabled{ false };
static void ApplyLogLevel() {
    const auto lvl = g_LoggingEnabled.load() ? spdlog::level::info : spdlog::level::off;
    spdlog::set_level(lvl);
    spdlog::flush_on(lvl);
}
static void DiagOnce(const char* channel, int code, void* callerAddr) {
    if (!g_LoggingEnabled.load()) return;
    static std::mutex m;
    static std::set<std::string> seen;
    std::string key = std::string(channel) + ":" + std::to_string(code);
    {
        std::lock_guard<std::mutex> lk(m);
        if (!seen.insert(key).second) return;
    }
    SKSE::log::info("[DIAG] {} <- key 0x{:02X}  caller={}", channel, code, DiagModuleName(callerAddr));
}
static void DiagCaller(const char* channel, int code, void* ra) {
    if (!g_LoggingEnabled.load()) return;
    std::string mod = DiagModuleName(ra);
    static std::mutex m;
    static std::set<std::string> seen;
    std::string key = std::string(channel) + ":" + std::to_string(code) + ":" + mod;
    {
        std::lock_guard<std::mutex> lk(m);
        if (!seen.insert(key).second) return;
    }
    SKSE::log::info("[DIAG] {} <- key 0x{:02X}  caller={}", channel, code, mod);
}
static bool DiagIsSystemModule(const std::string& m) {
    static const char* sys[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "gameoverlayrenderer64.dll",
        "dinput8.dll", "DINPUT8.dll", "d3d11.dll", "d3d11on12.dll", "dxgi.dll", "user32.dll", "win32u.dll",
        "skse64_1_6_1170.dll", "SkyrimSE.exe", "1RisaAllInOneMenu.dll", "?" };
    for (auto* s : sys) if (_stricmp(m.c_str(), s) == 0) return true;
    return false;
}
// Walk past the masking frame (Steam overlay / d3d11 / dinput8) to find the real module that
// polled. Logs the first non-system caller + the full module chain. Bounded: at most a few
// distinct callers per (channel,key), and stops walking that key once explored.
static void DiagStack(const char* channel, int code) {
    if (!g_LoggingEnabled.load()) return; // skip the costly CaptureStackBackTrace when logging is off
    static std::mutex mtx;
    static std::set<std::string> seen;
    static std::map<std::string, int> explored;
    std::string base = std::string(channel) + ":" + std::to_string(code);
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (explored[base] >= 6) return; // seen enough distinct callers for this key
    }
    void* frames[32];
    USHORT n = ::CaptureStackBackTrace(0, 32, frames, nullptr);
    std::string topReal = "?", chain, last;
    for (USHORT i = 0; i < n; ++i) {
        std::string m = DiagModuleName(frames[i]);
        if (m != last) { chain += m + " > "; last = m; }
        if (topReal == "?" && !DiagIsSystemModule(m)) topReal = m;
    }
    std::string key = base + ":" + topReal;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!seen.insert(key).second) return;
        explored[base]++;
    }
    SKSE::log::info("[DIAG] {} key 0x{:02X}  realCaller={}  | stack: {}", channel, code, topReal, chain);
}
static void DiagSinkOnce(void* sink) {
    if (!g_LoggingEnabled.load()) return;
    void* vt = *reinterpret_cast<void**>(sink);
    void* fn = (*reinterpret_cast<void***>(sink))[1];
    static std::mutex m;
    static std::set<void*> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        if (!seen.insert(vt).second) return;
    }
    SKSE::log::info("[DIAG] InputSink present: ProcessEvent={:p}  module={}", fn, DiagModuleName(vt));
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
};
static CSConfig g_CSConfig;

struct CatMenuConfig {
    bool enabled = false;
    int toggleImGuiKey = 577; // ImGuiKey_F6
    WORD toggleDIK = 0x40;
    WORD toggleVK = VK_F6;
};
static CatMenuConfig g_CatMenuConfig;

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
// fields through SetMenuOpen/IsMenuOpen avoids depending on FLICK's ImGui types.
#pragma pack(push, 1)
struct FLICKInterface {
    std::uint32_t version;
    void* functionsBeforeSetMenuOpen[12];
    void (*SetMenuOpen)(bool);
    bool (*IsMenuOpen)();
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
    CatMenu,
    Dragonborn,
    ReShade
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

static void LoadDebugMenuConfig() {
    g_DebugMenuConfig.enabled = IsPluginPresent("DebugMenu");
    if (!g_DebugMenuConfig.enabled) return;

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

    const auto iniPath = FindModFile({ "Data/FUCKs/FUCK/keybinds.ini", "FUCKs/FUCK/keybinds.ini",
                                       "Data/SKSE/Plugins/FUCKs/FUCK/keybinds.ini" });
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

    g_CSConfig.toggleVK = VK_END; // default
    if (std::filesystem::exists(jsonPath)) {
        try {
            std::ifstream f(jsonPath);
            nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("Menu") && j["Menu"].contains("ToggleKey")) {
                g_CSConfig.toggleVK = j["Menu"]["ToggleKey"].get<WORD>();
            }
        } catch (...) {
            SKSE::log::warn("LoadCSConfig: failed to parse {}", jsonPath.string());
        }
    }

    g_CSConfig.toggleDIK = g_CSConfig.toggleVK == VK_END ? 0xCF : DIKFromVK(g_CSConfig.toggleVK);
    SKSE::log::info("LoadCSConfig: enabled=true, toggleDIK={:#x}, toggleVK={:#x}",
        g_CSConfig.toggleDIK, g_CSConfig.toggleVK);
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

    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/SkyrimCheatMenu.json";
    if (std::filesystem::exists(jsonPath)) {
        try {
            std::ifstream f(jsonPath);
            const nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("toggleKey")) {
                if (j["toggleKey"].is_number_integer()) {
                    const int dik = j["toggleKey"].get<int>();
                    if (dik > 0 && dik < 256) {
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

    bool found = g_DragonbornConfig.toggleVK != 0 && g_DragonbornConfig.toggleDIK != 0 &&
        g_DragonbornConfig.toggleKeyName != "F1";
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

static std::atomic<float> g_WindowPosX{ -1.0f };
static std::atomic<float> g_WindowPosY{ -1.0f };
static std::atomic<float> g_SettingsWindowHeight{ 754.0f }; // user-resized Settings tab height (persisted)
static std::atomic<float> g_LauncherFontScale{ 0.9f };
static std::atomic<WORD> g_LauncherHotkeyDIK{ 0x3B };  // Default F1 (0x3B)
static std::atomic<WORD> g_LauncherHotkeyVK{ 0x70 };   // Default VK_F1 (0x70)
static std::atomic<bool> g_LauncherHotkeyCtrl{ false };
static std::atomic<bool> g_LauncherHotkeyShift{ false };
static std::atomic<bool> g_LauncherHotkeyAlt{ false };
static std::atomic<bool> g_LauncherHotkeyDoubleTap{ false };
// When on: opening still needs the full hotkey (modifier and/or double-tap), but a single
// bare press of the key alone closes whatever is open.
static std::atomic<bool> g_LauncherHotkeyEasyClose{ false };
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

// User-rebindable "alias" hotkey per mod: the key the launcher listens for to open that mod.
// Defaults to the mod's ORIGINAL key; the mod's real key stays relocated, so changing this
// edits no mod files — it only changes which key our bridge watches. Index order matches the
// chordDown[] array in PollOriginalHotkeyAliases.
// Indices 0..AI_Dragonborn are the chordDown[] watcher mods. AI_ReShade (raw-input bridge) and
// AI_MF (own bridge below) are rebindable too but handled outside the chordDown loop.
enum AliasIdx { AI_OAR = 0, AI_IED, AI_ENB, AI_DMenu, AI_IC, AI_FLICK, AI_DebugMenu, AI_KreatE, AI_CS, AI_CatMenu, AI_Dragonborn, AI_ReShade, AI_MF, AI_COUNT };
static std::atomic<WORD> g_AliasDik[AI_COUNT];
static std::atomic<bool> g_AliasCtrl[AI_COUNT];
static std::atomic<bool> g_AliasShift[AI_COUNT];
static std::atomic<bool> g_AliasAlt[AI_COUNT];
static const char* const g_AliasIds[AI_COUNT] = {
    "OAR", "IED", "ENB", "dMenu", "ImprovedCamera", "FLICK", "DebugMenu", "KreatE", "CS", "CatMenu", "Dragonborn", "ReShade", "MF"
};
static std::atomic<bool>* const g_AliasUnblock[AI_COUNT] = {
    &g_UnblockOAR, &g_UnblockIED, &g_UnblockENB, &g_UnblockDMenu, &g_UnblockImprovedCamera,
    &g_UnblockFLICK, &g_UnblockDebugMenu, &g_UnblockKreatE, &g_UnblockCS, &g_UnblockCatMenu,
    &g_UnblockDragonborn, &g_UnblockReShade, &g_UnblockMF
};
static std::atomic<int> g_CapturingAlias{ -1 }; // which alias row is in "press a key" capture (-1 = none)
static std::atomic<long long> g_SuppressAliasUntilMs{ 0 }; // brief grace after a rebind so the just-pressed key doesn't fire the mod

// True when this alias matches the launcher hotkey exactly (key + modifiers).
static bool AliasEqualsLauncher(int i) {
    return g_AliasDik[i].load() == g_LauncherHotkeyDIK.load() &&
           g_AliasCtrl[i].load() == g_LauncherHotkeyCtrl.load() &&
           g_AliasShift[i].load() == g_LauncherHotkeyShift.load() &&
           g_AliasAlt[i].load() == g_LauncherHotkeyAlt.load();
}

static void InitAliasDefaults() {
    auto set = [](int i, WORD d, bool c, bool s, bool a) {
        g_AliasDik[i].store(d); g_AliasCtrl[i].store(c); g_AliasShift[i].store(s); g_AliasAlt[i].store(a);
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
    set(AI_MF, 0x3B, false, false, false);      // F1 (launcher default)
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
    "MF", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "KreatE", "CS", "CatMenu", "Dragonborn", "ReShade"
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
        outfile << "SettingsHeight = " << g_SettingsWindowHeight.load() << "\n";
        outfile << "HotkeyDIK = " << g_LauncherHotkeyDIK.load() << "\n";
        outfile << "HotkeyCtrl = " << (g_LauncherHotkeyCtrl.load() ? 1 : 0) << "\n";
        outfile << "HotkeyShift = " << (g_LauncherHotkeyShift.load() ? 1 : 0) << "\n";
        outfile << "HotkeyAlt = " << (g_LauncherHotkeyAlt.load() ? 1 : 0) << "\n";
        outfile << "HotkeyDoubleTap = " << (g_LauncherHotkeyDoubleTap.load() ? 1 : 0) << "\n";
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
        outfile << "EnableLogging = " << (g_LoggingEnabled.load() ? 1 : 0) << "\n";
        for (int i = 0; i < AI_COUNT; ++i) {
            outfile << "Alias" << g_AliasIds[i] << " = " << g_AliasDik[i].load() << ","
                    << (g_AliasCtrl[i].load() ? 1 : 0) << "," << (g_AliasShift[i].load() ? 1 : 0) << ","
                    << (g_AliasAlt[i].load() ? 1 : 0) << "\n";
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
                } else if (key == "FONTSCALE") {
                    try {
                        g_LauncherFontScale.store(std::stof(val));
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
                } else if (key == "ENABLELOGGING") {
                    try { g_LoggingEnabled.store(std::stoi(val) != 0); } catch (...) {}
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

    std::vector<std::string> defaultOrder = {
        "MF", "OAR", "IED", "DebugMenu", "dMenu", "ImprovedCamera", "ENB", "FLICK", "KreatE", "CS", "CatMenu", "Dragonborn", "ReShade"
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
    ApplyLogLevel(); // honor the saved logging toggle
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
static std::atomic<bool> g_AllowCSOpen{ false };
static std::atomic<long long> g_AllowCSOpenUntilMs{ 0 };
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

static std::atomic<bool> g_AllowKreatEOpen{ false };
static std::atomic<long long> g_AllowKreatEOpenUntilMs{ 0 };
static std::atomic<bool> g_KreatEIniManaged{ false };
static std::atomic<long long> g_LastKreatEIniAttemptMs{ 0 };

static std::atomic<bool> g_AllowCatMenuOpen{ false };
static std::atomic<long long> g_AllowCatMenuOpenUntilMs{ 0 };
static std::atomic<bool> g_CatMenuIniManaged{ false };
static std::atomic<long long> g_LastCatMenuIniAttemptMs{ 0 };

static std::atomic<bool> g_AllowDragonbornOpen{ false };
static std::atomic<long long> g_AllowDragonbornOpenUntilMs{ 0 };
static std::atomic<bool> g_AllowReShadeOpen{ false };
static std::atomic<long long> g_AllowReShadeOpenUntilMs{ 0 };
static std::atomic<bool> g_DragonbornIniManaged{ false };
static std::atomic<long long> g_LastDragonbornIniAttemptMs{ 0 };

static constexpr WORD kEscapeDIK   = 0x01;  // ESC scan code

static long long NowMs();
static void InjectEngineKey(WORD dik, WORD modifier = 0);
static void InjectEngineTap(WORD dik);
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
static void CloseActiveModMenu(ActiveMenu active);
static void OpenAnimationReplacer();
static void OpenImmersiveEquipmentDisplays();
static void OpenENB();
static void OpenDebugMenu();
static void OpenSKSEMenuFramework();
static void OpenDMenu();
static void OpenImprovedCamera();
static void OpenFLICK();
static void OpenKreatE();
static void OpenCommunityShaders();
static void OpenCatMenu();
static void OpenDragonbornToolkit();
static void OpenReShade();
static bool SetCatMenuOpen(bool open);
static void TryManageKreatEHotkey(bool lateRetry = false);
static void TryManageCSHotkey(bool lateRetry = false);
static void TryManageCatMenuHotkey(bool lateRetry = false);
static void TryManageDragonbornHotkey(bool lateRetry = false);
static void RestoreLegacyKreatEBlocking();
static void RestoreAllModDefaults();

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
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t g_OrigGetRawInputData = nullptr;

static UINT WINAPI HookedGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    UINT res = g_OrigGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (res != (UINT)-1 && uiCommand == RID_INPUT && pData && cbSizeHeader == sizeof(RAWINPUTHEADER)) {
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            USHORT vkey = raw->data.keyboard.VKey;
            USHORT flags = raw->data.keyboard.Flags;
            bool isDown = (flags & RI_KEY_BREAK) == 0;
            if (vkey == g_LauncherHotkeyVK.load() && isDown && !IsRebinding()) {
                if (g_ActiveMenu.load() == ActiveMenu::ReShade) {
                    const auto now = NowMs();
                    if (now - g_LastLauncherToggleMs.load() > 700) {
                        g_LastLauncherToggleMs.store(now);
                        std::thread([]() {
                            CloseActiveModMenu(ActiveMenu::ReShade);
                        }).detach();
                        SKSE::log::info("HookedGetRawInputData: detected F1 keydown, closing ReShade.");
                    }
                }
            }

            // "Enable default hotkey" for ReShade: bridge its (rebindable) alias key live (no restart).
            // ReShade's real key is relocated to F22, so the alias does nothing natively — we forward
            // it here (the raw-input channel fires under ReShade whether the overlay is open or not).
            const WORD reshadeAliasVK = VKFromDIK(g_AliasDik[AI_ReShade].load());
            const auto asyncDown = [&](int vk) {
                return ((g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vk) : ::GetAsyncKeyState(vk)) & 0x8000) != 0;
            };
            const bool reshadeModsOk = asyncDown(VK_CONTROL) == g_AliasCtrl[AI_ReShade].load() &&
                                       asyncDown(VK_SHIFT) == g_AliasShift[AI_ReShade].load() &&
                                       asyncDown(VK_MENU) == g_AliasAlt[AI_ReShade].load();
            if (reshadeAliasVK != 0 && vkey == reshadeAliasVK && isDown && reshadeModsOk && !IsRebinding() &&
                !AliasEqualsLauncher(AI_ReShade) && g_UnblockReShade.load() && g_ReShadeConfig.enabled) {
                static std::atomic<long long> s_lastReShadeAliasMs{ 0 };
                const auto now = NowMs();
                if (now - s_lastReShadeAliasMs.load() > 700) {
                    const auto active = g_ActiveMenu.load();
                    if (active == ActiveMenu::ReShade) {
                        s_lastReShadeAliasMs.store(now);
                        std::thread([]() { CloseActiveModMenu(ActiveMenu::ReShade); }).detach();
                        SKSE::log::info("HookedGetRawInputData: Home alias closing ReShade.");
                    } else if (active == ActiveMenu::None && !IsExternalMenuOpen() &&
                               now >= g_MenuOpenLockUntilMs.load()) {
                        s_lastReShadeAliasMs.store(now);
                        std::thread([]() { OpenReShade(); }).detach();
                        SKSE::log::info("HookedGetRawInputData: Home alias opening ReShade.");
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
        // dMenu toggles on its key — re-emit it through the engine to close.
        g_AllowDMenuOpen.store(true);
        g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);
        InjectEngineKey(g_DMenuConfig.toggleDIK);
        SKSE::log::info("CloseActiveModMenu: closing dMenu via engine key event.");
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
        // Community Shaders toggles on its key — re-emit it through the engine to close.
        g_AllowCSOpen.store(true);
        g_AllowCSOpenUntilMs.store(NowMs() + 1000);
        InjectEngineKey(g_CSConfig.toggleDIK);
        SKSE::log::info("CloseActiveModMenu: closing Community Shaders via engine key event.");
    } else if (active == ActiveMenu::CatMenu) {
        if (SetCatMenuOpen(false)) {
            SKSE::log::info("CloseActiveModMenu: closed CatMenu directly.");
        }
    } else if (active == ActiveMenu::Dragonborn) {
        g_AllowDragonbornOpen.store(true);
        g_AllowDragonbornOpenUntilMs.store(NowMs() + 1000);
        InjectEngineTap(g_DragonbornConfig.toggleDIK);
        SKSE::log::info("CloseActiveModMenu: toggled Dragonborn's Toolkit closed with {}.", NameFromDIK(g_DragonbornConfig.toggleDIK));
    } else if (active == ActiveMenu::ReShade) {
        // ReShade's overlay closes on ESC (native) — and ESC reaches it even though its overlay
        // captures other input. (ReShade pauses the input update, so F1 can't be detected while
        // it's open; the user can also just press ESC themselves.)
        SimulateModifiedKey(0, kEscapeDIK);
        SKSE::log::info("CloseActiveModMenu: closed ReShade overlay via simulated ESC.");
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
    if (IsUserTyping()) {
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

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) { // [DIAG]
        int w = static_cast<int>(wParam);
        if (w == g_OARConfig.toggleVK || w == g_DMenuConfig.toggleVK ||
            w == g_FLICKConfig.toggleVK || w == g_IEDConfig.toggleVK)
            DiagOnce("WndProc-KEYDOWN", w, nullptr);
    }

    if (IsRebinding()) {
        return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
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

        // 2. Check for Risa's menu hotkey and ENB/OAR/IED window close
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
                        const bool act = dtap ? CheckLauncherDoubleTap(now)
                                              : (now - g_LastLauncherToggleMs.load() > 700);
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
                                        g_LauncherWindow->BlockUserInput.store(true);
                                        SKSE::log::info("WndProc: Launcher opened.");
                                    }
                                }
                            }
                        }
                    }

                if (g_LauncherHotkeyDoubleTap.load()) {
                    if (triggered) return 0;
                    return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
                }
                return 0; // block launcher hotkey completely from reaching other WndProc hooks
            }
            return ::CallWindowProc(g_OrigWndProc.load(), hWnd, uMsg, wParam, lParam);
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

        if (g_KreatEConfig.enabled && wParam == static_cast<WPARAM>(g_KreatEConfig.toggleVK) && !g_UnblockKreatE.load()) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
                if (!allowed) {
                    SKSE::log::info("WndProc: BLOCKED KreatE key 0x{:02X} (msg=0x{:04X})", wParam, uMsg);
                    return 0;
                }
            }
        }

        if (g_CatMenuConfig.enabled && wParam == static_cast<WPARAM>(g_CatMenuConfig.toggleVK) && !g_UnblockCatMenu.load()) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) {
                const bool allowed = g_AllowCatMenuOpen.load() && NowMs() <= g_AllowCatMenuOpenUntilMs.load();
                if (!allowed) return 0;
            }
        }

        if (g_DragonbornConfig.enabled && wParam == static_cast<WPARAM>(g_DragonbornConfig.toggleVK) && !g_UnblockDragonborn.load()) {
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
static std::atomic<long long> g_LastDIKeyboardPollMs{ 0 };
static std::atomic<long long> g_LastLauncherFallbackPressMs{ 0 };
static std::atomic<bool> g_LauncherFallbackLogged{ false };

// ----------------------------------------------------------------------------
// Per-mod block for the ImGui mods (OAR / dMenu / FLICK / Improved Camera).
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
    if (!idm) { DiagOnce("Suppress", 0xF0, nullptr); return; }   // [DIAG] no idm
    auto* kb = idm->GetKeyboard();
    if (!kb) { DiagOnce("Suppress", 0xF1, nullptr); return; }    // [DIAG] no keyboard
    DiagOnce("Suppress", 0xF2, nullptr);                         // [DIAG] running with keyboard
    const long long now = NowMs();
    auto zap = [&](bool enabled, WORD dik, bool unblock, bool allow, long long until) {
        if (!enabled || dik == 0) return;
        if (unblock) return;                 // user allows this mod's own hotkey
        if (allow && now <= until) return;   // we're opening this mod from the launcher right now
        if (kb->curState[dik] & 0x80) DiagOnce("CurStateWasPressed", dik, nullptr); // [DIAG] we saw it pressed + zeroed it
        kb->curState[dik] = 0;
        kb->prevState[dik] = 0;
    };
    zap(g_OARConfig.enabled,            g_OARConfig.toggleDIK,            g_UnblockOAR.load(),            g_AllowOAROpen.load(),            g_AllowOAROpenUntilMs.load());
    zap(g_DMenuConfig.enabled,          g_DMenuConfig.toggleDIK,          g_UnblockDMenu.load(),          g_AllowDMenuOpen.load(),          g_AllowDMenuOpenUntilMs.load());
    zap(g_FLICKConfig.enabled,          g_FLICKConfig.toggleDIK,          g_UnblockFLICK.load(),          g_AllowFLICKOpen.load(),          g_AllowFLICKOpenUntilMs.load());
    zap(g_ImprovedCameraConfig.enabled, g_ImprovedCameraConfig.toggleDIK, g_UnblockImprovedCamera.load(), g_AllowImprovedCameraOpen.load(), g_AllowImprovedCameraOpenUntilMs.load());
    zap(g_KreatEConfig.enabled,         g_KreatEConfig.toggleDIK,         g_UnblockKreatE.load(),         g_AllowKreatEOpen.load(),         g_AllowKreatEOpenUntilMs.load());
    zap(g_CatMenuConfig.enabled,        g_CatMenuConfig.toggleDIK,        g_UnblockCatMenu.load(),        g_AllowCatMenuOpen.load(),        g_AllowCatMenuOpenUntilMs.load());
    zap(g_DragonbornConfig.enabled,     g_DragonbornConfig.toggleDIK,     g_UnblockDragonborn.load(),     g_AllowDragonbornOpen.load(),     g_AllowDragonbornOpenUntilMs.load());
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
static std::atomic<WORD> g_TapInjectDIK{ 0 };
static std::atomic<int>  g_TapInjectStep{ 0 };

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
    g_OpenInjectStep.store(0);
    g_OpenInjectMod.store(modifier);
    g_OpenInjectDIK.store(dik);
}

static void InjectEngineTap(WORD dik) {
    if (dik == 0) return;
    g_TapInjectStep.store(0);
    g_TapInjectDIK.store(dik);
}

static void HookedKbProcess(RE::BSWin32KeyboardDevice* self, float a_dt) {
    g_OrigKbProcess(self, a_dt);
    if (!self) return;
    const long long now = NowMs();

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
                if (diStale && !simulating && !lockActive && !recentToggle &&
                    !IsRebinding() && !IsUserTyping()) {
                    const bool launcherOpen = g_LauncherWindow && g_LauncherWindow->IsOpen.load();
                    const bool somethingOpen = launcherOpen || g_ActiveMenu.load() != ActiveMenu::None;
                    if (g_LauncherHotkeyEasyClose.load() && somethingOpen) {
                        ToggleLauncher();
                    } else if (AreLauncherModifiersOk()) {
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

    // Synthesize a key event through the engine to open the mod the launcher requested.
    if (WORD openDik = g_OpenInjectDIK.load()) {
        const WORD mod = g_OpenInjectMod.load();
        int step = g_OpenInjectStep.fetch_add(1);
        if (step == 0) {                                                       // modifier (if any) + key down
            if (mod) self->SetButtonState(mod, 0.0f, false, true);
            self->SetButtonState(openDik, 0.0f, false, true);
        } else if (step < 4) {                                                 // held a few frames
            if (mod) self->SetButtonState(mod, a_dt, true, true);
            self->SetButtonState(openDik, a_dt, true, true);
        } else if (step == 4) {                                               // key up (modifier still held)
            self->SetButtonState(openDik, a_dt, true, false);
        } else if (step == 5) {                                               // modifier up
            if (mod) self->SetButtonState(mod, a_dt, true, false);
        } else { g_OpenInjectDIK.store(0); g_OpenInjectMod.store(0); g_OpenInjectStep.store(0); }
    }
    // CatMenu and Dragonborn's Toolkit toggle on every pressed frame. Give them one
    // down frame and one release frame, with no repeated held state in between.
    if (WORD tapDik = g_TapInjectDIK.load()) {
        int step = g_TapInjectStep.fetch_add(1);
        if (step == 0)      self->SetButtonState(tapDik, 0.0f, false, true);
        else if (step == 1) self->SetButtonState(tapDik, a_dt, true, false);
        else { g_TapInjectDIK.store(0); g_TapInjectStep.store(0); }
    }
    auto clear = [&](bool enabled, WORD dik, bool unblock, bool allow, long long until) {
        if (!enabled || dik == 0 || unblock) return;
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
        0x18, false, false, 0);
    clear(g_IEDConfig.enabled && g_UnblockIED.load() && IEDOriginalMoved() && plain,
        0x0E, false, false, 0);
    clear(g_ENBConfig.enabled && g_UnblockENB.load() && ENBOriginalMoved() && shift && !ctrl && !alt,
        0x1C, false, false, 0);
    clear(g_DMenuConfig.enabled && g_UnblockDMenu.load() && DMenuOriginalMoved() && plain,
        0xC7, false, false, 0);
    clear(g_ImprovedCameraConfig.enabled && g_UnblockImprovedCamera.load() && ImprovedCameraOriginalMoved() && shift && !ctrl && !alt,
        0xC7, false, false, 0);
    clear(g_FLICKConfig.enabled && g_UnblockFLICK.load() && FLICKOriginalMoved() && plain,
        0x41, false, false, 0);
    clear(g_DebugMenuConfig.enabled && g_UnblockDebugMenu.load() && DebugMenuOriginalMoved() &&
            g_LauncherHotkeyDIK.load() != 0x3B && plain,
        0x3B, false, false, 0);
    clear(g_KreatEConfig.enabled && g_UnblockKreatE.load() && KreatEOriginalMoved() && plain,
        0xCF, false, false, 0);
    clear(g_CatMenuConfig.enabled && g_UnblockCatMenu.load() && CatMenuOriginalMoved() && plain,
        0x40, false, false, 0);
    clear(g_DragonbornConfig.enabled && g_UnblockDragonborn.load() && DragonbornOriginalMoved() &&
            g_LauncherHotkeyDIK.load() != 0x3B && plain,
        0x3B, false, false, 0);

    clear(g_OARConfig.enabled,            g_OARConfig.toggleDIK,            g_UnblockOAR.load(),            g_AllowOAROpen.load(),            g_AllowOAROpenUntilMs.load());
    clear(g_DMenuConfig.enabled,          g_DMenuConfig.toggleDIK,          g_UnblockDMenu.load(),          g_AllowDMenuOpen.load(),          g_AllowDMenuOpenUntilMs.load());
    clear(g_FLICKConfig.enabled,          g_FLICKConfig.toggleDIK,          g_UnblockFLICK.load(),          g_AllowFLICKOpen.load(),          g_AllowFLICKOpenUntilMs.load());
    clear(g_ImprovedCameraConfig.enabled, g_ImprovedCameraConfig.toggleDIK, g_UnblockImprovedCamera.load(), g_AllowImprovedCameraOpen.load(), g_AllowImprovedCameraOpenUntilMs.load());
    clear(g_KreatEConfig.enabled,         g_KreatEConfig.toggleDIK,         g_UnblockKreatE.load(),         g_AllowKreatEOpen.load(),         g_AllowKreatEOpenUntilMs.load());
    clear(g_CatMenuConfig.enabled,        g_CatMenuConfig.toggleDIK,        g_UnblockCatMenu.load(),        g_AllowCatMenuOpen.load(),        g_AllowCatMenuOpenUntilMs.load());
    clear(g_DragonbornConfig.enabled,     g_DragonbornConfig.toggleDIK,     g_UnblockDragonborn.load(),     g_AllowDragonbornOpen.load(),     g_AllowDragonbornOpenUntilMs.load());
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
    const std::array<bool, 11> chordDown = {
        g_UnblockOAR.load() && g_OARConfig.enabled && OAROriginalMoved() && !AliasEqualsLauncher(AI_OAR) && aliasDown(AI_OAR),
        g_UnblockIED.load() && g_IEDConfig.enabled && IEDOriginalMoved() && !AliasEqualsLauncher(AI_IED) && aliasDown(AI_IED),
        g_UnblockENB.load() && g_ENBConfig.enabled && !enbAliasIsNative && !AliasEqualsLauncher(AI_ENB) && aliasDown(AI_ENB),
        g_UnblockDMenu.load() && g_DMenuConfig.enabled && DMenuOriginalMoved() && !AliasEqualsLauncher(AI_DMenu) && aliasDown(AI_DMenu),
        g_UnblockImprovedCamera.load() && g_ImprovedCameraConfig.enabled && ImprovedCameraOriginalMoved() && !AliasEqualsLauncher(AI_IC) && aliasDown(AI_IC),
        g_UnblockFLICK.load() && g_FLICKConfig.enabled && FLICKOriginalMoved() && !AliasEqualsLauncher(AI_FLICK) && aliasDown(AI_FLICK),
        g_UnblockDebugMenu.load() && g_DebugMenuConfig.enabled &&
            g_AliasDik[AI_DebugMenu].load() != g_DebugMenuConfig.toggleDIK && // alias == real key: it opens directly, don't also inject
            !AliasEqualsLauncher(AI_DebugMenu) && aliasDown(AI_DebugMenu),
        g_UnblockKreatE.load() && g_KreatEConfig.enabled && KreatEOriginalMoved() && !AliasEqualsLauncher(AI_KreatE) && aliasDown(AI_KreatE),
        g_UnblockCS.load() && g_CSConfig.enabled && CSOriginalMoved() && !AliasEqualsLauncher(AI_CS) && aliasDown(AI_CS),
        g_UnblockCatMenu.load() && g_CatMenuConfig.enabled && CatMenuOriginalMoved() && !AliasEqualsLauncher(AI_CatMenu) && aliasDown(AI_CatMenu),
        g_UnblockDragonborn.load() && g_DragonbornConfig.enabled && DragonbornOriginalMoved() &&
            !AliasEqualsLauncher(AI_Dragonborn) && aliasDown(AI_Dragonborn)
    };
    const std::array<bool, 11> primaryDown = {
        down(g_AliasDik[AI_OAR].load()), down(g_AliasDik[AI_IED].load()), down(g_AliasDik[AI_ENB].load()),
        down(g_AliasDik[AI_DMenu].load()), down(g_AliasDik[AI_IC].load()), down(g_AliasDik[AI_FLICK].load()),
        down(g_AliasDik[AI_DebugMenu].load()), down(g_AliasDik[AI_KreatE].load()), down(g_AliasDik[AI_CS].load()),
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

    for (std::size_t i = 0; i < chordDown.size(); ++i) {
        const bool pressed = chordDown[i] && !wasPrimaryDown[i];
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
        static bool mfWasDown = false;
        const bool mfDownNow = g_AliasDik[AI_MF].load() != 0 && down(g_AliasDik[AI_MF].load());
        const bool mfChord = g_UnblockMF.load() && !AliasEqualsLauncher(AI_MF) && aliasDown(AI_MF);
        if (mfChord && !mfWasDown) {
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
        mfWasDown = mfDownNow;
    }
}

static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(IDirectInputDevice8A* pDevice,
                                                       DWORD cbData, LPVOID lpvData) {
    void* _diagRA = _ReturnAddress(); // [DIAG]
    HRESULT hr = g_OrigGetDeviceState(pDevice, cbData, lpvData);
    SubclassGameWindow();
    // Zero each managed ImGui mod's toggle key in the engine's cached keyboard state so
    // their per-frame IsPressed() poll sees "not pressed" (gameplay ButtonEvents unaffected).
    SuppressManagedPollKeys();
    if (SUCCEEDED(hr) && lpvData && cbData == 256) {
        g_LastDIKeyboardPollMs.store(NowMs(), std::memory_order_relaxed);
        auto* state = reinterpret_cast<BYTE*>(lpvData);
        PollOriginalHotkeyAliases(state);

        // [DIAG] Identify who polls slot 9 with each leaking mod's toggle key pressed.
        for (WORD dik : { g_OARConfig.toggleDIK, g_DMenuConfig.toggleDIK, g_FLICKConfig.toggleDIK, g_IEDConfig.toggleDIK })
            if (dik != 0 && (state[dik] & 0x80)) DiagStack("GetDeviceState", dik);

        // Sink registration timing varies between plugins. Discover managed sinks from
        // this guaranteed keyboard poll instead of waiting for the launcher to render.
        static std::atomic<long long> lastSinkScanMs{ 0 };
        const long long now = NowMs();
        long long lastScan = lastSinkScanMs.load(std::memory_order_relaxed);
        if (now - lastScan >= 1000 &&
            lastSinkScanMs.compare_exchange_strong(lastScan, now, std::memory_order_relaxed)) {
            TryHookModSinks();
            TryHookKeyboardProcess();
            TryManageKreatEHotkey(true);
            TryManageCSHotkey(true);
            TryManageCatMenuHotkey(true);
            TryManageDragonbornHotkey(true);
        }

        if (g_AllowESCSinkBlock.load()) {
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
                } else if (AreLauncherModifiersOk()) {
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
    if (g_AllowESCSinkBlock.load() && dik == 0x01) return true;
    return false;
}

using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetDeviceData_t g_OrigGetDeviceData = nullptr;
static std::atomic<bool> g_DI8DataHookFiredOnce{ false };

static HRESULT STDMETHODCALLTYPE HookedGetDeviceData(IDirectInputDevice8A* pDevice, DWORD cbObjectData,
        LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    void* _diagRA = _ReturnAddress(); // [DIAG]
    HRESULT hr = g_OrigGetDeviceData(pDevice, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (SUCCEEDED(hr) && rgdod && pdwInOut && *pdwInOut > 0 && cbObjectData == sizeof(DIDEVICEOBJECTDATA)) {
        if (!g_DI8DataHookFiredOnce.exchange(true))
            SKSE::log::info("DI8 GetDeviceData hook firing — buffered input path is active.");

        DWORD count = *pdwInOut;
        DWORD outIdx = 0;
        for (DWORD i = 0; i < count; ++i) {
            WORD dik = static_cast<WORD>(rgdod[i].dwOfs & 0xFF);
            if (dik == g_OARConfig.toggleDIK || dik == g_DMenuConfig.toggleDIK ||
                dik == g_FLICKConfig.toggleDIK || dik == g_IEDConfig.toggleDIK) // [DIAG]
                DiagStack("GetDeviceData", dik);
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
    void* _diagRA = _ReturnAddress(); // [DIAG]
    const auto real = [&]() -> SHORT { return g_OrigGetAsyncKeyState ? g_OrigGetAsyncKeyState(vKey) : ::GetAsyncKeyState(vKey); };
    if (g_WaitingForHotkeyPress.load()) return real();
    if (vKey != 0 && (vKey == g_OARConfig.toggleVK || vKey == g_DMenuConfig.toggleVK ||
        vKey == g_FLICKConfig.toggleVK || vKey == g_IEDConfig.toggleVK ||
        vKey == g_KreatEConfig.toggleVK || vKey == g_CatMenuConfig.toggleVK ||
        vKey == g_DragonbornConfig.toggleVK)) // [DIAG]
        DiagStack("GetAsyncKeyState", vKey);

    // OAR polls its configured key through User32. Hide only the toggle key and only
    // when the call originates from OAR, leaving the same key intact everywhere else.
    const bool isOARKey = g_OARConfig.enabled && g_OARConfig.toggleVK != 0 &&
        vKey == g_OARConfig.toggleVK;
    if (isOARKey && !g_UnblockOAR.load() && !IsUserTyping()) {
        const bool allowed = g_AllowOAROpen.load() && NowMs() <= g_AllowOAROpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "OpenAnimationReplacer")) return 0;
    }

    const bool isKreatEKey = g_KreatEConfig.enabled && g_KreatEConfig.toggleVK != 0 &&
        vKey == g_KreatEConfig.toggleVK;
    if (isKreatEKey && !g_UnblockKreatE.load() && !IsUserTyping()) {
        const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "KreatE")) return 0;
    }

    const bool isCSKey = g_CSConfig.enabled && g_CSConfig.toggleVK != 0 &&
        vKey == g_CSConfig.toggleVK;
    if (isCSKey && !g_UnblockCS.load() && !IsUserTyping()) {
        const bool allowed = g_AllowCSOpen.load() && NowMs() <= g_AllowCSOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "CommunityShaders")) return 0;
    }

    const bool isCatMenuKey = g_CatMenuConfig.enabled && g_CatMenuConfig.toggleVK != 0 &&
        vKey == g_CatMenuConfig.toggleVK;
    if (isCatMenuKey && !g_UnblockCatMenu.load() && !IsUserTyping()) {
        const bool allowed = g_AllowCatMenuOpen.load() && NowMs() <= g_AllowCatMenuOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "catmenu")) return 0;
    }

    const bool isDragonbornKey = g_DragonbornConfig.enabled && g_DragonbornConfig.toggleVK != 0 &&
        vKey == g_DragonbornConfig.toggleVK;
    if (isDragonbornKey && !g_UnblockDragonborn.load() && !IsUserTyping()) {
        const bool allowed = g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "SkyrimCheatMenu")) return 0;
    }

    // Improved Camera polls its menu key. Hide the completed chord only from Improved
    // Camera so Skyrim and other mods can continue using the same gameplay binding.
    const bool isICKey = g_ImprovedCameraConfig.enabled && g_ImprovedCameraConfig.toggleVK != 0 &&
        vKey == g_ImprovedCameraConfig.toggleVK;
    if (isICKey && !g_UnblockImprovedCamera.load() && !IsUserTyping()) {
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
    void* _diagRA = _ReturnAddress(); // [DIAG]
    const auto real = [&]() -> SHORT { return g_OrigGetKeyState ? g_OrigGetKeyState(vKey) : ::GetKeyState(vKey); };
    if (g_WaitingForHotkeyPress.load()) return real();
    if (vKey != 0 && (vKey == g_OARConfig.toggleVK || vKey == g_DMenuConfig.toggleVK ||
        vKey == g_FLICKConfig.toggleVK || vKey == g_IEDConfig.toggleVK ||
        vKey == g_KreatEConfig.toggleVK || vKey == g_CatMenuConfig.toggleVK ||
        vKey == g_DragonbornConfig.toggleVK)) // [DIAG]
        DiagStack("GetKeyState", vKey);

    const bool isOARKey = g_OARConfig.enabled && g_OARConfig.toggleVK != 0 &&
        vKey == g_OARConfig.toggleVK;
    if (isOARKey && !g_UnblockOAR.load() && !IsUserTyping()) {
        const bool allowed = g_AllowOAROpen.load() && NowMs() <= g_AllowOAROpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "OpenAnimationReplacer")) return 0;
    }

    const bool isKreatEKey = g_KreatEConfig.enabled && g_KreatEConfig.toggleVK != 0 &&
        vKey == g_KreatEConfig.toggleVK;
    if (isKreatEKey && !g_UnblockKreatE.load() && !IsUserTyping()) {
        const bool allowed = g_AllowKreatEOpen.load() && NowMs() <= g_AllowKreatEOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "KreatE")) return 0;
    }

    const bool isCSKey = g_CSConfig.enabled && g_CSConfig.toggleVK != 0 &&
        vKey == g_CSConfig.toggleVK;
    if (isCSKey && !g_UnblockCS.load() && !IsUserTyping()) {
        const bool allowed = g_AllowCSOpen.load() && NowMs() <= g_AllowCSOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "CommunityShaders")) return 0;
    }

    const bool isCatMenuKey = g_CatMenuConfig.enabled && g_CatMenuConfig.toggleVK != 0 &&
        vKey == g_CatMenuConfig.toggleVK;
    if (isCatMenuKey && !g_UnblockCatMenu.load() && !IsUserTyping()) {
        const bool allowed = g_AllowCatMenuOpen.load() && NowMs() <= g_AllowCatMenuOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "catmenu")) return 0;
    }

    const bool isDragonbornKey = g_DragonbornConfig.enabled && g_DragonbornConfig.toggleVK != 0 &&
        vKey == g_DragonbornConfig.toggleVK;
    if (isDragonbornKey && !g_UnblockDragonborn.load() && !IsUserTyping()) {
        const bool allowed = g_AllowDragonbornOpen.load() && NowMs() <= g_AllowDragonbornOpenUntilMs.load();
        if (!allowed && IsCallerModule(_ReturnAddress(), "SkyrimCheatMenu")) return 0;
    }

    const bool isICKey = g_ImprovedCameraConfig.enabled && g_ImprovedCameraConfig.toggleVK != 0 &&
        vKey == g_ImprovedCameraConfig.toggleVK;
    if (isICKey && !g_UnblockImprovedCamera.load() && !IsUserTyping()) {
        const bool allowed = g_AllowImprovedCameraOpen.load() &&
            NowMs() <= g_AllowImprovedCameraOpenUntilMs.load();
        if (!allowed && IsImprovedCameraModifierDown() &&
            IsCallerModule(_ReturnAddress(), "ImprovedCameraSE")) return 0;
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

enum class ManagedSink { DMenu, IC, FLICK, IED, DebugMenu, CS, CatMenu, Dragonborn, MF };

static WORD SinkBlockDIK(ManagedSink m) {
    switch (m) {
        case ManagedSink::DMenu:     return g_DMenuConfig.toggleDIK;
        case ManagedSink::IC:        return g_ImprovedCameraConfig.toggleDIK;
        case ManagedSink::FLICK:     return g_FLICKConfig.toggleDIK;
        case ManagedSink::IED:       return g_IEDConfig.toggleDIK;
        case ManagedSink::DebugMenu: return g_DebugMenuConfig.toggleDIK;
        case ManagedSink::CS:        return g_CSConfig.toggleDIK;
        case ManagedSink::CatMenu:   return g_CatMenuConfig.toggleDIK;
        case ManagedSink::Dragonborn:return g_DragonbornConfig.toggleDIK;
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
        case ManagedSink::CS:        return g_UnblockCS.load()             || (g_AllowCSOpen.load()             && now <= g_AllowCSOpenUntilMs.load());
        case ManagedSink::CatMenu:   return g_UnblockCatMenu.load()        || (g_AllowCatMenuOpen.load()        && now <= g_AllowCatMenuOpenUntilMs.load());
        case ManagedSink::Dragonborn:return g_UnblockDragonborn.load()     || (g_AllowDragonbornOpen.load()     && now <= g_AllowDragonbornOpenUntilMs.load());
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
            for (RE::InputEvent* ev = a_event ? *a_event : nullptr; ev; ev = ev->next) { // [DIAG]
                if (IsButtonForDIK(ev, g_IEDConfig.toggleDIK) || IsButtonForDIK(ev, g_DMenuConfig.toggleDIK) ||
                    IsButtonForDIK(ev, g_FLICKConfig.toggleDIK) || IsButtonForDIK(ev, g_OARConfig.toggleDIK)) {
                    int dik = static_cast<const RE::ButtonEvent*>(ev)->GetIDCode();
                    DiagOnce("SinkSawDIK", dik, fn);
                }
            }
            // Engage the passCount guard while blocked OR during a simulated open/close window —
            // even when unblocked — so the simulated F2 only toggles DebugMenu once (otherwise the
            // held simulated key spams toggle events and the menu opens/closes repeatedly).
            const bool dbgSimWindow = g_AllowDebugMenuOpen.load() && NowMs() <= g_AllowDebugMenuOpenUntilMs.load();
            if (e.which == ManagedSink::DebugMenu && (!g_UnblockDebugMenu.load() || dbgSimWindow)) {
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
        { "CommunityShaders",           ManagedSink::CS,        g_CSConfig.enabled },
        { "catmenu",                    ManagedSink::CatMenu,   g_CatMenuConfig.enabled },
        { "SkyrimCheatMenu",            ManagedSink::Dragonborn,g_DragonbornConfig.enabled },
        { "SKSEMenuFramework",          ManagedSink::MF,        true },
    };

    for (auto* sink : source->sinks) {
        if (!sink) continue;
        DiagSinkOnce(sink); // [DIAG] log every input sink's owning module once
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

    // --- GetAsyncKeyState (caller-scoped polling for OAR, ENB and Improved Camera) ---
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

    if (g_ActiveMenu.load() == ActiveMenu::OAR && IsExternalMenuOpen()) {
        CloseLauncher();
        g_AllowOAROpen.store(true);
        g_AllowOAROpenUntilMs.store(NowMs() + 1000);
        InjectEngineKey(g_OARConfig.toggleDIK, g_OARConfig.shift ? 0x2A : 0); // OAR toggles on its key — re-emit to close
        SKSE::log::info("OpenAnimationReplacer: closing OAR via engine key event.");
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::OAR);
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowOAROpen.store(true);
    g_AllowOAROpenUntilMs.store(NowMs() + 1000);
    InjectEngineKey(g_OARConfig.toggleDIK, g_OARConfig.shift ? 0x2A : 0); // emit (Shift +) key through the engine
    SKSE::log::info("OpenAnimationReplacer: opening via engine key event {}{}.",
        g_OARConfig.shift ? "SHIFT + " : "", NameFromDIK(g_OARConfig.toggleDIK));
}

static void OpenImmersiveEquipmentDisplays() {
    if (!g_IEDConfig.enabled || g_IEDConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenImmersiveEquipmentDisplays: IED UI hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::IED && IsExternalMenuOpen()) {
        CloseLauncher();
        g_AllowIEDOpen.store(true);
        g_AllowIEDOpenUntilMs.store(NowMs() + 1000);
        InjectEngineKey(g_IEDConfig.toggleDIK); // IED toggles on its key — re-emit to close
        SKSE::log::info("OpenImmersiveEquipmentDisplays: closing IED via engine key event.");
        g_ActiveMenu.store(ActiveMenu::None);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::IED);
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowIEDOpen.store(true);
    g_AllowIEDOpenPolls.store(20);
    g_AllowIEDOpenUntilMs.store(NowMs() + 1000);
    InjectEngineKey(g_IEDConfig.toggleDIK); // emit the key through the engine
    SKSE::log::info("OpenImmersiveEquipmentDisplays: opening via engine key event {}.", NameFromDIK(g_IEDConfig.toggleDIK));
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
    g_AllowDMenuOpen.store(true);
    g_AllowDMenuOpenUntilMs.store(NowMs() + 1000);
    g_OpenInjectStep.store(0);
    g_OpenInjectDIK.store(g_DMenuConfig.toggleDIK); // emit the key through the engine (Process hook)
    SKSE::log::info("OpenDMenu: opening via engine key event {}.", NameFromDIK(g_DMenuConfig.toggleDIK));
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
        g_AllowImprovedCameraOpen.store(true);
        g_AllowImprovedCameraOpenUntilMs.store(NowMs() + 1000);
        SimulateModifiedKey(g_ImprovedCameraConfig.modifierDIK, g_ImprovedCameraConfig.toggleDIK);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_AllowImprovedCameraOpen.store(false);
        g_AllowImprovedCameraOpenUntilMs.store(0);
        SKSE::log::info("OpenImprovedCamera: opened ImprovedCamera via simulated Shift+MenuKey (0x{:02X}+0x{:02X}).",
            g_ImprovedCameraConfig.modifierDIK, g_ImprovedCameraConfig.toggleDIK);
    }).detach();
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
    g_LastLauncherToggleMs.store(NowMs());
    g_AllowCSOpen.store(true);
    g_AllowCSOpenUntilMs.store(NowMs() + 1000);
    g_OpenInjectStep.store(0);
    g_OpenInjectDIK.store(g_CSConfig.toggleDIK); // emit End through the engine (Process hook)
    SKSE::log::info("OpenCommunityShaders: opening via engine key event {}.", NameFromDIK(g_CSConfig.toggleDIK));
}

static void OpenCatMenu() {
    if (!g_CatMenuConfig.enabled || g_CatMenuConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenCatMenu: CatMenu hotkey is not configured.");
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

static void OpenDragonbornToolkit() {
    if (!g_DragonbornConfig.enabled || g_DragonbornConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenDragonbornToolkit: Dragonborn's Toolkit hotkey is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::Dragonborn) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::Dragonborn);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::Dragonborn);
    g_LastLauncherToggleMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 1000);
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        g_AllowDragonbornOpen.store(true);
        g_AllowDragonbornOpenUntilMs.store(NowMs() + 1000);
        InjectEngineTap(g_DragonbornConfig.toggleDIK);
        SKSE::log::info("OpenDragonbornToolkit: toggled Dragonborn's Toolkit with {}.", NameFromDIK(g_DragonbornConfig.toggleDIK));
    }).detach();
}

static void OpenReShade() {
    if (!g_ReShadeConfig.enabled || g_ReShadeConfig.toggleDIK == 0) {
        SKSE::log::warn("OpenReShade: ReShade overlay key is not configured.");
        return;
    }

    if (g_ActiveMenu.load() == ActiveMenu::ReShade) {
        CloseLauncher();
        CloseActiveModMenu(ActiveMenu::ReShade);
        g_LastLauncherToggleMs.store(NowMs());
        return;
    }

    CloseLauncher();
    g_ActiveMenu.store(ActiveMenu::ReShade);
    g_LastLauncherToggleMs.store(NowMs());
    g_MenuOpenLockUntilMs.store(NowMs() + 1000);
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        g_AllowReShadeOpen.store(true);
        g_AllowReShadeOpenUntilMs.store(NowMs() + 1000);
        // ReShade reads its overlay key from window messages + raw input; SendInput drives both.
        SimulateModifiedKey(g_ReShadeConfig.modifierDIK, g_ReShadeConfig.toggleDIK);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        g_AllowReShadeOpen.store(false);
        g_AllowReShadeOpenUntilMs.store(0);
        SKSE::log::info("OpenReShade: toggled ReShade overlay with {}.", NameFromDIK(g_ReShadeConfig.toggleDIK));
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
        const long long now = NowMs();
        if ((g_AllowDebugMenuOpen.load() && now <= g_AllowDebugMenuOpenUntilMs.load()) ||
            (g_AllowDragonbornOpen.load() && now <= g_AllowDragonbornOpenUntilMs.load())) {
            return false; // let simulated F1 pass!
        }
        if (IsENBOpeningTransition()) {
            return true;
        }
        if (now < g_MenuOpenLockUntilMs.load()) {
            return true; // block physical hotkey!
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
                } else if (AreLauncherModifiersOk()) {
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
        return true;
    }

    if (!btn->IsDown())
        return false;

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
    SKSE::log::info("SKSE Menu Framework: detected={}, key={}, mode={}, INI result={}",
        GetModuleHandleA("SKSEMenuFramework.dll") != nullptr,
        g_MFConfig.toggleKeyName.empty() ? "Unknown" : g_MFConfig.toggleKeyName,
        g_MFConfig.toggleMode.empty() ? "Unknown" : g_MFConfig.toggleMode,
        IniWriteStatus(g_MFIniWriteResult.load()));
    SKSE::log::info("Open Animation Replacer: detected={}, key={}, original Shift+O={}",
        g_OARConfig.enabled,
        g_OARConfig.enabled ? FormatHotkey(g_OARConfig.toggleDIK, g_OARConfig.ctrl, g_OARConfig.shift, g_OARConfig.alt) : "N/A",
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
    SKSE::log::info("CatMenu: detected={}, key={}, original F6={}, JSON managed={}",
        g_CatMenuConfig.enabled, g_CatMenuConfig.enabled ? FormatHotkey(g_CatMenuConfig.toggleDIK) : "N/A",
        g_UnblockCatMenu.load() ? "enabled" : "disabled", g_CatMenuIniManaged.load());
    SKSE::log::info("Dragonborn's Toolkit: detected={}, key={}, original F1={}, JSON managed={}",
        g_DragonbornConfig.enabled, g_DragonbornConfig.enabled ? FormatHotkey(g_DragonbornConfig.toggleDIK) : "N/A",
        g_UnblockDragonborn.load() ? "enabled" : "disabled", g_DragonbornIniManaged.load());
    SKSE::log::info("Hooks: DI state={}, DI buffered={}, GetAsyncKeyState={}, GetKeyState={}, framework callback={}",
        g_OrigGetDeviceState != nullptr, g_OrigGetDeviceData != nullptr,
        g_OrigGetAsyncKeyState != nullptr, g_OrigGetKeyState != nullptr,
        g_FrameworkInputEvent != nullptr);
    SKSE::log::info("Privacy: data-relative paths only; no usernames, save names, hardware IDs, or unrelated mod list.");
    SKSE::log::info("=====================================================");
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

    // Per-tab window behavior:
    //   Launcher tab — auto-sizes to its buttons (as before).
    //   Settings tab — can get long, so it's manually resizable VERTICALLY only (width is
    //   locked via equal min/max width) and shows a vertical scrollbar. The active tab is
    //   known one frame late, which is fine.
    static bool  s_settingsActive = false;       // which tab was active last frame
    static bool  s_prevSettingsActive = false;
    static float s_baseMenuWidth = 0.0f;         // unscaled width captured on the Launcher tab

    const bool settingsActive = s_settingsActive;
    const bool justSwitchedToSettings = settingsActive && !s_prevSettingsActive;
    s_prevSettingsActive = settingsActive;

    int kFlags = ImGuiMCP::ImGuiWindowFlags_NoCollapse;
    if (settingsActive) {
        const float layoutScaleVal = g_LauncherFontScale.load() / 0.9f;
        const float w = (s_baseMenuWidth > 50.0f) ? (s_baseMenuWidth * layoutScaleVal) : (612.0f * layoutScaleVal);
        float maxH = 800.0f;
        if (auto* io = ImGuiMCP::GetIO()) maxH = io->DisplaySize.y * 0.92f;
        const float minH = 220.0f;
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

    if (g_WindowPosX.load() != -1.0f && g_WindowPosY.load() != -1.0f) {
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(g_WindowPosX.load(), g_WindowPosY.load()), ImGuiMCP::ImGuiCond_FirstUseEver, ImGuiMCP::ImVec2(0.0f, 0.0f));
    } else if (auto* io = ImGuiMCP::GetIO()) {
        ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f), ImGuiMCP::ImGuiCond_FirstUseEver, ImGuiMCP::ImVec2(0.5f, 0.5f));
    }

    bool open = true;
    if (!ImGuiMCP::Begin("Risa's Menu Launcher", &open, kFlags)) {
        ImGuiMCP::PopStyleVar(2); ImGuiMCP::PopStyleColor(4); ImGuiMCP::End(); return;
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
    } else {
        const float layoutScaleVal = g_LauncherFontScale.load() / 0.9f;
        s_baseMenuWidth = ImGuiMCP::GetWindowWidth() / layoutScaleVal;
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

    const bool hasMF = IsPluginPresent("SKSEMenuFramework");
    const bool hasOAR = g_OARConfig.enabled;
    const bool hasIED = g_IEDConfig.enabled;
    const bool hasENB = g_ENBConfig.enabled;
    const bool hasDebugMenu = g_DebugMenuConfig.enabled;
    const bool hasDMenu = g_DMenuConfig.enabled;
    const bool hasImprovedCamera = g_ImprovedCameraConfig.enabled;
    const bool hasFLICK = g_FLICKConfig.enabled;
    const bool hasKreatE = g_KreatEConfig.enabled;
    const bool hasCS = g_CSConfig.enabled;
    const bool hasCatMenu = g_CatMenuConfig.enabled;
    const bool hasDragonborn = g_DragonbornConfig.enabled;
    const bool hasReShade = g_ReShadeConfig.enabled;

    struct LauncherButton {
        std::string id;
        std::string icon;
        std::string name;
        void (*action)();
        bool active;
    };

    std::vector<LauncherButton> allButtons;
    allButtons.push_back({ "MF", FontAwesome::UnicodeToUtf8(0xf013), "SKSE Menu Framework", OpenSKSEMenuFramework, hasMF });
    allButtons.push_back({ "OAR", FontAwesome::UnicodeToUtf8(0xf144), "Open Animation Replacer", OpenAnimationReplacer, hasOAR });
    allButtons.push_back({ "IED", FontAwesome::UnicodeToUtf8(0xf132), "Immersive Equipment Displays", OpenImmersiveEquipmentDisplays, hasIED });
    allButtons.push_back({ "DebugMenu", FontAwesome::UnicodeToUtf8(0xf188), "Debug Menu", OpenDebugMenu, hasDebugMenu });
    allButtons.push_back({ "dMenu", FontAwesome::UnicodeToUtf8(0xf520), "dMenu", OpenDMenu, hasDMenu });
    allButtons.push_back({ "ImprovedCamera", FontAwesome::UnicodeToUtf8(0xf030), "Improved Camera SE", OpenImprovedCamera, hasImprovedCamera });
    allButtons.push_back({ "ENB", FontAwesome::UnicodeToUtf8(0xf53f), "ENB Editor", OpenENB, hasENB });
    allButtons.push_back({ "FLICK", FontAwesome::UnicodeToUtf8(0xf1b3), "FLICK", OpenFLICK, hasFLICK });
    allButtons.push_back({ "KreatE", FontAwesome::UnicodeToUtf8(0xf6c3), "KreatE", OpenKreatE, hasKreatE });
    allButtons.push_back({ "CS", FontAwesome::UnicodeToUtf8(0xf043), "Community Shaders", OpenCommunityShaders, hasCS });
    allButtons.push_back({ "CatMenu", FontAwesome::UnicodeToUtf8(0xf6be), "CatMenu", OpenCatMenu, hasCatMenu });
    allButtons.push_back({ "Dragonborn", FontAwesome::UnicodeToUtf8(0xf6d5), "Dragonborn's Toolkit", OpenDragonbornToolkit, hasDragonborn });
    allButtons.push_back({ "ReShade", FontAwesome::UnicodeToUtf8(0xf5aa), "ReShade", OpenReShade, hasReShade });

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
                    const float centerY = (itemMin.y + itemMax.y) * 0.5f;
                    const auto color = ImGuiMCP::GetColorU32(static_cast<ImGuiMCP::ImGuiCol>(0));
                    auto* drawList = ImGuiMCP::GetWindowDrawList();
                    ImGuiMCP::ImDrawListManager::AddText(drawList, iconFont, fontSize,
                        ImGuiMCP::ImVec2(left + (iconSlotWidth - iconSize.x) * 0.5f, centerY - iconSize.y * 0.5f),
                        color, button.icon.c_str());
                    ImGuiMCP::ImDrawListManager::AddText(drawList, textFont, fontSize,
                        ImGuiMCP::ImVec2(left + iconSlotWidth + 8.0f * layoutScale, centerY - nameSize.y * 0.5f),
                        color, button.name.c_str());
                };

                auto DrawButton = [&](size_t i) {
                    bool disabled = (buttons[i].id == "DebugMenu" ||
                                     buttons[i].id == "IED" ||
                                     buttons[i].id == "ENB") && !IsGameLoaded();

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

        const bool settingsTabOpen = ImGuiMCP::BeginTabItem((FontAwesome::UnicodeToUtf8(0xf013) + "  Settings").c_str(), nullptr, 0);
        s_settingsActive = settingsTabOpen; // drives the per-tab resize/scroll behavior next frame
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
                ImGuiMCP::ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiMCP::ImGuiWindowFlags_NoNav); // NoNav: stop Home/End scrolling the list
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

            SectionHeader("Menu");
            ImGuiMCP::Text("UI Scale:");
            float scale = g_LauncherFontScale.load();
            if (ImGuiMCP::SliderFloat("##FontScaleSlider", &scale, 0.6f, 1.4f, "%.2f", 0)) {
                g_LauncherFontScale.store(scale);
                SaveButtonOrder();
            }

            ImGuiMCP::Text("Launcher Toggle Hotkey:");
            const bool waitingForKey = g_WaitingForHotkeyPress.load();
            std::string hotkeyBtnLabel;
            if (waitingForKey) {
                hotkeyBtnLabel = "[ Press any key... ]##LauncherHotkeyBtn";
            } else {
                std::string prefix = "";
                if (g_LauncherHotkeyCtrl.load()) prefix += "Ctrl + ";
                if (g_LauncherHotkeyShift.load()) prefix += "Shift + ";
                if (g_LauncherHotkeyAlt.load()) prefix += "Alt + ";
                hotkeyBtnLabel = prefix + NameFromDIK(g_LauncherHotkeyDIK.load()) + "##LauncherHotkeyBtn";
            }
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button,        ImGuiMCP::ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, ImGuiMCP::ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive,  ImGuiMCP::ImVec4(0.24f, 0.24f, 0.30f, 1.0f));
            if (ImGuiMCP::Button(hotkeyBtnLabel.c_str(), launcherButtonSize)) {
                if (!g_WaitingForHotkeyPress.load()) {
                    g_WaitingForHotkeyPress.store(true);
                }
            }
            ImGuiMCP::PopStyleColor(3);
            if (!waitingForKey) {
                ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
                ImGuiMCP::AlignTextToFramePadding();
                ImGuiMCP::TextDisabled("(Click to change)");
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
            ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
            if (ImGuiMCP::Checkbox("Double Tap", &doubleTapVal)) { g_LauncherHotkeyDoubleTap.store(doubleTapVal); hotkeyOptChanged = true; }

            if (ImGuiMCP::Checkbox("Easy Close (single key)", &easyCloseVal)) { g_LauncherHotkeyEasyClose.store(easyCloseVal); hotkeyOptChanged = true; }
            ImGuiMCP::SetItemTooltip("Opening still needs the full hotkey (modifier and/or double-tap).\nWhile open, a single press of the key alone closes it.");

            if (hotkeyOptChanged) {
                SaveButtonOrder();
            }

            SectionHeader("Hotkey Control");

            // Each category is a collapsing header with its own table. catTableOpen tracks the
            // table for whichever category is currently being drawn.
            bool catTableOpen = false;
            auto BeginCategory = [&](const char* label) -> bool {
                const bool open = ImGuiMCP::CollapsingHeader(label, ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen);
                catTableOpen = false;
                if (open) {
                    const std::string tblId = std::string("HKTbl_") + label;
                    ImGuiMCP::ImVec2 catAvail; ImGuiMCP::GetContentRegionAvail(&catAvail);
                    catTableOpen = ImGuiMCP::BeginTable(tblId.c_str(), 5, ImGuiMCP::ImGuiTableFlags_SizingStretchProp,
                        ImGuiMCP::ImVec2(catAvail.x - 12.0f * layoutScale, 0.0f)); // small right margin off the window edge
                    if (catTableOpen) {
                        // name | spacer | key (centered) | spacer | toggle (at the right edge)
                        ImGuiMCP::TableSetupColumn("Mod", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 200.0f * layoutScale);
                        ImGuiMCP::TableSetupColumn("##sp1", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGuiMCP::TableSetupColumn("Key", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 150.0f * layoutScale);
                        ImGuiMCP::TableSetupColumn("##sp2", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGuiMCP::TableSetupColumn("Enabled", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 44.0f * layoutScale);
                    }
                }
                return open;
            };
            auto EndCategory = [&]() {
                if (catTableOpen) { ImGuiMCP::EndTable(); catTableOpen = false; }
            };

            auto SmallTooltip = [&](const std::string& text) {
                if (text.empty()) return;
                if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled) && ImGuiMCP::BeginTooltip()) {
                    ImGuiMCP::SetWindowFontScale(uiScale * 0.8f); // smaller than the UI text
                    ImGuiMCP::TextUnformatted(text.c_str());
                    ImGuiMCP::EndTooltip();
                }
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
                if (aliasIsLauncher(idx)) return "Clashes with the launcher hotkey.";
                for (int j = 0; j < AI_COUNT; ++j)
                    if (j != idx && g_AliasUnblock[j]->load() && aliasEq(idx, j))
                        return std::string("Clashes with ") + g_AliasIds[j] + " (enabled).";
                return "";
            };

            auto DrawOriginalHotkeyRow = [&](const char* id, const char* modName, const char* defaultHotkey,
                    const std::string& tooltip, const std::string& disabledReason, int aliasIdx,
                    std::atomic<bool>& setting, bool installed, bool originalAvailable, auto simulateAction) {
                if (installed && catTableOpen) {
                    ImGuiMCP::TableNextRow(0, 36.0f * layoutScale);

                    // Column 0: mod name button (uniform width).
                    ImGuiMCP::TableSetColumnIndex(0);
                    const std::string nameBtnLabel = std::string(modName) + "##ModBtn_" + id;
                    if (ImGuiMCP::Button(nameBtnLabel.c_str(), ImGuiMCP::ImVec2(-1.0f, 0.0f))) {
                        simulateAction();
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
                            lbl = std::string("[ press a key... ]##alias") + id;
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
                                        // If the user set it to the launcher key, auto-disable so the
                                        // launcher and the mod can't both live on the same key.
                                        if (AliasEqualsLauncher(aliasIdx)) setting.store(false);
                                        g_CapturingAlias.store(-1);
                                        g_SuppressAliasUntilMs.store(NowMs() + 1000); // don't let this keypress fire the mod
                                        SaveButtonOrder();
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    // Column 4 (right edge): the enable toggle — OR, when the key clashes, the
                    // warning marker replaces the toggle entirely.
                    ImGuiMCP::TableSetColumnIndex(4);
                    const std::string conflict = (aliasIdx >= 0) ? AliasConflict(aliasIdx) : std::string();
                    if (!conflict.empty()) {
                        // Nudge the ⚠ to sit where the checkbox would (right + down).
                        ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + 8.0f * layoutScale);
                        ImGuiMCP::SetCursorPosY(ImGuiMCP::GetCursorPosY() + 4.0f * layoutScale);
                        ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s",
                            FontAwesome::UnicodeToUtf8(0xf071).c_str()); // warning triangle in the toggle's slot
                        SmallTooltip(conflict);
                    } else {
                        bool val = originalAvailable && setting.load();
                        ImGuiMCP::BeginDisabled(!originalAvailable);
                        if (!originalAvailable) {
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_FrameBg,        ImGuiMCP::ImVec4(0.16f, 0.16f, 0.17f, 0.45f));
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_FrameBgHovered, ImGuiMCP::ImVec4(0.16f, 0.16f, 0.17f, 0.45f));
                            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_CheckMark,      ImGuiMCP::ImVec4(0.40f, 0.40f, 0.42f, 0.40f));
                        }
                        const std::string checkboxId = "##EnableOriginal_" + std::string(id);
                        if (ImGuiMCP::Checkbox(checkboxId.c_str(), &val) && originalAvailable) {
                            setting.store(val);
                            SaveButtonOrder();
                        }
                        if (!originalAvailable) ImGuiMCP::PopStyleColor(3);
                        ImGuiMCP::EndDisabled();
                        if (!originalAvailable) SmallTooltip(disabledReason);
                    }
                }
            };

            // Tooltip always shows the mod's untouched original hotkey (the editable field shows
            // the current/custom key). The second arg is ignored, kept for call-site compatibility.
            auto MakeTooltip = [](const std::string& originalHotkey, const std::string&) {
                return "Original: " + originalHotkey;
            };

            const std::string oarTooltip = MakeTooltip("SHIFT + O",
                FormatHotkey(g_OARConfig.toggleDIK, g_OARConfig.ctrl, g_OARConfig.shift, g_OARConfig.alt));
            const std::string iedTooltip = MakeTooltip("BACKSPACE", FormatHotkey(g_IEDConfig.toggleDIK));

            std::string enbHotkey;
            if (g_ENBConfig.combinationVK != 0) {
                enbHotkey = FormatHotkey(DIKFromVK(static_cast<WORD>(g_ENBConfig.combinationVK))) + " + ";
            }
            enbHotkey += FormatHotkey(DIKFromVK(static_cast<WORD>(g_ENBConfig.editorVK)));
            const std::string enbTooltip = MakeTooltip("SHIFT + ENTER", enbHotkey);

            const std::string debugMenuTooltip = MakeTooltip("F1", FormatHotkey(g_DebugMenuConfig.toggleDIK));
            const std::string dMenuTooltip = MakeTooltip("HOME",
                FormatModifiedHotkey(g_DMenuConfig.toggleDIK, g_DMenuConfig.modifierDIK));
            const std::string icTooltip = MakeTooltip("SHIFT + HOME",
                FormatModifiedHotkey(g_ImprovedCameraConfig.toggleDIK, g_ImprovedCameraConfig.modifierDIK));
            const std::string flickTooltip = MakeTooltip("F7", FormatHotkey(g_FLICKConfig.toggleDIK));
            const std::string kreateTooltip = MakeTooltip("END", FormatHotkey(g_KreatEConfig.toggleDIK));
            const std::string csTooltip = MakeTooltip("END", FormatHotkey(g_CSConfig.toggleDIK));
            const std::string catMenuTooltip = MakeTooltip("F6", FormatHotkey(g_CatMenuConfig.toggleDIK));
            const std::string dragonbornTooltip = MakeTooltip("F1", FormatHotkey(g_DragonbornConfig.toggleDIK));
            const std::string reshadeTooltip = MakeTooltip("HOME", FormatHotkey(g_ReShadeConfig.toggleDIK));
            const std::string mfTooltip = MakeTooltip("F1", "");

            // A mod's toggle is only blocked if its (rebindable) alias key equals the launcher
            // hotkey — clicking the key to rebind it elsewhere frees the toggle.
            auto aliasEqualsLauncher = [&](int idx) {
                return g_AliasDik[idx].load() == g_LauncherHotkeyDIK.load() &&
                       g_AliasCtrl[idx].load() == g_LauncherHotkeyCtrl.load() &&
                       g_AliasShift[idx].load() == g_LauncherHotkeyShift.load() &&
                       g_AliasAlt[idx].load() == g_LauncherHotkeyAlt.load();
            };
            const std::string launcherClashReason =
                "Can't enable: this key is the launcher hotkey.\nClick the key to rebind it to something else.";

            const bool debugOriginalAvailable = !aliasEqualsLauncher(AI_DebugMenu);
            const std::string& debugOriginalTooltip = debugMenuTooltip;
            const std::string& debugDisabledReason = launcherClashReason;

            const bool dragonbornOriginalAvailable = !aliasEqualsLauncher(AI_Dragonborn);
            const std::string& dragonbornOriginalTooltip = dragonbornTooltip;
            const std::string& dragonbornDisabledReason = launcherClashReason;

            const bool anyMenusTools = hasMF || hasDMenu || hasFLICK || hasCatMenu || hasDragonborn || hasDebugMenu;
            const bool anyAnimGear   = hasOAR || hasIED;
            const bool anyGraphics   = hasENB || hasCS || hasImprovedCamera || hasReShade || hasKreatE;

            if (anyMenusTools && BeginCategory("Menus & Tools")) {
                DrawOriginalHotkeyRow("MF", "SKSE Menu Framework", "F1", mfTooltip, "", AI_MF, g_UnblockMF, hasMF, true, []() {
                    OpenSKSEMenuFramework();
                });
                DrawOriginalHotkeyRow("dMenu", "dMenu", "Home", dMenuTooltip, "", AI_DMenu, g_UnblockDMenu, hasDMenu, true, []() {
                    OpenDMenu();
                });
                DrawOriginalHotkeyRow("FLICK", "FLICK", "F7", flickTooltip, "", AI_FLICK, g_UnblockFLICK, hasFLICK, true, []() {
                    OpenFLICK();
                });
                DrawOriginalHotkeyRow("CatMenu", "CatMenu", "F6", catMenuTooltip, "", AI_CatMenu, g_UnblockCatMenu, hasCatMenu, true, []() {
                    OpenCatMenu();
                });
                DrawOriginalHotkeyRow("Dragonborn", "Dragonborn's Toolkit", "F1", dragonbornOriginalTooltip, dragonbornDisabledReason,
                    AI_Dragonborn, g_UnblockDragonborn, hasDragonborn, dragonbornOriginalAvailable, []() {
                    OpenDragonbornToolkit();
                });
                DrawOriginalHotkeyRow("DebugMenu", "Debug Menu", "F1", debugOriginalTooltip, debugDisabledReason,
                    AI_DebugMenu, g_UnblockDebugMenu, hasDebugMenu, debugOriginalAvailable, []() {
                    OpenDebugMenu();
                });
            }
            EndCategory();

            if (anyAnimGear && BeginCategory("Animation & Gear")) {
                DrawOriginalHotkeyRow("OAR", "Open Animation Replacer", "Shift + O", oarTooltip, "", AI_OAR, g_UnblockOAR, hasOAR, true, []() {
                    OpenAnimationReplacer();
                });
                DrawOriginalHotkeyRow("IED", "IED", "Backspace", iedTooltip, "", AI_IED, g_UnblockIED, hasIED, true, []() {
                    OpenImmersiveEquipmentDisplays();
                });
            }
            EndCategory();

            if (anyGraphics && BeginCategory("Visual and Lighting")) {
                DrawOriginalHotkeyRow("ENB", "ENB Editor", "Shift + Enter", enbTooltip, "", AI_ENB, g_UnblockENB, hasENB, true, []() {
                    OpenENB();
                });
                DrawOriginalHotkeyRow("CS", "Community Shaders", "End", csTooltip, "", AI_CS, g_UnblockCS, hasCS, true, []() {
                    OpenCommunityShaders();
                });
                DrawOriginalHotkeyRow("ImprovedCamera", "Improved Camera", "Shift + Home", icTooltip, "",
                    AI_IC, g_UnblockImprovedCamera, hasImprovedCamera, true, []() {
                    OpenImprovedCamera();
                });
                DrawOriginalHotkeyRow("ReShade", "ReShade", "Home", reshadeTooltip, "", AI_ReShade, g_UnblockReShade, hasReShade, true, []() {
                    OpenReShade();
                });
                DrawOriginalHotkeyRow("KreatE", "KreatE", "End", kreateTooltip, "", AI_KreatE, g_UnblockKreatE, hasKreatE, true, []() {
                    OpenKreatE();
                });
            }
            EndCategory();

            SectionHeader("Maintenance");
            {
                bool logVal = g_LoggingEnabled.load();
                if (ImGuiMCP::Checkbox("Enable logging", &logVal)) {
                    g_LoggingEnabled.store(logVal);
                    ApplyLogLevel();
                    SaveButtonOrder();
                }
                ImGuiMCP::SetItemTooltip("Documents\\My Games\\Skyrim Special Edition\\SKSE\\RisaAllInOneMenu.log");

                static bool s_confirmRestore = false;
                static bool s_restoreDone = false;
                static bool s_scrollToBottom = false;
                if (s_restoreDone) {
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.40f, 1.00f, 0.55f, 1.0f),
                        "All mod hotkeys restored to their defaults.");
                    ImGuiMCP::Text("Restart Skyrim once, then it's safe to uninstall this mod.");
                } else if (s_confirmRestore) {
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.00f, 0.65f, 0.30f, 1.0f),
                        "Are you sure? This reverts every mod to its original hotkey.");
                    if (ImGuiMCP::Button("Yes, restore for uninstall", ImGuiMCP::ImVec2(0.0f, 0.0f))) {
                        RestoreAllModDefaults();
                        s_confirmRestore = false;
                        s_restoreDone = true;
                    }
                    ImGuiMCP::SameLine(0.0f, 8.0f * layoutScale);
                    if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(0.0f, 0.0f))) {
                        s_confirmRestore = false;
                    }
                    // Confirm buttons just appeared below the fold — scroll them into view.
                    if (s_scrollToBottom) {
                        ImGuiMCP::SetScrollHereY(1.0f);
                        s_scrollToBottom = false;
                    }
                } else {
                    if (ImGuiMCP::Button("Restore Hotkeys for Uninstall", launcherButtonSize)) {
                        s_confirmRestore = true;
                        s_scrollToBottom = true;
                    }
                    ImGuiMCP::SetItemTooltip("Run this BEFORE uninstalling: it rewrites each managed mod's config\nback to its original hotkey, undoing every change this mod made.");
                }
            }

            ImGuiMCP::PopStyleColor(); // separator color
            ImGuiMCP::EndChild();
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
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    ApplyLogLevel(); // off by default; LoadButtonOrder re-applies the saved toggle after settings load
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
        if (g_KreatEConfig.enabled    && dik == g_KreatEConfig.toggleDIK)    return true;
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
        if (g_KreatEConfig.enabled         && dik == g_KreatEConfig.toggleDIK)         return true;
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
    if (!out.is_open()) { SKSE::log::error("SetIniValue: cannot write {}.", ini.string()); return -1; }
    for (const auto& l : lines) out << l << "\n";
    return 1;
}

static void TryManageKreatEHotkey(bool lateRetry) {
    if (g_KreatEIniManaged.load() || !IsPluginPresent("KreatE")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastKreatEIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    const std::filesystem::path ini = "Data/KreatE/UserSettings.ini";
    if (!std::filesystem::exists(ini)) return;
    const int result = SetIniValue(ini, "GUIToggleKeys", VK_F18, true);
    if (result < 0) return;

    g_KreatEIniManaged.store(true);
    if (result == 1) {
        SKSE::log::info("TryManageKreatEHotkey: KreatE GUIToggleKeys -> F18 (was End).{}",
            lateRetry ? " Restart Skyrim once so KreatE reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageKreatEHotkey: KreatE GUIToggleKeys is already F18.");
    }
}

static int SetJsonKeyValue(const std::filesystem::path& path, const std::string& section, const std::string& key, int newValue) {
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
        if (!out.is_open()) return -1;
        out << j.dump(4);
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
        if (!out.is_open()) return -1;
        out << j.dump(4) << '\n';
        return 1;
    } catch (...) {
        return -1;
    }
}

static std::atomic<bool> g_CSIniManaged{ false };
static std::atomic<long long> g_LastCSIniAttemptMs{ 0 };

static void TryManageCSHotkey(bool lateRetry) {
    if (g_CSIniManaged.load() || !IsPluginPresent("CommunityShaders")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastCSIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    bool any = false;
    const std::filesystem::path userJson = "Data/SKSE/Plugins/CommunityShaders/SettingsUser.json";
    const std::filesystem::path defaultJson = "Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json";

    if (std::filesystem::exists(userJson)) {
        int r = SetJsonKeyValue(userJson, "Menu", "ToggleKey", 130); // 130 = VK_F19
        if (r == 1) any = true;
    }
    if (std::filesystem::exists(defaultJson)) {
        int r = SetJsonKeyValue(defaultJson, "Menu", "ToggleKey", 130);
        if (r == 1) any = true;
    }

    if (any) {
        g_CSIniManaged.store(true);
        SKSE::log::info("TryManageCSHotkey: Community Shaders ToggleKey -> VK_F19 (130). Restart required.");
    } else if (std::filesystem::exists(userJson) || std::filesystem::exists(defaultJson)) {
        g_CSIniManaged.store(true);
        SKSE::log::info("TryManageCSHotkey: Community Shaders ToggleKey is already VK_F19.");
    }
}

static void TryManageCatMenuHotkey(bool lateRetry) {
    if (g_CatMenuIniManaged.load() || !IsPluginPresent("catmenu")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastCatMenuIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/catmenu/settings.json";
    const int result = SetJsonRootValue(jsonPath, "toggle_key", 591); // ImGuiKey_F20
    if (result < 0) return;

    g_CatMenuIniManaged.store(true);
    if (result == 1) {
        SKSE::log::info("TryManageCatMenuHotkey: CatMenu toggle_key -> ImGuiKey_F20 (591).{}",
            lateRetry ? " Restart Skyrim once so CatMenu reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageCatMenuHotkey: CatMenu toggle_key is already ImGuiKey_F20 (591).");
    }
}

static void TryManageDragonbornHotkey(bool lateRetry) {
    if (g_DragonbornIniManaged.load() || !IsPluginPresent("SkyrimCheatMenu")) return;

    const long long now = NowMs();
    if (lateRetry) {
        const long long last = g_LastDragonbornIniAttemptMs.exchange(now);
        if (now - last < 2000) return;
    }

    const std::filesystem::path jsonPath = "Data/SKSE/Plugins/SkyrimCheatMenu.json";
    const int result = SetJsonRootValue(jsonPath, "toggleKey", 108); // DIK_F21
    if (result < 0) return;

    g_DragonbornIniManaged.store(true);
    if (result == 1) {
        SKSE::log::info("TryManageDragonbornHotkey: Dragonborn's Toolkit toggleKey -> F21 (scan code 108).{}",
            lateRetry ? " Restart Skyrim once so Dragonborn's Toolkit reads the generated setting." : "");
    } else {
        SKSE::log::info("TryManageDragonbornHotkey: Dragonborn's Toolkit toggleKey is already F21 (scan code 108).");
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
        return -1;
    }
    for (const auto& line : lines) out << line << "\n";
    return 1;
}

static void RestoreLegacyKreatEBlocking() {
    const int original = g_LegacyKreatEOriginalBlocking.exchange(-1);
    if (original < 0) return;

    const std::filesystem::path ini = "Data/KreatE/UserSettings.ini";
    const int result = SetIniTextValue(ini, "Blocking", original != 0 ? "true" : "false");
    if (result >= 0) {
        SKSE::log::info("RestoreLegacyKreatEBlocking: restored KreatE Input.Blocking to {} and removed the legacy keep-open option.", original != 0);
        SaveButtonOrder();
    }
}

static void DisableMFHotkey() {
    const std::filesystem::path ini = "Data/SKSE/Plugins/SKSEMenuFramework.ini";
    const int result = SetIniTextValue(ini, "ToggleMode", "OFF");
    g_MFIniWriteResult.store(result);
    if (result == 1) {
        SKSE::log::info("DisableMFHotkey: set SKSE Menu Framework ToggleMode to OFF before it loaded.");
    } else if (result == 0) {
        SKSE::log::info("DisableMFHotkey: SKSE Menu Framework ToggleMode is already OFF.");
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

// Point the mods that can't be blocked at the input layer (they read the shared engine event
// stream) at "unpressable" hotkeys, freeing their original key for vanilla. The launcher opens
// each by emitting that key through the engine. FLICK is opened via its API, so its key just
// needs to be moved off F7. Idempotent: writes only when a value isn't already set, so it runs
// at most once; takes effect on the next launch (mods read their config before us).
// Move ReShade's overlay key off Home to an unpressable F22 so Home is free for vanilla and
// ReShade opens only from the launcher (it still closes on ESC, which ReShade handles natively).
// ReShade.ini KeyOverlay format is "VK,ctrl,shift,alt". Idempotent; takes effect next launch.
// NOTE: ReShade rewrites ReShade.ini itself, so this can get clobbered — if Home keeps coming
// back, set the Overlay key inside ReShade's own Settings > Input instead (that sticks).
static void ManageReShadeHotkey() {
    if (!g_ReShadeConfig.enabled) return;
    const std::filesystem::path ini = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" });
    if (ini.empty()) { SKSE::log::warn("ManageReShadeHotkey: ReShade.ini not found."); return; }
    const int targetVK = 0x85; // F22

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
    SKSE::log::info("ManageReShadeHotkey: ReShade KeyOverlay -> F22 (was Home) at {}. RESTART once.", ini.string());
}

static void ManageModHotkeys() {
    bool any = false;
    if (g_OARConfig.enabled) {
        int a = SetIniValue("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKey", 0x64);    // F13
        // Keep the Shift requirement (Shift+F13). Dropping it meant a held Shift (from the original
        // Shift+O chord) turned the injected F13 into Shift+F13 and OAR rejected it, forcing us to
        // wait for key release. Keeping Shift lets OAR open on key-down.
        int b = SetIniValue("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyShift", 1);
        if (a == 1 || b == 1) { any = true; SKSE::log::info("ManageModHotkeys: OAR UI hotkey -> Shift+F13 (was Shift+O)."); }
    }
    if (g_DMenuConfig.enabled) {
        if (SetIniValue("Data/SKSE/Plugins/dmenu/dmenu.ini", "key_toggle_dmenu", 0x65) == 1) {        // F14
            any = true; SKSE::log::info("ManageModHotkeys: dMenu hotkey -> F14 (was End/Home).");
        }
    }
    if (g_ImprovedCameraConfig.enabled) {
        if (SetIniValue("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", 0x7E, true) == 1) { // VK_F15
            any = true; SKSE::log::info("ManageModHotkeys: Improved Camera menu key -> F15 (was Home).");
        }
    }
    if (g_IEDConfig.enabled) {
        if (SetIniValue("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys", 0x67, true) == 1) { // unpressable
            any = true; SKSE::log::info("ManageModHotkeys: IED toggle key -> 0x67 (was Backspace).");
        }
    }
    if (g_FLICKConfig.enabled) {
        auto flick = FindModFile({ "Data/FUCKs/FUCK/keybinds.ini", "FUCKs/FUCK/keybinds.ini",
                                   "Data/SKSE/Plugins/FUCKs/FUCK/keybinds.ini" });
        if (flick.empty()) {
            SKSE::log::warn("ManageModHotkeys: FLICK keybinds.ini not found at any known path; F7 NOT disabled.");
        } else if (SetIniValue(flick, "iToggleFUCK_Key", 0x68) == 1) {  // unpressable; opened via API
            any = true; SKSE::log::info("ManageModHotkeys: FLICK hotkey -> 0x68 at {} (was F7); opened via API.", flick.string());
        }
    }
    if (any) SKSE::log::info("ManageModHotkeys: mod hotkeys changed — RESTART Skyrim once for them to take effect.");
}

// Revert every managed mod's hotkey back to its original default — undoes all the edits this
// mod made. Intended as "uninstall prep": run it, restart once, then it's safe to disable the
// mod and every other mod behaves as if Risa's menu had never touched it. Files that don't
// exist are skipped automatically.
static void RestoreAllModDefaults() {
    SetIniValue("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKey", 24);      // O
    SetIniValue("Data/SKSE/Plugins/OpenAnimationReplacer.ini", "uToggleUIKeyShift", 1);  // + Shift
    SetIniValue("Data/SKSE/Plugins/dmenu/dmenu.ini", "key_toggle_dmenu", 199);           // Home
    SetIniValue("Data/SKSE/Plugins/ImprovedCameraSE/ImprovedCameraSE.ini", "MenuKey", 0x24, true); // Home
    SetIniValue("Data/SKSE/Plugins/ImmersiveEquipmentDisplays.ini", "ToggleKeys", 0x0E, true);     // Backspace

    if (auto flick = FindModFile({ "Data/FUCKs/FUCK/keybinds.ini", "FUCKs/FUCK/keybinds.ini",
                                   "Data/SKSE/Plugins/FUCKs/FUCK/keybinds.ini" }); !flick.empty()) {
        SetIniValue(flick, "iToggleFUCK_Key", 65); // F7
    }

    {
        std::filesystem::path dm = "Data/MCM/Settings/DebugMenu.ini";
        if (!std::filesystem::exists(dm)) dm = "Data/MCM/Config/DebugMenu/settings.ini";
        SetIniValue(dm, "uOpenMenuHotkey", 59); // F1
    }

    if (std::filesystem::exists("Data/KreatE/UserSettings.ini")) {
        SetIniValue("Data/KreatE/UserSettings.ini", "GUIToggleKeys", VK_END, true); // End
    }

    if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json")) {
        SetJsonKeyValue("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", "Menu", "ToggleKey", 35); // 35 = VK_END
    }
    if (std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json")) {
        SetJsonKeyValue("Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Menu", "ToggleKey", 35);
    }
    SetJsonRootValue("Data/SKSE/Plugins/catmenu/settings.json", "toggle_key", 577); // ImGuiKey_F6
    SetJsonRootValue("Data/SKSE/Plugins/SkyrimCheatMenu.json", "toggleKey", "F1");

        // ReShade: restore overlay key to Home (36).
    if (const auto rs = FindModFile({ "ReShade.ini", "Data/../ReShade.ini" }); !rs.empty()) {
        std::vector<std::string> lines;
        bool found = false;
        {
            std::ifstream f(rs);
            std::string line;
            while (std::getline(f, line)) {
                const auto eq = line.find('=');
                if (eq != std::string::npos && ToUpper(TrimStr(line.substr(0, eq))) == "KEYOVERLAY") {
                    line = "KeyOverlay=36,0,0,0"; found = true;
                }
                lines.push_back(line);
            }
        }
        if (found) { std::ofstream out(rs, std::ios::trunc); for (const auto& l : lines) out << l << "\n"; }
    }

    SaveButtonOrder();
    SKSE::log::info("RestoreAllModDefaults: reverted all managed mods' hotkeys to their defaults.");
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
        LoadKreatEConfig();
        LoadCSConfig();
        LoadCatMenuConfig();
        LoadDragonbornConfig();
        LoadReShadeConfig();
        TryManageKreatEHotkey();
        TryManageCSHotkey();
        TryManageCatMenuHotkey();
        TryManageDragonbornHotkey();
        ManageReShadeHotkey();         // move ReShade overlay key off Home to F22 (launcher-only)
        ManageModHotkeys();            // move OAR/dMenu/IC to unpressable F13-F15; disable FLICK's key
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
        LogStartupDiagnostics();
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

    // Run before the managed plugins load so they observe collision-safe settings during
    // this launch. The normal full configuration pass still runs at kPostLoad.
    LoadButtonOrder();
    RestoreLegacyKreatEBlocking();
    DisableMFHotkey();
    TryManageKreatEHotkey();
    TryManageCSHotkey();
    TryManageCatMenuHotkey();
    TryManageDragonbornHotkey();
    LoadDebugMenuConfig();
    ManageDebugMenuKey();

    auto* msg = SKSE::GetMessagingInterface();
    if (!msg || !msg->RegisterListener(MessageHandler)) {
        SKSE::log::error("Failed to register messaging listener."); return false;
    }
    return true;
}
