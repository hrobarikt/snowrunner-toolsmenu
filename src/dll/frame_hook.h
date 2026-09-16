// The per-frame hook.
//
// A validated per-frame function in the game is patched with an absolute jump
// to an assembly detour, which preserves every register, calls the drain point,
// and continues into a trampoline holding the stolen bytes. Draining from there
// is what lets a command run on the thread the game already owns, at a point
// where the game's own state is consistent.
//
// Ported from the snowrunner-mod PowerShell host, where it was validated live
// and hardened against two detach races. It takes an address, a steal count and
// a sample of the bytes that must be there; it knows nothing about SnowRunner.

#pragma once

#include <windows.h>
#include <stdint.h>

namespace srtm {

// The longest byte sample a site may carry, and the bound on a steal region.
constexpr uint32_t kMaxSiteBytes = 64;

// FF 25 00000000 followed by an 8-byte target. Nothing shorter can be patched
// without clobbering the instruction after the stolen region.
constexpr uint32_t kAbsoluteJumpLength = 14;

// Installs the hook. `target` must currently hold `expected` byte for byte, or
// the install is refused without writing anything. This is the check that keeps
// a patched, updated or mis-resolved build from being written to at all.
bool InstallFrameHook(void* target,
                      uint32_t steal_bytes,
                      const uint8_t* expected,
                      uint32_t expected_count);

// Removes the patch, restores the original bytes and verifies the restoration,
// then waits until no other thread can execute another instruction of this
// module or of a trampoline page. Reports both answers separately: the bytes
// can be provably back while a thread is still on its way out, and the module
// must not be unloaded until `module_quiescent` is true. Trampoline pages are
// never freed. Calling this again after a non-quiescent result re-runs only the
// quiescence wait.
bool RemoveFrameHook(bool* original_bytes_verified, bool* module_quiescent);

// Restores the patched bytes without suspending threads, sleeping or freeing
// anything. Safe to call under the loader lock; the trampoline is kept.
bool RestoreFrameHookBytesOnly();

bool IsFrameHookInstalled();
uint32_t StolenByteCount();

// Incremented once per detour entry, for diagnostics.
uint64_t FrameCounter();

// Number of threads that claimed the detour and have not yet released it. The
// detour claims it in its first instruction and releases it in its
// second-to-last one, so a thread that is counted is definitely inside, and a
// thread that is inside but not yet counted has its instruction pointer on this
// module's image. Detach checks both.
long ActiveDetourCount();

// Called from the detour, once per frame, already on the game's own thread at a
// safe point. Defined by whoever owns the command queue.
void OnFrame();

}  // namespace srtm

// Called by the detour. Counts the frame and runs the per-frame work.
extern "C" void SrtmFrameDrain();

// Claimed and released by the detour itself, in assembly, so that no module
// memory is touched after the release. See detour.asm.
extern "C" volatile LONG g_active_detours;
