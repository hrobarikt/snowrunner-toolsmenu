// The tray app: one icon, and one window for when something is wrong.
//
// It waits for SnowRunner, puts the module in, and otherwise stays out of the
// way. The common action -- toggling the menu -- is the hotkey, in the game,
// with no alt-tab; the window exists to say what is happening when that does
// not work. Closing the window hides it, and only Exit ends the app.
#include "link.h"
// For ModuleState and ToolsMenuStatus: the window puts words to what the module
// reports, and the names come from the wire rather than from a local copy.
#include "protocol.h"
#include "resource.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves the common dialogs out, and Save... needs one.
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>

#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayMenuShow = 1;
constexpr UINT kTrayMenuToggle = 2;
constexpr UINT kTrayMenuExit = 3;
constexpr wchar_t kWindowTitle[] = L"SnowRunner Tools Menu";

// A second copy of the app has nothing to do except hand the click to the
// first one. Registered rather than a WM_APP constant, because this one
// crosses processes and only a registered message is guaranteed not to mean
// something else in whatever other window the broadcast reaches.
UINT g_show_message = 0;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
NOTIFYICONDATAW g_tray = {};
// False until something shows the window, which ShowWindowAgain is now the
// only way to do: the flag and the device are made true together or not at all.
bool g_window_visible = false;
// Whether the ImGui DX11 backend has been handed a device. It has not until
// the window is first shown, and shutdown must not undo what was never done.
bool g_dx11_ready = false;

// The keys worth offering. A free-text virtual-key code would be a worse
// question to ask a user than a short list of keys nothing else in the game
// uses.
struct HotkeyChoice {
    const char* label;
    uint32_t vk;
};

const HotkeyChoice kHotkeys[] = {
    {"Home", VK_HOME},     {"End", VK_END},     {"Insert", VK_INSERT},
    {"Delete", VK_DELETE}, {"Page Up", VK_PRIOR}, {"Page Down", VK_NEXT},
    {"F1", VK_F1},         {"F2", VK_F2},       {"F3", VK_F3},
    {"F4", VK_F4},         {"F5", VK_F5},       {"F6", VK_F6},
    {"F7", VK_F7},         {"F8", VK_F8},       {"F9", VK_F9},
    {"F10", VK_F10},       {"F11", VK_F11},     {"F12", VK_F12},
    {"Scroll Lock", VK_SCROLL}, {"Pause", VK_PAUSE},
};

void CreateRenderTarget() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
        back_buffer->Release();
    }
}

void ReleaseRenderTarget() {
    if (g_render_target != nullptr) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

bool CreateDeviceD3D(HWND window) {
    DXGI_SWAP_CHAIN_DESC description = {};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level = {};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT created = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &description, &g_swap_chain, &g_device, &level, &g_context);
    if (created == DXGI_ERROR_UNSUPPORTED) {
        created = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &description, &g_swap_chain, &g_device, &level, &g_context);
    }
    if (created != S_OK) {
        return false;
    }
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    ReleaseRenderTarget();
    if (g_swap_chain != nullptr) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context != nullptr) { g_context->Release(); g_context = nullptr; }
    if (g_device != nullptr) { g_device->Release(); g_device = nullptr; }
}

// The device is the one thing a hidden app has no use for: nothing draws until
// the window is shown, and an app that starts with Windows may never be shown
// at all. Asking for a GPU at login is also the worst moment to ask -- the
// driver is not necessarily up yet -- so the device, and the ImGui backend that
// owns it, wait for the first show.
bool EnsureDeviceD3D(HWND window) {
    if (g_device != nullptr) {
        return true;
    }
    if (!CreateDeviceD3D(window)) {
        // Partial work is still work: the swap chain can exist where the device
        // does not, and the next attempt starts from nothing.
        CleanupDeviceD3D();
        return false;
    }
    ImGui_ImplDX11_Init(g_device, g_context);
    g_dx11_ready = true;
    return true;
}

void ShowWindowAgain(HWND window) {
    if (!EnsureDeviceD3D(window)) {
        // No window, but the app is not the window: the tray icon, the hotkey
        // and the module already in the game all keep working without one.
        MessageBoxW(window,
                    L"The window could not be opened: Direct3D was not "
                    L"available. The tray icon and the hotkey still work.",
                    kWindowTitle, MB_ICONERROR);
        return;
    }
    g_window_visible = true;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
}

