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
constexpr uint32_t kProtocolVersion = 1;

// The largest diagnostics payload a response may carry. The report is a few
// hundred bytes; this only exists so a reader can refuse a nonsense length
// without trying to allocate it.
constexpr uint32_t kMaxReportBytes = 64 * 1024;

enum class Opcode : uint32_t {
    Status = 0,     // argument ignored
    SetMenu = 1,    // argument: 1 to turn on, 0 to turn off
    SetHotkey = 2,  // argument: a virtual-key code, 0 to disable the hotkey
    Detach = 3,     // argument ignored
};

enum class PipeStatus : uint32_t {
    Ok = 0,
    ProtocolError = 1,      // wrong magic or version
    UnsupportedOpcode = 2,
    CommandFailed = 3,      // the opcode ran; last_command says what happened
    NotReady = 4,           // the build is unsupported, or the scan is not done
};

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
