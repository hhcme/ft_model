cmake_minimum_required(VERSION 3.24)
project(cad_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(CAD_BUILD_TESTS "Build unit tests" ON)
option(CAD_BUILD_WASM "Build for WebAssembly (Emscripten)" OFF)
option(CAD_BUILD_FLUTTER_PLUGIN "Build Flutter plugin native library" OFF)

# Third-party dependencies
# add_subdirectory(third_party)  # fmt not needed for now

# Core library (always built)
add_subdirectory(core)

# Graphics HAL (always built)
add_subdirectory(gfx)

# Platform-specific builds
if(CAD_BUILD_WASM)
    add_subdirectory(platforms/electron)
endif()

if(CAD_BUILD_FLUTTER_PLUGIN)
    add_subdirectory(platforms/flutter)
endif()

if(CAD_BUILD_TESTS)
    enable_testing()
endif()
