get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    ../../src/plugins_cpp/DenseInfill/DenseInfill.cpp
    ../../src/plugins_cpp/ExtraPerimeterOverhangWave/ExtraPerimeterOverhangWave.cpp
    ../../src/plugins_cpp/FlatAreaLayerHeight/FlatAreaLayerHeight.cpp
    ../../src/plugins_cpp/Polyholes/Polyholes.cpp
    gcode_test_helpers.cpp
    perimeter_test_helpers.cpp
    plugin_test_helpers.cpp
    test_auxiliary_layer_helpers.cpp
    test_clipper_views.cpp
    test_extrusion_continuity.cpp
    test_extrusion_ordered_leaf.cpp
    test_extrusion_split_by_areas.cpp
    test_plugin_activation_config.cpp
    test_plugin_config.cpp
    test_plugin_loader_diagnostics.cpp
    test_plugin_property.cpp
    test_print_records.cpp
    test_printing_entity_property_traversal.cpp
    test_plugin_repository.cpp
    test_plugin_translation_catalogs.cpp
    test_repository_updater.cpp
    test_gcode_default_firmware_session.cpp
    test_gcode_custom_gcode_per_print_z.cpp
    test_gcode_feature_gcode.cpp
    test_gcode_firmware_dialects.cpp
    test_gcode_firmware_state.cpp
    test_gcode_line_parser.cpp
    test_gcode_reader_compatibility.cpp
    test_gcode_printing_plan_file_writer.cpp
    test_gcode_script_processor.cpp
    test_gcode_settings_scripts.cpp
    test_layer_extrusion_edit_entry_state.cpp
    test_layer_extrusion_edit_extrusion_tree_ordering.cpp
    test_layer_extrusion_edit_lift.cpp
    test_layer_extrusion_edit_retraction.cpp
    test_layer_extrusion_edit_retraction_wipe.cpp
    test_layer_extrusion_edit_toolchange.cpp
    test_layer_extrusion_edit_transition_scope.cpp
    test_layer_extrusion_edit_travel.cpp
    test_layer_extrusion_edit_travel_deceleration.cpp
    test_layer_extrusion_edit_speed.cpp
    test_flat_area_layer_height.cpp
    test_ordering_engine.cpp
    test_ordering_printing_extrusion_presort.cpp
    test_orchestrator_plugin_execution.cpp
    test_perimeter_empty_steps.cpp
    test_perimeter_arachne_generator.cpp
    test_perimeter_classic_generator.cpp
    test_perimeter_detect_overhang.cpp
    test_perimeter_extra_perimeter_below_area.cpp
    test_perimeter_extra_perimeter_count.cpp
    test_perimeter_extra_perimeter_odd_layer.cpp
    test_perimeter_extra_perimeters_on_overhangs.cpp
    test_perimeter_fuzzy_skin.cpp
    test_perimeter_hairy_object.cpp
    test_perimeter_mark_first_loop.cpp
    test_perimeter_only_one_perimeter_first_layer.cpp
    test_perimeter_only_one_perimeter_on_top.cpp
    test_perimeter_python_simple_generator.cpp
    test_perimeter_remove_gap_fill_on_overhangs.cpp
    test_perimeter_separate_hole_contour.cpp
    test_perimeter_simple_generator.cpp
    test_polyholes.cpp
    test_printing_plan.cpp
    test_skirt_brim_default_generator.cpp
    test_step_pipeline_invalidation.cpp
    test_dense_infill.cpp
    test_support_demand_modifiers.cpp
    test_support_demand_overhangs.cpp
    test_surface_clean_infill_surfaces.cpp
    test_surface_initial_typed_surface_builder.cpp
    test_surface_infill_region_compatibility_splitter.cpp
    test_surface_solid_shells.cpp
    test_surface_top_surface_expansion.cpp
    test_vase_multi_island_connector.cpp
    )

add_library(plugin_loader_bad_abi_fixture SHARED plugin_loader_fixture.cpp)
target_compile_definitions(plugin_loader_bad_abi_fixture PRIVATE
    SLIC3R_PLUGIN_EXPORTS
    SLIC3R_TEST_PLUGIN_BAD_ABI)
