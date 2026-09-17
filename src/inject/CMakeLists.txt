# Finding the game and getting a DLL into it. Shared by the tray app and the
# development tool.
add_library(srtm_inject STATIC injector.cpp)
target_include_directories(srtm_inject PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
