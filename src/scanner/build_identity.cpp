// Build identity. See build_identity.h.
#include "build_identity.h"

#include <windows.h>

#include <bcrypt.h>

#include <vector>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")

namespace srtm {
namespace {

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

// The version resource: the fixed four-part number, and the ProductVersion
// string beside it. Both, because SnowRunner's four-part number is 1.0.0.0 on
// every build and the string is the one that names the build.
void ReadVersions(const std::wstring& path, std::string* version, std::string* build) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return;
    }
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) {
        return;
    }

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_size = 0;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed),
                       &fixed_size) &&
        fixed != nullptr) {
        char text[64];
        snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                 LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                 LOWORD(fixed->dwFileVersionLS));
        *version = text;
    }

    // The string table is per language and codepage, and there is no fixed
    // name for it: the translation table says which one this file has.
    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translations = nullptr;
    UINT translations_size = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&translations), &translations_size) ||
        translations == nullptr || translations_size < sizeof(Translation)) {
        return;
    }
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductVersion",
             translations[0].language, translations[0].codepage);
    wchar_t* text = nullptr;
    UINT text_length = 0;
    if (VerQueryValueW(block.data(), key, reinterpret_cast<LPVOID*>(&text),
                       &text_length) &&
        text != nullptr && text_length > 0) {
        *build = Narrow(std::wstring(text, wcsnlen(text, text_length)));
    }
}

// Streamed in chunks: the game executable is large, and this runs on a worker
// thread while the game is playing.
std::string FileSha256(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    std::vector<uint8_t> digest(32);
    std::vector<uint8_t> buffer(64 * 1024);

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                          nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                ok = false;
                break;
            }
        }
        if (ok && BCryptFinishHash(hash, digest.data(),
                                   static_cast<ULONG>(digest.size()), 0) == 0) {
            char hex[3];
            for (const uint8_t byte : digest) {
                snprintf(hex, sizeof(hex), "%02x", byte);
                result += hex;
            }
        }
    }

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    return result;
}

}  // namespace

BuildIdentity IdentifyFile(const std::wstring& path) {
    BuildIdentity identity;
    identity.file = Narrow(path);
    if (path.empty()) {
        return identity;
    }
    ReadVersions(path, &identity.version, &identity.build);
    identity.sha256 = FileSha256(path);
    return identity;
}

std::wstring MainModulePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, count);
}

}  // namespace srtm
