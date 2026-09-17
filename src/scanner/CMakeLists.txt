# The signature scanner. Pure computation over a byte buffer, so the same code
# runs inside the game, in the scan tool, and behind the diagnostics panel.
add_library(srtm_scanner STATIC
    image.cpp
    pattern.cpp
    tools_menu_scan.cpp
)

target_include_directories(srtm_scanner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Which executable a report is about: version and hash. Separate from the
# scanner because it opens files, and the scanner deliberately does not.
add_library(srtm_build_identity STATIC
    build_identity.cpp
)

target_include_directories(srtm_build_identity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
