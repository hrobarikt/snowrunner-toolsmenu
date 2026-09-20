// The wire between the injected module and the tray app.
//
// One connection per request: the app connects, writes a Request, reads a
// response and disconnects. There is no session and no state on the wire,
// because the module is the only thing that holds state and the app is only
// ever asking it what it knows or telling it to do one thing.
//
// A fixed header, optionally followed by report_length bytes of diagnostics
// text. Both ends are built from this header in the same repository at the same
// time, so the version exists to fail loudly on a stale exe, not to negotiate.

#pragma once

#include <stdint.h>

#include <string>

namespace srtm {

constexpr uint32_t kProtocolMagic = 0x4D545253;  // 'SRTM'
// 2 adds Opcode::Rescan. Bumped rather than added silently, because a module
// from an older build can still be loaded in a game that has been running
// since before an update, and a stale pair should say so rather than treat an
// unknown opcode as a refusal.
constexpr uint32_t kProtocolVersion = 2;

// The largest diagnostics payload a response may carry. The report is a few
// hundred bytes; this only exists so a reader can refuse a nonsense length
// without trying to allocate it.
constexpr uint32_t kMaxReportBytes = 64 * 1024;

enum class Opcode : uint32_t {
    Status = 0,     // argument ignored
    SetMenu = 1,    // argument: 1 to turn on, 0 to turn off
    SetHotkey = 2,  // argument: a virtual-key code, 0 to disable the hotkey
    Detach = 3,     // argument ignored
    Rescan = 4,     // argument ignored: look for the build again, from the top
};

enum class PipeStatus : uint32_t {
    Ok = 0,
    ProtocolError = 1,      // wrong magic or version
    UnsupportedOpcode = 2,
    CommandFailed = 3,      // the opcode ran; last_command says what happened
    NotReady = 4,           // the build is unsupported, or the scan is not done
};

// What the module is doing. On the wire in ResponseHeader::state, so it lives
// here rather than in module.h: three readers switch on it, and a bare integer
// in two of them was how the meanings drifted apart before.
enum class ModuleState : uint32_t {
    Scanning = 0,      // the worker is still resolving the build
    Unsupported = 1,   // the scan did not produce a usable layout
    Failed = 2,        // the scan was usable but the hook or the menu refused
    Ready = 3,         // hooked, and the toggle works
    Detaching = 4,     // detach is running, or ran and left the module warm
    Detached = 5,      // the bytes are verifiably back
    DetachFailed = 6,  // the bytes are NOT back; the game is still patched
};

// How the last menu command ended. On the wire in ResponseHeader::last_command.
enum class ToolsMenuStatus : uint32_t {
    Ok = 0,
    NotConfigured = 1,     // no usable layout, so there is nothing to call
    SiteChanged = 2,       // a binding no longer holds the bytes the scanner saw
    WorldUnavailable = 3,  // no world loaded, or its pointers are not readable
    MenuPresent = 4,       // a menu is already up, and this module did not make it
    MenuForeign = 5,       // asked to remove a menu this module does not own
    MenuFailed = 6,        // the call returned, but the state it left is wrong
    Faulted = 7,           // the call itself raised
    Busy = 8,              // another command is already in flight
    HookStalled = 9,       // queued, but the game's thread never drained it
};

// The canonical short phrase for each, for the diagnostics report and for
// command-line output. The tray app has its own, longer wording: the same
// facts said to a user rather than to the author.
inline const char* ToolsMenuStatusText(ToolsMenuStatus status) {
    switch (status) {
        case ToolsMenuStatus::Ok: return "ok";
        case ToolsMenuStatus::NotConfigured: return "not configured";
        case ToolsMenuStatus::SiteChanged: return "site changed";
        case ToolsMenuStatus::WorldUnavailable: return "world unavailable";
        case ToolsMenuStatus::MenuPresent: return "menu already present";
        case ToolsMenuStatus::MenuForeign: return "menu is not ours";
        case ToolsMenuStatus::MenuFailed: return "menu call did not take";
        case ToolsMenuStatus::Faulted: return "call faulted";
        case ToolsMenuStatus::Busy: return "busy";
        case ToolsMenuStatus::HookStalled: return "hook not draining";
    }
    return "unknown";
}

inline const char* ModuleStateText(ModuleState state) {
    switch (state) {
        case ModuleState::Scanning: return "scanning";
        case ModuleState::Unsupported: return "unsupported build";
        case ModuleState::Failed: return "failed";
        case ModuleState::Ready: return "ready";
        case ModuleState::Detaching: return "detaching";
        case ModuleState::Detached: return "detached";
        case ModuleState::DetachFailed: return "detach failed";
    }
    return "unknown";
}

// Response flags.
constexpr uint32_t kFlagHookInstalled = 1u << 0;
constexpr uint32_t kFlagMenuOn = 1u << 1;
constexpr uint32_t kFlagLastCommandValid = 1u << 2;
// Detach finished and the module is unloading itself. The app must not send
// anything else; the pipe is already gone.
constexpr uint32_t kFlagUnloading = 1u << 3;
// Detach removed the patch but a thread was still inside the module, so it
// stayed loaded. Sending Detach again finishes the unload.
constexpr uint32_t kFlagStillWarm = 1u << 4;

#pragma pack(push, 4)
struct Request {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    uint32_t opcode = 0;
    uint32_t argument = 0;
};

struct ResponseHeader {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    uint32_t status = 0;        // PipeStatus
    uint32_t state = 0;         // ModuleState
    uint32_t flags = 0;
    uint32_t hotkey_vk = 0;
    uint32_t last_command = 0;  // ToolsMenuStatus
    uint32_t report_length = 0;
    uint64_t frames = 0;
};
#pragma pack(pop)

// One pipe per game process, so two games running at once stay separate.
inline std::wstring PipeName(uint32_t pid) {
    return L"\\\\.\\pipe\\srtm-" + std::to_wstring(pid);
}

}  // namespace srtm
