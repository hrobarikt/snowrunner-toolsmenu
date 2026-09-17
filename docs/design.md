# Design

The decisions this project is built on, and why. Agreed 2026-09-16.

## What it is

A trainer that unlocks the developer tools menu SnowRunner already ships in
Proving Grounds, in an offline campaign. One feature, done properly.

Nothing is reimplemented. The menu is turned on by calling the game's own
script-binding body -- the same call the Proving Grounds level script makes --
which writes a mode gate byte and then allocates the menu. The free camera is
not a separate feature: that same gate is what the per-frame free camera block
reads, so it comes with the menu rather than as its own toggle.

## Product shape

- **Tray-resident injector exe.** It waits for `SnowRunner.exe`, injects, and
  stays in the tray. Attach-only: the user launches the game normally through
  Steam, Epic or Xbox, and start order does not matter.
- **A configurable global hotkey** (default `HOME`) toggles the menu in-game,
  so the common action needs no alt-tab.
- **One desktop window**, opened from the tray, for when something is wrong: a
  toggle, a status line that always says something true, the hotkey setting,
  and a collapsed diagnostics panel with Copy and Save.
- **Diagnostics** is a live report, produced while the game runs. There is no
  *Scan a game file...* picker: Steam ships the executable SteamStub-wrapped
  with `.text` encrypted on disk, so a file scan matches nothing until the game
  has launched and decrypted itself in memory. Since a user reporting a problem
  has the game running anyway, the live report covers that case and also
  cross-checks the resolved addresses against each other, which a file scan
  cannot. `srtm-scan.exe --file` stays as an author-only command-line option.
- **Closing the tool restores the game**: menu off, hook removed, original
  bytes verified back, module unloaded.

There is deliberately no in-game overlay. Toggling is a single action, and a
hotkey covers it without a `Present` hook, an in-game render backend, or a
`WndProc` hook -- the three riskiest pieces of the alternative.

## Architecture

**The DLL is the whole brain.** It scans its own module for the signatures,
validates them, installs the frame hook, owns menu state, polls the hotkey from
the frame hook, and detaches cleanly. This is the deliberate inverse of the
predecessor project, where the DLL resolved nothing and a PowerShell harness
handed it every address.

**The GUI knows nothing about SnowRunner.** It injects, toggles, and displays.
C++ with Dear ImGui, statically linked, no runtime dependency; the DLL is
embedded as a resource so there is one file to download and a version mismatch
between exe and DLL is structurally impossible. A named pipe carries
request/response and doubles as a liveness signal.

Commands run on the game's own thread, from inside the frame hook. That is the
reason the hook exists and the reason an external `WriteProcessMemory` approach
was rejected.

## Guard model

Kept:

- **The byte-sample check.** A binding whose bytes are not the sample the
  scanner validated is never called, and a hook site that does not match is
  never patched.
- **Ownership.** Only a menu this module created is ever removed, and detach
  verifies the original bytes are back and waits until no thread is inside the
  module before unloading.

Dropped, deliberately:

- The SHA-256 executable allowlist, the level-id allowlist, and the campaign
  check built on hardcoded struct offsets. These pinned the predecessor to a
  single Steam build; removing them is what buys Epic, Game Pass and future
  patches. The cost is that nothing technically prevents enabling the menu in a
  co-op session, so the README says plainly that this is a single-player tool.

**An unrecognised build disables the toggle.** Every signature must resolve to
exactly one match module-wide; zero matches and two matches both fail. There is
no "try anyway", because the only thing it could mean is calling an address
nobody verified. Enable and disable are validated as a pair: an on-switch
without a proven off-switch is never offered.

## Patch resilience

Signatures are compiled into the DLL. There is no user-facing configuration --
users are not expected to edit patterns or offsets. An undocumented override
file is read if it happens to exist, purely so the author can iterate against a
patched build without a rebuild cycle; it is not shipped or mentioned.

What makes a new build supportable is the diagnostics report: which signatures
resolved, at which RVAs, with how many matches, plus the executable's version
and hash. That is what turns "it doesn't work" into a fixable bug report, and
it is the only route to supporting a store the author does not own. It has to
come from a running game, for the reason given under Product shape.

Verification is manual. Nothing meaningful here is CI-testable; the game has to
be launched and looked at.

## Practicals

GPL-3.0, so the addresses and knowledge here cannot be folded into a closed,
paid trainer while anyone remains free to use, fork and improve the tool.
CMake with MSVC, Dear ImGui vendored in `third_party/`. GitHub Actions builds
on tag and publishes the binary with its SHA-256; that, plus public source, is
the trust story that stands in for a code-signing certificate. The binary is
never packed or obfuscated -- packers are themselves a detection trigger, and
looking less like an injector is not a goal.

## Known caveats

- **Microsoft Store / Game Pass is best-effort, not promised.** It runs in an
  AppContainer with restricted ACLs; injection may need elevation or may not
  work at all. Steam and Epic are the verified targets.
- **A game patch will break this.** The signatures are resilience, not a
  guarantee.

## Carried over from the predecessor

`frame_hook.cpp` and `detour.asm` are ported in substance unchanged from the
snowrunner-mod native host, where they were validated live and hardened against
two detach races. The menu ownership logic and the six anchor signatures come
from the same place, as does `runtime-discovery.md`.
