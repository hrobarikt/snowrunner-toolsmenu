// Injector implementation. See injector.h.
//
// LoadLibraryW in a remote thread: the plainest method there is. Nothing here
// is hidden from the game or from anything watching it. The design rules out
// packing and obfuscation, and a manual mapper would buy nothing except the
// appearance of one.
#include "injector.h"

#include <tlhelp32.h>

namespace srtm {
namespace {

struct WindowSearch {
    DWORD pid = 0;
    bool found = false;
};

BOOL CALLBACK VisibleWindowOfProcess(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window)) {
        return TRUE;
    }
    // EnumWindows already walks top-level windows only, but an owned window --
    // a dialog, a tooltip -- is one too, and it is the game's own frame that
    // says the process is up.
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search->found = true;
    return FALSE;
}

std::wstring FormatError(const wchar_t* what, DWORD error) {
    wchar_t buffer[256] = {};
    _snwprintf_s(buffer, _TRUNCATE, L"%s (Windows error %lu).", what, error);
    return buffer;
}

}  // namespace

DWORD FindGameProcess() {
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

bool GameHasWindow(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    WindowSearch search;
    search.pid = pid;
    EnumWindows(VisibleWindowOfProcess, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool IsModuleLoaded(DWORD pid, const std::wstring& dll_path) {
    // The snapshot fails while the process is still starting up, which is not
    // the same answer as "no".
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExePath, dll_path.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

InjectResult InjectLibrary(DWORD pid, const std::wstring& dll_path, std::wstring* detail) {
    if (pid == 0) {
        return InjectResult::GameNotRunning;
    }
    if (IsModuleLoaded(pid, dll_path)) {
        return InjectResult::AlreadyLoaded;
    }

    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (process == nullptr) {
        const DWORD error = GetLastError();
        if (detail != nullptr) {
            *detail = error == ERROR_ACCESS_DENIED
                          ? L"The game would not let this in. If SnowRunner is running "
                            L"as administrator, this has to as well."
                          : FormatError(L"Could not open the game", error);
        }
        return error == ERROR_ACCESS_DENIED ? InjectResult::AccessDenied
                                            : InjectResult::Failed;
    }

    InjectResult result = InjectResult::Failed;
    const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (remote == nullptr) {
        if (detail != nullptr) {
            *detail = FormatError(L"Could not reserve memory in the game", GetLastError());
        }
    } else if (!WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr)) {
        if (detail != nullptr) {
            *detail = FormatError(L"Could not write into the game", GetLastError());
        }
    } else {
        // kernel32 is mapped at one address per boot, the same in every
        // process, so this module's LoadLibraryW is the game's as well.
        const auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        const HANDLE thread =
            CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr);
        if (thread == nullptr) {
            if (detail != nullptr) {
                *detail = FormatError(L"Could not start the loader in the game",
                                      GetLastError());
            }
        } else {
            // The module's own work happens on threads its entry point starts,
            // so this waits only for the load itself.
            const DWORD waited = WaitForSingleObject(thread, 15000);
            DWORD exit_code = 0;
            GetExitCodeThread(thread, &exit_code);
            CloseHandle(thread);
            // A thread exit code is 32 bits and LoadLibraryW returns a 64-bit
            // HMODULE, so a zero exit code is not proof of failure: the module
            // list is. A timed-out loader is asked nothing, because its answer
            // has not happened yet.
            const bool loaded =
                exit_code != 0 || IsModuleLoaded(pid, dll_path);
            if (waited != WAIT_OBJECT_0) {
                if (detail != nullptr) {
                    *detail = L"The game did not finish loading the module.";
                }
                // The loader thread is still running and still reading the path
                // out of the buffer below, so it is deliberately leaked: a few
                // hundred bytes in the game's address space, against freeing
                // memory out from under a live thread.
                remote = nullptr;
            } else if (!loaded) {
                if (detail != nullptr) {
                    *detail = L"The game refused the module. It may be the wrong "
                              L"architecture, or blocked by anti-cheat or antivirus.";
                }
            } else {
                result = InjectResult::Ok;
            }
        }
    }

    if (remote != nullptr) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return result;
}

}  // namespace srtm
