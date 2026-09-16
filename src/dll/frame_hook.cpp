// Frame hook implementation. See frame_hook.h.
//
// Ported unchanged in substance from snowrunner-mod (src/native/snowrunner-host/
// src/hook.cpp), where it was validated live and hardened against two detach
// races. Only the namespace and the drain symbol were renamed.
#include "frame_hook.h"

#include <tlhelp32.h>
#include <vector>

extern "C" void SrtmFrameDetour();
extern "C" uint64_t g_trampoline = 0;

// Claimed by the detour's first instruction and released by its second-to-last
// one, both with a locked read-modify-write. Detach samples it with every other
// thread suspended, so the value it reads cannot change underneath it.
extern "C" volatile LONG g_active_detours = 0;

namespace srtm {
namespace {

constexpr size_t kTrampolineSize = kMaxSiteBytes + kAbsoluteJumpLength;

// Every trampoline page allocated in this module's lifetime is kept, because a
// thread can be standing on one, or on its way to one through a register, at
// any point after the patch is written. They are one page each and there is at
// most one per install, so keeping them costs almost nothing and removes the
// free-underneath-a-running-thread failure entirely. The budget bounds the
// install-detach-install cycle a module that could not unload allows.
constexpr size_t kMaxTrampolinePages = 4;

// Many frames. If a thread has not left the module by then the drain point is
// wedged or a thread is blocked inside a game call, and detach reports the
// module as not cold rather than unloading it anyway.
constexpr uint32_t kQuiescenceTimeoutMs = 2000;

struct AddressRange {
    uintptr_t begin;
    uintptr_t end;
};

// The hook site and the bytes that belong there, kept for the rest of the
// module's life once a hook has been installed: a detach that restored the
// bytes but could not confirm the module was cold is repeated, and the repeat
// has to re-read the site rather than trust that the first one succeeded.
uint8_t* g_site = nullptr;
uint8_t g_original[kMaxSiteBytes] = {};
uint32_t g_stolen = 0;
bool g_patched = false;
uint8_t* g_trampoline_memory = nullptr;
AddressRange g_trampoline_pages[kMaxTrampolinePages] = {};
size_t g_trampoline_page_count = 0;
volatile uint64_t g_frames = 0;

void ResumeThreads(std::vector<HANDLE>& threads);

bool RangesContain(const AddressRange* ranges, size_t count, uintptr_t address) {
    for (size_t index = 0; index < count; ++index) {
        if (address >= ranges[index].begin && address < ranges[index].end) {
            return true;
        }
    }
    return false;
}

// The mapped image of this module. Detach refuses to report the module cold
// unless it can establish this range, because without it there is no way to
// tell whether a thread is standing on code that is about to be unmapped.
bool SelfImageRange(AddressRange* range) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SrtmFrameDetour), &self) ||
        self == nullptr) {
        return false;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(self);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    range->begin = reinterpret_cast<uintptr_t>(base);
    range->end = range->begin + nt->OptionalHeader.SizeOfImage;
    return true;
}

// Suspending the other threads keeps one from being inside the 14 bytes while
// they are only partly rewritten, and gives detach a snapshot that cannot move
// while it decides on it. Both APIs are documented and nothing is hidden from
// them.
bool SuspendOtherThreads(const AddressRange* forbidden,
                         size_t forbidden_count,
                         std::vector<HANDLE>* suspended) {
    if (suspended == nullptr) {
        return false;
    }
    suspended->clear();
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool safe = true;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
                continue;
            }
            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                if (GetLastError() == ERROR_INVALID_PARAMETER) {
                    continue;  // The snapshotted thread exited meanwhile.
                }
                safe = false;
                break;
            }
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread);
                safe = false;
                break;
            }
            suspended->push_back(thread);

            CONTEXT context = {};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread, &context)) {
                safe = false;
                break;
            }
            if (RangesContain(forbidden, forbidden_count,
                              static_cast<uintptr_t>(context.Rip))) {
                safe = false;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    } else {
        safe = false;
    }

    CloseHandle(snapshot);
    if (!safe) {
        ResumeThreads(*suspended);
    }
    return safe;
}

void ResumeThreads(std::vector<HANDLE>& threads) {
    for (HANDLE thread : threads) {
        ResumeThread(thread);
        CloseHandle(thread);
    }
    threads.clear();
}

