// The tray app: one icon, and one window for when something is wrong.
//
// It waits for SnowRunner, puts the module in, and otherwise stays out of the
// way. The common action -- toggling the menu -- is the hotkey, in the game,
// with no alt-tab; the window exists to say what is happening when that does
// not work. Closing the window hides it, and only Exit ends the app.
#include "link.h"
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

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
NOTIFYICONDATAW g_tray = {};
bool g_window_visible = true;

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

void ShowWindowAgain(HWND window) {
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
        return srtm::DetachRequested()
                   ? "Detached. The game is untouched; press Attach to go back in."
                   : "SnowRunner is running. Getting in...";
    }
    switch (view.state) {
        case 0: return "Reading the game. This takes a moment.";
        case 1: return "This game build is not recognised, so the menu stays off.";
        case 2: return "The build was recognised but could not be hooked.";
        case 3: return view.menu_on ? "The tools menu is on." : "Ready. The tools menu is off.";
        case 4: return "Detaching...";
        case 5: return "Detached.";
        default: return "Unknown state.";
    }
}

const char* CommandText(uint32_t status) {
    switch (status) {
        case 0: return "ok";
        case 1: return "this game build is not recognised";
        case 2: return "the game's code changed under us";
        case 3: return "no world is loaded yet";
        case 4: return "a tools menu is already open";
        case 5: return "the open tools menu is not ours to close";
        case 6: return "the game did not do what the call asks";
        case 7: return "the game's own call raised";
        default: return "unknown";
    }
}

void CopyToClipboard(HWND window, const std::string& text) {
    if (!OpenClipboard(window)) {
        return;
    }
    EmptyClipboard();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (memory != nullptr) {
        void* locked = GlobalLock(memory);
        memcpy(locked, text.c_str(), text.size() + 1);
        GlobalUnlock(memory);
        SetClipboardData(CF_TEXT, memory);
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

    const bool can_toggle = view.module_present && view.state == 3 && !view.busy;
    ImGui::BeginDisabled(!can_toggle);
    if (ImGui::Button(view.menu_on ? "Turn the tools menu off"
                                   : "Turn the tools menu on",
                      ImVec2(-1, 34))) {
        srtm::RequestMenu(!view.menu_on);
    }
    ImGui::EndDisabled();

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
    if (view.module_present) {
        if (ImGui::Button("Detach")) {
            srtm::RequestDetachModule();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Removes everything from the game, which keeps running.");
    } else if (view.game_running) {
        if (ImGui::Button("Attach")) {
            srtm::RequestAttach();
        }
    }

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
    // One instance: two of these would fight over the same game.
    const HANDLE only = CreateMutexW(nullptr, TRUE, L"snowrunner-toolsmenu-single");
    if (only != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
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
    if (window == nullptr || !CreateDeviceD3D(window)) {
        CleanupDeviceD3D();
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

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No settings file: the app has nothing to remember between runs, and the
    // design keeps it that way.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

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
        if (!g_window_visible) {
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
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
