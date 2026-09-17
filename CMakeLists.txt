cmake_minimum_required(VERSION 3.21)
project(snowrunner-toolsmenu LANGUAGES CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SnowRunner is 64-bit; configure a 64-bit generator (-A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Static CRT everywhere: the shipped exe must run with no runtime to install,
# and the injected DLL must not depend on a CRT the game does not have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(src/protocol)
add_subdirectory(src/scanner)
add_subdirectory(src/dll)
add_subdirectory(tools/scan)
add_subdirectory(tools/inject)