// True once no other thread can execute another instruction of this module or
// of a trampoline page it allocated. Two conditions have to hold at the same
// instant, which is why they are sampled with the other threads suspended:
//
// - no thread's instruction pointer is on the module image or a trampoline
//   page, which covers the detour entry before the count is claimed, the tail
//   after it is released, and the trampoline itself;
// - no thread has entered the detour without leaving it, which covers a thread
//   whose instruction pointer is elsewhere -- inside a game function the drain
//   called -- but whose stack holds a return address into this module.
bool WaitForModuleQuiescence(uint32_t timeout_ms) {
    AddressRange image = {};
    if (!SelfImageRange(&image)) {
        return false;
    }
    std::vector<AddressRange> ranges;
    ranges.push_back(image);
    for (size_t index = 0; index < g_trampoline_page_count; ++index) {
        ranges.push_back(g_trampoline_pages[index]);
    }

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        // The cheap test first, and the expensive one only when it can pass.
        if (ActiveDetourCount() == 0) {
            std::vector<HANDLE> suspended;
            if (SuspendOtherThreads(ranges.data(), ranges.size(), &suspended)) {
                const bool cold = ActiveDetourCount() == 0;
                ResumeThreads(suspended);
                if (cold) {
                    return true;
                }
            }
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(10);
    }
}

bool WriteCode(void* address, const void* bytes, size_t count) {
    DWORD previous = 0;
    if (!VirtualProtect(address, count, PAGE_EXECUTE_READWRITE, &previous)) {
        return false;
    }
    memcpy(address, bytes, count);
    DWORD ignored = 0;
    VirtualProtect(address, count, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, count);
    return true;
}

// FF 25 00 00 00 00 <target>: an absolute indirect jump through the eight
// bytes that follow it. Position independent, so it is safe both in the
// patched function and at the end of the trampoline. The trampoline's copy
// reads its target out of the trampoline page itself, so a thread leaving the
// trampoline touches no module memory on the way out either.
void WriteAbsoluteJump(uint8_t* destination, const void* target) {
    destination[0] = 0xFF;
    destination[1] = 0x25;
    destination[2] = 0x00;
    destination[3] = 0x00;
    destination[4] = 0x00;
    destination[5] = 0x00;
    const uint64_t value = reinterpret_cast<uint64_t>(target);
    memcpy(destination + 6, &value, sizeof(value));
}

// Only for install failures, where the patch was never written and so the
// detour could never have reached this page. Every other path keeps it.
void ReleaseUnreachableTrampoline() {
    if (g_trampoline_memory == nullptr) {
        return;
    }
    if (g_trampoline_page_count > 0 &&
        g_trampoline_pages[g_trampoline_page_count - 1].begin ==
            reinterpret_cast<uintptr_t>(g_trampoline_memory)) {
        --g_trampoline_page_count;
        g_trampoline_pages[g_trampoline_page_count] = AddressRange{};
    }
    VirtualFree(g_trampoline_memory, 0, MEM_RELEASE);
    g_trampoline_memory = nullptr;
    g_trampoline = 0;
}

}  // namespace

uint64_t FrameCounter() { return g_frames; }

long ActiveDetourCount() { return InterlockedCompareExchange(&g_active_detours, 0, 0); }

bool IsFrameHookInstalled() { return g_patched; }

uint32_t StolenByteCount() { return g_stolen; }

bool InstallFrameHook(void* target,
                      uint32_t steal_bytes,
                      const uint8_t* expected,
                      uint32_t expected_count) {
    if (g_patched) {
        return false;
    }
    if (target == nullptr || steal_bytes < kAbsoluteJumpLength || steal_bytes > kMaxSiteBytes) {
        return false;
    }
    // Trampoline pages are never reclaimed, so a session that has exhausted the
    // budget refuses to install rather than reusing a page a thread may still
    // be standing on. A session needs more than one only when a detach left the
    // module loaded and the user configured it again.
    if (g_trampoline_page_count >= kMaxTrampolinePages) {
        return false;
    }
    // The harness captured these bytes when it validated the anchor. Refusing
    // to patch a site that no longer matches is what keeps a patched, updated
    // or mis-resolved build from being written to at all.
    if (expected == nullptr || expected_count < steal_bytes) {
        return false;
    }
    uint8_t* site = static_cast<uint8_t*>(target);
    if (memcmp(site, expected, steal_bytes) != 0) {
        return false;
    }

    g_trampoline_memory = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kTrampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_trampoline_memory == nullptr) {
        return false;
    }
    // Registered before the patch exists, because from the instant the patch is
    // written a thread can already be executing on this page.
    g_trampoline_pages[g_trampoline_page_count].begin =
        reinterpret_cast<uintptr_t>(g_trampoline_memory);
    g_trampoline_pages[g_trampoline_page_count].end =
        reinterpret_cast<uintptr_t>(g_trampoline_memory) + kTrampolineSize;
    ++g_trampoline_page_count;

    memcpy(g_original, site, steal_bytes);
    memcpy(g_trampoline_memory, site, steal_bytes);
    WriteAbsoluteJump(g_trampoline_memory + steal_bytes, site + steal_bytes);
    FlushInstructionCache(GetCurrentProcess(), g_trampoline_memory, kTrampolineSize);
    g_trampoline = reinterpret_cast<uint64_t>(g_trampoline_memory);

    uint8_t patch[kMaxSiteBytes];
    WriteAbsoluteJump(patch, reinterpret_cast<void*>(&SrtmFrameDetour));
    // Any byte of the stolen region past the jump is padded with a one-byte NOP
    // so the region stays a valid instruction stream.
    memset(patch + kAbsoluteJumpLength, 0x90, steal_bytes - kAbsoluteJumpLength);

    const AddressRange stolen_region = {reinterpret_cast<uintptr_t>(site),
                                        reinterpret_cast<uintptr_t>(site) + steal_bytes};
    std::vector<HANDLE> suspended;
    if (!SuspendOtherThreads(&stolen_region, 1, &suspended)) {
        ReleaseUnreachableTrampoline();
        return false;
    }
    const bool written = WriteCode(site, patch, steal_bytes);
    ResumeThreads(suspended);

    if (!written) {
        ReleaseUnreachableTrampoline();
        return false;
    }

    g_site = site;
    g_stolen = steal_bytes;
    g_patched = true;
    return true;
}

