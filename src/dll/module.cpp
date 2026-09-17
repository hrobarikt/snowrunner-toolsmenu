// Module implementation. See module.h.
//
// The command queue and the detach sequence are ported from snowrunner-mod
// (src/native/snowrunner-host/src/dllmain.cpp). What is gone with the host is
// the protocol: there is no Configure request, because the module scans for
// itself, and no guard to consult, because the guard the design keeps lives in
// tools_menu.cpp. The pipe that will let the tray app read the status and send
// a toggle is a later step; nothing here waits for it.
#include "module.h"

#include "frame_hook.h"
#include "image.h"
#include "pipe.h"
#include "tools_menu_scan.h"

namespace srtm {
namespace {

HMODULE g_self = nullptr;
HANDLE g_worker = nullptr;

CRITICAL_SECTION g_status_lock;
ModuleStatus g_status;

// The hotkey is read by the game's thread every frame and written by whoever
// changes it, so it is interlocked rather than held under the status lock.
volatile LONG g_hotkey_vk = VK_HOME;

// Set once the hook is in and cleared before it comes out, so the drain point
// stops doing work before the patch is removed.
volatile LONG g_running = 0;

// Single-slot command queue. A caller publishes and blocks; the frame hook
// claims and completes. One slot is enough: every caller waits for its own
// command, and a deeper queue would only make the failure modes harder to
// reason about.
enum QueueState : LONG {
    kQueueEmpty = 0,
    kQueuePending = 1,
    kQueueRunning = 2,
    kQueueDone = 3,
};

volatile LONG g_queue_state = kQueueEmpty;
bool g_queued_enable = false;
ToolsMenuStatus g_queued_status = ToolsMenuStatus::NotConfigured;
ToolsMenuObservation g_queued_observation = {};
HANDLE g_queue_done = nullptr;
CRITICAL_SECTION g_queue_lock;

void SetState(ModuleState state) {
    EnterCriticalSection(&g_status_lock);
    g_status.state = state;
    LeaveCriticalSection(&g_status_lock);
}

void RecordCommand(ToolsMenuStatus status, const ToolsMenuObservation& observation) {
    EnterCriticalSection(&g_status_lock);
    g_status.last_command = status;
    g_status.last_command_valid = true;
    g_status.menu_on = observation.owned && observation.menu_slot != 0;
    LeaveCriticalSection(&g_status_lock);
}

// The loaded game image, laid out by RVA exactly as the scanner wants it: the
// loader has already done the laying out, so the module in memory is the view.
const uint8_t* MainModuleBase(uint32_t* size) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return nullptr;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    *size = nt->OptionalHeader.SizeOfImage;
    return base;
}

// True only while the game itself has focus. Without this the hotkey would fire
// while the user is typing somewhere else, which is the one thing a global
// polled key must not do.
bool GameHasFocus() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

// Edge detection, so holding the key toggles once. Both the previous state and
// the poll live on the game's thread, so neither needs guarding.
void PollHotkey() {
    static bool was_down = false;
    const int key = static_cast<int>(InterlockedCompareExchange(&g_hotkey_vk, 0, 0));
    const bool down = key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0 && GameHasFocus();
    const bool pressed = down && !was_down;
    was_down = down;
    if (!pressed) {
        return;
    }

    ToolsMenuObservation observation = {};
    const ToolsMenuStatus status = SetToolsMenu(!ToolsMenuOwned(), &observation);
    RecordCommand(status, observation);
}

void DrainQueuedCommand() {
    if (InterlockedCompareExchange(&g_queue_state, kQueueRunning, kQueuePending) !=
        kQueuePending) {
        return;
    }
    g_queued_status = SetToolsMenu(g_queued_enable, &g_queued_observation);
    RecordCommand(g_queued_status, g_queued_observation);
    InterlockedExchange(&g_queue_state, kQueueDone);
    SetEvent(g_queue_done);
}

DWORD WINAPI WorkerThread(LPVOID) {
    uint32_t module_size = 0;
    const uint8_t* module_base = MainModuleBase(&module_size);

    ScanReport report;
    if (module_base != nullptr) {
        const std::optional<Image> image =
            Image::Parse(module_base, module_size,
                         reinterpret_cast<uint64_t>(module_base), /*live=*/true);
        if (image.has_value()) {
            report = ScanToolsMenu(*image);
        }
    }

    EnterCriticalSection(&g_status_lock);
    g_status.scan_report = FormatReport(report);
    LeaveCriticalSection(&g_status_lock);

    if (!report.usable) {
        // Nothing is patched and nothing is called. The module stays loaded so
        // that the report can still be read out of it.
        SetState(ModuleState::Unsupported);
        return 0;
    }

    if (!ConfigureToolsMenu(report.layout, module_base, module_size)) {
        SetState(ModuleState::Failed);
        return 0;
    }

    void* target = const_cast<uint8_t*>(module_base) + report.layout.frame_tick_rva;
    if (!InstallFrameHook(target, report.layout.frame_tick_steal_bytes,
                          report.layout.frame_tick_sample,
                          report.layout.frame_tick_sample_length)) {
        SetState(ModuleState::Failed);
        return 0;
    }

    InterlockedExchange(&g_running, 1);
    EnterCriticalSection(&g_status_lock);
    g_status.hook_installed = true;
    g_status.state = ModuleState::Ready;
    LeaveCriticalSection(&g_status_lock);
    return 0;
}

}  // namespace

