// The module's end of the wire. See src/protocol/protocol.h.
//
// One thread, one connection at a time. The app is a single tray process
// asking short questions, so a serial server is the whole requirement, and it
// keeps the failure modes to the two that matter: a request that never arrives,
// and a command the game's thread never drains.
//
// Ported in shape from snowrunner-mod's channel thread, including the pipe's
// security descriptor and the rule that the module only ever unloads itself
// from its own thread, after a response has been fully delivered.
#include "pipe.h"

#include "frame_hook.h"
#include "module.h"
#include "pipe_io.h"
#include "protocol.h"

#include <sddl.h>

#include <string>
#include <vector>

namespace srtm {
namespace {

HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;

// Everything the module knows, in the shape the wire wants it.
ResponseHeader StatusHeader(std::string* report) {
    const ModuleStatus status = GetModuleStatus();
    ResponseHeader header;
    header.status = static_cast<uint32_t>(PipeStatus::Ok);
    header.state = static_cast<uint32_t>(status.state);
    header.hotkey_vk = status.hotkey_vk;
    header.last_command = static_cast<uint32_t>(status.last_command);
    header.frames = status.frames;
    if (status.hook_installed) {
        header.flags |= kFlagHookInstalled;
    }
    if (status.menu_on) {
        header.flags |= kFlagMenuOn;
    }
    if (status.last_command_valid) {
        header.flags |= kFlagLastCommandValid;
    }
    *report = status.scan_report;
    if (report->size() > kMaxReportBytes) {
        report->resize(kMaxReportBytes);
    }
    header.report_length = static_cast<uint32_t>(report->size());
    return header;
}

// Fills in the answer. `unload` says the caller must deliver this response and
// then take the module out of the process; it is the one request that ends with
// the code that is running being gone.
ResponseHeader Handle(const Request& request, std::string* report, bool* unload) {
    *unload = false;

    if (request.magic != kProtocolMagic || request.version != kProtocolVersion) {
        ResponseHeader header;
        header.status = static_cast<uint32_t>(PipeStatus::ProtocolError);
        return header;
    }

    switch (static_cast<Opcode>(request.opcode)) {
        case Opcode::Status:
            return StatusHeader(report);

        case Opcode::SetMenu: {
            const ToolsMenuStatus result = RequestSetMenu(request.argument != 0);
            ResponseHeader header = StatusHeader(report);
            header.last_command = static_cast<uint32_t>(result);
            header.flags |= kFlagLastCommandValid;
            if (result != ToolsMenuStatus::Ok) {
                header.status = static_cast<uint32_t>(IsFrameHookInstalled()
                                                          ? PipeStatus::CommandFailed
                                                          : PipeStatus::NotReady);
            }
            return header;
        }

        case Opcode::SetHotkey:
            SetHotkey(request.argument);
            return StatusHeader(report);

        case Opcode::Detach: {
            // Detach runs here, before the reply, so the reply can say what it
            // actually achieved rather than what it was about to attempt. Only
            // the unload itself outlives the response, because that is the one
            // step after which this code is gone.
            const DetachOutcome outcome = DetachModule();
            ResponseHeader header = StatusHeader(report);
            if (!outcome.bytes_verified) {
                // The patch is not provably out. Unloading now would leave the
                // game jumping into freed memory, so the module stays put and
                // says so.
                header.status = static_cast<uint32_t>(PipeStatus::CommandFailed);
            } else if (outcome.cold) {
                *unload = true;
                header.flags |= kFlagUnloading;
            } else {
                // The bytes are back and the game is whole, but a thread is
                // still inside this module. A second detach finishes it.
                header.flags |= kFlagStillWarm;
            }
            return header;
        }

        default: {
            ResponseHeader header = StatusHeader(report);
            header.status = static_cast<uint32_t>(PipeStatus::UnsupportedOpcode);
            return header;
        }
    }
}

DWORD WINAPI PipeThread(LPVOID) {
    const std::wstring name = PipeName(GetCurrentProcessId());

    // Local pipe, this user and SYSTEM only, remote clients rejected. Anyone
    // who could talk to it could toggle the menu; anyone who could already do
    // that could inject their own module anyway, but there is no reason to
    // widen it beyond the user who started the game.
    SECURITY_ATTRIBUTES attributes = {};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return 1;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;

    int failures = 0;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        const HANDLE pipe = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(ResponseHeader) + kMaxReportBytes, sizeof(Request), 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            // Transient at first: an old instance's pipe may still be closing.
            // Twenty tries at 50ms is a second, after which it is not coming.
            if (++failures >= 20) {
                break;
            }
            Sleep(50);
            continue;
        }
        failures = 0;

        bool unload = false;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            Request request = {};
            if (ReadAll(pipe, &request, sizeof(request))) {
                std::string report;
                ResponseHeader header = Handle(request, &report, &unload);
                if (WriteAll(pipe, &header, sizeof(header)) &&
                    (report.empty() || WriteAll(pipe, report.data(),
                                                static_cast<DWORD>(report.size())))) {
                    FlushFileBuffers(pipe);
                }
                // A reply that did not arrive does not change what happened: by
                // here a detach has already run, and the module is in whatever
                // state it reached. Unloading still follows, because staying
                // loaded with the hook out helps nobody.
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);

        if (unload) {
            LocalFree(descriptor);
            const HANDLE self_thread = g_thread;
            g_thread = nullptr;
            if (self_thread != nullptr) {
                CloseHandle(self_thread);
            }
            // From a module-owned thread, with the response already delivered,
            // so nothing outside can race a FreeLibrary against a thread still
            // executing in here.
            FreeLibraryAndExitThread(ModuleHandle(), 0);
        }
    }

    LocalFree(descriptor);
    return 0;
}

}  // namespace

bool StartPipeServer() {
    g_thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void StopPipeServer() { InterlockedExchange(&g_stop, 1); }

}  // namespace srtm
