# ReShade F1-close fix — SOLVED (2026-06-29)

>>> ACTUAL FIX (not Option A/B below): hook GetRawInputData.
>>> ReShade reads the keyboard via Raw Input (WM_INPUT/GetRawInputData), so a
>>> MinHook on GetRawInputData sees F1 on the SAME channel ReShade reads — visible
>>> even with InputProcessing=2 (no input leak to the game). HookedGetRawInputData
>>> parses RAWINPUT; on launcher-key keydown while g_ActiveMenu==ReShade (700ms
>>> guard) it spawns a detached thread -> CloseActiveModMenu(ReShade) -> simulated
>>> ESC. Hooked in InstallHooks. KeyOverlay still relocated Home->F22 to free Home.
>>> Options A/B below (InputProcessing=0) were the abandoned plan — kept for record.

---

# ReShade F1-close fix — implementation plan (ABANDONED — see SOLVED above)

**Goal:** Make ReShade's overlay closable with F1. Root cause (proven by [RSDIAG] logs):
the keyboard hook IS alive while ReShade's overlay is open, but ReShade's
`InputProcessing=2` (aggressive block) swallows F1 below every layer we can read
(engine DI poll AND OS-level GetAsyncKeyState). Fix = set `InputProcessing=0` so
input reaches the game while ReShade's overlay is open, letting our existing F1
detection in HookedKbProcess fire -> CloseActiveModMenu(ReShade) -> simulated ESC.

Takes effect after ONE restart (ReShade reads its ini before our plugin loads).
Tradeoff of InputProcessing=0: while RESHADE'S OWN overlay is open, input also
reaches the game. Does NOT affect the launcher menu or gameplay.

---

## Option A — No-code shortcut (fastest)

Edit: F:\Games\Steam\steamapps\common\Mod Organizer\mods\RESHADE\Root\ReShade.ini
Under [INPUT], change:
    InputProcessing=2
to:
    InputProcessing=0
(KeyOverlay=133,0,0,0 is already set for F22 — leave it.)
Save, restart Skyrim once.

Caveat: ReShade may rewrite the ini on exit. If InputProcessing reverts, set
"Input processing" = "Pass on all input" inside ReShade's own Settings > General.

---

## Option B — Code change (plugin does it automatically)

File: F:\Games\Steam\steamapps\common\Mod Organizer\mods\Risa's All In One Menu\SKSE_Plugin\src\main.cpp

1) ManageReShadeHotkey(): make the line loop handle InputProcessing too.
   Replace the single KEYOVERLAY check with:

    if (eq != std::string::npos) {
        const std::string key = ToUpper(TrimStr(line.substr(0, eq)));
        if (key == "KEYOVERLAY") {
            foundKey = true;
            int curVK = 0;
            try { curVK = std::stoi(TrimStr(line.substr(eq + 1))); } catch (...) {}
            if (curVK != targetVK) { line = std::format("KeyOverlay={},0,0,0", targetVK); changed = true; }
        } else if (key == "INPUTPROCESSING") {
            int cur = -1;
            try { cur = std::stoi(TrimStr(line.substr(eq + 1))); } catch (...) {}
            if (cur != 0) { line = "InputProcessing=0"; changed = true; }
        }
    }

   (rename existing `found` bool -> `foundKey`; update the success log line text.)

2) RestoreAllModDefaults(): in the ReShade block (currently rewrites
   KeyOverlay=36,0,0,0), add a branch that resets any InputProcessing line to
   "InputProcessing=2".

3) Remove the two temporary [RSDIAG] diagnostic blocks (top of HookedKbProcess
   and top of RenderLauncher, both guarded by
   `if (g_ActiveMenu.load() == ActiveMenu::ReShade)`).

4) Build:
   cmake --build "F:/Games/Steam/steamapps/common/Mod Organizer/mods/Risa's All In One Menu/SKSE_Plugin/build" --config Release
   (outputs SKSE\Plugins\1RisaAllInOneMenu.dll)

5) Restart Skyrim once, then test: open ReShade from launcher -> press F1 -> closes.

---

## What was already tried for the ReShade CLOSE issue (chronological)

1. Normal launcher F1 path (HookedKbProcess reading curState[F1]).
   RESULT: dead under ReShade. ReShade wraps the DirectInput keyboard the engine
   polls, so HookedGetDeviceState (DI slot 9) never fired. That hook was the ONLY
   caller of TryHookKeyboardProcess()/TryHookModSinks(), so under ReShade those
   hooks never installed -> all engine-event opens, key suppression, and F1
   detection were silently dead.

2. FIX for #1: bootstrap TryHookKeyboardProcess()+TryHookModSinks() from
   HandleLauncherHotkeys (throttled 1s). That input path DOES fire under ReShade.
   RESULT: most managed mods now open/close under ReShade. ReShade itself still
   wouldn't close with F1.

3. KreatE-blocking regression under ReShade (couldn't F1-close KreatE): switched
   HookedKbProcess F1 detection from curState to GetAsyncKeyState (OS-level,
   immune to KreatE's input blocking AND ReShade).
   RESULT: fixed KreatE close. ReShade still wouldn't close.

4. Render-path F1 poll: added a GetAsyncKeyState(F1) check inside RenderLauncher
   to catch F1 while ReShade's overlay was up.
   RESULT: did not work. REVERTED. (ReShade disrupts rendering, so the render/MF
   callback is unreliable under ReShade.)

5. Switched CloseActiveModMenu(ReShade) to simulate ESC instead of the relocated
   key, since ReShade closes its overlay on ESC natively. ESC works when pressed
   MANUALLY, but we still couldn't trigger it from F1 because F1 was never
   detected while the overlay was open.

6. Relocated ReShade KeyOverlay off Home -> F22 (133,0,0,0) via ManageReShadeHotkey
   so Home is free for vanilla and ReShade opens launcher-only. RESULT: open works
   on F22, Home freed. Close still unsolved.

7. Added temporary [RSDIAG] logging to HookedKbProcess and RenderLauncher to
   MEASURE (instead of assume) what runs while ReShade's overlay is open.
   RESULT (the key finding): "KbProcess alive while ReShade open" prints every
   frame -> the keyboard hook is NOT frozen (earlier assumption was wrong). BUT
   "F1 pressed=false, curState[F1]=false" the whole time -> ReShade's
   InputProcessing=2 swallows F1 below BOTH the engine DI poll AND OS-level
   GetAsyncKeyState. The hook runs; it just never sees the key.

CONCLUSION: the only lever left is ReShade's own InputProcessing setting. Set it
to 0 so input reaches the game while the overlay is open; then the existing F1
detection in HookedKbProcess fires and closes ReShade via simulated ESC.
That is the fix in Option A / Option B above.

Still-true fallback if InputProcessing=0 is unwanted: ReShade closes on ESC
natively at any InputProcessing level, and our launcher state self-heals
afterward (RenderLauncher resets g_ActiveMenu when the cursor hides). So
"open via launcher, close with ESC" works today with no further change.
