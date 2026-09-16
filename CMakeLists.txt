cmake_minimum_required(VERSION 3.21)
project(snowrunner-toolsmenu LANGUAGES CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SnowRunner is 64-bit; configure a 64-bit generator (-A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere: the shipped exe must run with no runtime to install,
# and the injected DLL must not depend on a CRT the game does not have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(src/dll)
