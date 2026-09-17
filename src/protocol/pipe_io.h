// Framing for the wire in protocol.h.
//
// The pipe is a byte pipe, so a single ReadFile or WriteFile may move less than
// it was asked for. Both ends loop, and both ends loop the same way: the module
// had these and the two clients each open-coded a short single-call version
// that failed a request on a short read.

#pragma once

#include <windows.h>

#include <stdint.h>

namespace srtm {

inline bool WriteAll(HANDLE pipe, const void* data, DWORD count) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (count > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, count, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        count -= written;
    }
    return true;
}

inline bool ReadAll(HANDLE pipe, void* data, DWORD count) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    while (count > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, count, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        count -= read;
    }
    return true;
}

}  // namespace srtm
