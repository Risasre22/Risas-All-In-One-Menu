#pragma once

// ============================================================================
//  Dragonborn's Toolkit - integration API
//
//  Public header for other SKSE plugins that want to open / close / query the
//  Dragonborn's Toolkit overlay menu WITHOUT simulating its toggle hotkey.
//  Drop this header into your project; no linking required.
//
//  The toolkit's internal SKSE plugin / DLL name is "SkyrimCheatMenu".
//
//  Recommended usage (after our DLL is loaded, e.g. on SKSE kPostLoad):
//
//      #include "DragonbornsToolkitAPI.h"
//      if (auto* dbt = DBTK_API::GetAPI()) {
//          dbt->Open();
//          bool open = dbt->IsOpen();
//      }
//
//  Or the simplest possible path via plain-C exports:
//      bool DragonbornsToolkit_IsMenuOpen();
//      void DragonbornsToolkit_SetMenuOpen(bool open);
//      void DragonbornsToolkit_ToggleMenu();
// ============================================================================

#include <Windows.h>
#include <cstdint>

namespace DBTK_API
{
    inline constexpr const char* PluginName    = "SkyrimCheatMenu";
    inline constexpr const char* PluginDLLName = "SkyrimCheatMenu.dll";

    enum class InterfaceVersion : std::uint32_t
    {
        kV1 = 1,
    };

    // Versioned interface. Methods are NEVER reordered or removed across versions;
    // new capabilities go into a new interface (IVDBTK2, ...) with a bumped version.
    class IVDBTK1
    {
    public:
        // The interface version implemented by this object (1 for IVDBTK1).
        [[nodiscard]] virtual std::uint32_t GetVersion() = 0;

        // Open / close / toggle the toolkit overlay menu. Call from the main thread.
        virtual void Open()   = 0;
        virtual void Close()  = 0;
        virtual void Toggle() = 0;

        // True while the toolkit menu is currently open.
        [[nodiscard]] virtual bool IsOpen() = 0;
    };

    using RequestPluginAPI_t = void* (*)(InterfaceVersion a_version);

    // Fetches the toolkit API from the already-loaded DLL. Returns nullptr if the
    // toolkit isn't installed or the requested interface version is unsupported.
    [[nodiscard]] inline IVDBTK1* GetAPI(InterfaceVersion a_version = InterfaceVersion::kV1)
    {
        if (const HMODULE handle = GetModuleHandleA(PluginDLLName)) {
            if (const auto request =
                    reinterpret_cast<RequestPluginAPI_t>(GetProcAddress(handle, "RequestPluginAPI"))) {
                return static_cast<IVDBTK1*>(request(a_version));
            }
        }
        return nullptr;
    }
}
