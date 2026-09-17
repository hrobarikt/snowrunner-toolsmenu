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
#include <tlhelp32.h>

#include "protocol.h"

#include <cstdio>
#include <string>

namespace {

DWORD FindGame() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"SnowRunner.exe") == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

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

bool AlreadyLoaded(DWORD pid, const std::wstring& dll) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExePath, dll.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

const wchar_t* StateName(uint32_t state) {
    switch (state) {
        case 0: return L"scanning";
        case 1: return L"unsupported build";
        case 2: return L"failed";
        case 3: return L"ready";
        case 4: return L"detaching";
        case 5: return L"detached";
        default: return L"unknown";
    }
}

// Mirrors srtm::ToolsMenuStatus. The module's own text is not on the wire: the
// tray app will have its own wording, and this is a development tool.
const wchar_t* CommandName(uint32_t status) {
    switch (status) {
        case 0: return L"ok";
        case 1: return L"not configured";
        case 2: return L"site changed";
        case 3: return L"world unavailable";
        case 4: return L"menu already present";
        case 5: return L"menu is not ours";
        case 6: return L"menu call did not take";
        case 7: return L"call faulted";
        default: return L"unknown";
    }
}

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
    DWORD moved = 0;
    if (!WriteFile(pipe, &request, sizeof(request), &moved, nullptr) ||
        moved != sizeof(request)) {
        wprintf(L"Could not send the request (error %lu).\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }

    srtm::ResponseHeader header = {};
    if (!ReadFile(pipe, &header, sizeof(header), &moved, nullptr) ||
        moved != sizeof(header)) {
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
        DWORD read = 0;
        if (!ReadFile(pipe, report.data(), header.report_length, &read, nullptr) ||
            read != header.report_length) {
            report.clear();
        }
    }
    CloseHandle(pipe);

    wprintf(L"state        %s\n", StateName(header.state));
    wprintf(L"hook         %s\n",
            (header.flags & srtm::kFlagHookInstalled) ? L"installed" : L"not installed");
    wprintf(L"menu         %s\n", (header.flags & srtm::kFlagMenuOn) ? L"on" : L"off");
    wprintf(L"hotkey       vk 0x%02X\n", header.hotkey_vk);
    wprintf(L"frames       %llu\n", static_cast<unsigned long long>(header.frames));
    if (header.flags & srtm::kFlagLastCommandValid) {
        wprintf(L"last command %s\n", CommandName(header.last_command));
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
        const DWORD pid = FindGame();
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

    const DWORD pid = FindGame();
    if (pid == 0) {
        wprintf(L"SnowRunner.exe is not running.\n");
        return 1;
    }
    if (AlreadyLoaded(pid, dll)) {
        wprintf(L"Already loaded in pid %lu. Restart the game to load it again.\n", pid);
        return 1;
    }

    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (process == nullptr) {
        wprintf(L"Could not open pid %lu (error %lu). Try an elevated prompt.\n", pid,
                GetLastError());
        return 1;
    }

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (remote == nullptr ||
        !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
        wprintf(L"Could not write the path into the game (error %lu).\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    // LoadLibraryW sits at the same address in every process on this machine,
    // kernel32 being mapped at one base per boot.
    const auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr);
    if (thread == nullptr) {
        wprintf(L"Could not start the loader thread (error %lu).\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    WaitForSingleObject(thread, 10000);
    DWORD loaded = 0;
    GetExitCodeThread(thread, &loaded);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (loaded == 0) {
        wprintf(L"LoadLibraryW refused the DLL in pid %lu.\n", pid);
        return 1;
    }
    wprintf(L"Loaded into pid %lu. Press HOME in the game to toggle the menu.\n", pid);
    return 0;
}
