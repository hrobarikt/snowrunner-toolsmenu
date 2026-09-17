// Link implementation. See link.h.
#include "link.h"

#include "injector.h"
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
std::atomic<int> g_want_hotkey{-1};      // -1 none

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
    DWORD moved = 0;
    bool ok = WriteFile(pipe, &request, sizeof(request), &moved, nullptr) &&
              moved == sizeof(request) &&
              ReadFile(pipe, header, sizeof(*header), &moved, nullptr) &&
              moved == sizeof(*header) && header->magic == kProtocolMagic &&
              header->version == kProtocolVersion;
    if (ok && header->report_length > 0 && header->report_length <= kMaxReportBytes) {
        report->resize(header->report_length);
        DWORD read = 0;
        if (!ReadFile(pipe, report->data(), header->report_length, &read, nullptr) ||
            read != header->report_length) {
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
            g_want_hotkey.store(-1);
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
        const int hotkey = g_want_hotkey.exchange(-1);
        const bool detach = g_want_detach.exchange(false);

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
        } else if (hotkey >= 0) {
            answered = Exchange(pid, Opcode::SetHotkey, static_cast<uint32_t>(hotkey),
                                &header, &report);
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
                // The module unloads itself after replying, so it is gone
                // before the next poll. Say so now rather than showing a stale
                // "ready" for a third of a second.
                std::lock_guard<std::mutex> guard(g_lock);
                g_view.module_present = false;
                g_view.hook_installed = false;
                g_view.menu_on = false;
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
    g_want_hotkey.store(static_cast<int>(virtual_key));
}
void RequestDetachModule() { g_want_detach.store(true); }

bool DetachRequested() { return g_detached_by_user.load(); }

}  // namespace srtm