void ShowTrayMenu(HWND window) {
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayMenuShow,
                g_window_visible ? L"Hide window" : L"Show window");
    AppendMenuW(menu, MF_STRING, kTrayMenuToggle, L"Toggle tools menu");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

    POINT cursor = {};
    GetCursorPos(&cursor);
    // Required, or the menu stays up after a click elsewhere.
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT WINAPI WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return true;
    }

    // Someone ran the app again while this one was already running -- most
    // likely a double-click on the exe while this copy sat in the tray. What
    // they wanted was the window.
    if (g_show_message != 0 && message == g_show_message) {
        ShowWindowAgain(window);
        return 0;
    }

    switch (message) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && g_device != nullptr) {
                ReleaseRenderTarget();
                g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam),
                                            DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_CLOSE:
            // Closing hides. The app is the thing that keeps the module in the
            // game, so an accidental close must not take the mod away.
            g_window_visible = false;
            ShowWindow(window, SW_HIDE);
            return 0;

        case kTrayMessage:
            if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
                ShowWindowAgain(window);
            } else if (LOWORD(lparam) == WM_RBUTTONUP) {
                ShowTrayMenu(window);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kTrayMenuShow:
                    if (g_window_visible) {
                        g_window_visible = false;
                        ShowWindow(window, SW_HIDE);
                    } else {
                        ShowWindowAgain(window);
                    }
                    return 0;
                case kTrayMenuToggle:
                    srtm::RequestMenu(!srtm::GetLinkView().menu_on);
                    return 0;
                case kTrayMenuExit:
                    PostQuitMessage(0);
                    return 0;
                default:
                    break;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// One line that is always true, which is the whole contract of the status area.
std::string StatusLine(const srtm::LinkView& view) {
    if (!view.game_running) {
        return "Waiting for SnowRunner. Start the game normally.";
    }
    if (!view.module_present) {
        return "SnowRunner is running. Getting in...";
    }
    switch (static_cast<srtm::ModuleState>(view.state)) {
        case srtm::ModuleState::Scanning:
            return "Reading the game. This takes a moment.";
        case srtm::ModuleState::Unsupported:
            return "This game build is not recognised, so the menu stays off.";
        case srtm::ModuleState::Failed:
            return "The build was recognised but could not be hooked.";
        case srtm::ModuleState::Ready:
            return view.menu_on ? "The tools menu is on." : "Ready. The tools menu is off.";
        case srtm::ModuleState::Detaching:
            return "Detaching...";
        case srtm::ModuleState::Detached:
            return "Detached.";
        case srtm::ModuleState::DetachFailed:
            return "Detach could not put the game's original code back, so the "
                   "module is staying in. Close SnowRunner when convenient.";
    }
    return "Unknown state.";
}

// The same facts as srtm::ToolsMenuStatusText, said to a user instead of to the
// author. The terse phrasing stays on the wire side; this is the only place the
// app puts words to a command result.
const char* CommandText(uint32_t status) {
    switch (static_cast<srtm::ToolsMenuStatus>(status)) {
        case srtm::ToolsMenuStatus::Ok: return "ok";
        case srtm::ToolsMenuStatus::NotConfigured:
            return "this game build is not recognised";
        case srtm::ToolsMenuStatus::SiteChanged:
            return "the game's code changed under us";
        case srtm::ToolsMenuStatus::WorldUnavailable: return "no world is loaded yet";
        case srtm::ToolsMenuStatus::MenuPresent: return "a tools menu is already open";
        case srtm::ToolsMenuStatus::MenuForeign:
            return "the open tools menu is not ours to close";
        case srtm::ToolsMenuStatus::MenuFailed:
            return "the game did not do what the call asks";
        case srtm::ToolsMenuStatus::Faulted: return "the game's own call raised";
        case srtm::ToolsMenuStatus::Busy: return "another command was still running";
        case srtm::ToolsMenuStatus::HookStalled:
            return "the game never picked the command up";
    }
    return "unknown";
}

void CopyToClipboard(HWND window, const std::string& text) {
    if (!OpenClipboard(window)) {
        return;
    }
    EmptyClipboard();
    // The report is UTF-8, and CF_TEXT is the system codepage. Widening it is
    // what keeps a pasted report readable.
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    const HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    if (memory != nullptr) {
        auto* locked = static_cast<wchar_t*>(GlobalLock(memory));
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            locked, length);
        locked[length] = L'\0';
        GlobalUnlock(memory);
        SetClipboardData(CF_UNICODETEXT, memory);
    }
    CloseClipboard();
}