target_include_directories(plugin_loader_bad_abi_fixture PRIVATE ../..)
set_target_properties(plugin_loader_bad_abi_fixture PROPERTIES FOLDER "tests/plugins")

add_library(plugin_loader_missing_register_fixture SHARED plugin_loader_fixture.cpp)
target_compile_definitions(plugin_loader_missing_register_fixture PRIVATE
    SLIC3R_PLUGIN_EXPORTS
    SLIC3R_TEST_PLUGIN_MISSING_REGISTER)
target_include_directories(plugin_loader_missing_register_fixture PRIVATE ../..)
set_target_properties(plugin_loader_missing_register_fixture PROPERTIES FOLDER "tests/plugins")

add_library(plugin_loader_missing_abi_fixture SHARED plugin_loader_fixture.cpp)
target_compile_definitions(plugin_loader_missing_abi_fixture PRIVATE
    SLIC3R_PLUGIN_EXPORTS
    SLIC3R_TEST_PLUGIN_MISSING_ABI)
target_include_directories(plugin_loader_missing_abi_fixture PRIVATE ../..)
set_target_properties(plugin_loader_missing_abi_fixture PROPERTIES FOLDER "tests/plugins")

add_library(plugin_loader_throw_register_fixture SHARED plugin_loader_fixture.cpp)
target_compile_definitions(plugin_loader_throw_register_fixture PRIVATE
    SLIC3R_PLUGIN_EXPORTS
    SLIC3R_TEST_PLUGIN_THROW_REGISTER)
target_include_directories(plugin_loader_throw_register_fixture PRIVATE ../..)
set_target_properties(plugin_loader_throw_register_fixture PROPERTIES FOLDER "tests/plugins")

add_dependencies(${_TEST_NAME}_tests
    plugin_loader_bad_abi_fixture
    plugin_loader_missing_register_fixture
    plugin_loader_missing_abi_fixture
    plugin_loader_throw_register_fixture)
target_compile_definitions(${_TEST_NAME}_tests PRIVATE
    SLIC3R_FILESYSTEM_TRANSACTION_TESTING
    SLIC3R_TEST_PLUGIN_BAD_ABI_DLL=R"\($<TARGET_FILE:plugin_loader_bad_abi_fixture>\)"
    SLIC3R_TEST_PLUGIN_MISSING_REGISTER_DLL=R"\($<TARGET_FILE:plugin_loader_missing_register_fixture>\)"
    SLIC3R_TEST_PLUGIN_MISSING_ABI_DLL=R"\($<TARGET_FILE:plugin_loader_missing_abi_fixture>\)"
    SLIC3R_TEST_PLUGIN_THROW_REGISTER_DLL=R"\($<TARGET_FILE:plugin_loader_throw_register_fixture>\)")

target_link_libraries(${_TEST_NAME}_tests test_common test_common_data libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    target_sources(${_TEST_NAME}_tests PRIVATE plugin_version_metadata.rc)
    prusaslicer_copy_dlls(${_TEST_NAME}_tests)
    set_target_properties(${_TEST_NAME}_tests PROPERTIES ENABLE_EXPORTS ON)
endif()

# Exercise the metadata generated for an actual external plugin target. The
# same test follows plugin.dll, plugin.so or plugin.dylib through the package
# cache without duplicating version constants.
if (TARGET polyholes_plugin)
    add_dependencies(${_TEST_NAME}_tests polyholes_plugin)
    get_target_property(_polyholes_package_version
        polyholes_plugin SLIC3R_PLUGIN_PACKAGE_VERSION)
    get_target_property(_polyholes_slicer_version
        polyholes_plugin SLIC3R_PLUGIN_SLICER_VERSION)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE
        SLIC3R_TEST_POLYHOLES_PLUGIN_DLL=R"\($<TARGET_FILE:polyholes_plugin>\)"
        SLIC3R_TEST_POLYHOLES_PACKAGE_VERSION=R"\(${_polyholes_package_version}\)"
        SLIC3R_TEST_POLYHOLES_SLICER_VERSION=R"\(${_polyholes_slicer_version}\)"
        SLIC3R_TEST_BUILD_SLICER_VERSION=R"\(${SLIC3R_VERSION_FULL}\)"
        SLIC3R_TEST_BUILD_RC_VERSION=R"\(${SLIC3R_RC_VERSION_DOTS}\)"
    )
