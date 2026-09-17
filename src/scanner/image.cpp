#include "image.h"

#include <cstring>

namespace srtm {
namespace {

constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemWrite = 0x80000000;

template <typename T>
bool ReadAt(const uint8_t* bytes, size_t size, size_t offset, T* value) {
    if (offset > size || sizeof(T) > size - offset) {
        return false;
    }
    memcpy(value, bytes + offset, sizeof(T));
    return true;
}

}  // namespace

bool Section::Executable() const { return (characteristics & kScnMemExecute) != 0; }

bool Section::Writable() const { return (characteristics & kScnMemWrite) != 0; }

std::optional<Image> Image::Parse(const uint8_t* bytes,
                                  size_t size,
                                  uint64_t runtime_base,
                                  bool live) {
    uint16_t dos_magic = 0;
    uint32_t nt_offset = 0;
    if (bytes == nullptr || !ReadAt(bytes, size, 0, &dos_magic) || dos_magic != 0x5A4D ||
        !ReadAt(bytes, size, 0x3C, &nt_offset)) {
        return std::nullopt;
    }

    uint32_t nt_signature = 0;
    uint16_t machine = 0;
    uint16_t section_count = 0;
    uint16_t optional_size = 0;
    if (!ReadAt(bytes, size, nt_offset, &nt_signature) || nt_signature != 0x00004550 ||
        !ReadAt(bytes, size, nt_offset + 4, &machine) || machine != 0x8664 ||
        !ReadAt(bytes, size, nt_offset + 6, &section_count) ||
        !ReadAt(bytes, size, nt_offset + 20, &optional_size)) {
        return std::nullopt;
    }

    Image image;
    image.bytes_ = bytes;
    image.size_ = size;
    image.runtime_base_ = runtime_base;
    image.live_ = live;

    const size_t table = static_cast<size_t>(nt_offset) + 24 + optional_size;
    for (uint16_t index = 0; index < section_count; ++index) {
        const size_t entry = table + static_cast<size_t>(index) * 40;
        char name[9] = {};
        if (entry > size || 40 > size - entry) {
            return std::nullopt;
        }
        memcpy(name, bytes + entry, 8);
        Section section;
        section.name = name;
        ReadAt(bytes, size, entry + 8, &section.size);
        ReadAt(bytes, size, entry + 12, &section.rva);
        ReadAt(bytes, size, entry + 36, &section.characteristics);
        image.sections_.push_back(section);
    }
    return image;
}

const uint8_t* Image::At(uint32_t rva, size_t count) const {
    if (rva > size_ || count > size_ - rva) {
        return nullptr;
    }
    return bytes_ + rva;
}

const Section* Image::SectionAt(uint32_t rva) const {
    for (const Section& section : sections_) {
        if (rva >= section.rva && rva - section.rva < section.size) {
            return &section;
        }
    }
    return nullptr;
}

bool Image::InExecutable(uint32_t rva, size_t count) const {
    const Section* section = SectionAt(rva);
    return section != nullptr && section->Executable() &&
           count <= static_cast<size_t>(section->rva) + section->size - rva &&
           At(rva, count) != nullptr;
}

bool Image::InWritable(uint32_t rva, size_t count) const {
    const Section* section = SectionAt(rva);
    return section != nullptr && section->Writable() &&
           count <= static_cast<size_t>(section->rva) + section->size - rva &&
           At(rva, count) != nullptr;
}

bool Image::SteamStubWrapped() const {
    for (const Section& section : sections_) {
        if (section.name == ".bind") {
            return true;
        }
    }
    return false;
}

}  // namespace srtm
