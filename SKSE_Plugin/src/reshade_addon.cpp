// ReShade add-on integration, isolated in its own translation unit.
//
// Your ReShade build (6.7.3) ships the full add-on API, so instead of relocating the overlay key
// and simulating a keypress, we register as a ReShade add-on and drive ReShade's OWN
// effect_runtime::open_overlay(). That frees the overlay key entirely (no F-key, Home stays free)
// and removes the SendInput + ESC-close hacks. If the running ReShade has no add-on API, every
// function here is a safe no-op and main.cpp falls back to the key-relocation path.

#include "reshade_bridge.h"

#include <atomic>

// The ReShade SDK pulls in <Windows.h> and (optionally) ImGui glue — confined to this file.
#include "reshade/reshade.hpp"

namespace {
    std::atomic<reshade::api::effect_runtime*> g_runtime{ nullptr };
    std::atomic<bool>                          g_registered{ false };
    std::atomic<void (*)(bool)>                g_stateCb{ nullptr };

    HMODULE SelfModule() {
        HMODULE h = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SelfModule), &h);
        return h;
    }

    void OnInitRuntime(reshade::api::effect_runtime* runtime) {
        g_runtime.store(runtime);
    }
    void OnDestroyRuntime(reshade::api::effect_runtime* runtime) {
        auto* expected = runtime;
        g_runtime.compare_exchange_strong(expected, nullptr);
    }
    // Observe ReShade's overlay open/close so the launcher's active-menu state stays correct even
    // if the overlay is closed by something other than us. Never veto (return false).
    bool OnOpenOverlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source) {
        if (auto cb = g_stateCb.load()) cb(open);
        return false;
    }
}

namespace RisaReShade {
    bool RegisterAddon() {
        if (g_registered.load()) return true;
        if (!reshade::register_addon(SelfModule())) return false;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OnOpenOverlay);
        g_registered.store(true);
        return true;
    }

    void UnregisterAddon() {
        if (!g_registered.load()) return;
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(&OnOpenOverlay);
        reshade::unregister_addon(SelfModule());
        g_registered.store(false);
        g_runtime.store(nullptr);
    }

    bool RuntimeReady() { return g_runtime.load() != nullptr; }

    bool SetOverlayOpen(bool open) {
        auto* rt = g_runtime.load();
        if (!rt) return false;
        return rt->open_overlay(open, reshade::api::input_source::keyboard);
    }

    void SetOverlayStateCallback(void (*cb)(bool)) { g_stateCb.store(cb); }
}
