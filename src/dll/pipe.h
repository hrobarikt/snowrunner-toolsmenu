// The module's named-pipe server, which is how the tray app reads the status,
// toggles the menu and asks the module to leave. See src/protocol/protocol.h
// for the wire, and pipe.cpp for why it is one connection at a time.

#pragma once

#include <windows.h>

namespace srtm {

// Starts the server thread. The module works without it -- the hotkey does not
// go through the pipe -- so a failure here is not fatal to the injection.
bool StartPipeServer();

// Asks the server to stop accepting connections. It does not wait: the thread
// is blocked in ConnectNamedPipe until something connects, and the only caller
// is a process that is going away anyway.
void StopPipeServer();

}  // namespace srtm
