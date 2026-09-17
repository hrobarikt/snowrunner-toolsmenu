# The frame hook, built on its own until the command queue that defines
# srtm::OnFrame lands. It has no SnowRunner knowledge in it and is linked into
# both the injected DLL and any test host.
add_library(srtm_frame_hook STATIC
    frame_hook.cpp
    detour.asm
)

target_include_directories(srtm_frame_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Menu state and the calls into the game. Built on its own until the command
# queue and DllMain land; it takes a scanned layout and knows nothing about how
# that layout was obtained or who asked for a toggle.
add_library(srtm_tools_menu STATIC
    tools_menu.cpp
)

target_include_directories(srtm_tools_menu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srtm_tools_menu PUBLIC srtm_scanner)

# The injected module itself. It scans, hooks, owns the menu and detaches; the
# tray app injects it and (from the next step) talks to it over a pipe.
add_library(srtm_dll SHARED
    dllmain.cpp
    module.cpp
)

set_target_properties(srtm_dll PROPERTIES OUTPUT_NAME "srtm")
target_link_libraries(srtm_dll PRIVATE srtm_frame_hook srtm_tools_menu)
