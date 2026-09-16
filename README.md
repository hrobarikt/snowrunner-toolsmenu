# SnowRunner Tools Menu

Unlocks the developer tools menu that SnowRunner already ships in Proving
Grounds, so it can be used in an offline campaign.

Nothing is reimplemented or injected into the game's logic: the tool calls the
game's own code, the same call the Proving Grounds level script makes. The free
camera comes with it, because it is gated on the same flag.

> **Status: in development.** Not yet usable. The frame hook is ported and
> building; the scanner, the menu toggle and the user interface are not written
> yet. See [`docs/design.md`](docs/design.md) for the agreed design.

## Planned usage

1. Start SnowRunner normally, from Steam, Epic or Xbox.
2. Run `SnowRunnerToolsMenu.exe`. It sits in the tray.
3. Press `HOME` in game to toggle the menu. The key is configurable.

Order does not matter -- the tool can be started before or during a session --
and closing it puts the game back exactly as it was found.

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

## Licence

GPL-3.0. Use it, fork it, improve it. It may not be folded into a closed-source
or paid product.

If it is useful to you, a donation is welcome but never required.
