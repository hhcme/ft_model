# Emscripten (WASM) build for the CAD engine Electron/web platform.
#
# This file is only processed when building with an Emscripten toolchain
# (i.e. when EMSCRIPTEN is defined). On native builds the top-level
# CMakeLists.txt gates this behind CAD_BUILD_WASM=ON.
#
# Build example (from project root):
#   emcmake cmake -B build/wasm -DCAD_BUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/wasm -j$(nproc)
#
# Output artefacts appear in build/wasm/platforms/electron/public/:
#   cad_engine.js   — Emscripten JS loader
#   cad_engine.wasm — WebAssembly binary

if(NOT EMSCRIPTEN)
    message(STATUS "Skipping electron platform (not an Emscripten build)")
    return()
endif()

# ---------- WASM output settings ----------

set(WASM_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/public")
file(MAKE_DIRECTORY ${WASM_OUTPUT_DIR})

set(WASM_BASENAME "cad_engine")

# Tell Emscripten to produce a single JS + WASM file pair
set_target_properties(cad_engine PROPERTIES
    OUTPUT_NAME "${WASM_BASENAME}"
)

# ---------- Embind bridge source ----------

target_sources(cad_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/cad_wasm_bridge.cpp
)

target_include_directories(cad_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/core/include
)

# ---------- Emscripten link flags ----------

set(EMSCRIPTEN_LINK_FLAGS
    # Produce ES6 module output
    "-s EXPORT_ES6=1"
    "-s MODULARIZE=1"
    "-s EXPORT_NAME=createCadEngineModule"

    # Enable Embind
    "-s ALLOW_MEMORY_GROWTH=1"

    # WebGL2 support
    "-s USE_WEBGL2=1"

    # Minimum web environment
    "-s MIN_WEBGL_VERSION=2"
    "-s ENVIRONMENT='web'"

    # Disable file system by default (we use buffers)
    "-s FILESYSTEM=0"

    # Output file settings — place JS/WASM in public dir
    "-o ${WASM_OUTPUT_DIR}/${WASM_BASENAME}.js"
)

# Apply link flags to the cad_engine target
target_link_options(cad_engine PRIVATE ${EMSCRIPTEN_LINK_FLAGS})

message(STATUS "WASM output: ${WASM_OUTPUT_DIR}/${WASM_BASENAME}.js")
