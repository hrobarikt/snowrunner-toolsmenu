#include "pattern.h"

#include <cstring>

namespace srtm {
namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

bool Pattern::Matches(const uint8_t* candidate) const {
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (fixed[index] && candidate[index] != bytes[index]) {
            return false;
        }
    }
    return true;
}

std::optional<Pattern> ParsePattern(std::string_view text) {
    Pattern pattern;
    size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] == ' ') {
            ++cursor;
            continue;
        }
        if (cursor + 1 >= text.size()) {
            return std::nullopt;
        }
        if (text[cursor] == '?' && text[cursor + 1] == '?') {
            pattern.bytes.push_back(0);
            pattern.fixed.push_back(false);
        } else {
            const int high = HexValue(text[cursor]);
            const int low = HexValue(text[cursor + 1]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            pattern.bytes.push_back(static_cast<uint8_t>(high << 4 | low));
            pattern.fixed.push_back(true);
        }
        cursor += 2;
    }
    // A pattern has to anchor on a real byte, or it matches everywhere.
    if (pattern.bytes.empty() || !pattern.fixed[0]) {
        return std::nullopt;
    }
    return pattern;
}

std::vector<uint32_t> FindAll(const Image& image, const Pattern& pattern, size_t limit) {
    std::vector<uint32_t> matches;
    const size_t length = pattern.Length();
    for (const Section& section : image.Sections()) {
        if (!section.Executable() || section.size < length) {
            continue;
        }
        const uint8_t* begin = image.At(section.rva, section.size);
        if (begin == nullptr) {
            continue;
        }
        const uint8_t* last = begin + (section.size - length);
        const uint8_t* cursor = begin;
        while (cursor <= last) {
            const auto* hit = static_cast<const uint8_t*>(
                memchr(cursor, pattern.bytes[0], static_cast<size_t>(last - cursor) + 1));
            if (hit == nullptr) {
                break;
            }
            if (pattern.Matches(hit)) {
                matches.push_back(section.rva + static_cast<uint32_t>(hit - begin));
                if (matches.size() >= limit) {
                    return matches;
                }
            }
            cursor = hit + 1;
        }
    }
    return matches;
}

}  // namespace srtm
