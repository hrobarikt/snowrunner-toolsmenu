// Byte signatures with wildcards, written the way the discovery record writes
// them: "48 8B 05 ?? ?? ?? ??".

#pragma once

#include "image.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace srtm {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> fixed;  // false where the pattern has a wildcard

    size_t Length() const { return bytes.size(); }
    bool Matches(const uint8_t* candidate) const;
};

std::optional<Pattern> ParsePattern(std::string_view text);

// Every match inside the image's executable sections, up to `limit` of them.
// Callers pass a small limit: the only answers that matter are zero, one, or
// more than one.
std::vector<uint32_t> FindAll(const Image& image, const Pattern& pattern, size_t limit);

}  // namespace srtm
