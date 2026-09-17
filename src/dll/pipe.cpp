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
#include "protocol.h"

#include <sddl.h>

#include <string>
#include <vector>

namespace srtm {
namespace {

HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;

bool WriteAll(HANDLE pipe, const void* data, DWORD count) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (count > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, count, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        count -= written;
    }
    return true;
}

bool ReadAll(HANDLE pipe, void* data, DWORD count) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    while (count > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, count, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        count -= read;
    }
    return true;
}

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

// Fills in the answer. `detach` says the caller must deliver this response and
// then take the module down; it is the one request whose work outlives the
// reply.
ResponseHeader Handle(const Request& request, std::string* report, bool* detach) {
    *detach = false;

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
            // The work happens after the reply is on the wire, because the
            // module may not exist afterwards. What the app is told is what
            // detach is about to attempt, plus whether it can expect the module
            // to disappear.
            *detach = true;
            ResponseHeader header = StatusHeader(report);
            header.state = static_cast<uint32_t>(ModuleState::Detaching);
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

        bool detach = false;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            Request request = {};
            if (ReadAll(pipe, &request, sizeof(request))) {
                std::string report;
                ResponseHeader header = Handle(request, &report, &detach);
                if (WriteAll(pipe, &header, sizeof(header)) &&
                    (report.empty() || WriteAll(pipe, report.data(),
                                                static_cast<DWORD>(report.size())))) {
                    FlushFileBuffers(pipe);
                } else {
                    // The app went away mid-reply. Detaching now would tear the
                    // hook out for a request nobody is waiting on.
                    detach = false;
                }
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);

        if (detach) {
            const bool cold = DetachModule();
            if (!cold) {
                // A thread is still inside the module. The patch is out and the
                // game is whole, so the server keeps running and a second
                // detach finishes the unload once that thread leaves.
                continue;
            }
            LocalFree(descriptor);
            g_thread = nullptr;
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
