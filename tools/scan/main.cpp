// srtm-scan: runs the tools-menu scanner outside the game.
//
//   srtm-scan                 scan the running SnowRunner.exe
//   srtm-scan --file <exe>    scan an executable on disk
//
// Reading the running game is the reliable path: the Steam build is SteamStub
// wrapped, so its code only exists decrypted in memory. The file mode is for
// executables that are not wrapped, such as a build from another store.

#include "build_identity.h"
#include "tools_menu_scan.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Snapshot {
    std::vector<uint8_t> bytes;
    uint64_t runtime_base = 0;
    bool live = false;
    std::string source;
    // The file the bytes ultimately came from, for the report's identity block.
    std::wstring image_path;
};

bool FindGameModule(DWORD* pid, uint64_t* base, uint32_t* size) {
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W process = {};
    process.dwSize = sizeof(process);
    bool found = false;
    for (BOOL more = Process32FirstW(processes, &process); more && !found;
         more = Process32NextW(processes, &process)) {
        if (_wcsicmp(process.szExeFile, L"SnowRunner.exe") != 0) {
            continue;
        }
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process.th32ProcessID);
        if (modules == INVALID_HANDLE_VALUE) {
            continue;
        }
        MODULEENTRY32W module = {};
        module.dwSize = sizeof(module);
        if (Module32FirstW(modules, &module)) {
            *pid = process.th32ProcessID;
            *base = reinterpret_cast<uint64_t>(module.modBaseAddr);
            *size = module.modBaseSize;
            found = true;
        }
        CloseHandle(modules);
    }
    CloseHandle(processes);
    return found;
}

// Copies the game's image out page range by page range. A range that cannot be
// read is left zeroed rather than failing the whole snapshot; the scanner then
// simply does not match inside it.
bool SnapshotRunningGame(Snapshot* snapshot) {
    DWORD pid = 0;
    uint64_t base = 0;
    uint32_t size = 0;
    if (!FindGameModule(&pid, &base, &size)) {
        fprintf(stderr, "SnowRunner.exe is not running.\n");
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        fprintf(stderr, "Cannot open SnowRunner.exe (pid %lu): error %lu.\n", pid, GetLastError());
        return false;
    }

    // The file behind the running process, so a live scan can report the same
    // version and hash a file scan would.
    wchar_t image_path[MAX_PATH] = {};
    DWORD image_path_length = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, image_path, &image_path_length)) {
        snapshot->image_path.assign(image_path, image_path_length);
    }
    snapshot->bytes.assign(size, 0);
    uint64_t cursor = base;
    const uint64_t end = base + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &region, sizeof(region)) !=
            sizeof(region)) {
            break;
        }
        const uint64_t region_end = reinterpret_cast<uint64_t>(region.BaseAddress) + region.RegionSize;
        const uint64_t chunk_end = region_end < end ? region_end : end;
        if (region.State == MEM_COMMIT && (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
            SIZE_T read = 0;
            ReadProcessMemory(process, reinterpret_cast<LPCVOID>(cursor),
                              snapshot->bytes.data() + (cursor - base),
                              static_cast<SIZE_T>(chunk_end - cursor), &read);
        }
        cursor = chunk_end;
    }
    CloseHandle(process);
    snapshot->runtime_base = base;
    snapshot->live = true;
    char source[96];
    snprintf(source, sizeof(source), "running SnowRunner.exe (pid %lu, base 0x%llX)", pid,
             static_cast<unsigned long long>(base));
    snapshot->source = source;
    return true;
}

template <typename T>
T ReadField(const std::vector<uint8_t>& file, size_t offset) {
    T value{};
    if (offset <= file.size() && sizeof(T) <= file.size() - offset) {
        memcpy(&value, file.data() + offset, sizeof(T));
    }
    return value;
}

// Lays a file out the way the loader would: headers, then each section's raw
// data at its RVA, the rest zero.
bool MapFile(const char* path, Snapshot* snapshot) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fprintf(stderr, "Cannot open %s.\n", path);
        return false;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)),
                                    std::istreambuf_iterator<char>());
    const uint32_t nt = ReadField<uint32_t>(file, 0x3C);
    const uint16_t sections = ReadField<uint16_t>(file, nt + 6);
    const uint16_t optional_size = ReadField<uint16_t>(file, nt + 20);
    const uint32_t image_size = ReadField<uint32_t>(file, nt + 24 + 56);
    const uint32_t header_size = ReadField<uint32_t>(file, nt + 24 + 60);
    if (ReadField<uint16_t>(file, 0) != 0x5A4D || ReadField<uint32_t>(file, nt) != 0x00004550 ||
        image_size == 0 || header_size > file.size() || header_size > image_size) {
        fprintf(stderr, "%s is not a PE image.\n", path);
        return false;
    }
    snapshot->bytes.assign(image_size, 0);
    memcpy(snapshot->bytes.data(), file.data(), header_size);
    const size_t table = static_cast<size_t>(nt) + 24 + optional_size;
    for (uint16_t index = 0; index < sections; ++index) {
        const size_t entry = table + static_cast<size_t>(index) * 40;
        const uint32_t virtual_size = ReadField<uint32_t>(file, entry + 8);
        const uint32_t rva = ReadField<uint32_t>(file, entry + 12);
        const uint32_t raw_size = ReadField<uint32_t>(file, entry + 16);
        const uint32_t raw_offset = ReadField<uint32_t>(file, entry + 20);
        size_t count = raw_size < virtual_size ? raw_size : virtual_size;
        if (count == 0) {
            count = raw_size;
        }
        if (raw_offset > file.size() || rva > image_size) {
            continue;
        }
        count = (std::min)({count, file.size() - raw_offset, static_cast<size_t>(image_size - rva)});
        memcpy(snapshot->bytes.data() + rva, file.data() + raw_offset, count);
    }
    snapshot->runtime_base = ReadField<uint64_t>(file, nt + 24 + 24);
    snapshot->live = false;
    snapshot->source = path;
    const int wide = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (wide > 0) {
        snapshot->image_path.resize(static_cast<size_t>(wide) - 1);
        MultiByteToWideChar(CP_ACP, 0, path, -1, snapshot->image_path.data(), wide);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Snapshot snapshot;
    if (argc == 3 && strcmp(argv[1], "--file") == 0) {
        if (!MapFile(argv[2], &snapshot)) {
            return 2;
        }
    } else if (argc == 1) {
        if (!SnapshotRunningGame(&snapshot)) {
            return 2;
        }
    } else {
        fprintf(stderr, "usage: srtm-scan [--file <SnowRunner.exe>]\n");
        return 2;
    }

    const auto image = srtm::Image::Parse(snapshot.bytes.data(), snapshot.bytes.size(),
                                          snapshot.runtime_base, snapshot.live);
    if (!image) {
        fprintf(stderr, "Not a 64-bit PE image.\n");
        return 2;
    }
    srtm::ScanReport report = srtm::ScanToolsMenu(*image);
    report.identity = srtm::IdentifyFile(snapshot.image_path);
    printf("Source: %s\n\n%s", snapshot.source.c_str(), srtm::FormatReport(report).c_str());
    return report.usable ? 0 : 1;
}
