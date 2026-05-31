#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

add_library(dense_infill_plugin SHARED
    DenseInfill.cpp
    DenseInfill.hpp
)

target_compile_definitions(dense_infill_plugin PRIVATE DENSE_INFILL_PLUGIN_DLL SLIC3R_PLUGIN_EXPORTS)
target_include_directories(dense_infill_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
if (WIN32)
    target_link_libraries(dense_infill_plugin PRIVATE Slic3r)
else()
    target_link_libraries(dense_infill_plugin PRIVATE libslic3r)
endif()

set_target_properties(dense_infill_plugin PROPERTIES
    FOLDER "plugins"
    OUTPUT_NAME "dense_infill"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins"
)
