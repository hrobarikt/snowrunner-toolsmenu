// Module implementation. See module.h.
//
// The command queue and the detach sequence are ported from snowrunner-mod
// (src/native/snowrunner-host/src/dllmain.cpp). What is gone with the host is
// the protocol: there is no Configure request, because the module scans for
// itself, and no guard to consult, because the guard the design keeps lives in
// tools_menu.cpp. The pipe server is started from here but owns its own thread;
// nothing in this file waits on it.
#include "module.h"

#include "build_identity.h"
#include "frame_hook.h"
#include "image.h"
#include "pipe.h"
#include "tools_menu_scan.h"

#include <cstdio>

namespace srtm {
namespace {

HMODULE g_self = nullptr;
HANDLE g_worker = nullptr;

// A scan that runs the instant the module lands reads a game that cannot be
// read yet. SnowRunner is SteamStub-wrapped, so its code only exists decrypted
// in memory once startup has decrypted it, and the frame-dispatch slot the scan
// validates is written later still. One attempt meant the module declared a
// perfectly supported build unrecognised whenever the app got in first, which
// is why the scan is a loop.
constexpr DWORD kScanIntervalMs = 1000;

// Measured on 2026-09-20, Steam build 1.886173.SNOW_DLC_18: with the tray
// waiting for the game's window before injecting, the first scan resolved
// 0.2s after the module loaded. Thirty seconds is that with a hundredfold
// margin, and the margin is what this constant is for -- a machine slower than
// the one it was measured on, or a startup that does not order itself as
// tidily. The deadline is only here so that a build which genuinely does not
// match reaches a verdict instead of spinning forever, and Try again restarts
// it; sizing it much larger only means staring longer at "reading" before
// being told the truth.
constexpr ULONGLONG kScanDeadlineMs = 30 * 1000;

// Manual-reset: the module is leaving, and the scan loop must be gone before
// anything unloads it. Auto-reset: the user asked for another look.
HANDLE g_scan_stop = nullptr;
HANDLE g_scan_again = nullptr;

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

// How the scan is going, in the report and nowhere else. The number that
// matters is how long after the module loaded the scan first resolved: it is
// the only evidence that says whether the deadline above is the right size,
// and it can only be measured on a real machine loading a real game.
void PublishReport(const ScanReport& report, unsigned attempt, ULONGLONG started,
                   bool still_looking) {
    const ULONGLONG elapsed = GetTickCount64() - started;
    const unsigned seconds = static_cast<unsigned>(elapsed / 1000);
    const unsigned tenths = static_cast<unsigned>((elapsed % 1000) / 100);

    char line[200] = {};
    if (report.usable) {
        _snprintf_s(line, _TRUNCATE,
                    "\n  scan    resolved %u.%us after the module loaded, on attempt %u\n",
                    seconds, tenths, attempt);
    } else if (still_looking) {
        _snprintf_s(line, _TRUNCATE,
                    "\n  scan    still looking: %u.%us in, %u attempts. The game may "
                    "still be starting up.\n",
                    seconds, tenths, attempt);
    } else {
        _snprintf_s(line, _TRUNCATE,
                    "\n  scan    gave up after %u.%us and %u attempts\n",
                    seconds, tenths, attempt);
    }

    std::string text = FormatReport(report);
    text += line;

    EnterCriticalSection(&g_status_lock);
    g_status.scan_report = text;
    LeaveCriticalSection(&g_status_lock);
}

// Asks the scan loop to leave and waits for it. False means it is still in
// there, which makes the module not cold no matter what the hook says: a loop
// waiting between passes has its instruction pointer outside this module, so
// quiescence cannot see it, and an unload would unmap the code it is about to
// return into.
void CloseScanEvents() {
    if (g_scan_stop != nullptr) {
        CloseHandle(g_scan_stop);
        g_scan_stop = nullptr;
    }
    if (g_scan_again != nullptr) {
        CloseHandle(g_scan_again);
        g_scan_again = nullptr;
    }
}

bool StopScanWorker(DWORD timeout_ms) {
    if (g_scan_stop != nullptr) {
        SetEvent(g_scan_stop);
    }
    if (g_worker == nullptr) {
        return true;
    }
    if (WaitForSingleObject(g_worker, timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    CloseHandle(g_worker);
    g_worker = nullptr;
    return true;
}

DWORD WINAPI WorkerThread(LPVOID) {
    uint32_t module_size = 0;
    const uint8_t* module_base = MainModuleBase(&module_size);
    // Which build this is, for the report. Hashing the executable is disk work
    // and the answer cannot change while the process is alive, so it happens
    // once here rather than on every pass of the loop below.
    const BuildIdentity identity = IdentifyFile(MainModulePath());
    const ULONGLONG started = GetTickCount64();

    for (;;) {
        SetState(ModuleState::Scanning);
        const ULONGLONG deadline = GetTickCount64() + kScanDeadlineMs;
        ScanReport report;
        unsigned attempt = 0;

        for (;;) {
            ++attempt;
            report = ScanReport{};
            if (module_base != nullptr) {
                const std::optional<Image> image =
                    Image::Parse(module_base, module_size,
                                 reinterpret_cast<uint64_t>(module_base), /*live=*/true);
                if (image.has_value()) {
                    report = ScanToolsMenu(*image);
                }
            }
            report.identity = identity;

            const bool out_of_time = GetTickCount64() >= deadline;
            PublishReport(report, attempt, started, !report.usable && !out_of_time);
            if (report.usable || out_of_time) {
                break;
            }
            if (WaitForSingleObject(g_scan_stop, kScanIntervalMs) == WAIT_OBJECT_0) {
                return 0;
            }
        }

        if (report.usable) {
            if (!ConfigureToolsMenu(report.layout, module_base, module_size)) {
                SetState(ModuleState::Failed);
            } else {
                void* target =
                    const_cast<uint8_t*>(module_base) + report.layout.frame_tick_rva;
                if (!InstallFrameHook(target, report.layout.frame_tick_steal_bytes,
                                      report.layout.frame_tick_sample,
                                      report.layout.frame_tick_sample_length)) {
                    SetState(ModuleState::Failed);
                } else {
                    InterlockedExchange(&g_running, 1);
                    EnterCriticalSection(&g_status_lock);
                    g_status.hook_installed = true;
                    g_status.state = ModuleState::Ready;
                    LeaveCriticalSection(&g_status_lock);
                    return 0;
                }
            }
        } else {
            // Nothing is patched and nothing is called. The module stays loaded
            // so that the report can still be read out of it.
            SetState(ModuleState::Unsupported);
        }

        // Neither ending is worth a thread spinning, but both are worth being
        // able to take back: the user may have been sitting on a load screen
        // longer than the deadline. Try again lands here.
        const HANDLE waits[] = {g_scan_stop, g_scan_again};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            return 0;
        }
    }
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
    g_scan_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_scan_again = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_queue_done == nullptr || g_scan_stop == nullptr || g_scan_again == nullptr) {
        CloseScanEvents();
        if (g_queue_done != nullptr) {
            CloseHandle(g_queue_done);
            g_queue_done = nullptr;
        }
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
        CloseScanEvents();
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
        return ToolsMenuStatus::Busy;
    }
    g_queued_enable = enable;
    g_queued_status = ToolsMenuStatus::NotConfigured;
    g_queued_observation = ToolsMenuObservation{};
    ResetEvent(g_queue_done);
    InterlockedExchange(&g_queue_state, kQueuePending);

    // Two seconds is many frames. If the hook has not drained by then the drain
    // point is not running, and the command must not be retried off-thread. That
    // is a stalled hook, not a missing world, and the status line says so.
    ToolsMenuStatus status = ToolsMenuStatus::HookStalled;
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

DetachOutcome DetachModule() {
    // First, because the scan loop may be one pass away from installing the
    // hook this is about to remove, and because a loop waiting between passes
    // is invisible to the quiescence check below.
    const bool scan_worker_gone = StopScanWorker(2000);

    SetState(ModuleState::Detaching);

    // A menu of ours is taken away through the game's own destroy path, on the
    // game's thread, while the hook that reaches that thread is still in.
    if (ToolsMenuOwned() && IsFrameHookInstalled()) {
        RequestSetMenu(false);
    }

    // From here the drain point stops doing work, so the hook can come out from
    // under a thread that is already on its way into it.
    InterlockedExchange(&g_running, 0);

    DetachOutcome outcome;
    RemoveFrameHook(&outcome.bytes_verified, &outcome.cold);

    // A scan loop that would not leave is a thread that can still execute this
    // module's code, whatever the hook says. Cold has to mean nobody is in
    // here, so it is the whole answer that gives way, not part of it.
    outcome.cold = outcome.cold && scan_worker_gone;

    // Three different endings, and the state has to tell them apart. Bytes that
    // are not provably back mean the game is still patched, which the app must
    // never render as "the game is untouched"; bytes back but a thread still
    // inside means detach is not finished, and a second one completes it.
    ModuleState state = ModuleState::DetachFailed;
    if (outcome.bytes_verified) {
        state = outcome.cold ? ModuleState::Detached : ModuleState::Detaching;
    }

    EnterCriticalSection(&g_status_lock);
    g_status.hook_installed = IsFrameHookInstalled();
    g_status.menu_on = ToolsMenuOwned();
    g_status.state = state;
    LeaveCriticalSection(&g_status_lock);

    return outcome;
}

void RequestRescan() {
    if (g_scan_again != nullptr) {
        SetEvent(g_scan_again);
    }
}

void StopModuleScan() {
    // No wait: the only caller is DllMain on an unload nobody asked for, and
    // waiting under the loader lock is what deadlocks it. Signalling still
    // shortens the window in which the loop is running during a teardown.
    if (g_scan_stop != nullptr) {
        SetEvent(g_scan_stop);
    }
}

HMODULE ModuleHandle() { return g_self; }

}  // namespace srtm
