#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

find_package(Python3 COMPONENTS Interpreter Development QUIET)
include(../plugins_cpp/PluginPackage.cmake)

if (TARGET Python3::Python)
    add_library(python_plugin_loader SHARED
        ../libslic3r/Api/host/PythonPluginLoader.cpp
        ../libslic3r/Plugins/PluginApiCompatibility.cpp
    )

    target_compile_definitions(python_plugin_loader PRIVATE SLIC3R_PLUGIN_EXPORTS)
    target_include_directories(python_plugin_loader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    target_link_libraries(python_plugin_loader PRIVATE Python3::Python boost_libs)

    set_target_properties(python_plugin_loader PROPERTIES
        FOLDER "plugins"
    )

    set(SLIC3R_PYTHON_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/python")
    set(SLIC3R_PYTHON_API_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/slic3r_api_generated.py")
    set(SLIC3R_PYTHON_ABI_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/python-api-abi.ini")
    set_target_properties(python_plugin_loader PROPERTIES SLIC3R_PLUGIN_EXTRA_ABI "${SLIC3R_PYTHON_ABI_MANIFEST}")
    file(GLOB SLIC3R_PLUGIN_C_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/c/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libslic3r/Api/plugin/c/steps/*.h"
    )

    add_custom_command(
        OUTPUT "${SLIC3R_PYTHON_API_GENERATED}" "${SLIC3R_PYTHON_ABI_MANIFEST}"
        COMMAND "${Python3_EXECUTABLE}" "${SLIC3R_PYTHON_API_DIR}/generate_slic3r_api.py"
            --output "${SLIC3R_PYTHON_API_GENERATED}"
            --abi-output "${SLIC3R_PYTHON_ABI_MANIFEST}"
        DEPENDS
            "${SLIC3R_PYTHON_API_DIR}/generate_slic3r_api.py"
            ${SLIC3R_PLUGIN_C_HEADERS}
        COMMENT "Generating Python ctypes bindings for the plugin C ABI"
        VERBATIM
    )

    add_custom_target(slic3r_python_api_generated
        DEPENDS "${SLIC3R_PYTHON_API_GENERATED}" "${SLIC3R_PYTHON_ABI_MANIFEST}"
    )
    add_dependencies(python_plugin_loader slic3r_python_api_generated)

    add_custom_command(TARGET python_plugin_loader POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "$<TARGET_FILE_DIR:python_plugin_loader>/python/plugins"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:python_plugin_loader>/python/steps"
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
        COMMENT "Copying the Python plugin API runtime"
        VERBATIM
    )

    # The bridge is runtime infrastructure shared by every pure Python package.
    # INTERNAL lets it load successfully without registering a user-selectable
    # plugin id; the package itself remains manageable in Plugin updates.
    slic3r_package_plugin(
        python_plugin_loader
        python
        INTERNAL
        NAME "Python plugin support"
        DESCRIPTION "Runtime support for plugins implemented in Python."
        FILES python
    )

    # Each script is its own package. Its package id matches the plugin id
    # registered at runtime, which keeps installation and activation names easy
    # to correlate in both plugin dialogs.
    slic3r_package_python_plugin(
        python_example_noop_package
        python.example.noop
        examples/example_python_plugin.py
        VERSION 1.0.0
        NAME "Python no-op example"
        DESCRIPTION "Minimal Python plugin example that does not change print data."
    )
    slic3r_package_python_plugin(
        python_polyholes_package
        python.polyholes
        polyholes/polyholes_low_level.py
        VERSION 1.0.0
        NAME "Python polyholes"
        DESCRIPTION "Low-level Python version of the polyholes post-slicing plugin."
    )
    slic3r_package_python_plugin(
        python_polyholes_high_level_package
        python.polyholes.high_level
        polyholes/polyholes_high_level.py
        VERSION 1.0.0
        NAME "Python polyholes high-level"
        DESCRIPTION "High-level Python version of the polyholes post-slicing plugin."
    )
    slic3r_package_python_plugin(
        python_simple_perimeter_generator_package
        python.perimeter.generator.simple
        perimeter/simple_perimeter_generator.py
        VERSION 1.0.0
        NAME "Python simple perimeter generator"
        DESCRIPTION "Python implementation of the simple perimeter generator."
    )
    slic3r_package_python_plugin(
        python_hairy_object_package
        python.perimeter.post_process.hairy_object
        perimeter/hairy_object.py
        VERSION 1.0.0
        NAME "HairyObject"
        DESCRIPTION "Adds painted decorative hair strokes after normal perimeter extrusion."
    )
    slic3r_package_python_plugin(
        python_initial_typed_surface_builder_package
        python.surface.initial_typed_surface_builder
        surface/initial_typed_surface_builder.py
        VERSION 1.0.0
        NAME "Python initial typed surface builder"
        DESCRIPTION "Python example that creates initial bottom, internal, and top infill surfaces."
    )
else()
    message(STATUS "Python3 development files were not found; python_plugin_loader will not be built.")
endif()
