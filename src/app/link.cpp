// Link implementation. See link.h.
#include "link.h"

#include "injector.h"
#include "pipe_io.h"
#include "protocol.h"
#include "resource.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace srtm {
namespace {

std::mutex g_lock;
LinkView g_view;
std::wstring g_dll_path;

std::thread g_worker;
std::atomic<bool> g_stop{false};

// One pending command at a time. The window can only produce them as fast as a
// person can click, and each is answered before the next poll.
std::atomic<bool> g_want_attach{false};
std::atomic<bool> g_want_detach{false};
std::atomic<int> g_want_menu{-1};        // -1 none, 0 off, 1 on

// The hotkey the user chose, which is a wish rather than a command: the worker
// pushes it whenever the module disagrees, so it survives a re-attach and a
// game restart as well as a change made here.
std::atomic<uint32_t> g_desired_hotkey{VK_HOME};

// The one thing the app remembers between runs. A keybinding is not the kind of
// configuration the design rules out -- that is about patterns and offsets,
// which no user should be editing -- so it lives in the registry rather than a
// file beside the exe.
constexpr wchar_t kSettingsKey[] = L"Software\\snowrunner-toolsmenu";
constexpr wchar_t kHotkeyValue[] = L"HotkeyVirtualKey";

uint32_t LoadHotkey() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kHotkeyValue, RRF_RT_REG_DWORD,
                     &type, &value, &size) != ERROR_SUCCESS) {
        return VK_HOME;
    }
    // A key code out of range would leave the toggle unreachable with no way to
    // say so, so anything unexpected falls back to the default.
    return value <= 0xFF ? static_cast<uint32_t>(value) : VK_HOME;
}

void SaveHotkey(uint32_t virtual_key) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = virtual_key;
    RegSetValueExW(key, kHotkeyValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

// Set by a detach, so the worker stops putting the module back into a game the
// user just asked it to leave. Cleared by an attach, or by a new game.
std::atomic<bool> g_detached_by_user{false};

void SetTrouble(const std::wstring& text) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_view.trouble = text;
}

// The DLL travels inside this exe, so there is one file to download and
// nothing to install. It is written out only when an attach needs it.
bool WriteDllFromResources(const std::wstring& path, std::wstring* trouble) {
    const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SRTM_DLL), RT_RCDATA);
    const HGLOBAL loaded = found != nullptr ? LoadResource(nullptr, found) : nullptr;
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    const DWORD size = found != nullptr ? SizeofResource(nullptr, found) : 0;
    if (bytes == nullptr || size == 0) {
        *trouble = L"This build of the app has no module inside it.";
        return false;
    }

    // An identical file that is already there is left alone: the game may have
    // it open, and rewriting it would fail for no reason.
    const HANDLE existing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
    if (existing != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER existing_size = {};
        bool same = GetFileSizeEx(existing, &existing_size) &&
                    existing_size.QuadPart == static_cast<LONGLONG>(size);
        if (same) {
            std::vector<uint8_t> buffer(size);
            DWORD read = 0;
            same = ReadFile(existing, buffer.data(), size, &read, nullptr) && read == size &&
                   memcmp(buffer.data(), bytes, size) == 0;
        }
        CloseHandle(existing);
        if (same) {
            return true;
        }
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *trouble = L"Could not write the module to disk. Antivirus may have blocked it.";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) {
        *trouble = L"Could not finish writing the module to disk.";
        return false;
    }
    return true;
}

// One request, one connection. False means nothing is listening, which is the
// ordinary answer when the module is not in the game yet.
bool Exchange(DWORD pid, Opcode opcode, uint32_t argument, ResponseHeader* header,
              std::string* report) {
    const std::wstring name = PipeName(pid);
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    Request request;
    request.opcode = static_cast<uint32_t>(opcode);
    request.argument = argument;
    const bool ok = WriteAll(pipe, &request, sizeof(request)) &&
                    ReadAll(pipe, header, sizeof(*header)) &&
                    header->magic == kProtocolMagic &&
                    header->version == kProtocolVersion;
    if (ok && header->report_length > 0 && header->report_length <= kMaxReportBytes) {
        report->resize(header->report_length);
        if (!ReadAll(pipe, report->data(), header->report_length)) {
            report->clear();
        }
    }
    CloseHandle(pipe);
    return ok;
}

void PublishReply(DWORD pid, const ResponseHeader& header, const std::string& report) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_view.game_running = true;
    g_view.module_present = true;
    g_view.pid = pid;
    g_view.state = header.state;
    g_view.hook_installed = (header.flags & kFlagHookInstalled) != 0;
    g_view.menu_on = (header.flags & kFlagMenuOn) != 0;
    g_view.hotkey_vk = header.hotkey_vk;
    g_view.frames = header.frames;
    g_view.last_command = header.last_command;
    g_view.last_command_valid = (header.flags & kFlagLastCommandValid) != 0;
    if (!report.empty()) {
        g_view.report = report;
    }
}

