// Menu state, owned in-process.
//
// The menu is turned on by calling the game's own script binding -- the same
// call the Proving Grounds level script makes -- which writes the mode gate
// byte the per-frame free camera block reads and then allocates the menu. Off
// is its counterpart. Nothing here is reimplemented and nothing here is
// resolved: every address arrives in a ToolsMenuLayout the scanner produced and
// cross-checked. See docs/design.md, "Guard model".
//
// Every function here must run on the game's own thread, from the frame hook's
// drain point. Configure is the exception: it only reads the module image.

#pragma once

#include "tools_menu_scan.h"

#include <stdint.h>

namespace srtm {

enum class ToolsMenuStatus {
    Ok,
    NotConfigured,      // no usable layout, so there is nothing to call
    SiteChanged,        // a binding no longer holds the bytes the scanner saw
    WorldUnavailable,   // no world loaded, or its pointers are not readable
    MenuPresent,        // a menu is already up, and this module did not make it
    MenuForeign,        // asked to remove a menu this module does not own
    MenuFailed,         // the call returned, but the state it left is wrong
    Faulted,            // the call itself raised
};

// One reading of everything a caller is told about.
struct ToolsMenuObservation {
    uint64_t menu_slot = 0;
    uint8_t gate = 0;
    bool owned = false;
};

// Validates the layout against the loaded module and captures it. False means
// the module refuses to call anything, and the toggle stays disabled.
bool ConfigureToolsMenu(const ToolsMenuLayout& layout,
                        const uint8_t* module_base,
                        uint32_t module_size);

bool ToolsMenuConfigured();

// True while this module has a menu or a gate of its own to undo. Detach uses
// this to decide whether it owes the game a teardown.
bool ToolsMenuOwned();

// Turns the menu on or off. Fills `observation` in every case, including the
// failures, so a status line can always say something true.
ToolsMenuStatus SetToolsMenu(bool enable, ToolsMenuObservation* observation);

// A short phrase for the status line and the diagnostics report.
const char* ToolsMenuStatusText(ToolsMenuStatus status);

}  // namespace srtm
