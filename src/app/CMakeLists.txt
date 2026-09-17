# The tray app. This is what a user downloads and runs.

# The module is embedded as a resource, so the release is one file. The path is
# only known per configuration, hence a generated .rc rather than a checked-in
# one.
set(module_rc "${CMAKE_CURRENT_BINARY_DIR}/module_$<CONFIG>.rc")
file(GENERATE OUTPUT "${module_rc}"
     CONTENT "#include \"resource.h\"\nIDR_SRTM_DLL RCDATA \"$<TARGET_FILE:srtm_dll>\"\n")

add_executable(srtm-toolsmenu WIN32
    main.cpp
    link.cpp
    "${module_rc}"
)

# WIN32 above means no console window: this is a tray app, not a command.
set_source_files_properties("${module_rc}" PROPERTIES GENERATED TRUE)
target_include_directories(srtm-toolsmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srtm-toolsmenu PRIVATE imgui srtm_inject srtm_protocol comdlg32)
add_dependencies(srtm-toolsmenu srtm_dll)
