# HOOPS Exchange DWG→SCS converter
# Requires HOOPS Exchange SDK (set A3DLIBS_ROOT or HOOPS_EXCHANGE_ROOT)

# Locate SDK root
if(NOT DEFINED A3DLIBS_ROOT AND DEFINED ENV{A3DLIBS_ROOT})
    set(A3DLIBS_ROOT $ENV{A3DLIBS_ROOT})
endif()
if(NOT DEFINED A3DLIBS_ROOT AND DEFINED ENV{HOOPS_EXCHANGE_ROOT})
    set(A3DLIBS_ROOT $ENV{HOOPS_EXCHANGE_ROOT})
endif()

# Find include directory
find_path(HOOPS_EXCHANGE_INCLUDE_DIR
    NAMES "A3DSDK.h"
    PATHS "${A3DLIBS_ROOT}/include"
    NO_DEFAULT_PATH
)

if(NOT HOOPS_EXCHANGE_INCLUDE_DIR)
    message(STATUS "HOOPS Exchange SDK not found (set A3DLIBS_ROOT). Skipping dwg_to_scs.")
    return()
endif()

message(STATUS "HOOPS Exchange found: ${HOOPS_EXCHANGE_INCLUDE_DIR}")

# Determine bin directory for runtime DLL loading
if(WIN32)
    set(HOOPS_SDK_BIN_DIR "${A3DLIBS_ROOT}/bin/win64_v142")
else()
    set(HOOPS_SDK_BIN_DIR "${A3DLIBS_ROOT}/bin/linux_x64_gcc81")
endif()

add_executable(dwg_to_scs dwg_to_scs.cpp)

target_include_directories(dwg_to_scs PRIVATE
    ${HOOPS_EXCHANGE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(dwg_to_scs PRIVATE
    INITIALIZE_A3D_API
    UNICODE
    _UNICODE
    HOOPS_SDK_BIN_DIR="${HOOPS_SDK_BIN_DIR}"
)

target_compile_features(dwg_to_scs PRIVATE cxx_std_20)

# Increase stack size to 8MB (DWG deep nesting can overflow the default 1MB)
if(MSVC)
    target_link_options(dwg_to_scs PRIVATE /STACK:8388608)
endif()

# No .lib linking needed — HOOPS Exchange uses dynamic loading (A3DSDKLoadLibrary)
if(NOT WIN32)
    target_link_libraries(dwg_to_scs PRIVATE ${CMAKE_DL_LIBS})
endif()
