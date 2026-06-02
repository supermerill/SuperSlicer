#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

find_package(Python3 COMPONENTS Interpreter Development QUIET)

if (Python3_FOUND)
    add_library(python_plugin_loader SHARED
        ../libslic3r/Api/host/PythonPluginLoader.cpp
    )

    target_compile_definitions(python_plugin_loader PRIVATE SLIC3R_PLUGIN_EXPORTS)
    target_include_directories(python_plugin_loader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(python_plugin_loader PRIVATE Python3::Python boost_libs)

    set_target_properties(python_plugin_loader PROPERTIES
        FOLDER "plugins"
        OUTPUT_NAME "python_plugin_loader"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins"
    )

    set(SLIC3R_PYTHON_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python")
    set(SLIC3R_PYTHON_API_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/slic3r_api_generated.py")
    file(GLOB SLIC3R_PLUGIN_C_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/c/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/c/steps/*.h"
    )

    add_custom_command(
        OUTPUT "${SLIC3R_PYTHON_API_GENERATED}"
        COMMAND "${Python3_EXECUTABLE}" "${SLIC3R_PYTHON_API_DIR}/generate_slic3r_api.py"
            --output "${SLIC3R_PYTHON_API_GENERATED}"
        DEPENDS
            "${SLIC3R_PYTHON_API_DIR}/generate_slic3r_api.py"
            ${SLIC3R_PLUGIN_C_HEADERS}
        COMMENT "Generating Python ctypes bindings for the plugin C ABI"
        VERBATIM
    )

    add_custom_target(slic3r_python_api_generated
        DEPENDS "${SLIC3R_PYTHON_API_GENERATED}"
    )
    add_dependencies(python_plugin_loader slic3r_python_api_generated)

    add_custom_command(TARGET python_plugin_loader POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/polyholes_python.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/polyholes_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/default_surface_generator.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/simple_perimeter_generator2.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_api.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_api.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_geometry_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_geometry_views.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_extrusion_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_extrusion_views.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_datatree_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_datatree_views.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_clipper_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_clipper_views.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/slic3r_region_settings_views.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_region_settings_views.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/__init__.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/__init__.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/layer_height.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/layer_height.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/slicing.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/slicing.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/post_slicing.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/post_slicing.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/perimeter.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/perimeter.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/perimeter_module.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/perimeter_module.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/post_perimeter.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/post_perimeter.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/post_infill.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/post_infill.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python/steps/surface_generation.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps/surface_generation.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SLIC3R_PYTHON_API_GENERATED}"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/slic3r_api_generated.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/example_python_plugin.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/example_python_plugin.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/polyholes/polyholes_low_level.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/polyholes_low_level.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/polyholes/polyholes_high_level.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/polyholes_high_level.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/perimeter/simple_perimeter_generator.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/simple_perimeter_generator.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/perimeter/hairy_object.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/hairy_object.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/surface/initial_typed_surface_builder.py"
            "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins/initial_typed_surface_builder.py"
        COMMENT "Copying Python plugin helper and examples"
        VERBATIM
    )
else()
    message(STATUS "Python3 development files were not found; python_plugin_loader will not be built.")
endif()
