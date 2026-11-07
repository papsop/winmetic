cmake_minimum_required(VERSION 3.22)
project(winmetic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

# Core Library (Header-only / Interface target)
add_library(winmetic INTERFACE)
target_include_directories(winmetic INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(winmetic INTERFACE
    user32
    d3d11
    dxgi
    windowscodecs
)

# CLI Tool
add_executable(winmetic_cli
    cli/main.cpp
)
set_target_properties(winmetic_cli PROPERTIES OUTPUT_NAME "winmetic")
target_link_libraries(winmetic_cli PRIVATE winmetic)

# Test Suite
add_executable(winmetic_tests
    tests/main_tests.cpp
)
target_link_libraries(winmetic_tests PRIVATE winmetic)

# Enable CTest
enable_testing()
add_test(NAME WinmeticUnitTests COMMAND winmetic_tests)
