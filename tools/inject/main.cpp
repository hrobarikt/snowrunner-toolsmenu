// A development injector and pipe client: finds SnowRunner, loads srtm.dll into
// it, and then talks to it. The tray app does all of this properly later, with
// the DLL embedded as a resource and a window instead of arguments; this exists
// so the module can be exercised in the game before there is a GUI.
//
//   srtm-inject [path\to\srtm.dll]   load the DLL (the default is beside this exe)
//   srtm-inject --status             print what the module knows
//   srtm-inject --on | --off         toggle the menu over the pipe
//   srtm-inject --hotkey <vk>        set the toggle key, 0 to disable it
//   srtm-inject --detach             menu off, unhook, unload

#include <windows.h>

#include "injector.h"
#include "pipe_io.h"
#include "protocol.h"

#include <cstdio>
#include <string>

namespace {

std::wstring DefaultDllPath() {
    wchar_t self[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path(self, count);
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    return path + L"srtm.dll";
}

// The state and status names come from protocol.h, which is where the enums
// themselves live. This tool used to carry its own copies, keyed by number, and
// they drifted from the module's.

// One request, one connection. Prints the answer and returns the exit code.
int Talk(DWORD pid, srtm::Opcode opcode, uint32_t argument, bool print_report) {
    const std::wstring name = srtm::PipeName(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            wprintf(L"No module listening in pid %lu. Inject it first.\n", pid);
            return 1;
        }
        WaitNamedPipeW(name.c_str(), 200);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        wprintf(L"The module's pipe stayed busy.\n");
        return 1;
    }

    srtm::Request request;
    request.opcode = static_cast<uint32_t>(opcode);
    request.argument = argument;
    if (!srtm::WriteAll(pipe, &request, sizeof(request))) {
        wprintf(L"Could not send the request (error %lu).\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }

    srtm::ResponseHeader header = {};
    if (!srtm::ReadAll(pipe, &header, sizeof(header))) {
        wprintf(L"No reply (error %lu).\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }
    if (header.magic != srtm::kProtocolMagic || header.version != srtm::kProtocolVersion) {
        wprintf(L"The module speaks a different protocol. Rebuild both ends.\n");
        CloseHandle(pipe);
        return 1;
    }

    std::string report;
    if (header.report_length > 0 && header.report_length <= srtm::kMaxReportBytes) {
        report.resize(header.report_length);
        if (!srtm::ReadAll(pipe, report.data(), header.report_length)) {
            report.clear();
        }
    }
    CloseHandle(pipe);

    wprintf(L"state        %S\n",
            srtm::ModuleStateText(static_cast<srtm::ModuleState>(header.state)));
    wprintf(L"hook         %s\n",
            (header.flags & srtm::kFlagHookInstalled) ? L"installed" : L"not installed");
    wprintf(L"menu         %s\n", (header.flags & srtm::kFlagMenuOn) ? L"on" : L"off");
    wprintf(L"hotkey       vk 0x%02X\n", header.hotkey_vk);
    wprintf(L"frames       %llu\n", static_cast<unsigned long long>(header.frames));
    if (header.flags & srtm::kFlagLastCommandValid) {
        wprintf(L"last command %S\n",
                srtm::ToolsMenuStatusText(
                    static_cast<srtm::ToolsMenuStatus>(header.last_command)));
    }
    if (header.flags & srtm::kFlagUnloading) {
        wprintf(L"detach       done, the module is unloading\n");
    } else if (header.flags & srtm::kFlagStillWarm) {
        wprintf(L"detach       patch out, a thread is still inside; detach again\n");
    }
    if (print_report && !report.empty()) {
        wprintf(L"\n%S\n", report.c_str());
    }
    return header.status == static_cast<uint32_t>(srtm::PipeStatus::Ok) ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring first = argc > 1 ? argv[1] : L"";
    if (!first.empty() && first[0] == L'-') {
        const DWORD pid = srtm::FindGameProcess();
        if (pid == 0) {
            wprintf(L"SnowRunner.exe is not running.\n");
            return 1;
        }
        if (first == L"--status") {
            return Talk(pid, srtm::Opcode::Status, 0, /*print_report=*/true);
        }
        if (first == L"--on") {
            return Talk(pid, srtm::Opcode::SetMenu, 1, false);
        }
        if (first == L"--off") {
            return Talk(pid, srtm::Opcode::SetMenu, 0, false);
        }
        if (first == L"--detach") {
            return Talk(pid, srtm::Opcode::Detach, 0, false);
        }
        if (first == L"--hotkey" && argc > 2) {
            return Talk(pid, srtm::Opcode::SetHotkey,
                        static_cast<uint32_t>(wcstoul(argv[2], nullptr, 0)), false);
        }
        wprintf(L"Usage: srtm-inject [dll] | --status | --on | --off | "
                L"--hotkey <vk> | --detach\n");
        return 1;
    }

    const std::wstring dll = argc > 1 ? argv[1] : DefaultDllPath();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"No DLL at %s\n", dll.c_str());
        return 1;
    }

    const DWORD pid = srtm::FindGameProcess();
    if (pid == 0) {
        wprintf(L"SnowRunner.exe is not running.\n");
        return 1;
    }
    std::wstring detail;
    switch (srtm::InjectLibrary(pid, dll, &detail)) {
        case srtm::InjectResult::Ok:
            wprintf(L"Loaded into pid %lu. Press HOME in the game to toggle the menu.\n",
                    pid);
            return 0;
        case srtm::InjectResult::AlreadyLoaded:
            wprintf(L"Already loaded in pid %lu.\n", pid);
            return 1;
        case srtm::InjectResult::GameNotRunning:
            wprintf(L"SnowRunner.exe is not running.\n");
            return 1;
        default:
            wprintf(L"%s\n", detail.c_str());
            return 1;
    }
}
