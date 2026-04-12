# gfx/CMakeLists.txt

add_library(cad_gfx STATIC
    src/null_backend/null_device.cpp
    src/software_backend/sw_device.cpp
    src/webgl_backend/webgl_device.cpp
)

target_include_directories(cad_gfx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cad_gfx PUBLIC
    cad_core
)

# TODO: add gfx tests when ready
# if(CAD_BUILD_TESTS)
#     add_subdirectory(test)
# endif()
