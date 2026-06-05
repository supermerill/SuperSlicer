get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    ../../src/plugins_cpp/DenseInfill/DenseInfill.cpp
    ../../src/plugins_cpp/ExtraPerimeterOverhangWave/ExtraPerimeterOverhangWave.cpp
    ../../src/plugins_cpp/FlatAreaLayerHeight/FlatAreaLayerHeight.cpp
    ../../src/plugins_cpp/Polyholes/Polyholes.cpp
    perimeter_test_helpers.cpp
    plugin_test_helpers.cpp
    test_clipper_views.cpp
    test_extrusion_continuity.cpp
    test_plugin_property.cpp
    test_gcode_printing_plan_file_writer.cpp
    test_flat_area_layer_height.cpp
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

target_link_libraries(${_TEST_NAME}_tests test_common test_common_data libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    prusaslicer_copy_dlls(${_TEST_NAME}_tests)
    set_target_properties(${_TEST_NAME}_tests PROPERTIES ENABLE_EXPORTS ON)
endif()

if (TARGET python_plugin_loader)
    add_dependencies(${_TEST_NAME}_tests python_plugin_loader)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE SLIC3R_TEST_PYTHON_PLUGINS)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:python_plugin_loader>"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/plugins/$<TARGET_FILE_NAME:python_plugin_loader>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_FILE_DIR:python_plugin_loader>/python"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/plugins/python"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/plugins/python/plugins/default_surface_generator.py"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/plugins/python/plugins/simple_perimeter_generator2.py"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/Slic3r.dll"
        COMMENT "Copy Python plugin runtime to plugin tests"
        VERBATIM
    )
endif()

add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "[plugins]" ${CATCH_EXTRA_ARGS})
