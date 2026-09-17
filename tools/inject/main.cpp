// A development injector: finds SnowRunner, loads srtm.dll into it, and says
// what happened. The tray app does this properly later, with the DLL embedded
// as a resource; this exists so the module can be exercised in the game before
// there is a GUI to do it from.
//
//   srtm-inject [path\to\srtm.dll]
//
// The DLL defaults to the one built beside this exe.

#include <windows.h>
#include <tlhelp32.h>

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

}  // namespace

int wmain(int argc, wchar_t** argv) {
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
