# Development-only injector. Not shipped: the tray app replaces it.
add_executable(srtm-inject main.cpp)
add_dependencies(srtm-inject srtm_dll)
target_link_libraries(srtm-inject PRIVATE srtm_protocol)

# Next to the DLL, so the default path finds it with no arguments.
set_target_properties(srtm-inject PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:srtm_dll>)
