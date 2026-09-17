#include "tools_menu_scan.h"

#include "pattern.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace srtm {
namespace {

// The signatures, from docs/runtime-discovery.md. Two struct offsets are used
// alongside them, and both are literals inside the signatures themselves, so a
// build that moved either one fails to match instead of resolving wrongly:
// the mode gate 0xC8 appears in the enable, disable and gate signatures, and
// TERRAIN at +8 in the create signature.
constexpr uint32_t kGateOffset = 0xC8;
constexpr uint32_t kTerrainOffset = 0x8;

// The drain point's prologue. Its first 19 bytes are three whole instructions,
// none RIP-relative or a relative branch, so they can be copied verbatim into a
// trampoline; the hook needs no length disassembler.
constexpr const char* kFrameTick =
    "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 D8 FD FF FF 48 81 EC 00 03 00 00 "
    "48 C7 85 A8 00 00 00 FE FF FF FF";
constexpr uint32_t kFrameTickStealBytes = 19;

// The window-message loop calls one module-owned slot every iteration. That
// slot holding FrameTick is what proves FrameTick runs once per frame; the
// prologue alone proves nothing about when it runs.
constexpr const char* kFrameDispatch =
    "4C 89 65 F0 4C 89 6D 08 48 8D 4D F0 E8 ?? ?? ?? ?? 45 33 C0 F3 0F 10 0D ?? ?? ?? ?? "
    "F2 0F 10 05 ?? ?? ?? ?? FF 15 ?? ?? ?? ??";
constexpr uint32_t kFrameDispatchCallOffset = 36;

// The script binding that turns the menu on: write 1 to the gate, fetch the
// GAME_LOGIC slot through its accessor, tail-jump to menu create.
constexpr const char* kEnable =
    "48 83 EC 28 C6 81 C8 00 00 00 01 E8 ?? ?? ?? ?? 48 8B 08 48 83 C4 28 E9 ?? ?? ?? ??";
// Its other half: write 0 and tail-jump to menu destroy.
constexpr const char* kDisable =
    "48 83 EC 28 C6 81 C8 00 00 00 00 E8 ?? ?? ?? ?? 48 8B 08 48 83 C4 28 E9 ?? ?? ?? ??";
constexpr uint32_t kBindingAccessorCall = 11;
constexpr uint32_t kBindingTailJump = 23;

// The GAME_LOGIC method that allocates POLYGON_MENU, covering its own TERRAIN
// and empty-slot preconditions and the call to the slot's accessor.
constexpr const char* kCreate =
    "40 55 56 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 8B F9 "
    "48 83 79 08 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F0 48 83 38 00 75 ??";
constexpr uint32_t kCreateAccessorCall = 36;

// The teardown, which reads the same slot and does nothing when it is empty.
constexpr const char* kDestroy =
    "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B 08 48 85 C9 74 0F 48 8B 01 BA 01 00 00 00 "
    "48 83 C4 28 48 FF 20 48 83 C4 28 C3";
constexpr uint32_t kDestroyAccessorCall = 4;

// The per-frame free camera gate: load the system object from its slot, skip
// unless +0xC8 is set. The camera routine holds the same gate with longer
// branch encodings, which is why the two short branches are part of this.
constexpr const char* kGate =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 80 B8 C8 00 00 00 00 75 0D E8 ?? ?? ?? ?? "
    "84 C0 0F 84 ?? ?? ?? ??";

// A singleton accessor: lea rax, [rip + slot]; ret.
constexpr const char* kAccessor = "48 8D 05 ?? ?? ?? ?? C3";

int32_t ReadInt32(const uint8_t* bytes) {
    int32_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

// Target of a relative displacement, or nullopt if it leaves the 32-bit RVA
// space the image lives in.
std::optional<uint32_t> RelativeTarget(uint32_t next_instruction, int32_t displacement) {
    const int64_t target = static_cast<int64_t>(next_instruction) + displacement;
    if (target < 0 || target > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(target);
}

std::string Hex(uint64_t value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

class Scanner {
public:
    explicit Scanner(const Image& image) : image_(image) {}

    // Finds a signature and requires exactly one match. The report records the
    // outcome either way.
    std::optional<uint32_t> Find(const char* name, const char* text, AnchorReport* report) {
        report->name = name;
        const std::optional<Pattern> pattern = ParsePattern(text);
        if (!pattern) {
            return Fail(report, "signature does not parse");
        }
        const std::vector<uint32_t> matches = FindAll(image_, *pattern, 2);
        report->matches = static_cast<uint32_t>(matches.size());
        if (matches.empty()) {
            report->status = AnchorStatus::NotFound;
            return std::nullopt;
        }
        if (matches.size() > 1) {
            // Two matches is as much a failure as none: picking one would mean
            // calling code nobody verified.
            report->status = AnchorStatus::Ambiguous;
            report->detail = "matches at " + Hex(matches[0]) + " and " + Hex(matches[1]);
            return std::nullopt;
        }
        report->status = AnchorStatus::Resolved;
        report->rva = matches[0];
        return matches[0];
    }

    // Follows `call accessor` at `site + offset` into a `lea rax, [slot]; ret`
    // accessor and returns the slot, which must be writable data.
    std::optional<uint32_t> AccessorSlot(uint32_t site, uint32_t offset, AnchorReport* report) {
        const uint8_t* call = image_.At(site + offset, 5);
        if (call == nullptr || call[0] != 0xE8) {
            return Fail(report, "no call at +" + Hex(offset));
        }
        const std::optional<uint32_t> accessor =
            RelativeTarget(site + offset + 5, ReadInt32(call + 1));
        const std::optional<Pattern> shape = ParsePattern(kAccessor);
        if (!accessor || !image_.InExecutable(*accessor, shape->Length()) ||
            !shape->Matches(image_.At(*accessor, shape->Length()))) {
            return Fail(report, "call at +" + Hex(offset) + " does not reach a slot accessor");
        }
        const std::optional<uint32_t> slot =
            RelativeTarget(*accessor + 7, ReadInt32(image_.At(*accessor + 3, 4)));
        if (!slot || !image_.InWritable(*slot, sizeof(uint64_t))) {
            return Fail(report, "accessor slot is not in writable data");
        }
        return slot;
    }

    // Target of the `jmp rel32` at `site + offset`.
    std::optional<uint32_t> TailJump(uint32_t site, uint32_t offset, AnchorReport* report) {
        const uint8_t* jump = image_.At(site + offset, 5);
        if (jump == nullptr || jump[0] != 0xE9) {
            return Fail(report, "no tail jump at +" + Hex(offset));
        }
        return RelativeTarget(site + offset + 5, ReadInt32(jump + 1));
    }

    // Slot addressed by the `mov rax, [rip + slot]` at `site`.
    std::optional<uint32_t> RipSlot(uint32_t site, AnchorReport* report) {
        const std::optional<uint32_t> slot =
            RelativeTarget(site + 7, ReadInt32(image_.At(site + 3, 4)));
        if (!slot || !image_.InWritable(*slot, sizeof(uint64_t))) {
            return Fail(report, "slot is not in writable data");
        }
        return slot;
    }

    // Slot called through by the `call [rip + slot]` at `site + offset`.
    std::optional<uint32_t> IndirectCallSlot(uint32_t site, uint32_t offset, AnchorReport* report) {
        const uint8_t* call = image_.At(site + offset, 6);
        if (call == nullptr || call[0] != 0xFF || call[1] != 0x15) {
            return Fail(report, "no indirect call at +" + Hex(offset));
        }
        const std::optional<uint32_t> slot =
            RelativeTarget(site + offset + 6, ReadInt32(call + 2));
        if (!slot || !image_.InWritable(*slot, sizeof(uint64_t))) {
            return Fail(report, "dispatch slot is not in writable data");
        }
        return slot;
    }

    bool Sample(uint32_t rva, uint32_t length, uint8_t* destination) const {
        const uint8_t* bytes = image_.At(rva, length);
        if (bytes == nullptr || length > kMaxSampleBytes) {
            return false;
        }
        memcpy(destination, bytes, length);
        return true;
    }

private:
    static std::nullopt_t Fail(AnchorReport* report, std::string detail) {
        report->status = AnchorStatus::DecodeFailed;
        report->detail = std::move(detail);
        return std::nullopt;
    }

    const Image& image_;
};

const char* StatusText(AnchorStatus status) {
    switch (status) {
        case AnchorStatus::Resolved: return "ok";
        case AnchorStatus::NotFound: return "NOT FOUND";
        case AnchorStatus::Ambiguous: return "AMBIGUOUS";
        case AnchorStatus::DecodeFailed: return "DECODE FAILED";
    }
    return "?";
}

}  // namespace

ScanReport ScanToolsMenu(const Image& image) {
    ScanReport report;
    Scanner scanner(image);

    AnchorReport frame_tick, dispatch, enable, disable, create, destroy, gate;

    const auto frame_tick_rva = scanner.Find("FrameTick", kFrameTick, &frame_tick);
    const auto dispatch_rva = scanner.Find("FrameDispatch", kFrameDispatch, &dispatch);
    std::optional<uint32_t> dispatch_slot;
    if (dispatch_rva) {
        dispatch_slot = scanner.IndirectCallSlot(*dispatch_rva, kFrameDispatchCallOffset, &dispatch);
    }

    const auto enable_rva = scanner.Find("ToolsMenuEnable", kEnable, &enable);
    std::optional<uint32_t> enable_game_logic, enable_tail;
    if (enable_rva) {
        enable_game_logic = scanner.AccessorSlot(*enable_rva, kBindingAccessorCall, &enable);
        if (enable_game_logic) {
            enable_tail = scanner.TailJump(*enable_rva, kBindingTailJump, &enable);
        }
    }

    const auto disable_rva = scanner.Find("ToolsMenuDisable", kDisable, &disable);
    std::optional<uint32_t> disable_game_logic, disable_tail;
    if (disable_rva) {
        disable_game_logic = scanner.AccessorSlot(*disable_rva, kBindingAccessorCall, &disable);
        if (disable_game_logic) {
            disable_tail = scanner.TailJump(*disable_rva, kBindingTailJump, &disable);
        }
    }

    const auto create_rva = scanner.Find("ToolsMenuCreate", kCreate, &create);
    std::optional<uint32_t> create_menu_slot;
    if (create_rva) {
        create_menu_slot = scanner.AccessorSlot(*create_rva, kCreateAccessorCall, &create);
    }

    const auto destroy_rva = scanner.Find("ToolsMenuDestroy", kDestroy, &destroy);
    std::optional<uint32_t> destroy_menu_slot;
    if (destroy_rva) {
        destroy_menu_slot = scanner.AccessorSlot(*destroy_rva, kDestroyAccessorCall, &destroy);
    }

    const auto gate_rva = scanner.Find("ToolsMenuGate", kGate, &gate);
    std::optional<uint32_t> system_slot;
    if (gate_rva) {
        system_slot = scanner.RipSlot(*gate_rva, &gate);
    }

    report.anchors = {frame_tick, dispatch, enable, disable, create, destroy, gate};

    const bool every_anchor_resolved =
        std::all_of(report.anchors.begin(), report.anchors.end(), [](const AnchorReport& anchor) {
            return anchor.status == AnchorStatus::Resolved;
        });

    // Independent resolutions of the same thing have to agree. Each of these is
    // a place where a wrong match would otherwise go unnoticed.
    auto check = [&](const char* name, bool passed, std::string detail) {
        report.checks.push_back({name, passed, std::move(detail)});
        return passed;
    };
    bool agreed = every_anchor_resolved;
    if (every_anchor_resolved) {
        agreed &= check("enable tail-jumps to create", enable_tail == create_rva,
                        enable_tail ? Hex(*enable_tail) : "none");
        agreed &= check("disable tail-jumps to destroy", disable_tail == destroy_rva,
                        disable_tail ? Hex(*disable_tail) : "none");
        agreed &= check("enable and disable read the same GAME_LOGIC slot",
                        enable_game_logic == disable_game_logic,
                        Hex(*enable_game_logic) + " / " + Hex(*disable_game_logic));
        agreed &= check("create and destroy read the same POLYGON_MENU slot",
                        create_menu_slot == destroy_menu_slot,
                        Hex(*create_menu_slot) + " / " + Hex(*destroy_menu_slot));
        agreed &= check("GAME_LOGIC and POLYGON_MENU are different slots",
                        *enable_game_logic != *create_menu_slot, "");
        // The dispatch slot is written at startup, so this can only be checked
        // against a running game. A file scan reports it as not checked.
        if (image.Live()) {
            uint64_t dispatched = 0;
            memcpy(&dispatched, image.At(*dispatch_slot, sizeof(dispatched)), sizeof(dispatched));
            agreed &= check("frame dispatch calls FrameTick",
                            dispatched == image.RuntimeBase() + *frame_tick_rva,
                            "slot holds " + Hex(dispatched));
        } else {
            check("frame dispatch calls FrameTick", true, "not checked: not a running game");
        }
    }

    ToolsMenuLayout& layout = report.layout;
    if (agreed) {
        layout.frame_tick_rva = *frame_tick_rva;
        layout.frame_tick_steal_bytes = kFrameTickStealBytes;
        layout.enable_rva = *enable_rva;
        layout.disable_rva = *disable_rva;
        layout.game_logic_slot_rva = *enable_game_logic;
        layout.polygon_menu_slot_rva = *create_menu_slot;
        layout.system_slot_rva = *system_slot;
        layout.gate_offset = kGateOffset;
        layout.terrain_offset = kTerrainOffset;

        layout.frame_tick_sample_length = static_cast<uint32_t>(ParsePattern(kFrameTick)->Length());
        layout.binding_sample_length = static_cast<uint32_t>(ParsePattern(kEnable)->Length());
        agreed = scanner.Sample(layout.frame_tick_rva, layout.frame_tick_sample_length,
                                layout.frame_tick_sample) &&
                 scanner.Sample(layout.enable_rva, layout.binding_sample_length,
                                layout.enable_sample) &&
                 scanner.Sample(layout.disable_rva, layout.binding_sample_length,
                                layout.disable_sample);
    }

    report.usable = agreed;
    if (report.usable) {
        report.summary = "Supported: every signature resolved and agrees.";
    } else if (!image.Live() && image.SteamStubWrapped() && !every_anchor_resolved) {
        report.summary = "Cannot scan this file: it is DRM-wrapped (SteamStub) and its code is "
                         "encrypted on disk. Start the game and scan the running process instead.";
    } else if (!every_anchor_resolved) {
        report.summary = "Not supported: this build does not match every signature.";
    } else {
        report.summary = "Not supported: signatures matched but do not agree with each other.";
    }
    if (!report.usable) {
        layout = ToolsMenuLayout{};
    }
    return report;
}

std::string FormatReport(const ScanReport& report) {
    std::string text = report.summary + "\n\n";
    char line[320];
    for (const AnchorReport& anchor : report.anchors) {
        const std::string rva = anchor.status == AnchorStatus::Resolved ? Hex(anchor.rva) : "-";
        snprintf(line, sizeof(line), "  %-18s %-13s matches=%u rva=%s%s%s\n", anchor.name.c_str(),
                 StatusText(anchor.status), anchor.matches, rva.c_str(),
                 anchor.detail.empty() ? "" : "  ", anchor.detail.c_str());
        text += line;
    }
    if (!report.checks.empty()) {
        text += "\n";
        for (const CheckReport& check : report.checks) {
            snprintf(line, sizeof(line), "  [%s] %s%s%s\n", check.passed ? "ok" : "FAIL",
                     check.name.c_str(), check.detail.empty() ? "" : "  ", check.detail.c_str());
            text += line;
        }
    }
    if (report.usable) {
        const ToolsMenuLayout& layout = report.layout;
        snprintf(line, sizeof(line), "\n  game_logic_slot=%s polygon_menu_slot=%s system_slot=%s\n",
                 Hex(layout.game_logic_slot_rva).c_str(), Hex(layout.polygon_menu_slot_rva).c_str(),
                 Hex(layout.system_slot_rva).c_str());
        text += line;
    }
    return text;
}

}  // namespace srtm