void SaveReport(HWND window, const std::string& text) {
    wchar_t path[MAX_PATH] = L"snowrunner-toolsmenu-report.txt";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) {
        return;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

void DrawWindow(HWND window) {
    const srtm::LinkView view = srtm::GetLinkView();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextWrapped("%s", StatusLine(view).c_str());
    if (!view.trouble.empty()) {
        const std::wstring& trouble = view.trouble;
        const int length = WideCharToMultiByte(CP_UTF8, 0, trouble.c_str(), -1, nullptr,
                                               0, nullptr, nullptr);
        std::vector<char> utf8(length > 0 ? length : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, trouble.c_str(), -1, utf8.data(), length,
                            nullptr, nullptr);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", utf8.data());
    }

    ImGui::Separator();

    const bool can_toggle = view.module_present &&
                            static_cast<srtm::ModuleState>(view.state) ==
                                srtm::ModuleState::Ready &&
                            !view.busy;
    ImGui::BeginDisabled(!can_toggle);
    if (ImGui::Button(view.menu_on ? "Turn the tools menu off"
                                   : "Turn the tools menu on",
                      ImVec2(-1, 34))) {
        srtm::RequestMenu(!view.menu_on);
    }
    ImGui::EndDisabled();

    // Only when the module has stopped looking. Its scan keeps retrying for
    // three minutes, which outlasts any load; past that, a game that sat on a
    // load screen longer than that has no other way back, and neither does a
    // build the scan cannot make sense of yet.
    const auto state = static_cast<srtm::ModuleState>(view.state);
    if (view.module_present && (state == srtm::ModuleState::Unsupported ||
                                state == srtm::ModuleState::Failed)) {
        ImGui::Spacing();
        if (ImGui::Button("Try again", ImVec2(-1, 0))) {
            srtm::RequestRescan();
        }
        ImGui::TextDisabled("Looks again, in case the game was still loading.");
    }

    ImGui::Spacing();
    ImGui::Text("Hotkey");
    ImGui::SameLine();
    int selected = -1;
    for (int index = 0; index < IM_ARRAYSIZE(kHotkeys); ++index) {
        if (kHotkeys[index].vk == view.hotkey_vk) {
            selected = index;
            break;
        }
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##hotkey",
                          selected >= 0 ? kHotkeys[selected].label : "(none)")) {
        for (int index = 0; index < IM_ARRAYSIZE(kHotkeys); ++index) {
            if (ImGui::Selectable(kHotkeys[index].label, index == selected)) {
                srtm::RequestHotkey(kHotkeys[index].vk);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(only while the game window has focus)");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::Text("Game process: %s", view.game_running ? "running" : "not running");
        if (view.module_present) {
            ImGui::Text("Hook: %s", view.hook_installed ? "installed" : "not installed");
            ImGui::Text("Frames seen: %llu",
                        static_cast<unsigned long long>(view.frames));
            if (view.last_command_valid) {
                ImGui::Text("Last action: %s", CommandText(view.last_command));
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Copy")) {
            CopyToClipboard(window, view.report);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save...")) {
            SaveReport(window, view.report);
        }
        ImGui::Spacing();
        ImGui::BeginChild("report", ImVec2(0, 0), ImGuiChildFlags_Border,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(view.report.empty() ? "No report yet."
                                                   : view.report.c_str());
        ImGui::EndChild();
    }

    ImGui::End();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // Registered before anything can receive it, and the same string in every
    // copy of the app, which is what makes the handoff below find its target.
    g_show_message = RegisterWindowMessageW(L"SnowRunnerToolsMenuShow");

    // One instance: two of these would fight over the same game.
    const HANDLE only = CreateMutexW(nullptr, TRUE, L"snowrunner-toolsmenu-single");
    if (only != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Exiting in silence would look exactly like a broken download. Ask the
        // copy that is already running to show itself instead, and let that be
        // what the user sees for their click.
        if (g_show_message != 0) {
            PostMessageW(HWND_BROADCAST, g_show_message, 0, 0);
        }
        return 0;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = WndProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Null when no app.ico was present at configure time, so fall back rather
    // than show nothing.
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    window_class.hIcon = icon;
    window_class.hIconSm = icon;
    window_class.lpszClassName = L"SnowRunnerToolsMenu";
    RegisterClassExW(&window_class);

    const HWND window = CreateWindowExW(
        0, window_class.lpszClassName, kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
        CW_USEDEFAULT, 560, 460, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, instance);
        MessageBoxW(nullptr, L"Could not create the window.", kWindowTitle, MB_ICONERROR);
        return 1;
    }

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = icon;
    wcscpy_s(g_tray.szTip, kWindowTitle);
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini: the window is one fixed layout, so there is nothing about it
    // worth remembering. The one thing the app does remember, the hotkey, is in
    // the registry -- see link.cpp.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(window);

    // The first show is the one that builds the device, through the same path
    // the tray icon uses later.
    ShowWindowAgain(window);

    wchar_t dll_path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dll_path);
    wcscat_s(dll_path, L"srtm.dll");
    srtm::StartLink(dll_path);

    bool done = false;
    while (!done) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }
        if (!g_window_visible || g_device == nullptr) {
            // Hidden in the tray, the app is only waiting for the worker and
            // for the user. Drawing would be wasted, so it does not.
            Sleep(100);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawWindow(window);
        ImGui::Render();

        const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_context->ClearRenderTargetView(g_render_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    // Leaving restores the game: the menu goes off, the hook comes out and the
    // module unloads itself, exactly as the Detach button does.
    srtm::DetachAndWait(3000);
    srtm::StopLink();
    if (g_dx11_ready) {
        ImGui_ImplDX11_Shutdown();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
