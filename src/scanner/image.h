// A read-only view of a PE image laid out by RVA.
//
// The same scanner runs in three places: inside the game on the loaded module,
// in the scan tool on a snapshot read out of a running process, and on an
// executable file mapped section by section. All three present the image the
// way the loader lays it out, so every lookup here is by RVA.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace srtm {

struct Section {
    std::string name;
    uint32_t rva = 0;
    uint32_t size = 0;
    uint32_t characteristics = 0;

    bool Executable() const;
    bool Writable() const;
};

class Image {
public:
    // `bytes` must hold `size` bytes laid out by RVA. `runtime_base` is where
    // the image is loaded in the process it came from, and `live` says whether
    // its writable data is a real snapshot of that process. A file has neither,
    // so checks that read runtime state are skipped for it.
    static std::optional<Image> Parse(const uint8_t* bytes,
                                      size_t size,
                                      uint64_t runtime_base,
                                      bool live);

    const uint8_t* At(uint32_t rva, size_t count) const;
    const Section* SectionAt(uint32_t rva) const;

    // True when [rva, rva + count) lies inside one section of that kind.
    bool InExecutable(uint32_t rva, size_t count) const;
    bool InWritable(uint32_t rva, size_t count) const;

    const std::vector<Section>& Sections() const { return sections_; }
    uint64_t RuntimeBase() const { return runtime_base_; }
    bool Live() const { return live_; }

    // SteamStub wraps the executable in a `.bind` section and encrypts `.text`
    // on disk; the code only exists decrypted in the running process.
    bool SteamStubWrapped() const;

private:
    const uint8_t* bytes_ = nullptr;
    size_t size_ = 0;
    uint64_t runtime_base_ = 0;
    bool live_ = false;
    std::vector<Section> sections_;
};

}  // namespace srtm
