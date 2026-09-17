# SnowRunner Tools Menu

Unlocks the developer tools menu that SnowRunner already ships in Proving
Grounds, so it can be used in an offline campaign.

Nothing is reimplemented or injected into the game's logic: the tool calls the
game's own code, the same call the Proving Grounds level script makes. The free
camera comes with it, because it is gated on the same flag.

> **Status: working, not yet released.** Everything below has been tried in the
> game on Steam build `1.886173.SNOW_DLC_18`. It has not been tested on any
> other build, or on Epic or Game Pass at all. There is no release to download
> yet. See [`docs/design.md`](docs/design.md) for the design.

## Usage

1. Start SnowRunner normally, from Steam, Epic or Xbox, and load a save.
2. Run `srtm-toolsmenu.exe`. It sits in the tray.
3. Press `HOME` in game to toggle the menu. The key is configurable.

Order does not matter -- the tool can be started before or during a session --
and closing it from the tray puts the game back exactly as it was found: the
menu off, the hook removed, the original bytes checked back into place and the
module unloaded, with the game still running.

The menu needs a world loaded. At the main menu there is nothing to attach it
to, and the tool will say so rather than do anything.

One caveat: if the tool is force-killed from Task Manager it never gets the
chance to clean up, so its module stays in the game until the game is closed.
Nothing can be done about that from inside a process that has been killed.

## Single-player only

This is a tool for playing on your own. Please do not enable it in a co-op
session with people who have not asked for it. Earlier versions of this work
blocked co-op in code; that check depended on addresses pinned to one specific
Steam build, and removing it is what allows this tool to work on other stores
and to survive game updates. The honest trade is that it is now asking rather
than enforcing.

## Windows will warn you

This tool injects a DLL into a running game, which is behaviourally what a lot
of malware does, so Windows SmartScreen will warn about it and antivirus may
flag it. That is expected for an unsigned tool of this kind.

What is done about it:

- The binary is **never packed or obfuscated**. Packers are themselves a common
  detection trigger, and there is nothing here to hide.
- Every release is **built by GitHub Actions from a public commit**, and its
  SHA-256 is published alongside it, so the binary you download can be checked
  against the source that produced it.
- The full source is here. It can be built from scratch with `Build.ps1`.

## Game updates

A game update will usually break this tool until the signatures are updated.
When that happens, open the tool, expand **Diagnostics**, press **Copy**, and
paste the result into an issue -- that report is what makes a new build
supportable, including on stores the author does not own.

The Microsoft Store / Game Pass build is best-effort: it runs under restricted
permissions and injection may not work there. Steam and Epic are the targets
that get verified.

## Building

Requires Visual Studio 2022 Build Tools with the "Desktop development with C++"
workload.

```powershell
.\Build.ps1
```

This produces `build\src\app\Release\srtm-toolsmenu.exe`, which carries the
module inside it as a resource. Two development tools come with it and are not
part of a release: `srtm-scan.exe`, which prints the signature report for a
running game or a file, and `srtm-inject.exe`, which loads and drives the module
from a command line.

## Licence

GPL-3.0. Use it, fork it, improve it. It may not be folded into a closed-source
or paid product.

If it is useful to you, a donation is welcome but never required.
