// Finding the game and getting a DLL into it.
//
// Attach-only, by design: the user launches the game normally through Steam,
// Epic or Xbox, and this waits for it. Shared by the tray app and the
// development tool so there is one copy of the only code here that touches
// another process.

#pragma once

#include <windows.h>

#include <string>

namespace srtm {

enum class InjectResult {
    Ok,
    GameNotRunning,
    AlreadyLoaded,
    AccessDenied,   // almost always: the game is elevated and this is not
    Failed,
};

// Zero when the game is not running. Only the first match is returned; two
// copies of SnowRunner at once is not a case worth handling.
DWORD FindGameProcess();

bool IsModuleLoaded(DWORD pid, const std::wstring& dll_path);

// True once `pid` owns a visible top-level window. A process appears in the
// process list before Windows has finished setting it up, and injecting into
// one that has not reached its message loop is the fragile moment; a window --
// even a splash -- means that moment has passed. Deliberately not matched by
// title: a localised or renamed build is still the game.
bool GameHasWindow(DWORD pid);

// Loads `dll_path` into `pid` and waits for its entry point to return.
// `detail` gets a sentence fit to show a user; it is set on failure only.
InjectResult InjectLibrary(DWORD pid, const std::wstring& dll_path, std::wstring* detail);

}  // namespace srtm
