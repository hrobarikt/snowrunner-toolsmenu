# The tray app. This is what a user downloads and runs.

# The module is embedded as a resource, so the release is one file. The path is
# only known per configuration, hence a generated .rc rather than a checked-in
# one.
set(module_rc "${CMAKE_CURRENT_BINARY_DIR}/module_$<CONFIG>.rc")

# The icon is optional. Drop an app.ico beside this file and re-run CMake to
# pick it up; without one the app uses the stock Windows icon and still builds,
# so a missing piece of art never blocks the code.
set(icon_line "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")
    set(icon_line "IDI_APP ICON \"${CMAKE_CURRENT_SOURCE_DIR}/app.ico\"\n")
endif()

file(GENERATE OUTPUT "${module_rc}"
     CONTENT "#include \"resource.h\"\nIDR_SRTM_DLL RCDATA \"$<TARGET_FILE:srtm_dll>\"\n${icon_line}")

add_executable(srtm-toolsmenu WIN32
    autostart.cpp
    main.cpp
    link.cpp
    "${module_rc}"
)

# WIN32 above means no console window: this is a tray app, not a command.
set_source_files_properties("${module_rc}" PROPERTIES GENERATED TRUE)
target_include_directories(srtm-toolsmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srtm-toolsmenu PRIVATE imgui srtm_inject srtm_protocol comdlg32)
add_dependencies(srtm-toolsmenu srtm_dll)