bool RestoreFrameHookBytesOnly() {
    // Last resort for DLL_PROCESS_DETACH, where suspending threads, sleeping or
    // allocating under the loader lock is not allowed. It restores the patched
    // bytes and nothing else; the trampoline stays mapped and registered,
    // because a thread may still be inside it.
    if (!g_patched) {
        return true;
    }
    const bool written = WriteCode(g_site, g_original, g_stolen);
    if (!written) {
        return false;
    }
    const bool verified = memcmp(g_site, g_original, g_stolen) == 0;
    if (verified) {
        g_patched = false;
        g_trampoline_memory = nullptr;
    }
    return verified;
}

bool RemoveFrameHook(bool* original_bytes_verified, bool* module_quiescent) {
    if (original_bytes_verified != nullptr) {
        *original_bytes_verified = false;
    }
    if (module_quiescent != nullptr) {
        *module_quiescent = false;
    }

    if (!g_patched) {
        // Nothing is patched: either the hook was never installed, or an
        // earlier detach restored it and could not confirm the module was cold.
        // Either way the only open question is whether it is cold now, so
        // repeating detach is how an unload that had to wait finishes. The
        // site is re-read rather than assumed, because this answer is the one
        // the host reports as a verified restoration.
        if (original_bytes_verified != nullptr) {
            *original_bytes_verified =
                g_site == nullptr || memcmp(g_site, g_original, g_stolen) == 0;
        }
        const bool cold = WaitForModuleQuiescence(kQuiescenceTimeoutMs);
        if (module_quiescent != nullptr) {
            *module_quiescent = cold;
        }
        return true;
    }

    const AddressRange stolen_region = {reinterpret_cast<uintptr_t>(g_site),
                                        reinterpret_cast<uintptr_t>(g_site) + g_stolen};
    std::vector<HANDLE> suspended;
    if (!SuspendOtherThreads(&stolen_region, 1, &suspended)) {
        return false;
    }
    const bool written = WriteCode(g_site, g_original, g_stolen);
    ResumeThreads(suspended);

    if (!written) {
        // The hook could not be restored, so entries continue. The trampoline
        // stays mapped, as it does on every other path.
        return false;
    }

    const bool verified = memcmp(g_site, g_original, g_stolen) == 0;
    if (original_bytes_verified != nullptr) {
        *original_bytes_verified = verified;
    }
    if (!verified) {
        return false;
    }

    // The original bytes went back while the other threads were suspended and
    // none of them was inside the stolen region, so no thread can enter the
    // detour from here on. What is left is the threads that entered before
    // that, which is what quiescence waits out. g_trampoline keeps pointing at
    // the page, because a thread already inside the detour has not read it yet.
    g_patched = false;
    g_trampoline_memory = nullptr;

    const bool cold = WaitForModuleQuiescence(kQuiescenceTimeoutMs);
    if (module_quiescent != nullptr) {
        *module_quiescent = cold;
    }
    return true;
}

}  // namespace srtm

extern "C" void SrtmFrameDrain() {
    // The detour claimed g_active_detours before calling in here, so this runs
    // inside that claim and the module cannot be unloaded underneath it.
    ++srtm::g_frames;
    srtm::OnFrame();
}
