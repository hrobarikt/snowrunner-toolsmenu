// Entry point. Everything real happens on the worker thread StartModule
// creates: the loader lock is held here, so this does as little as it can.
#include "module.h"

#include "frame_hook.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        return srtm::StartModule(module) ? TRUE : FALSE;
    }

    if (reason == DLL_PROCESS_DETACH) {
        // Detach is meant to come through RequestDetach, which can turn the
        // menu off first and wait for the module to go cold. This is the last
        // resort -- an unload nobody asked for, or the process going away.
        // Suspending threads or sleeping under the loader lock would deadlock
        // it, so only the patched bytes go back.
        srtm::RestoreFrameHookBytesOnly();
    }
    return TRUE;
}
