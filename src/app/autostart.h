// "Start with Windows", which is one value under HKCU\...\Run and one piece of
// Explorer bookkeeping next to it.
//
// The Run value is the whole mechanism: no scheduled task, because nothing here
// needs elevation, and no shortcut in the Startup folder, because that is a
// file to lose track of. What the Run key does not tell you is whether the user
// has since switched the entry off in Task Manager's Startup tab -- that lives
// in StartupApproved, and reading only the Run value is how a checkbox ends up
// lying. Both are read here, and the user's Task Manager choice is never
// overruled behind their back.

#pragma once

#include <string>

namespace srtm {

// True only when Windows will actually start the app at logon: the Run value is
// there and Explorer has not been told to skip it.
bool AutostartEnabled();

// Turns it on or off. False when Windows refused -- policy, or an antivirus
// guarding the Run key -- and `trouble` then holds a sentence fit to show.
bool SetAutostart(bool enabled, std::wstring* trouble);

// The exe is portable: it gets moved, and a new release lands in a new folder.
// When autostart is on and the stored path is not this exe, this puts the
// current one back. Only ever fixes a copy that has been run at least once;
// nothing can fix one that was moved and never opened again.
void RefreshAutostartPath();

}  // namespace srtm