endif()

if (TARGET flat_area_layer_height_plugin)
    get_target_property(_flat_area_package_version
        flat_area_layer_height_plugin SLIC3R_PLUGIN_PACKAGE_VERSION)
    get_target_property(_flat_area_slicer_version
        flat_area_layer_height_plugin SLIC3R_PLUGIN_SLICER_VERSION)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE
        SLIC3R_TEST_FLAT_AREA_PACKAGE_VERSION=R"\(${_flat_area_package_version}\)"
        SLIC3R_TEST_FLAT_AREA_SLICER_VERSION=R"\(${_flat_area_slicer_version}\)"
    )
endif()

if (TARGET python_plugin_loader)
    set(_python_plugin_package_targets
        python_example_noop_package
        python_hairy_object_package
        python_initial_typed_surface_builder_package
        python_polyholes_high_level_package
        python_polyholes_package
        python_simple_perimeter_generator_package
    )
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE SLIC3R_TEST_PYTHON_PLUGINS)

    # The production loader discovers pure Python packages as siblings of its
    # own package. Recreate that installed layout around the test executable so
    # tests exercise the same path instead of relying on scripts embedded in
    # the bridge package.
    set(_python_test_runtime_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
    set(_python_plugin_package_ids
        python.example.noop
        python.perimeter.post_process.hairy_object
        python.surface.initial_typed_surface_builder
        python.polyholes.high_level
        python.polyholes
        python.perimeter.generator.simple
    )
    set(_python_plugin_copy_commands)
    list(LENGTH _python_plugin_package_targets _python_plugin_package_count)
    math(EXPR _python_plugin_package_last "${_python_plugin_package_count} - 1")
    foreach(_python_plugin_package_idx RANGE ${_python_plugin_package_last})
        list(GET _python_plugin_package_targets ${_python_plugin_package_idx} _python_plugin_package_target)
        list(GET _python_plugin_package_ids ${_python_plugin_package_idx} _python_plugin_package_id)
        get_target_property(_python_plugin_package_directory
            ${_python_plugin_package_target} SLIC3R_PLUGIN_PACKAGE_DIRECTORY)
        list(APPEND _python_plugin_copy_commands
            COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${_python_test_runtime_directory}/plugins/${_python_plugin_package_id}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_python_plugin_package_directory}"
                "${_python_test_runtime_directory}/plugins/${_python_plugin_package_id}"
        )
    endforeach()

    add_custom_target(${_TEST_NAME}_python_runtime
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${_python_test_runtime_directory}/plugins/python"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${_python_test_runtime_directory}/plugins/python"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:python_plugin_loader>"
            "${_python_test_runtime_directory}/plugins/python/$<TARGET_FILE_NAME:python_plugin_loader>"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${_python_test_runtime_directory}/plugins/python/python"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_FILE_DIR:python_plugin_loader>/python"
            "${_python_test_runtime_directory}/plugins/python/python"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${_python_test_runtime_directory}/plugins/python.external.package_root"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/pure_python_package"
            "${_python_test_runtime_directory}/plugins/python.external.package_root"
        ${_python_plugin_copy_commands}
        COMMENT "Copy Python plugin runtime to plugin tests"
        VERBATIM
    )
    add_dependencies(${_TEST_NAME}_python_runtime python_plugin_loader ${_python_plugin_package_targets})
    add_dependencies(${_TEST_NAME}_tests ${_TEST_NAME}_python_runtime)

    # The test executable exports the host ABI itself. Remove the application
    # DLL after the normal runtime-copy commands so the Python bridge resolves
    # registration functions from the test process.
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/Slic3r.dll"
        VERBATIM
    )
endif()

add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "[plugins]" ${CATCH_EXTRA_ARGS})
