#include "autostart.h"

#include <windows.h>

#include <vector>

namespace srtm {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// What Task Manager's Startup tab shows the user, so it reads like the product
// rather than like a folder name. The app's own settings key stays a slug:
// nothing but this app ever reads that one.
constexpr wchar_t kRunValue[] = L"SnowRunner Tools Menu";

constexpr wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// The command Windows runs at logon. Quoted, because the exe lives wherever the
// user put it and that path will contain spaces sooner or later; --tray,
// because a window nobody asked for at every login is how a feature like this
// gets switched back off.
std::wstring StartupCommand() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    return L"\"" + std::wstring(path, length) + L"\" --tray";
}

std::wstring ReadRunValue() {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr,
                     buffer, &size) != ERROR_SUCCESS) {
        return std::wstring();
    }
    return std::wstring(buffer);
}

// Task Manager does not delete the Run value when the user disables an entry:
// it writes a blob here instead, and the low bit of the first byte is the
// disabled flag. The format is not documented, so anything unreadable or
// shorter than expected is treated as "not disabled" -- erring towards
// believing the Run value, which is the half of this that is specified.
bool DisabledByExplorer() {
    unsigned char blob[32] = {};
    DWORD size = sizeof(blob);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY,
                     nullptr, blob, &size) != ERROR_SUCCESS) {
        return false;
    }
    if (size < 1) {
        return false;
    }
    return (blob[0] & 1) != 0;
}

// Enabling has to clear this, or Windows keeps honouring a disable the user has
// just undone in our own window. Absent is what Explorer treats as enabled.
void ClearExplorerDisable() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
}

}  // namespace

bool AutostartEnabled() {
    return !ReadRunValue().empty() && !DisabledByExplorer();
}

bool SetAutostart(bool enabled, std::wstring* trouble) {
    if (trouble != nullptr) {
        trouble->clear();
    }

    if (!enabled) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) !=
            ERROR_SUCCESS) {
            if (trouble != nullptr) {
                *trouble = L"Windows would not let the tool remove itself from "
                           L"startup.";
            }
            return false;
        }
        const LSTATUS removed = RegDeleteValueW(key, kRunValue);
        RegCloseKey(key);
        // Already gone is the outcome that was asked for.
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
            if (trouble != nullptr) {
                *trouble = L"Windows would not let the tool remove itself from "
                           L"startup.";
            }
            return false;
        }
        return true;
    }

    const std::wstring command = StartupCommand();
    if (command.empty()) {
        if (trouble != nullptr) {
            *trouble = L"The tool could not work out where its own exe is.";
        }
        return false;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        if (trouble != nullptr) {
            *trouble = L"Windows would not let the tool add itself to startup. An "
                       L"antivirus or a policy may be blocking it.";
        }
        return false;
    }
    const LSTATUS written = RegSetValueExW(
        key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        if (trouble != nullptr) {
            *trouble = L"Windows would not let the tool add itself to startup. An "
                       L"antivirus or a policy may be blocking it.";
        }
        return false;
    }

    ClearExplorerDisable();
    return true;
}

void RefreshAutostartPath() {
    // Deliberately keyed on the Run value rather than on AutostartEnabled: an
    // entry the user has switched off in Task Manager should still have the
    // right path in it for when they switch it back on.
    const std::wstring stored = ReadRunValue();
    if (stored.empty()) {
        return;
    }
    const std::wstring command = StartupCommand();
    if (command.empty() || command == stored) {
        return;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, kRunValue, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(command.c_str()),
                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

}  // namespace srtm
