// Resolves everything the tools menu needs from the game image, by signature.
//
// This replaces the build allowlist of the predecessor project. Nothing here is
// an address: every RVA is decoded out of instruction bytes that a signature
// matched, and the result is only usable when every signature matched exactly
// once and every independent resolution of the same thing agrees. See
// docs/design.md, "Guard model", and docs/runtime-discovery.md for where each
// signature comes from.

#pragma once

#include "image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace srtm {

constexpr uint32_t kMaxSampleBytes = 64;

enum class AnchorStatus {
    Resolved,
    NotFound,       // the signature matched nowhere
    Ambiguous,      // the signature matched more than once
    DecodeFailed,   // matched, but a call, accessor or slot it leads to is wrong
};

struct AnchorReport {
    std::string name;
    AnchorStatus status = AnchorStatus::NotFound;
    uint32_t matches = 0;
    uint32_t rva = 0;
    std::string detail;
};

struct CheckReport {
    std::string name;
    bool passed = false;
    std::string detail;
};

// What the in-process module needs, all module-relative. Valid only when
// ScanReport::usable is true.
struct ToolsMenuLayout {
    uint32_t frame_tick_rva = 0;
    uint32_t frame_tick_steal_bytes = 0;
    uint32_t enable_rva = 0;
    uint32_t disable_rva = 0;
    uint32_t game_logic_slot_rva = 0;
    uint32_t polygon_menu_slot_rva = 0;
    uint32_t system_slot_rva = 0;
    uint32_t gate_offset = 0;
    uint32_t terrain_offset = 0;

    // The bytes each site held when it was resolved. The hook refuses to patch,
    // and the module refuses to call, a site that no longer holds them.
    uint32_t frame_tick_sample_length = 0;
    uint8_t frame_tick_sample[kMaxSampleBytes] = {};
    uint32_t binding_sample_length = 0;
    uint8_t enable_sample[kMaxSampleBytes] = {};
    uint8_t disable_sample[kMaxSampleBytes] = {};
};

struct ScanReport {
    bool usable = false;
    std::string summary;
    std::vector<AnchorReport> anchors;
    std::vector<CheckReport> checks;
    ToolsMenuLayout layout;
};

ScanReport ScanToolsMenu(const Image& image);

// The plain-text report a user pastes into an issue.
std::string FormatReport(const ScanReport& report);

}  // namespace srtm
