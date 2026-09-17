// The injected module's brain.
//
// On load it does nothing but start a worker thread. That thread scans the
// loaded game image, and only if every signature resolves and agrees does it
// configure the menu and install the frame hook. An unrecognised build leaves
// the module loaded, inert and able to explain itself: nothing is patched and
// nothing is called. See docs/design.md, "Architecture".
//
// Two threads matter. The game's own thread reaches this module through the
// frame hook's drain point, and that is the only thread a command ever runs
// on. The worker thread scans, and later serves the pipe; it publishes work
// through a single-slot queue and waits.

#pragma once

#include "tools_menu.h"

#include <windows.h>

#include <string>

namespace srtm {

enum class ModuleState {
    Scanning,       // the worker is still resolving the build
    Unsupported,    // the scan did not produce a usable layout
    Failed,         // the scan was usable but the hook or the menu refused
    Ready,          // hooked, and the toggle works
    Detaching,
    Detached,
};

// Everything the tray app will eventually show, and everything the diagnostics
// report needs. Copied out under a lock, so a caller never reads it mid-write.
struct ModuleStatus {
    ModuleState state = ModuleState::Scanning;
    bool hook_installed = false;
    bool menu_on = false;
    uint32_t hotkey_vk = VK_HOME;
    uint64_t frames = 0;
    ToolsMenuStatus last_command = ToolsMenuStatus::NotConfigured;
    bool last_command_valid = false;
    std::string scan_report;  // the full text report, for Copy and Save
};

// Called from DllMain on load. Starts the worker and returns immediately;
// false only if the module could not get far enough to have a worker at all.
bool StartModule(HMODULE self);

ModuleStatus GetModuleStatus();

// Queue a toggle for the game's thread and wait for it. Safe to call from any
// thread except the game's own; the hotkey path does not go through this,
// because it is already on the right thread.
ToolsMenuStatus RequestSetMenu(bool enable);

// Which key toggles the menu. Takes effect on the next frame.
void SetHotkey(uint32_t virtual_key);

// Turns off a menu of ours through the game's own destroy path while the hook
// is still in, then removes the hook and waits for the module to go cold. Runs
// on the calling thread, which must not be the game's own.
//
// True means the module is cold and the caller -- which is a thread this module
// owns, and therefore the only one safe to unload from -- should finish with
// FreeLibraryAndExitThread. False means the patch is already gone but a thread
// was still inside; the module stays loaded and a second detach completes it.
bool DetachModule();

HMODULE ModuleHandle();

}  // namespace srtm
