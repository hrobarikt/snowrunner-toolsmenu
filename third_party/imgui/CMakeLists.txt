# Dear ImGui, with the Win32 and Direct3D 11 backends. See ../README.md.
add_library(imgui STATIC
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_win32.cpp
    backends/imgui_impl_dx11.cpp
)

target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi dwmapi)

# Vendored code is not ours to keep warning-clean.
target_compile_options(imgui PRIVATE /W0)
