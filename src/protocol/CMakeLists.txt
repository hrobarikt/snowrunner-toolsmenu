# Header-only: the wire both ends are built from.
add_library(srtm_protocol INTERFACE)
target_include_directories(srtm_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