void OnFrame() {
    if (InterlockedCompareExchange(&g_running, 0, 0) == 0) {
        return;
    }
    EnterCriticalSection(&g_status_lock);
    g_status.frames = FrameCounter();
    LeaveCriticalSection(&g_status_lock);

    DrainQueuedCommand();
    PollHotkey();
}

bool StartModule(HMODULE self) {
    g_self = self;
    InitializeCriticalSection(&g_status_lock);
    InitializeCriticalSection(&g_queue_lock);
    g_queue_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_queue_done == nullptr) {
        DeleteCriticalSection(&g_queue_lock);
        DeleteCriticalSection(&g_status_lock);
        return false;
    }
    // The pipe comes up before the scan finishes, so the app can connect
    // straight away and watch the state go from Scanning to whatever it turns
    // out to be, rather than guessing whether the module is there at all.
    StartPipeServer();

    g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (g_worker == nullptr) {
        CloseHandle(g_queue_done);
        g_queue_done = nullptr;
        DeleteCriticalSection(&g_queue_lock);
        DeleteCriticalSection(&g_status_lock);
        return false;
    }
    return true;
}

ModuleStatus GetModuleStatus() {
    EnterCriticalSection(&g_status_lock);
    ModuleStatus copy = g_status;
    LeaveCriticalSection(&g_status_lock);
    return copy;
}

ToolsMenuStatus RequestSetMenu(bool enable) {
    if (!IsFrameHookInstalled()) {
        return ToolsMenuStatus::NotConfigured;
    }

    EnterCriticalSection(&g_queue_lock);
    const LONG existing =
        InterlockedCompareExchange(&g_queue_state, kQueueEmpty, kQueueDone);
    if (existing == kQueuePending || existing == kQueueRunning) {
        LeaveCriticalSection(&g_queue_lock);
        return ToolsMenuStatus::WorldUnavailable;
    }
    g_queued_enable = enable;
    g_queued_status = ToolsMenuStatus::NotConfigured;
    g_queued_observation = ToolsMenuObservation{};
    ResetEvent(g_queue_done);
    InterlockedExchange(&g_queue_state, kQueuePending);

    // Two seconds is many frames. If the hook has not drained by then the drain
    // point is not running, and the command must not be retried off-thread.
    ToolsMenuStatus status = ToolsMenuStatus::WorldUnavailable;
    if (WaitForSingleObject(g_queue_done, 2000) == WAIT_OBJECT_0) {
        status = g_queued_status;
        InterlockedExchange(&g_queue_state, kQueueEmpty);
    } else {
        // Only withdraw a command the hook has not already claimed. If it is
        // mid-flight the slot is left alone, so the drain does not write into a
        // request that has been replaced.
        InterlockedCompareExchange(&g_queue_state, kQueueEmpty, kQueuePending);
    }
    LeaveCriticalSection(&g_queue_lock);
    return status;
}

void SetHotkey(uint32_t virtual_key) {
    InterlockedExchange(&g_hotkey_vk, static_cast<LONG>(virtual_key));
    EnterCriticalSection(&g_status_lock);
    g_status.hotkey_vk = virtual_key;
    LeaveCriticalSection(&g_status_lock);
}

bool DetachModule() {
    SetState(ModuleState::Detaching);

    // A menu of ours is taken away through the game's own destroy path, on the
    // game's thread, while the hook that reaches that thread is still in.
    if (ToolsMenuOwned() && IsFrameHookInstalled()) {
        RequestSetMenu(false);
    }

    // From here the drain point stops doing work, so the hook can come out from
    // under a thread that is already on its way into it.
    InterlockedExchange(&g_running, 0);

    bool verified = false;
    bool quiescent = false;
    RemoveFrameHook(&verified, &quiescent);

    EnterCriticalSection(&g_status_lock);
    g_status.hook_installed = IsFrameHookInstalled();
    g_status.menu_on = ToolsMenuOwned();
    g_status.state = ModuleState::Detached;
    LeaveCriticalSection(&g_status_lock);

    return quiescent;
}

HMODULE ModuleHandle() { return g_self; }

}  // namespace srtm