void Attach(DWORD pid) {
    std::wstring trouble;
    if (!WriteDllFromResources(g_dll_path, &trouble)) {
        SetTrouble(trouble);
        return;
    }
    const InjectResult result = InjectLibrary(pid, g_dll_path, &trouble);
    switch (result) {
        case InjectResult::Ok:
        case InjectResult::AlreadyLoaded:
            SetTrouble(L"");
            break;
        case InjectResult::GameNotRunning:
            SetTrouble(L"SnowRunner is not running.");
            break;
        default:
            SetTrouble(trouble);
            break;
    }
}

// What the module said its detach achieved. Only an unloading module is gone;
// the other two endings leave it in the game, and the view has to keep saying
// so or the status line starts lying about an untouched game.
void HandleDetachReply(const ResponseHeader& header) {
    if ((header.flags & kFlagUnloading) != 0) {
        // Gone before the next poll. Said now rather than showing a stale
        // "ready" for a third of a second.
        std::lock_guard<std::mutex> guard(g_lock);
        g_view.module_present = false;
        g_view.hook_installed = false;
        g_view.menu_on = false;
        return;
    }
    if ((header.flags & kFlagStillWarm) != 0) {
        // The patch is out but a thread is still inside the module. Ask again:
        // the next poll is 300ms away, and the wait is what finishes the
        // unload.
        g_want_detach.store(true);
        return;
    }
    // Detach did not get the original bytes back. The module stays, the state
    // it published says why, and asking again would not change it.
    SetTrouble(L"The game's original code could not be put back, so the module "
               L"is staying loaded. Close SnowRunner when convenient.");
}

void WorkerLoop() {
    DWORD known_pid = 0;
    while (!g_stop.load()) {
        const DWORD pid = FindGameProcess();
        if (pid != known_pid) {
            // A different game process is a clean slate: whatever the user
            // asked of the last one does not carry over.
            known_pid = pid;
            g_detached_by_user.store(false);
            std::lock_guard<std::mutex> guard(g_lock);
            g_view = LinkView{};
        }

        if (pid == 0) {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_view.game_running = false;
                g_view.module_present = false;
                g_view.pid = 0;
            }
            g_want_attach.store(false);
            g_want_detach.store(false);
            g_want_menu.store(-1);
            Sleep(500);
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_view.game_running = true;
            g_view.pid = pid;
        }

        if (g_want_attach.exchange(false)) {
            g_detached_by_user.store(false);
        }

        ResponseHeader header = {};
        std::string report;
        bool answered = false;

        const int menu = g_want_menu.exchange(-1);
        const bool detach = g_want_detach.exchange(false);

        // Pushed whenever the module's idea of the hotkey is not the user's.
        // The module always starts on its own default, so this is also what
        // restores the choice after an attach.
        uint32_t hotkey = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            const uint32_t desired = g_desired_hotkey.load();
            if (g_view.module_present && g_view.hotkey_vk != desired) {
                hotkey = desired;
            }
        }

        if (detach) {
            g_detached_by_user.store(true);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_view.busy = true;
            }
            answered = Exchange(pid, Opcode::Detach, 0, &header, &report);
        } else if (menu >= 0) {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_view.busy = true;
            }
            answered = Exchange(pid, Opcode::SetMenu, menu != 0 ? 1u : 0u, &header, &report);
        } else if (hotkey != 0) {
            answered = Exchange(pid, Opcode::SetHotkey, hotkey, &header, &report);
        } else {
            answered = Exchange(pid, Opcode::Status, 0, &header, &report);
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_view.busy = false;
        }

        if (answered) {
            PublishReply(pid, header, report);
            if (detach) {
                HandleDetachReply(header);
            }
        } else {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_view.module_present = false;
                g_view.hook_installed = false;
                g_view.menu_on = false;
            }
            // Nothing is listening. Unless the user asked the module to leave,
            // this is a game that has just appeared, and attaching to it is the
            // whole job of the tray.
            if (!g_detached_by_user.load()) {
                Attach(pid);
            }
        }

        Sleep(300);
    }
}

}  // namespace

bool StartLink(const std::wstring& dll_path) {
    g_dll_path = dll_path;
    g_desired_hotkey.store(LoadHotkey());
    g_stop.store(false);
    g_worker = std::thread(WorkerLoop);
    return true;
}

void StopLink() {
    g_stop.store(true);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

LinkView GetLinkView() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_view;
}

void RequestAttach() { g_want_attach.store(true); }
void RequestMenu(bool on) { g_want_menu.store(on ? 1 : 0); }
void RequestHotkey(uint32_t virtual_key) {
    g_desired_hotkey.store(virtual_key);
    SaveHotkey(virtual_key);
}
void RequestDetachModule() { g_want_detach.store(true); }

void DetachAndWait(unsigned timeout_ms) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_view.module_present) {
            return;
        }
    }
    RequestDetachModule();
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        Sleep(50);
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_view.module_present) {
            return;
        }
    }
}

bool DetachRequested() { return g_detached_by_user.load(); }

}  // namespace srtm
