#pragma once
// Thin bridge to the ReShade add-on API. The implementation (reshade_addon.cpp) is a separate
// translation unit so the ReShade SDK headers — which carry optional Dear ImGui glue that would
// collide with the plugin's own ImGui usage in main.cpp — never enter main.cpp. Every function is
// a safe no-op when ReShade (or its add-on API) is not present.
namespace RisaReShade {
    // Register this module as a ReShade add-on and subscribe to its runtime/overlay events.
    // Returns true only when a compatible ReShade add-on API is present. Call once, early
    // (before ReShade creates its effect runtime) so the init event is captured.
    bool RegisterAddon();
    void UnregisterAddon();

    // True once ReShade has handed us an effect runtime — i.e. the overlay can be driven.
    bool RuntimeReady();

    // Open or close ReShade's overlay through its own API. Returns false if no runtime yet.
    bool SetOverlayOpen(bool open);

    // Register a callback invoked whenever ReShade reports its overlay opened/closed (any source),
    // so the launcher can keep its active-menu state in sync even on an external close.
    void SetOverlayStateCallback(void (*cb)(bool open));
}
