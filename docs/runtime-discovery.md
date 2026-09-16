> **Provenance and scope.** This document is the reverse-engineering record
> carried over from the predecessor project. It is what makes this tool
> maintainable by somebody other than its author: when a game patch breaks the
> signatures, this is how the addresses get re-derived.
>
> **Every absolute address in this document is build-specific.** The RVAs,
> struct offsets and slot addresses below were derived on the Steam build
> `1.886173.SNOW_DLC_18` (`SnowRunner.exe`, SHA-256 `198b1079…`). They are
> recorded as evidence of *how* each address was found, not as constants to
> rely on. A different store or a later patch will move them. The signatures in
> the scanner are the durable artefact; these addresses are the working notes
> that produced them.

---

# Runtime discovery harness

The runtime discovery harness is a read-only research tool for comparing a
recognized SnowRunner build in Proving Grounds and offline campaign. It does
not expose a mutation command, call `WriteProcessMemory`, scan another process,
inspect saves, or walk private heap regions.

## Safety boundary

Live discovery first runs the build diagnostic. It continues only for the
exact supported fingerprint and automatically selected `SnowRunner.exe`
process. The reader requests only `PROCESS_VM_READ` and
`PROCESS_QUERY_LIMITED_INFORMATION`. It reads only PE sections selected by a
configured anchor's source and target section characteristics, caps any one
section read at 64 MiB, and rejects sections outside the diagnostic module
bounds.

Raw sections remain in memory. A saved observation contains module-relative
RVAs, validation outcomes, plausible scalar state, and at most
`maxCaptureBytesPerAnchor` bytes from a uniquely validated signature. It never
contains complete module regions or an absolute process address. This makes
the observation useful for comparison without turning it into a general
memory dump.

There is deliberately no experimental or mutating action in this milestone.
If mutation is added later, it belongs in a separate explicit experimental
entry point and must still pass the campaign session guard.

## Workflow

Start SnowRunner in Proving Grounds and capture an observation:

```powershell
.\SnowRunnerDiscovery.ps1 -Action Capture `
    -Context ProvingGrounds `
    -OutputPath .\logs\discovery-proving-grounds.json
```

Then load the same build and vehicle in an offline campaign and capture the
second observation:

```powershell
.\SnowRunnerDiscovery.ps1 -Action Capture `
    -Context OfflineCampaign `
    -OutputPath .\logs\discovery-campaign.json
```

Compare the two captures:

```powershell
.\SnowRunnerDiscovery.ps1 -Action Compare `
    -ProvingGroundsPath .\logs\discovery-proving-grounds.json `
    -CampaignPath .\logs\discovery-campaign.json
```

`Available` means exactly one signature candidate passed every configured
check. `Unavailable` covers an absent signature, an unconfigured signature, or
candidates that failed validation. `Ambiguous` means more than one candidate
passed validation; no address is selected in that case.

The committed production manifest leaves unvalidated runtime signatures unset.
Five current-build anchors are promoted exceptions. `ActiveVehicle` identifies
the TruckControl constructor, validates the constructor's call to a small
accessor, and then decodes the accessor's RIP-relative module singleton slot.
`VehicleTransform` identifies the truck world-transform accessor, validates its
call to the frame resolver, and then decodes the frame-chain offsets out of
those validated instruction bytes. `VehiclePlacement` identifies the truck
world-placement routine, validates the vector-growth helper it calls, and
decodes the truck offsets the routine itself reads. `VehiclePlacementCheck`
identifies the blocked-placement predicate and requires that the accessor it
calls is the same function `VehicleTransform` resolves independently.
`FrameTick` identifies the per-frame drain point by its own prologue and then
requires that the module-owned slot the window-message loop dispatches through
on every iteration holds exactly that candidate.

The decoded `ActiveVehicle` result is the TruckControl singleton *slot*, not the
truck itself. Current-build live switching confirmed that the singleton remains
stable while its pointer at `+0x8` changes to the selected truck. That
build-scoped field offset is declared beside the pointer validator and is passed
to the in-process module; the host must never substitute the constructor RVA for
the decoded slot RVA.

Historical byte sequences and offsets are not promoted until they have been
independently rediscovered and pass all of these checks on the installed build:

1. complete match within a selected PE section;
2. required read/write/execute section characteristics;
3. expected instruction or data shape;
4. every intermediate call target remains in a selected section and matches
   its expected shape;
5. a decoded module-relative pointer within a selected target section, or
   structural offsets decoded from validated instruction bytes;
6. plausible observed runtime state, including decoded offsets that stay
   within plausible ranges, agree where the same field is read twice, and
   hold any declared fixed relationship to each other;
7. for a dispatched anchor, a dispatch site that itself matches exactly once
   and a dispatch slot holding this candidate and no other;
8. exactly one validated candidate.

The capability report fails closed when any dependency is unavailable. It
covers in-process execution, vehicle spawning, free camera, teleportation,
active-vehicle access, cargo, repair/refuel, and time/weather.

## Fixture format

Synthetic or sanitized captured fixtures use schema version 1 and contain only
the minimum module regions needed for a test. Each region declares a name,
module-relative RVA, PE characteristics, and bytes as `hex` or `base64`.
Fixtures under `tests/fixtures/discovery` exercise unique, absent, malformed,
and ambiguous discoveries without reading a live process in the test suite.

## Prior art and current-build revalidation

Historical projects are useful as technique-level research leads only:

- [SnowRunner Noclip](https://github.com/FindMuck/SnowRunner_Noclip) documents
  active-vehicle pointer chasing and moving every rigid body in a connected
  Havok simulation island. Its mapping targets
  `1.148111.SNOW_DLC8`, not the installed build.
- [SnowFlyer2](https://github.com/NakedDevA/SnowFlyer2) demonstrates module
  pattern scans, RIP-relative address decoding, free-camera investigation, and
  time-of-day discovery. It also contains fixed offsets and memory patches;
  those values and mutation techniques are not imported here.
- [SMT](https://github.com/drafty46/SMT) demonstrates that reverse-engineered
  SnowRunner functions and structures can be reached through an injected hook.
  Injection and hook addresses are outside this read-only milestone.
- [SnowMap](https://github.com/ElAmine01/Minimap-For-SnowRunner-Source-Code)
  corroborates the usefulness of the TruckControl/active-truck chain and notes
  that protected runtime code makes static offsets brittle. Its offsets are
  likewise treated as untrusted.

On 2026-09-08, the harness prerequisite independently recognized the installed
Steam executable as `1.886173.SNOW_DLC_18`, build `25096372`, SHA-256
`198b10794b58b511cffdfd3fc0f6d5927903fccd1aa08f0e55dccc95d00a4a2b`.
The shipped `initial.cache_block` was rechecked and contains the semantic names
`FakeSpawnTruckAtCamera` and `inputFreeCamera` exactly once each; a bounded
read-only scan of the current executable module found neither literal string.
These names remain research anchors, not evidence that a retail action
dispatcher is callable.

The same live reader scanned only the current module's executable `.text` and
`.bind` sections (35,604,040 bytes total) for six exact legacy SnowFlyer2 byte
sequences. This was a presence check, not structural validation:

| Historical sequence | Raw matches on current build | Disposition |
| --- | ---: | --- |
| Dev-mode check A | 1 | Not promoted: object pointer and runtime state remain unvalidated. |
| Dev-mode check B | 1 | Not promoted: object pointer and runtime state remain unvalidated. |
| Free-camera check A | 0 | Unavailable. |
| Free-camera check B | 0 | Unavailable. |
| Time tick | 2 | Ambiguous. |
| Player coordinates | 1 | Not promoted: downstream call/LEA chain and state remain unvalidated. |

This is why raw uniqueness is insufficient: three sequences still occur once,
but none satisfies the complete validator contract required by the manifest.

Legacy SnowFlyer2 and Noclip offsets are not current facts. They remain absent
from `config/discovery-manifest.json` unless Proving Grounds/campaign captures
establish update-tolerant signatures and structural validators for this exact
fingerprint. The independently rediscovered TruckControl chain is configured;
the remaining dependencies still fail closed, so vehicle spawning and every
other mutation capability remain unavailable.

## Vehicle construction observations

On 2026-09-09, read-only observations of the same supported build found a
module-owned vehicle pointer vector at RVA `0x2AA1210`. Summer Proving Grounds
initially populated it with the default Chevrolet CK1500 and an Azov 64131.
Each of two user-triggered TOOLS-menu CK1500 spawns advanced the vector end by
exactly one pointer, producing four distinct vehicle objects. Switching into
the first spawned CK1500 also changed the active pointer reached through the
TruckControl singleton while leaving the singleton itself stable.

Static control-flow analysis traced the vector append to the vehicle object
constructor at RVA `0xD59150` and its sole direct caller, a higher-level
game-owned factory at RVA `0xD5AEE0`. Several loading and gameplay paths call
that factory. This proves normal construction and registration below the
Proving Grounds UI, but it does not yet identify which caller supplies the
TOOLS-menu truck definition and placement, nor prove a safe campaign call
contract. The production spawn adapter therefore remains disabled. The next
research step is to discriminate those factory callers during a controlled
TOOLS-menu spawn and then validate the matching path against normal campaign
garage deployment without writing process memory.

## Factory callers and construction contract

Continued read-only analysis on 2026-09-09 kept the same fingerprint, process
`SnowRunner.exe`, and Proving Grounds state described above, and confirmed the
four live truck objects still reachable through the vector at RVA `0x2AA1210`.
Reading each object showed three distinct Chevrolet CK1500 instances
(`g_scout_default`, `wheels_scout1`) and one Azov 64131 (`s_azov_64131`), each
carrying its own GUID. Two identical CK1500 spawns therefore produce two
separate objects, so `0xD5AEE0` runs per truck instance rather than once per
truck class.

The retail `.text` is encrypted at rest by the Steam stub, so code analysis
still has to read the running process. The exception directory in `.pdata` is
*not* encrypted, matches the live image byte for byte, and gives exact function
bounds. Using it instead of int3-padding heuristics corrected two enclosing
functions recorded earlier: the call at `0x9D7152` is in `0x9D69D0`, and the
call at `0xAE8BDF` is in `0xAE8B00`.

### The eight callers

Log-message strings referenced by each caller identify its role. None of these
is a UI layer; they are all game-owned paths into the same construction and
registration chain.

| Caller | Evidence | Role |
| --- | --- | --- |
| `0x9D69D0` | `Cant read tmTruck(%u)`, `dwTruckCRC`, `isMod` | Save deserialization of a stored truck. |
| `0xA64480` | `Info,Game\| Drive logic reloaded` | Development drive-logic reload. |
| `0xA98510` | `Failed to install truck %s addon %s`, `... cargo ...`, `... trailer ...` | Shared construct-with-addons path used by level population, save load, revive, and trailers. |
| `0xAE8B00` | `tractor parent`, `trailer parent` | Trailer/tractor parenting. |
| `0xB06740` | `UI_OLD_NEED_TO_POSITION`, `UI_OLD_POSITION_BLOCKED`, `game_uninstall_fuel` | Garage/store deployment and placement, the campaign comparison path. |
| `0xB159B0` | no strings; reached from the engine's script-binding table | Minimal script-invocable creation, the TOOLS-menu candidate. |

`0xB159B0` is the only caller reached from a binding table rather than from
another gameplay routine. It is called by `0xB39D30`, a script-call adapter
that validates argument count, and `0xB39D30` is registered at `0xB59A40` as
the first of 39 entries in a class binding table built at `0xB50890` and stored
at RVA `0x2A9C5E0`. Each entry holds a 32-bit name hash, a function thunk, an
access-check function, and a slot index; the entry that reaches truck creation
has name hash `0xF4F14179` and access check `0xB66370`. Entry names are stored
only as hashes, so the binding has not been named yet; hashing the identifiers
in the shipped `initial.cache_block` with FNV-1a, FNV-1, and CRC-32 produced no
match, and the engine's hash function has not been located. Until it is, "this
binding is the TOOLS-menu spawn" remains an inference from elimination, not a
validated fact.

### What `0xD5AEE0` actually does

The function is a truck loader, not a placement routine. It opens the truck
class XML and parses the complete definition — chassis, wheels, axles,
steering, damage areas, sounds, cameras, driver, and occlusion volumes — before
the constructor at `0xD59150` appends the finished object to the vector at
`0x2AA1210`. Its arguments are consistent at all eight call sites:

1. `CL` — `1` on the garage (`0xB06CB1`) and script (`0xB15A3B`) paths but
   `0` on save load (`0x9D7150`); it gates a large post-construction block at
   `0xD60E45`.
2. `DL` — a flag copied from the source description at `+0x158`
   (`+0x138` on the trailer path). Both live captures below observed `0`.
3. `R8` — a `std::string*` holding the truck name, built from the source
   description's inline name at `+0xD0`. On the script path it was
   `"default"`.
4. `R9` — a `std::string*` (not a `const char*`, as an earlier revision of
   this document said) holding the class name, e.g. `chevrolet_ck1500`. It is
   copied at `0xD5AEF2` and used to open the class XML; a bad value reaches
   `XML| Cant open class %s: Cant open document`. The caller keeps ownership.
5. a stack byte — `0` on the script path, `1` on the garage path in
   `0xB06740` (`0xB06C9A`), variable on save load, and `1` on the trailer path
   at `0xAE8BDF`; it gates a registration call at `0xD60E20`. An earlier
   revision claimed `0` on every truck path, which the garage call contradicts.

No argument carries a transform. Nothing in this function sets a spawn
position, so placement must come from a separate step that has not been
identified. That is the gap that still blocks the spawn adapter: the milestone
needs a validated placement and ownership contract, not only a construction
one, and the campaign path through `0xB06740` has not been compared against a
live garage deployment yet.

The production spawn adapter therefore remains disabled and every mutation
capability still fails closed.

## Truck placement path

The garage deployment caller `0xB06740` was traced on the same running process
and fingerprint. It answers the placement question left open above: placement
is a separate step from construction, and it is shared by every path that puts
a truck into the world.

### Installation is not placement

Immediately after the loader at `0xD5AEE0` returns the new truck, `0xB06740`
calls `0xAE8110` with the truck, a descriptor, and a result flag. That routine
has seven callers,
including the shared construct-with-addons path at `0xA98FCF` and the trailer
paths, so it is the common install-and-attach step. Its fourth argument is the
attach descriptor named by the serialization strings in `0x8C22E0`
(`descAttach.vPivotTractor`, `descAttach.strTractorFrame`): pairs of a pivot
`Vec3` and a frame name. Every observed caller fills both pivots with `FLT_MAX`,
so that value is the "no explicit pivot" sentinel. No caller passes a world
position here.

An earlier note in this document attributed `UI_OLD_POSITION_BLOCKED` to this
routine's failure branch. That was wrong and is corrected below: `0xAE8110`
returning false at `0xB06DB6` jumps to the trailer-attach diagnostic block at
`0xB07848` (`Game| Trailer %s be attached`, `Game| conflicts: %s`), and its own
strings are about addon sockets (`AddonSockets`, `Socket`, `Names`,
`Game| Can't find type '%s' in addons`). It is an attach predicate, not a
world-space placement test.

### `0xD65BF0` applies a world transform

The placement call is `0xD65BF0`, reached from every relevant path — garage
deployment (`0xB07280`), the shared construct-with-addons path (`0xA9908E`),
save load (`0x9D7268`), trailer parenting (`0xAE9191`), drive-logic reload
(`0xA64848`), the teleport gateway at `0xA67710`, and a script-facing zone or
locator API at `0x903830` whose diagnostics read `Argument 'truck' is null` and
`Failed to find zone with id '%s'`. The argument shape is identical at every
site:

1. `RCX` — the truck;
2. `RDX` — a 64-byte row-major 4x4 world matrix;
3. `R8` — an optional pointer, null on the garage and zone-locator paths;
4. `R9B` — a flag, `1` at every observed site;
5. a stack byte — `1` at every observed site.

The garage path copies its matrix straight out of a frame at `+0x50`, and
`0xA98510` instead composes one: it reads the parent's transform through
`0xD65AF0`, multiplies by a local offset matrix, and passes the product. That
is how attached trailers and addons inherit a parent's placement.

### Where a truck's transform actually lives

`0xD65AF0` resolves a truck's world transform through the truck's model object
rather than an inline field. Reading the four live Proving Grounds trucks
confirmed the chain end to end: `truck+0x60` gives the model object `X`;
`[X+0x60]` gives `Y`; `[Y+0x50]` gives `Z`; the `uint16` at `Z+0x60` is the root
frame index; `[X+0x68]` is the frame array with a `0xE0` stride; and the frame's
world matrix is at `+0x50`, row-major with the translation in row 3.

All four trucks produced orthonormal rotations and plausible translations —
three Chevrolet CK1500s within a few metres of each other on the same ground
plane at `y ≈ 31.8`, and the Azov 64131 further away and rotated roughly 180
degrees about `Y`. `0xD65AF0` composes that frame matrix with a truck-local
matrix at `truck+0x220` before returning.

Separately, `truck+0x914` holds a cached position `Vec3`. The constructor
initializes it to `(0, 0, 0)`, and the truck update function `0xD67C70` — the
one carrying `Ignition`, `ReverseSignals`, and `StopSignals` — recomputes it
each tick. It tracks the root frame translation with a small fixed offset, so it
is a convenient read-only position sample but not the authoritative transform
and not a placement input.

### The game's own blocked-placement predicate

The `UI_OLD_POSITION_BLOCKED` message is raised from a different place, and it
is the most useful thing found for safe-transform normalization so far. Inside
`0xB06740`, the call at `0xB07AB1` is `0xAE7EC0(truck, candidate)` returning a
byte; only when that byte is non-zero does the code fall through to `0xB07B25`,
look up the localized `UI_OLD_POSITION_BLOCKED` text, and place a world marker
at the blocker's bounding-box centre — the midpoint of the two `Vec3`s at
`candidate+0xCC` and `candidate+0xD8`, computed at `0xB07ABE`.

A correction from the 2026-09-10 disassembly of the call site: at `0xB07AB1`
the first argument is `rsi`, the same object `0xB06740` passes as the *first*
argument of `0xAE8110` at `0xB06DA9`, while the freshly loaded truck is that
call's *second* argument. The second argument of `0xAE7EC0` is the object
returned by `0xAEF3E0(rsi, ..., name, 1)` at `0xB07A9A`, not a transform. The
"truck, candidate" labels below are therefore unconfirmed, and the spawn spike
did not call this predicate.

`0xAE7EC0` reads as the game's own "is this truck's current placement blocked"
test:

1. it collects a truck-owned object list into a local vector through
   `0xD65440`;
2. it reads the truck's world transform through `0xD65AF0` — the same accessor
   the `VehicleTransform` anchor validates;
3. it derives a bounding volume for the candidate through `0xDCAD20` and
   `0xDC9750`, then calls `0xAD7F40` with that volume, a float tolerance from
   `.rdata`, and an output vector, which returns a list of nearby objects;
4. it filters that list by a class field (`(*(int*)(object+0x4C)) & 0x1F`) and
   skips objects carrying the tag `0x64` in the array at `object+0xB8`;
5. for each survivor it searches the per-truck object vectors at `+0x248` /
   `+0x250` of the trucks in the list from step 1, so the truck's own parts do
   not count as blockers;
6. it returns `1` on the first survivor that belongs to nobody in that list,
   and `0` when the loop completes.

Steps 1 and 3 through 6 are read from the instruction sequence; the intent
labels — "bounding volume", "spatial query", "tolerance" — are inference from
argument shape and control flow, not from strings or a called API name. None of
these functions has been called.

If that reading holds, the milestone's safe-transform requirement does not need
a hand-written terrain probe: the sequence would be place with `0xD65BF0`, ask
`0xAE7EC0`, and reject or adjust. That is a call, not a read, so it stays
outside the current boundary until the spawn seam is deliberately opened.

The predicate is now the fourth promoted manifest anchor,
`VehiclePlacementCheck`. Its signature covers the prologue and both leading
calls and is unique across `.text` and `.bind`; a second validated shape at
candidate offset `0x130` covers the ownership loop, so the offsets decoded from
it sit inside validated bytes rather than in unchecked memory. The strongest
check is the call target: the anchor requires that the function called at
candidate offset `0x5E` matches the 59-byte truck world-transform accessor
shape, and on a live run that target resolves to RVA `0xD65AF0` — the same
address `VehicleTransform` reaches from a completely different signature. Two
anchors now corroborate each other instead of each standing on its own byte
pattern. The decoded state is
`ModelObject=0x60;OwnedBegin=0x248;OwnedEnd=0x250`, where the owned-object
vector's end pointer must sit exactly one pointer after its begin pointer.

Like the others it validates code only, resolves no heap address, dereferences
no runtime object, and calls nothing. It is a declared dependency of
`VehicleSpawning` and `Teleportation`.

### Status

This identifies a construction, installation, and placement chain that the
game's own campaign paths share, which is what the spawn milestone needs.

The transform accessor is now the second promoted manifest anchor,
`VehicleTransform`, using a new `CallTargetStructure` pointer kind. It matches
the accessor's 59-byte signature, follows its call to the frame resolver,
requires that target to match a 115-byte shape whose offset bytes are
wildcarded, and then decodes seven structural offsets from those validated
bytes: the model-object offset, the frame-array offset, the skeleton pointer,
the root-frame-index offset, the frame stride, and the frame matrix offset. The
model-object offset is read from both the accessor and the resolver and the two
must agree. Because the offsets are decoded rather than hard-coded, a patch that
only moves a field is reported with its new value instead of silently reusing a
stale one. Both signatures were confirmed unique across `.text` and `.bind`, and
a live run resolves the anchor at RVA `0xD65AF0` with
`ModelObject=0x60;FrameArray=0x68;SkeletonPointer=0x50;RootFrameIndex=0x60;FrameStride=0xE0;FrameMatrix=0x50`.

The anchor validates code only. It resolves no heap address and dereferences no
runtime object, so it stays inside the read-only boundary described above. It is
a dependency of `Teleportation` and `ActiveVehicleAccess`, both of which remain
unavailable on their other dependencies.

The placement routine is now the third promoted manifest anchor,
`VehiclePlacement`, reusing the same `CallTargetStructure` pointer kind. It is a
different validation problem from the transform accessor: the world matrix, the
optional pointer and both flags are supplied by the caller, so nothing about the
interesting arguments is encoded in the callee and none of it can be validated
from the routine's own bytes. What the anchor validates instead is the routine's
structure. It matches a 144-byte signature over the prologue and the first
attached-object loop setup, wildcarding only the bytes it decodes and the one
call displacement; follows that call to the vector-growth helper at `0xB476B0`
and requires a 75-byte shape ending in the `0x1FFFFFFFFFFFFFFF` maximum-size
constant; and then decodes three truck offsets out of the validated instruction
bytes: the model-object offset the routine reads at `truck+0x60`, and the begin
and end pointers of the attached-object vector at `truck+0x1E8` and
`truck+0x1F0`.

Two of those decoded values check each other. The model-object offset must land
in the same plausible range as, and on this build equals, the `0x60` that
`VehicleTransform` decodes independently from a different function. The
attached-object vector's end pointer must sit exactly one pointer after its
begin pointer, expressed as a declared `mustEqualOffset` relationship rather
than as a hard-coded pair of offsets. The signature was confirmed unique across
`.text` and `.bind` — one match in `.text`, none in `.bind` — and a live run
resolves the anchor at RVA `0xD65BF0` with
`ModelObject=0x60;AttachedBegin=0x1E8;AttachedEnd=0x1F0`.

Like the other two, this anchor validates code only. It resolves no heap
address, dereferences no runtime object, and nothing has been called. It is a
declared dependency of `VehicleSpawning` and `Teleportation`, both of which
remain unavailable on their other dependencies.

`0xAE8110`, the shared install-and-attach step, still has no signature,
structural validator, or fixture. Safe-transform normalization is understood in
outline and its predicate is anchored, but no normalization step has been built,
because using the predicate means calling it.

That is the honest state of this milestone: the construction, placement and
blocked-placement functions are all identified and validated, and none of them
can be reached. The harness holds `PROCESS_VM_READ` and nothing else, so a
resolved address is currently evidence, not a capability.

## Live call capture and spawn spike (2026-09-10)

A disposable, user-attested spike ran against a live offline campaign
(process 5316, same fingerprint) to find out whether the chain above spawns a
real truck. The user's confirmation that the session was an offline campaign
stood in for the still-`Unknown` production session detector; the spike code
is not part of the product and none of it is wired into the CLI.

**Mechanism.** The host DLL loaded, ran `Ping` and `GetActiveVehicleTransform`
from the `FrameTick` hook, and unloaded with the hook bytes verified. The
in-process transform agreed with the external frame-chain reading to within
`4.4e-5`, which also fixes the composition order: `0xD65AF0` returns
`local(truck+0x220) × frame`, row-major.

**What a garage deployment actually calls.** Pass-through recording hooks on
`0xD5AEE0`, `0xAE8110` and `0xD65BF0` captured an ordinary garage deployment.
It did not go through `0xB06740`:

| Order | Function | Caller | Arguments observed |
| --- | --- | --- | --- |
| 1 | loader `0xD5AEE0` | `0xB15A42` in script binding `0xB159B0` | `(1, 0, &"default", &"international_fleetstar_f2070a", 0)` — a garage preview, destroyed shortly after |
| 2 | placement `0xD65BF0` | `0x903CFA` in zone API `0x903830` | preview truck at `y ≈ -244` (the garage interior) |
| 3 | loader `0xD5AEE0` | `0xB15A42` | `(1, 0, &"default", &"chevrolet_ck1500", 0)` |
| 4 | placement `0xD65BF0` | `0x903CFA` | `(truck, matrix, nullptr, 1, 1)`, still inside the garage |
| 5 | placement `0xD65BF0` | `0xA7E65E` | same truck to the world spot outside the garage, `(truck, matrix, r12, 1, 0)` |

All calls ran on the process's oldest thread, the same one that runs
`FrameTick`. `0xAE8110` was never called, so install-and-attach is not part of
a plain truck deployment. `0xB159B0` is therefore a small, script-callable
creation entry point taking a class-name `std::string*` and an out handle.

**Spawn.** Calling `0xD5AEE0(1, 0, &"default", &"chevrolet_ck1500", 0)` and then
`0xD65BF0(truck, matrix, nullptr, 1, 1)` from the `FrameTick` hook produced one
CK1500 eight metres in front of the active truck (truck-local X is forward).
The truck vector at `0x2AA1210` grew by one with the new truck last, the user
saw it, switched to it, and drove it. Two gaps remain:

- the truck hovered at the requested height until the user switched to it —
  its physics stay asleep after `0xD65BF0` until something wakes them;
- it had no fuel.

**Fuel.** The fuel-station refuel routine (message reference at `0xA5CFE5`)
reads current fuel at `model+0x5E8` and capacity at `model+0x5F0`, where
`model = [truck+0x60]` (the getter `0xD629F0` is just that load), and fills the
tank with `0xC4B720(model, capacity)`. That setter clamps to `[0, capacity]`,
stores the value, and updates the fuel-mass bodies at `model+0x380`; it touches
no UI. A second spawn that called it after placement reported
`before=0 capacity=80 after=80` and stayed at 80 over the following seconds.
The garage-deployed CK1500 read `73.8 / 80` after being driven.

None of this weakens the production guard: every mutating capability in the
product remains fail-closed.

## The Proving Grounds tools menu (2026-09-10)

Issue [#19](https://github.com/hrobarikt/snowrunner-mod/issues/19) asked how
the built-in tools menu could be brought into campaign (ADR-0002). Static
analysis of the live image, then a pass-through recording probe (disposable,
`.scratch/probe19/`, same technique as step B) while the user opened the menu
in Proving Grounds, chose Create, placed, rotated and confirmed a truck.
Internally, Proving Grounds is "polygon".

**Objects.** The menu is `combine::POLYGON_MENU`, held in the module's core
system table at RVA `0x2A8EB40` (accessor `0x9DD9E0`), next to `GAME_LOGIC`
(`0x2A8EB48`), `RENDERER`, `HAVOK` and the other system singletons. Its `+8`
is a `combine::DIALOG_POLYGON` whose entries are `UI_POLYGON_ADD` (Create),
`RELOAD`, `INFO`, `GARAGE`, `FREE_CAMERA`, `CARGOES`, `REFILL`, `NIGHT_DAY`,
`ADD_MOD` and `MOD_MANADGER_DIALOG`. Create builds `combine::DIALOG_ADD_TRUCK`.

| RVA | Role |
| --- | --- |
| `0xA994E0` | `GAME_LOGIC` method: if `[GAME_LOGIC+8]` (`TERRAIN`) is set and the slot is empty, allocate `POLYGON_MENU` and its dialog (ctor `0xAA2580`). No mode check. |
| `0xA995D0` | Same with the alternate dialog ctor `0xAA3B10`; not used by Proving Grounds. |
| `0xA995A0` | Destroys the menu through its virtual deleting destructor; the destructor `0xA9A9F0` clears the slot. |
| `0x887F20` / `0x887F50` / `0x887F80` | Set a flag at `+0xC8` and call create / destroy / alternate on `GAME_LOGIC`. |
| `0xA1B8A0` / `0xA1B370` / `0xA1B760` | Script-binding thunks for those three, registered at `0x98FC78`–`0x98FCBE` under name hashes `0xA7575EF9` (create), `0x643534E7` (destroy), `0x2BD77B46` (alternate). |
| `0xA9ACE0`, `0xA9AC60` | Per-frame menu update and input, called from the game update `0xA90450` and from `FrameTick` whenever the slot is non-null. |
| `0xAB3E00` | `DIALOG_POLYGON` entry dispatch; builds `DIALOG_ADD_TRUCK` (ctor `0xAA17C0`). |
| `0xA68210` | "Place truck" helper `(flag, truck, &matrix, useMatrix)`: takes `TERRAIN+0x20` from `GAME_LOGIC` as the world argument, calls `0xD65BF0`, then optionally `0xD66720`. Called by level load `0xA98694` and by script placement bindings (`0xB34C70`, `0xB35040`). |

`isEnableDevMenu` is only referenced by a static reflection-name registration;
it is not the gate.

**Recorded chain (Proving Grounds).**

| Order | Thread | Call | Caller | Notes |
| --- | --- | --- | --- | --- |
| 1–6 | worker | loader ×4, placement ×2 | `0xA98626`, `0xA98EC6`, `0xA68327` | Level load spawning the map's own trucks. |
| 7 | worker | `0xA994E0(GAME_LOGIC)` | `0xA1B905` (script binding) | The level script turns the menu on. |
| 8 | main | `0xAB3E00(dialog, 0x101, 1, …)` | `0xDE619A` | Create chosen. |
| 9 | main | `0xAA17C0` | `0xAB3E6F` | Vehicle list opens. |
| 10 | main | loader `0xD5AEE0(1, 0, &"default", &"chevrolet_ck1500", …)` | `0xB15A42` | Only at confirm; the same script entry `0xB159B0` the garage uses. |
| 11 | main | placement `0xD65BF0(truck, matrix, TERRAIN+0x20, 1, 1)` | `0xA68327` in `0xA68210` | Yaw-only matrix at the clicked spot. |

"Main" is the process's oldest thread, the one that runs `FrameTick`.
Nothing was recorded while the ghost moved and rotated, so the ghost, ground
snap and rotation load no truck. `VehiclePlacementCheck` (`0xAE7EC0`),
install (`0xAE8110`) and the menu-open routine `0xAAAF10` were not called.
The third placement argument, which the #14 spike passed as null, is
`TERRAIN+0x20`. The probe unloaded with all nine prologues verified original.

**Campaign.** In an offline campaign the `POLYGON_MENU` slot is empty while
`GAME_LOGIC`, `TERRAIN` and the `+0x2A8` vector (9 entries) are all present,
so every prerequisite of `0xA994E0` exists. The menu is absent because the
campaign script never asks for it, not because a system is missing.

**Unlock.** A disposable, user-attested module (`.scratch/unlock/`) called
`0xA994E0(GAME_LOGIC)` once from the `FrameTick` hook on the main thread in
that campaign. It returned without an exception and filled the slot with a
real `POLYGON_MENU` and `DIALOG_POLYGON`. The user then used the menu by hand:
it stays on screen and can only be minimised, as in Proving Grounds. Every
tool worked except free camera, whose checkbox ticks without effect
([#21](https://github.com/hrobarikt/snowrunner-mod/issues/21)). Create worked
fully: ghost, click to move, drag to rotate, and a fuelled truck that spawned
normally. A map reload during the test removed the menu through the game's own
teardown. On stop the frame hook was restored and verified, and the module
unloaded. Productizing this is
[#20](https://github.com/hrobarikt/snowrunner-mod/issues/20).

**Anchors (2026-09-14, [#20](https://github.com/hrobarikt/snowrunner-mod/issues/20)).**
The unlock is now a production command, and the five addresses it needs are
manifest anchors validated on the live image like every other one. None of them
is a hard-coded constant: each slot is decoded out of the accessor call the
routine itself makes.

| Anchor | RVA | What its signature covers | What it resolves |
| --- | --- | --- | --- |
| `ToolsMenuEnable` | `0x887F20` | the gate write `mov byte [rcx+0xC8], 1`, the accessor call, the tail jump | `GAME_LOGIC` slot `0x2A8EB48` |
| `ToolsMenuDisable` | `0x887F50` | the same shape writing `0`, which is all that separates the two signatures | `GAME_LOGIC` slot `0x2A8EB48` |
| `ToolsMenuCreate` | `0xA994E0` | the `TERRAIN` test at `GAME_LOGIC+8` and the empty-slot test | `POLYGON_MENU` slot `0x2A8EB40` |
| `ToolsMenuDestroy` | `0xA995A0` | the empty-slot early return and the virtual deleting destructor call | `POLYGON_MENU` slot `0x2A8EB40` |
| `ToolsMenuGate` | `0x855CF3` | the `FrameTick` free camera gate, including the `+0xC8` literal and both short branches, which is what tells it apart from the twin gate at `0x86122A` | system-object slot `0x2A54160` |

All five resolve as `ValidatedUniqueCandidate` on the supported build, and the
`GAME_LOGIC` and system-object slots they decode match the ones the session
detector recorded independently in `config/supported-builds.json`, which the
host checks before it hands any of them to the module. The host
then cross-checks the set before any of it reaches the module: both bindings
must resolve the same `GAME_LOGIC` slot, create and destroy the same
`POLYGON_MENU` slot, the two slots must differ, and each binding's tail jump -
decoded from the bytes discovery captured at that binding - must land on the
routine its anchor names. That last check is what makes the create and destroy
anchors load-bearing, since the module calls neither of them directly.

## Free camera from the tools menu (2026-09-11)

Issue [#21](https://github.com/hrobarikt/snowrunner-mod/issues/21) asked why
the menu's free camera checkbox does nothing in campaign. Read-only static
analysis of the live image (same build), with the unlocked menu present in an
offline campaign. No recording probe has run yet.

**Checkbox chain.** `DIALOG_POLYGON` entry dispatch `0xAB3E00` handles
checkbox changes as event `0x401`, keyed by control id. The dialog ctor labels
control 6 `UI_POLYGON_FREE_CAMERA` (at `0xAA3153`) and control 4 the garage.

| Id | Handler | Effect |
| --- | --- | --- |
| 3 | inline | stores the checked state at `menu+0x44` |
| 4 | `0xAAAF10` (on) / virtual delete (off) | builds `TRUCK_GARAGE` with `DIALOG_GARAGE_ADDONS` (ctor `0xAE34F0`) into the singleton at `0x2A90728`; `0xA9AC60` re-creates it every frame while ticked |
| 6 | `0x887B60(checked)` | writes the free camera flag, a global dword at `0x2A5412C`, and nothing else |

`0xAAAF10`, called "MenuOpen" on #19, is therefore the garage, not a menu
open routine.

**Flag readers.** `0x2A5412C` is read at `0x855D15` (in `FrameTick`
`0x855080`), `0x85F30D`, `0x8612D6`, `0x8844A6` and getter `0x887B3F`. The
`FrameTick` block moves the camera from its matrix when the flag is set, but
it is reached only past this gate:

```text
if [0x29A2018] != 0 && byte [0x2A544B8] == 0
   && (sys == null || byte [sys+0xC8] == 0)   ; sys = [0x2A54160]
   && !0x9EF1F0()
    skip the free camera block (jump to 0x855F69)
```

The camera function around `0x86122A` has the same `sys+0xC8` /
`0x9EF1F0` gate in front of its own read at `0x8612D6`.

**The missing piece (hypothesis).** `sys+0xC8` is set only by `0x887F20`,
the body of the Proving Grounds script binding (thunk `0xA1B8A0`), which
writes `1` and then calls menu create `0xA994E0`. `0x887F50` clears it and
calls destroy. The #19 unlock calls `0xA994E0` directly, so in campaign the
flag stays `0`. Live campaign values: `[0x29A2018] = 1`,
`[0x2A544B8] = 0`, `sys+0xC8 = 0`. A scan for `+0xC8` accesses near the
118 loads of `0x2A54160` found only the two free camera gates
(`0x855CFF`, `0x86122A`). The scan only checked code shortly after each
load, so it may have missed others.

**Proving Grounds, same process.** After the user loaded Proving Grounds,
`[0x2A54160]` held the same object as in campaign and `sys+0xC8 = 1`, with
`[0x29A2018] = 1` and `[0x2A544B8] = 0` as before. Of the values in the gate,
only `sys+0xC8` differs between the two modes. When the user ticked free
camera, the flag at `0x2A5412C` changed from `0` to `1` and the garage
singleton stayed empty, which confirms that control 6 writes the flag.

**Campaign test (verdict: enable through the game's own path).** A
disposable, user-attested module (`.scratch/freecam/`) called `0x887F20(sys)`
once from the `FrameTick` hook on the main thread, in place of
`0xA994E0(GAME_LOGIC)`. The recorded result was `status=0`: `gate 0 → 1`,
a new `POLYGON_MENU`, no exception, and the game kept responding. The user
then ticked free camera, and it worked in the offline campaign as it does in
Proving Grounds. Its off path is `0x887F50(sys)`, which clears the gate
before calling destroy. Destroy does nothing when the slot is already empty,
so this path also resets the gate after a map reload has removed the menu.

Other observations:

- The gate read `0` again in campaign after a Proving Grounds visit, so
  something clears it between levels.
- The tools trainer holding the `FrameTick` hook makes discovery report the
  anchor `Unavailable`. Unload the trainer through its
  `Local\snowrunner-trainer-stop-<pid>` event before running another module.

For [#20](https://github.com/hrobarikt/snowrunner-mod/issues/20): enable the
menu through `0x887F20(sys)` and disable it through `0x887F50(sys)`, not
through the bare create and destroy calls.

## The execution mechanism, and what it still needs from discovery

Closing that gap is a design decision about the tool's shape rather than more
discovery work, and it has been made: see
[ADR-0001](adr/0001-in-process-execution-mechanism.md). The trainer loads a
small module into the running game on explicit request, that module hooks one
validated per-frame function and drains a command queue from inside the hook, so
queued commands run on the game's own thread at a safe point. Nothing is placed
in the install directory and detach restores every patched byte.

The decision handed discovery one new job: the command queue needs a drain
point, and that drain point is the fail-closed manifest anchor `FrameTick`. It
is now validated on the installed build; the work is recorded below.

Two further requirements fall on whichever anchor is chosen, and they are
discovery's responsibility rather than the module's, because the module contains
no length disassembler:

1. the declared `hook.stealBytes` must cover whole instructions;
2. those instructions must be position independent — no RIP-relative operand and
   no relative branch inside the stolen region — because they are copied
   verbatim into a trampoline elsewhere in the address space.

## The per-frame drain point

On 2026-09-09, read-only analysis of the same supported build and process
promoted `FrameTick` at RVA `0x855080`. The route to it started at `0xD67C70`,
the per-truck update tick recorded above, and climbed its callers using the
unencrypted `.pdata` exception directory for exact function bounds:

| RVA | Evidence | Role |
| --- | --- | --- |
| `0xD67C70` | — | Per-truck update tick; sole caller below. |
| `0xA90450` | `Terrain Process %f seconds`, `mr2/statistics/loading` | Game update; calls the truck tick once, not in a loop. |
| `0x855080` | `ui/splash_screen`, `Info,Game\| Scripts reloaded`, camera debug format | Registered per-frame game update. Sole caller of `0xA90450`. |
| `0x11684C0` | `Message WM_QUIT was received. Shutting down all systems.` | The window-message loop itself: it peeks messages, compares against `0x12` (`WM_QUIT`) and branches back until it arrives. |

`0x855080` has no direct callers. It is reached exactly once per loop iteration
through a module-owned function-pointer slot at RVA `0x2AB7448`, which the loop
calls at `0x11685D3` as `call qword ptr [rip + 0x194EE6F]`. That slot is the
only reference to `0x855080` anywhere in the module, and the loop is the only
code that calls through it.

That dispatch, not the prologue, is what makes the site per-frame, so it is what
the anchor validates. The signature is the drain point's own prologue — so the
resolved RVA is the site the hook patches and the captured sample is the bytes
the module refuses to patch without — and the pointer validator finds the
dispatching call site, requires it to match exactly once, decodes the slot out
of its instruction bytes rather than hard-coding an offset, and requires the
slot to hold this candidate and no other. The absolute address read from the
slot is converted straight back to an RVA, so no process address reaches a check
detail or a saved observation.

The slot is written during startup, so the anchor does not validate until the
game has installed its per-frame update. That fails in the right direction: a
capture taken too early reports `InProcessExecution` as `Unavailable` and the
host refuses to load rather than hooking a site nothing dispatches to yet.

The declared steal length is 19 bytes: `mov rax, rsp`, five register pushes and
`lea rbp, [rax - 0x228]`. Those are three whole instructions with no
RIP-relative operand and no relative branch, so they can be copied verbatim into
a trampoline. The `lea` reads the entry `rsp` that `mov rax, rsp` captured, which
holds because the patch is a jump rather than a call and the detour restores
`rsp` exactly before handing control to the trampoline.

`0xD67C70` was the earlier candidate and is not used: it runs once per truck per
frame, so a queue drained from it would run a command as many times per frame as
there are trucks unless the drain were idempotent. It is reached from
`0x855080` through the game update, which is the granularity the queue wants.

`InProcessExecution` now reports `Available` on the installed build. The host
still refuses to load, because the committed adapter is unconfigured and the
native module ships unbuilt; the read-only transform proof call therefore has
not run. The spawn adapter stays unconfigured and every mutation capability
still fails closed.

## Offline campaign session detection (2026-09-13)

Issue [#17](https://github.com/hrobarikt/snowrunner-mod/issues/17) replaced the
fail-closed `Unknown` detector. The work was read-only snapshot diffing of the
module `.data` section and the heads of a few known objects, on the supported
build, while the user moved the game through each mode. Nothing was loaded into
the game and nothing was written.

**What was ruled out.** The exe has no readable "campaign" string, and its code
is encrypted on disk (SteamStub `.bind`), so code has to be read from the live
image. `sys+0xC8` and the `POLYGON_MENU` slot both differ between Proving
Grounds and campaign, but the tools trainer sets both in campaign. The map sky
and night presets are shared by custom scenarios built on campaign maps. A
`.data` dword at `0x2A93530` and another at `0x2A54158` looked like mode
markers until they stayed changed after returning to campaign.

**Fields.** The session layout in `config/supported-builds.json` declares them
for this build only:

| Field | Where | Meaning |
| --- | --- | --- |
| Level id | `sys = [0x2A54160]`; u32 length at `sys+0x10`, text at `sys+0x18` inline below 16 characters, else a pointer there | The loaded level's id |
| Terrain | `[GAME_LOGIC+0x8]`, `GAME_LOGIC = [0x2A8EB48]` | Non-null once a level's terrain exists |
| Campaign flag | u8 `GAME_LOGIC+0x614` | `1` only while a campaign world is loaded |
| Mode value | u32 `GAME_LOGIC+0x618` | See below |
| Active truck | `[[0x2A8EB78]+0x8]` | The `ActiveVehicle` singleton slot; non-null once the player's truck exists |
| Load counter | u32 `0x2AA0D90` | Rises by one on every level load, the main menu included |

**Observed values.**

| State | Level id | Campaign flag | Mode value |
| --- | --- | --- | --- |
| Offline campaign (four maps, before and after other modes) | `level_us_02_01`, `level_ru_02_02`, … | 1 | 0 |
| Hosted co-op, same save | `level_us_02_01` | 1 | 3 |
| Proving Grounds | `level_ru_test_polygon` | 0 | 5 |
| mod.io custom scenario on a campaign map | `1141814` | 0 | 0 |
| Main menu | `level_main_menu_us18` | 0 | 1 |
| Loading | empty, terrain null | — | 1 or 0 |

The campaign flag is one byte. The three bytes above it are struct padding:
they read as zero while the field was being found, so it was first recorded as
a u32, but a long-running process leaves recycled bytes there (`01 5C 70 72`
was seen live in a loaded campaign on 2026-09-15), which made the dword read
report `Unknown` and blocked the tools menu. Only the low byte is the flag.

Hosted co-op is indistinguishable from offline campaign by level id and
campaign flag. Only the mode value separates it, and it returned to `0` when the
same save was loaded offline again.

**Classification.** An offline campaign requires all of: a level id on the
campaign allowlist (exact, case-sensitive), campaign flag `1`, mode value `0`,
a non-null terrain, and, for `WorldLoaded`, a non-null active truck. The
allowlist is the 53 `level_(us|ru)_NN_NN[_suffix]` level archives installed
under `preload/paks/client`, so trials (`level_trial_*`), Proving Grounds
(`*_test_polygon`), the main menu and any level not installed fail closed.
Mode value `3` is always a networked session. **Joining co-op as a client was
not observed**; every mode value other than `0`, `1`, `3` and `5` is therefore
`Unknown`, which fails closed, but a client session that reads `0` would not be
caught.

**Why fixed per-build fields rather than signatures.** Unlike the discovery
anchors, the session layout is a set of RVAs and offsets bound to one exact
executable SHA-256; any other build has no layout and reports `Unknown` without
a read. The fields are data slots and structure offsets, not code, and they were
validated by behaviour across modes rather than by a byte pattern, which a
signature could not express. Pointers are only checked for being non-null and
the level id length for plausibility, so a wrong layout can at worst read
nonsense, which the exact-match allowlist and mode values then refuse. A new
build needs the same live walk through the modes before a layout is added.

**Context id.** `processId:loadCounter:levelId:terrain`. The terrain address is
not enough on its own: the game reused one address across a map change. The
load counter rose on every one of ten recorded loads, including a reload of the
same map, and did not change within a load.
