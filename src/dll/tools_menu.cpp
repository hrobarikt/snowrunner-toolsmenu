// Tools menu implementation. See tools_menu.h.
//
// Ported from snowrunner-mod (src/native/snowrunner-host/src/tools_menu.cpp),
// where the on and off paths were validated live. Two things are deliberately
// gone: the campaign-flag/session-mode check, which was built on hardcoded
// struct offsets and pinned the predecessor to one Steam build, and the host
// session token, which had no counterpart once the DLL became the whole brain.
// See docs/design.md, "Guard model", for why, and what it costs.
#include "tools_menu.h"

#include <windows.h>

#include <cstring>

namespace srtm {
namespace {

// The bindings take the system object and return nothing.
using ToolsMenuBinding = void (*)(void* system_object);

const uint8_t* g_module_base = nullptr;
uint32_t g_module_size = 0;

ToolsMenuLayout g_layout;
bool g_configured = false;

// What this module turned on, so that the off path and detach only ever undo
// this module's own change. A menu somebody else created is left alone.
uint64_t g_created_menu = 0;
bool g_gate_set = false;

const uint8_t* ModuleAddress(uint32_t rva, size_t count) {
    if (g_module_base == nullptr || rva == 0 || rva >= g_module_size ||
        count > g_module_size - rva) {
        return nullptr;
    }
    return g_module_base + rva;
}

uint64_t ReadSlot(uint32_t rva) {
    const uint8_t* slot = ModuleAddress(rva, sizeof(uint64_t));
    if (slot == nullptr) {
        return 0;
    }
    return *reinterpret_cast<const uint64_t*>(slot);
}

bool IsReadableRange(const void* address, size_t count) {
    const uint8_t* cursor = static_cast<const uint8_t*>(address);
    const uint8_t* end = cursor + count;
    if (cursor == nullptr || end < cursor) {
        return false;
    }
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(cursor, &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const uint8_t* region_end =
            static_cast<const uint8_t*>(memory.BaseAddress) + memory.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        cursor = region_end;
    }
    return true;
}

// The game's own call, wrapped so that a fault inside it is reported rather
// than left to take the process down. It is deliberately the only thing in
// this function: nothing here needs unwinding.
bool CallBinding(const uint8_t* binding, void* system_object) {
    __try {
        reinterpret_cast<ToolsMenuBinding>(const_cast<uint8_t*>(binding))(system_object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Both bindings must still be the bytes the scanner validated. The off path is
// checked before the on path runs too, because a menu that cannot be taken away
// again must not be created in the first place.
bool BindingBytesUnchanged() {
    const uint32_t count = g_layout.binding_sample_length;
    const uint8_t* enable = ModuleAddress(g_layout.enable_rva, count);
    const uint8_t* disable = ModuleAddress(g_layout.disable_rva, count);
    if (enable == nullptr || disable == nullptr) {
        return false;
    }
    return memcmp(enable, g_layout.enable_sample, count) == 0 &&
           memcmp(disable, g_layout.disable_sample, count) == 0;
}

void Observe(uint64_t system_object, ToolsMenuObservation* observation) {
    observation->menu_slot = ReadSlot(g_layout.polygon_menu_slot_rva);
    observation->gate =
        *reinterpret_cast<const uint8_t*>(system_object + g_layout.gate_offset);
    observation->owned = ToolsMenuOwned();
}

}  // namespace

bool ConfigureToolsMenu(const ToolsMenuLayout& layout,
                        const uint8_t* module_base,
                        uint32_t module_size) {
    g_configured = false;
    g_module_base = module_base;
    g_module_size = module_size;
    g_layout = layout;

    const uint32_t count = layout.binding_sample_length;
    if (count == 0 || count > kMaxSampleBytes ||
        layout.gate_offset == 0 || layout.gate_offset > 0x1000 ||
        layout.terrain_offset == 0 || layout.terrain_offset > 0x1000 ||
        ModuleAddress(layout.enable_rva, count) == nullptr ||
        ModuleAddress(layout.disable_rva, count) == nullptr ||
        ModuleAddress(layout.polygon_menu_slot_rva, sizeof(uint64_t)) == nullptr ||
        ModuleAddress(layout.game_logic_slot_rva, sizeof(uint64_t)) == nullptr ||
        ModuleAddress(layout.system_slot_rva, sizeof(uint64_t)) == nullptr ||
        layout.polygon_menu_slot_rva == layout.game_logic_slot_rva ||
        layout.enable_rva == layout.disable_rva) {
        return false;
    }

    g_configured = BindingBytesUnchanged();
    return g_configured;
}

bool ToolsMenuConfigured() { return g_configured; }

bool ToolsMenuOwned() { return g_created_menu != 0 || g_gate_set; }

ToolsMenuStatus SetToolsMenu(bool enable, ToolsMenuObservation* observation) {
    observation->menu_slot = 0;
    observation->gate = 0;
    observation->owned = ToolsMenuOwned();

    if (!g_configured) {
        return ToolsMenuStatus::NotConfigured;
    }
    if (!BindingBytesUnchanged()) {
        return ToolsMenuStatus::SiteChanged;
    }

    const uint64_t game_logic = ReadSlot(g_layout.game_logic_slot_rva);
    const uint64_t system_object = ReadSlot(g_layout.system_slot_rva);
    if (game_logic == 0 || system_object == 0 ||
        !IsReadableRange(
            reinterpret_cast<const void*>(game_logic + g_layout.terrain_offset),
            sizeof(uint64_t)) ||
        !IsReadableRange(
            reinterpret_cast<const void*>(system_object + g_layout.gate_offset), 1)) {
        return ToolsMenuStatus::WorldUnavailable;
    }

    // The game removes the menu and clears the gate on a map reload, through
    // its own teardown. Ownership follows what is actually there, so that the
    // menu can simply be turned on again in the world the reload produced.
    Observe(system_object, observation);
    if (g_created_menu != 0 && observation->menu_slot == 0) {
        g_created_menu = 0;
    }
    if (g_gate_set && observation->gate == 0) {
        g_gate_set = false;
    }
    observation->owned = ToolsMenuOwned();

    void* system = reinterpret_cast<void*>(system_object);

    if (enable) {
        // A world with no terrain is a menu or a loading screen. The binding
        // allocates against it, so it has to be there.
        if (*reinterpret_cast<const uint64_t*>(game_logic + g_layout.terrain_offset) == 0) {
            return ToolsMenuStatus::WorldUnavailable;
        }
        if (observation->menu_slot != 0) {
            return ToolsMenuStatus::MenuPresent;
        }
        // A gate somebody else set is left alone. Our own, still set after the
        // game's teardown took the menu away, is not in the way: turning the
        // menu on again writes the same 1 the binding always writes.
        if (observation->gate != 0 && !g_gate_set) {
            return ToolsMenuStatus::MenuPresent;
        }
        if (!CallBinding(ModuleAddress(g_layout.enable_rva, g_layout.binding_sample_length),
                         system)) {
            return ToolsMenuStatus::Faulted;
        }
        // The gate is written before the menu is allocated, so from here on
        // there is something to undo whatever the rest of the call did.
        g_gate_set = true;
        Observe(system_object, observation);
        if (observation->menu_slot == 0 || observation->gate != 1) {
            return ToolsMenuStatus::MenuFailed;
        }
        g_created_menu = observation->menu_slot;
        observation->owned = true;
        return ToolsMenuStatus::Ok;
    }

    if (!ToolsMenuOwned()) {
        if (observation->menu_slot != 0) {
            // Somebody else's menu. It is reported rather than removed, so that
            // a caller is never told a menu was taken away while it is still
            // on screen.
            return ToolsMenuStatus::MenuForeign;
        }
        return ToolsMenuStatus::Ok;
    }
    if (observation->menu_slot != 0 && observation->menu_slot != g_created_menu) {
        return ToolsMenuStatus::MenuForeign;
    }
    if (!CallBinding(ModuleAddress(g_layout.disable_rva, g_layout.binding_sample_length),
                     system)) {
        return ToolsMenuStatus::Faulted;
    }
    Observe(system_object, observation);
    if (observation->menu_slot != 0 || observation->gate != 0) {
        return ToolsMenuStatus::MenuFailed;
    }
    g_created_menu = 0;
    g_gate_set = false;
    observation->owned = false;
    return ToolsMenuStatus::Ok;
}

}  // namespace srtm
