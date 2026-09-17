// The app's end of the wire, and the only thing in the app that knows the game
// exists.
//
// Every call into the game can block: injection waits for a remote thread, and
// a toggle waits up to two seconds for the game's own thread to drain it. So
// none of it happens on the UI thread. One worker owns all of it, polls the
// module a few times a second, and publishes a snapshot the window copies.

#pragma once

#include <stdint.h>

#include <string>

namespace srtm {

// What the window draws. A copy, taken under a lock, so it never changes
// halfway through a frame.
struct LinkView {
    bool game_running = false;
    bool module_present = false;   // the pipe answered
    bool busy = false;             // a command is in flight
    uint32_t pid = 0;

    uint32_t state = 0;            // ModuleState
    bool hook_installed = false;
    bool menu_on = false;
    uint32_t hotkey_vk = 0;
    uint64_t frames = 0;
    uint32_t last_command = 0;     // ToolsMenuStatus
    bool last_command_valid = false;

    // The last thing that went wrong, in a sentence fit to show a user. Empty
    // when nothing has.
    std::wstring trouble;

    // The module's diagnostics report, for the panel and for Copy and Save.
    std::string report;
};

// Starts the worker. The DLL is written out of this exe's resources to
// `dll_path` when an attach is asked for; Start only remembers where.
bool StartLink(const std::wstring& dll_path);
void StopLink();

LinkView GetLinkView();

// All of these return immediately. The worker picks them up, and the next
// snapshot says what happened.
void RequestAttach();
void RequestMenu(bool on);
void RequestHotkey(uint32_t virtual_key);
void RequestDetachModule();

// Asks the module to leave and waits for it to go, up to `timeout_ms`. Exiting
// the app restores the game, so this is what Exit does before shutting down.
// A kill cannot honour it, which is why a forced end leaves the module loaded
// until the game closes.
void DetachAndWait(unsigned timeout_ms);

// True while the app should keep the module out of the game: set by a detach,
// cleared by an attach. Without it the worker would put the module straight
// back after the user asked it to leave.
bool DetachRequested();

}  // namespace srtm
