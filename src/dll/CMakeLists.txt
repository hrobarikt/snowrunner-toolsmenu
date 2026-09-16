# The frame hook, built on its own until the command queue that defines
# srtm::OnFrame lands. It has no SnowRunner knowledge in it and is linked into
# both the injected DLL and any test host.
add_library(srtm_frame_hook STATIC
    frame_hook.cpp
    detour.asm
)

target_include_directories(srtm_frame_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
