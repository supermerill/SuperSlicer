#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/ Copyright (c) Prusa Research 2018 - 2023 Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena, Filip Sykala @Jony01, David Kocík @kocikdav, Vojtěch Král @vojtechkral
#/|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
#/|/ Copyright (c) 2023 Mimoja @Mimoja
#/|/ Copyright (c) 2022 ole00 @ole00
#/|/
#/|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/
cmake_minimum_required(VERSION 3.13)
project(libslic3r)

if(NOT WIN32)
    # Add DEBUG flags to debug builds.
    add_compile_options(${_CC_DEBUG_FLAGS})
endif()

include(PrecompiledHeader)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libslic3r_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h @ONLY)

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

set(OpenVDBUtils_SOURCES "")
if (TARGET OpenVDB::openvdb)
    set(OpenVDBUtils_SOURCES OpenVDBUtils.cpp OpenVDBUtils.hpp OpenVDBUtilsLegacy.hpp)
endif()

find_package(LibBGCode REQUIRED COMPONENTS Convert)
slic3r_remap_configs(LibBGCode::bgcode_core RelWithDebInfo Release)
slic3r_remap_configs(LibBGCode::bgcode_binarize RelWithDebInfo Release)
slic3r_remap_configs(LibBGCode::bgcode_convert RelWithDebInfo Release)

set(SLIC3R_SOURCES
    pchheader.cpp
    pchheader.hpp
    AStar.hpp
    AABBTreeIndirect.hpp
    AABBTreeLines.hpp
    AABBMesh.hpp
    AABBMesh.cpp
    Algorithm/PathSorting.hpp
    Algorithm/RegionExpansion.hpp
    Algorithm/RegionExpansion.cpp
	AnyPtr.hpp
	Api/host/ApiHostUtils.hpp
	Api/host/GCodeScriptProcessor.cpp
	Api/host/GCodeScriptProcessor.hpp
	Api/host/ClipperShapes.cpp
	Api/host/ClipperShapes.hpp
	Api/host/Orchestrator.cpp
	Api/host/Orchestrator.hpp
	Api/host/Plugin.cpp
	Api/host/Plugin.hpp
	Api/host/slic3r_bridge_detector.cpp
	Api/host/slic3r_clipper.cpp
	Api/host/slic3r_config.cpp
	Api/host/slic3r_config_def.cpp
	Api/host/slic3r_config_option.cpp
	Api/host/slic3r_data_tree.cpp
	Api/host/slic3r_extrusion_entity.cpp
	Api/host/slic3r_extrusion_split.cpp
	Api/host/slic3r_extrusion_polyline.cpp
	Api/host/slic3r_extrusion_property.cpp
	Api/host/slic3r_geometry.cpp
	Api/host/slic3r_orchestrator.cpp
	Api/host/slic3r_plugin.cpp
	Api/host/slic3r_printing_plan.cpp
	Api/host/slic3r_utils.cpp
	Api/host/slic3r_volume.cpp
	Api/host/steps/BridgeDetectorStep.cpp
	Api/host/steps/LayerHeightStep.cpp
	Api/host/steps/LayerHeightStep.hpp
	Api/host/steps/PostSlicingStep.cpp
	Api/host/steps/PostSlicingStep.hpp
	Api/host/steps/SlicingStep.cpp
	Api/host/steps/SlicingStep.hpp
	Api/host/steps/SupportDemandStep.cpp
	Api/host/steps/SupportDemandStep.hpp
	Api/internal/ArcPolylineAccess.hpp
	Api/internal/ExtrusionPropertyAccess.hpp
	Api/internal/LayerAccess.hpp
	Api/internal/LayerIslandAccess.hpp
	Api/internal/LayerRegionAccess.hpp
	Api/internal/PrintAccess.hpp
	Api/internal/PrintObjectAccess.hpp
	Api/plugin/c/slic3r_bridge_detector.h
	Api/plugin/c/slic3r_clipper.h
	Api/plugin/c/slic3r_config.h
	Api/plugin/c/slic3r_config_def.h
	Api/plugin/c/slic3r_config_option.h
	Api/plugin/c/slic3r_config_option_type.h
	Api/plugin/c/slic3r_config_types.h
	Api/plugin/c/slic3r_data_tree.h
	Api/plugin/c/slic3r_def.h
	Api/plugin/c/slic3r_extrusion_entity.h
	Api/plugin/c/slic3r_extrusion_polyline.h
	Api/plugin/c/slic3r_extrusion_property.h
	Api/plugin/c/slic3r_extrusions.h
	Api/plugin/c/slic3r_geometry.h
	Api/plugin/c/slic3r_gcode_firmware.h
	Api/plugin/c/slic3r_gcode_script.h
	Api/plugin/c/slic3r_orchestrator.h
	Api/plugin/c/slic3r_plugin.h
	Api/plugin/c/slic3r_plugin_run_context.h
	Api/plugin/c/slic3r_plugin_types.h
	Api/plugin/c/slic3r_printing_plan.h
	Api/plugin/c/slic3r_slicing_step.h
	Api/plugin/c/slic3r_utils.h
	Api/plugin/c/slic3r_volume.h
	Api/plugin/c/steps/slic3r_step_bridge_detector.h
	Api/plugin/c/steps/slic3r_step_common.h
	Api/plugin/c/steps/slic3r_step_extrusion_edit.h
	Api/plugin/c/steps/slic3r_step_extrusion_simplification.h
	Api/plugin/c/steps/slic3r_step_gcode.h
	Api/plugin/c/steps/slic3r_step_gcode_firmware.h
	Api/plugin/c/steps/slic3r_step_infill.h
	Api/plugin/c/steps/slic3r_step_infill_group.h
	Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h
	Api/plugin/c/steps/slic3r_step_layer_height.h
	Api/plugin/c/steps/slic3r_step_layer_stiching.h
	Api/plugin/c/steps/slic3r_step_ordering.h
	Api/plugin/c/steps/slic3r_step_perimeter.h
	Api/plugin/c/steps/slic3r_step_post_infill.h
	Api/plugin/c/steps/slic3r_step_post_perimeter.h
	Api/plugin/c/steps/slic3r_step_post_slicing.h
	Api/plugin/c/steps/slic3r_step_pre_gcode.h
	Api/plugin/c/steps/slic3r_step_pre_infill.h
	Api/plugin/c/steps/slic3r_step_pre_perimeter.h
	Api/plugin/c/steps/slic3r_step_slicing.h
	Api/plugin/c/steps/slic3r_step_support.h
	Api/plugin/c/steps/slic3r_step_support_demand.h
	Api/plugin/c/steps/slic3r_step_support_spot.h
	Api/plugin/c/steps/slic3r_step_surface_generation.h
	Api/plugin/c/steps/slic3r_step_skirt_brim.h
	Api/plugin/c/steps/slic3r_step_wipetower.h
	Api/plugin/cpp/BridgeDetectorViews.hpp
	Api/plugin/cpp/ConfigViews.hpp
	Api/plugin/cpp/AuxiliaryLayerHelpers.cpp
	Api/plugin/cpp/AuxiliaryLayerHelpers.hpp
	Api/plugin/cpp/ClipperViews.hpp
	Api/plugin/cpp/DataTreeViews.hpp
	Api/plugin/cpp/ExtrusionTreeVisitors.hpp
	Api/plugin/cpp/ExtrusionViews.hpp
	Api/plugin/cpp/GeometryViews.hpp
	Api/plugin/cpp/PluginPropertyKey.hpp
	Api/plugin/cpp/properties/DataTreeProperties.hpp
	Api/plugin/cpp/properties/ExtrusionProperties.hpp
	Api/plugin/cpp/gcode/EncodableState.hpp
	Api/plugin/cpp/gcode/DefaultExtruder.cpp
	Api/plugin/cpp/gcode/DefaultExtruder.hpp
	Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.cpp
	Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp
	Api/plugin/cpp/gcode/SingleAccelerationRegisterGCodeFirmwareSession.cpp
	Api/plugin/cpp/gcode/SingleAccelerationRegisterGCodeFirmwareSession.hpp
	Api/plugin/cpp/gcode/ExtrusionAxisState.cpp
	Api/plugin/cpp/gcode/ExtrusionAxisState.hpp
	Api/plugin/cpp/gcode/FanState.cpp
	Api/plugin/cpp/gcode/FanState.hpp
	Api/plugin/cpp/gcode/Gantry.cpp
	Api/plugin/cpp/gcode/Gantry.hpp
	Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp
	Api/plugin/cpp/gcode/GCodeFormatter.cpp
	Api/plugin/cpp/gcode/GCodeFormatter.hpp
	Api/plugin/cpp/gcode/GCodeLineParser.cpp
	Api/plugin/cpp/gcode/GCodeLineParser.hpp
	Api/plugin/cpp/gcode/GCodeScriptProcessorViews.hpp
	Api/plugin/cpp/gcode/HeaterState.cpp
	Api/plugin/cpp/gcode/HeaterState.hpp
	Api/plugin/cpp/gcode/MachineEnvelope.cpp
	Api/plugin/cpp/gcode/MachineEnvelope.hpp
	Api/plugin/cpp/gcode/PressureAdvanceState.cpp
	Api/plugin/cpp/gcode/PressureAdvanceState.hpp
	Api/plugin/cpp/gcode/Printer.cpp
	Api/plugin/cpp/gcode/Printer.hpp
	Api/plugin/cpp/gcode/Tool.cpp
	Api/plugin/cpp/gcode/Tool.hpp
	Api/plugin/cpp/LineDistancer.hpp
	Api/plugin/cpp/ParallelFor.hpp
	Api/plugin/cpp/PerimeterStepViews.hpp
	Api/plugin/cpp/PluginBase.hpp
	Api/plugin/cpp/PluginContext.hpp
	Api/plugin/cpp/PrintingPlanViews.hpp
	Api/plugin/cpp/PrintHelpers.cpp
	Api/plugin/cpp/PrintHelpers.hpp
	Api/plugin/cpp/RegionSettingsViews.hpp
	Api/plugin/cpp/SkirtBrimStepViews.hpp
	Api/plugin/cpp/VolumeViews.hpp
	Api/plugin/cpp/Views.cpp
	Api/plugin/cpp/Views.hpp
	Plugins/BridgeDetector.cpp
	Plugins/BridgeDetector.hpp
	Plugins/GCode/Firmware/BuiltinGCodeFirmwares.cpp
	Plugins/GCode/Firmware/BuiltinGCodeFirmwares.hpp
	Plugins/GCode/Firmware/KlipperGCodeFirmware.cpp
	Plugins/GCode/Firmware/KlipperGCodeFirmware.hpp
	Plugins/GCode/Firmware/Marlin1GCodeFirmware.cpp
	Plugins/GCode/Firmware/Marlin1GCodeFirmware.hpp
	Plugins/GCode/Firmware/Marlin2GCodeFirmware.cpp
	Plugins/GCode/Firmware/Marlin2GCodeFirmware.hpp
	Plugins/GCode/Firmware/PrusaGCodeFirmware.cpp
	Plugins/GCode/Firmware/PrusaGCodeFirmware.hpp
	Plugins/GCode/Firmware/RepRapGCodeFirmware.cpp
	Plugins/GCode/Firmware/RepRapGCodeFirmware.hpp
	Plugins/GCode/Firmware/SprinterGCodeFirmware.cpp
	Plugins/GCode/Firmware/SprinterGCodeFirmware.hpp
	Plugins/GCode/FeatureGCode.cpp
	Plugins/GCode/FeatureGCode.hpp
	Plugins/GCode/CustomGCodePerPrintZ.cpp
	Plugins/GCode/CustomGCodePerPrintZ.hpp
	Plugins/GCode/CustomGCodePerPrintZOrdering.cpp
	Plugins/GCode/CustomGCodePerPrintZRecord.cpp
	Plugins/GCode/CustomGCodePerPrintZRecord.hpp
	Plugins/GCode/LegacyGCodeGenerator.cpp
	Plugins/GCode/LegacyGCodeGenerator.hpp
	Plugins/GCode/PrintingPlanFileWriter.cpp
	Plugins/GCode/PrintingPlanFileWriter.hpp
	Plugins/GCode/SettingsGCodeScripts.cpp
	Plugins/GCode/SettingsGCodeScripts.hpp
	Plugins/GuiRulesExample.cpp
	Plugins/GuiRulesExample.hpp
	Plugins/Infill/DefaultInfillGenerator.cpp
	Plugins/Infill/DefaultInfillGenerator.hpp
	Plugins/Infill/LegacyInfillPatterns.cpp
	Plugins/Infill/LegacyInfillPatterns.hpp
	Plugins/Infill/PostInfillGapFill.cpp
	Plugins/Infill/PostInfillGapFill.hpp
	Plugins/LayerExtrusionEdit/DefaultAcceleration.cpp
	Plugins/LayerExtrusionEdit/DefaultAcceleration.hpp
	Plugins/LayerExtrusionEdit/DefaultSpeed.cpp
	Plugins/LayerExtrusionEdit/DefaultSpeed.hpp
	Plugins/LayerExtrusionEdit/ExtrusionProcessParameterHelpers.cpp
	Plugins/LayerExtrusionEdit/ExtrusionProcessParameterHelpers.hpp
	Plugins/LayerExtrusionEdit/RegionalProcessParameterHelpers.cpp
	Plugins/LayerExtrusionEdit/RegionalProcessParameterHelpers.hpp
	Plugins/MaxOverhangThreshold.cpp
	Plugins/MaxOverhangThreshold.hpp
	Plugins/Ordering/DefaultExtrusionTreeOrdering.cpp
	Plugins/Ordering/DefaultOrdering.cpp
	Plugins/Ordering/DefaultOrdering.hpp
	Plugins/Ordering/DefaultPlanBuilder.cpp
	Plugins/Ordering/DefaultToolGroupOrdering.cpp
	Plugins/PrintingPlan/PrintingLayerTimeProperty.h
	Plugins/SkirtBrim/AdhesionLayerHelpers.cpp
	Plugins/SkirtBrim/AdhesionLayerHelpers.hpp
	Plugins/SkirtBrim/DefaultBrimGenerator.cpp
	Plugins/SkirtBrim/DefaultBrimGenerator.hpp
	Plugins/SkirtBrim/DefaultBrimSkirtTrim.cpp
	Plugins/SkirtBrim/DefaultBrimSkirtTrim.hpp
	Plugins/SkirtBrim/DefaultSkirtGenerator.cpp
	Plugins/SkirtBrim/DefaultSkirtGenerator.hpp
	Plugins/VaseMultiIslandConnector.cpp
	Plugins/VaseMultiIslandConnector.hpp
	Plugins/Perimeter/ArachnePerimeterGenerator.cpp
	Plugins/Perimeter/ArachnePerimeterGenerator.hpp
	Plugins/Perimeter/ClassicPerimeterGenerator.cpp
	Plugins/Perimeter/ClassicPerimeterGenerator.hpp
	Plugins/Perimeter/ExtraPerimeterBelowArea.cpp
	Plugins/Perimeter/ExtraPerimeterBelowArea.hpp
	Plugins/Perimeter/ExtraPerimeterCount.cpp
	Plugins/Perimeter/ExtraPerimeterCount.hpp
	Plugins/Perimeter/ExtraPerimeterOddLayer.cpp
	Plugins/Perimeter/ExtraPerimeterOddLayer.hpp
	Plugins/Perimeter/DetectOverhang.cpp
	Plugins/Perimeter/DetectOverhang.hpp
	Plugins/Perimeter/ExtraPerimetersOnOverhangs.cpp
	Plugins/Perimeter/ExtraPerimetersOnOverhangs.hpp
	Plugins/Perimeter/FuzzySkin.cpp
	Plugins/Perimeter/FuzzySkin.hpp
	Plugins/Perimeter/MarkFirstLoop.cpp
	Plugins/Perimeter/MarkFirstLoop.hpp
	Plugins/Perimeter/OnlyOnePerimeterFirstLayer.cpp
	Plugins/Perimeter/OnlyOnePerimeterFirstLayer.hpp
	Plugins/Perimeter/OnlyOnePerimeterOnTop.cpp
	Plugins/Perimeter/OnlyOnePerimeterOnTop.hpp
	Plugins/Perimeter/RemoveGapFillOnOverhangs.cpp
	Plugins/Perimeter/RemoveGapFillOnOverhangs.hpp
	Plugins/Perimeter/SeparateHoleContour.cpp
	Plugins/Perimeter/SeparateHoleContour.hpp
	Plugins/Perimeter/SimplePerimeterGenerator.cpp
	Plugins/Perimeter/SimplePerimeterGenerator.hpp
	Plugins/PluginActivationConfig.cpp
	Plugins/PluginActivationConfig.hpp
	Plugins/PluginBinaryMetadata.hpp
	Plugins/PluginLoader.cpp
	Plugins/PluginLoader.hpp
	Plugins/PluginRepository.cpp
	Plugins/PluginRepository.hpp
	Plugins/SliceVolume.cpp
	Plugins/SliceVolume.hpp
	Plugins/StandardLayerHeightGenerator.cpp
	Plugins/StandardLayerHeightGenerator.hpp
	Plugins/Surface/CleanInfillSurfaces.cpp
	Plugins/Surface/CleanInfillSurfaces.hpp
	Plugins/Surface/InitialTypedSurfaceBuilder.cpp
	Plugins/Surface/InitialTypedSurfaceBuilder.hpp
	Plugins/Surface/InfillRegionCompatibilitySplitter.cpp
	Plugins/Surface/InfillRegionCompatibilitySplitter.hpp
	Plugins/Surface/SolidShells.cpp
	Plugins/Surface/SolidShells.hpp
	Plugins/Surface/TopSurfaceExpansion.cpp
	Plugins/Surface/TopSurfaceExpansion.hpp
	Plugins/Support/SupportDemandBridgeRemoval.cpp
	Plugins/Support/SupportDemandBridgeRemoval.hpp
	Plugins/Support/SupportDemandModifiers.cpp
	Plugins/Support/SupportDemandModifiers.hpp
	Plugins/Support/SupportDemandOverhangs.cpp
	Plugins/Support/SupportDemandOverhangs.hpp
	Plugins/Support/SupportDemandPainting.cpp
	Plugins/Support/SupportDemandPainting.hpp
	Printing/PrintingPlan.cpp
	Printing/PrintingPlan.hpp
	Steps/DebugPrintProcessComparator.cpp
	Steps/DebugPrintProcessComparator.hpp
	Steps/StepDetectSupportSpots.cpp
	Steps/StepDetectSupportSpots.hpp
	Steps/StepSkirtBrim.cpp
	Steps/StepSkirtBrim.hpp
	Steps/StepExtrusionEdition.cpp
	Steps/StepExtrusionEdition.hpp
	Steps/StepExtrusionOrdering.cpp
	Steps/StepExtrusionOrdering.hpp
	Steps/StepExtrusionSimplification.cpp
	Steps/StepExtrusionSimplification.hpp
	Steps/StepGenerateGcode.cpp
	Steps/StepGenerateGcode.hpp
	Steps/StepGenerateInfill.cpp
	Steps/StepGenerateInfill.hpp
	Steps/StepGeneratePerimeter.cpp
	Steps/StepGeneratePerimeter.hpp
	Steps/StepGenerateSupport.cpp
	Steps/StepGenerateSupport.hpp
	Steps/StepGenerateWipeTower.cpp
	Steps/StepGenerateWipeTower.hpp
	Steps/StepGroupInfillRegions.cpp
	Steps/StepGroupInfillRegions.hpp
	Steps/StepLayerExtrusionEdition.cpp
	Steps/StepLayerExtrusionEdition.hpp
	Steps/StepLayerHeightGeneration.cpp
	Steps/StepLayerHeightGeneration.hpp
	Steps/StepLayerStiching.cpp
	Steps/StepLayerStiching.hpp
	Steps/StepPostInfillGeneration.cpp
	Steps/StepPostInfillGeneration.hpp
	Steps/StepPostPerimeterGeneration.cpp
	Steps/StepPostPerimeterGeneration.hpp
	Steps/StepPostSlicing.cpp
	Steps/StepPostSlicing.hpp
	Steps/StepPipeline.cpp
	Steps/StepPipeline.hpp
	Steps/StepPrepareForPeriemters.cpp
	Steps/StepPrepareForPeriemters.hpp
	Steps/StepPrepareGcode.cpp
	Steps/StepPrepareGcode.hpp
	Steps/StepPrepareInfill.cpp
	Steps/StepPrepareInfill.hpp
	Steps/StepSlicing.cpp
	Steps/StepSlicing.hpp
	Steps/StepRunner.hpp
	Steps/StepSupportDemand.cpp
	Steps/StepSupportDemand.hpp
	Steps/StepSurfaceGeneration.cpp
	Steps/StepSurfaceGeneration.hpp
	Updater/Http.cpp
	Updater/Http.hpp
	Updater/PluginUpdater.cpp
	Updater/PluginUpdater.hpp
	Updater/PresetUpdater.cpp
	Updater/PresetUpdater.hpp
	Updater/RepositoryCacheIO.cpp
	Updater/RepositoryCacheIO.hpp
	Updater/RepositoryChangelogService.cpp
	Updater/RepositoryChangelogService.hpp
	Updater/RepositoryPackageCache.cpp
	Updater/RepositoryPackageCache.hpp
	Updater/RepositoryTagService.cpp
	Updater/RepositoryTagService.hpp
	Updater/RepositoryUpdater.cpp
	Updater/RepositoryUpdater.hpp
	Updater/UpdaterHttp.cpp
	Updater/UpdaterHttp.hpp
	Updater/UpdaterError.hpp
	Updater/UpdaterOperationExecutor.cpp
	Updater/UpdaterOperationExecutor.hpp
    BoundingBox.cpp
    BoundingBox.hpp
    BridgeDetector.cpp
    BridgeDetector.hpp
    Brim.cpp
    Brim.hpp
    BuildVolume.cpp
    BuildVolume.hpp
    BoostAdapter.hpp
    clipper.cpp
    clipper.hpp
    ClipperUtils.cpp
    ClipperUtils.hpp
    ClipperZUtils.hpp
    Color.cpp
    Color.hpp
    Config/ConfigDef.cpp
    Config/ConfigDef.hpp
    Config/ConfigOption.cpp
    Config/ConfigOption.hpp
    Config/ConfigSnapshotSerialization.cpp
    Config/ConfigSnapshotSerialization.hpp
    ContainerUtils.hpp
    CSGMesh/CSGMesh.hpp
    CSGMesh/SliceCSGMesh.hpp
    CSGMesh/ModelToCSGMesh.hpp
    CSGMesh/PerformCSGMeshBooleans.hpp
    CSGMesh/VoxelizeCSGMesh.hpp
    CSGMesh/TriangleMeshAdapter.hpp
    CSGMesh/CSGMeshCopy.hpp
    DataTreeFwd.hpp
    EdgeGrid.cpp
    EdgeGrid.hpp
    ElephantFootCompensation.cpp
    ElephantFootCompensation.hpp
    Emboss.cpp
    Emboss.hpp
    EmbossShape.hpp
    enum_bitmask.hpp
    ExPolygon.cpp
    ExPolygon.hpp
    ExPolygonsIndex.cpp
    ExPolygonsIndex.hpp
    Extruder.cpp
    Extruder.hpp
    ExtrusionEntity.cpp
    ExtrusionEntity.hpp
    ExtrusionEntityCollection.cpp
    ExtrusionEntityCollection.hpp
    ExtrusionEntityVisitors.cpp
    ExtrusionEntityVisitors.hpp
    ExtrusionProperty.cpp
    ExtrusionProperty.hpp
    ExtrusionRole.cpp
    ExtrusionRole.hpp
    ExtrusionSimulator.cpp
    ExtrusionSimulator.hpp
    Config/FFFPrintConfig.cpp
    Config/FFFPrintConfig.hpp
    FileParserError.hpp
    FilesystemTransaction.cpp
    FilesystemTransaction.hpp
    FilesystemTransactionTest.hpp
    Fill/Fill.cpp
    Fill/Fill3DHoneycomb.cpp
    Fill/Fill3DHoneycomb.hpp
    Fill/FillAdaptive.cpp
    Fill/FillAdaptive.hpp
    Fill/FillBase.cpp
    Fill/FillBase.hpp
    Fill/FillConcentric.cpp
    Fill/FillConcentric.hpp
    Fill/FillEnsuring.cpp
    Fill/FillEnsuring.hpp
    Fill/FillHoneycomb.cpp
    Fill/FillHoneycomb.hpp
    Fill/FillGyroid.cpp
    Fill/FillGyroid.hpp
    Fill/FillPlanePath.cpp
    Fill/FillPlanePath.hpp
    Fill/FillLine.cpp
    Fill/FillLine.hpp
    Fill/FillLightning.cpp
    Fill/FillLightning.hpp
    Fill/Lightning/DistanceField.cpp
    Fill/Lightning/DistanceField.hpp
    Fill/Lightning/Generator.cpp
    Fill/Lightning/Generator.hpp
    Fill/Lightning/LightningLayer.cpp
    Fill/Lightning/LightningLayer.hpp
    Fill/Lightning/TreeNode.cpp
    Fill/Lightning/TreeNode.hpp
    Fill/FillRectilinear.cpp
    Fill/FillRectilinear.hpp
    Fill/FillSmooth.cpp
    Fill/FillSmooth.hpp
    Flow.cpp
    Flow.hpp
    format.hpp
    Format/3mf.cpp
    Format/3mf.hpp
    Format/AMF.cpp
    Format/AMF.hpp
    Format/BBConfig.cpp
    Format/BBConfig.hpp
    Format/bbs_3mf.hpp
    Format/bbs_3mf.cpp
    Format/OBJ.cpp
    Format/OBJ.hpp
    Format/objparser.cpp
    Format/objparser.hpp
    Format/STL.cpp
    Format/STL.hpp
    Format/SLAArchiveWriter.hpp
    Format/SLAArchiveWriter.cpp
    Format/SLAArchiveReader.hpp
    Format/SLAArchiveReader.cpp
    Format/ZipperArchiveImport.hpp
    Format/ZipperArchiveImport.cpp
    Format/SL1.hpp
    Format/SL1.cpp
    Format/SL1_SVG.hpp
    Format/SL1_SVG.cpp
    Format/HFP.cpp
    Format/HFP.hpp
    Format/AnycubicSLA.hpp
    Format/AnycubicSLA.cpp
    Format/CWS.hpp
    Format/CWS.cpp
    Format/STEP.hpp
    Format/STEP.cpp
    Format/SVG.hpp
    Format/SVG.cpp
    Format/SLAArchiveFormatRegistry.hpp
    Format/SLAArchiveFormatRegistry.cpp
    GCode/ThumbnailData.cpp
    GCode/ThumbnailData.hpp
    GCode/Thumbnails.cpp
    GCode/Thumbnails.hpp
    GCode/ConflictChecker.cpp
    GCode/ConflictChecker.hpp
    GCode/CoolingBuffer.cpp
    GCode/CoolingBuffer.hpp
    GCode/ExtrusionProcessor.cpp
    GCode/ExtrusionProcessor.hpp
    GCode/FanMover.cpp
    GCode/FanMover.hpp
    GCode/FindReplace.cpp
    GCode/FindReplace.hpp
    GCode/LabelObjects.cpp
    GCode/LabelObjects.hpp
    GCode/GCodeFormatter.cpp
    GCode/GCodeFormatter.hpp
    GCode/GCodeWriter.cpp
    GCode/GCodeWriter.hpp
    GCode/PostProcessor.cpp
    GCode/PostProcessor.hpp
    GCode/PressureEqualizer.cpp
    GCode/PressureEqualizer.hpp
    GCode/PrintExtents.cpp
    GCode/PrintExtents.hpp
    GCode/RetractWhenCrossingPerimeters.cpp
    GCode/RetractWhenCrossingPerimeters.hpp
    GCode/SpiralVase.cpp
    GCode/SpiralVase.hpp
    GCode/SeamPlacer.cpp
    GCode/SeamPlacer.hpp
    #GCode/SmoothPath.cpp
    #GCode/SmoothPath.hpp
    GCode/TemperatureMover.cpp
    GCode/TemperatureMover.hpp
    GCode/ToolOrdering.cpp
    GCode/ToolOrdering.hpp
    GCode/Wipe.cpp
    GCode/Wipe.hpp
    GCode/WipeTower.cpp
    GCode/WipeTower.hpp
    GCode/WipeTower2.cpp
    GCode/WipeTower2.hpp
    GCode/WipeTowerIntegration.cpp
    GCode/WipeTowerIntegration.hpp
    GCode/GCodeProcessor.cpp
    GCode/GCodeProcessor.hpp
    GCode/AvoidCrossingPerimeters.cpp
    GCode/AvoidCrossingPerimeters.hpp
    GCode/Travels.cpp
    GCode/Travels.hpp
    GCode.cpp
    GCode.hpp
    GCodeReader.cpp
    GCodeReader.hpp
    # GCodeSender.cpp
    # GCodeSender.hpp
    Geometry.cpp
    Geometry.hpp
    Geometry/ArcFitter.cpp
    Geometry/ArcFitter.hpp
    Geometry/ArcWelder.cpp
    Geometry/ArcWelder.hpp
    Geometry/Bicubic.hpp
    Geometry/Circle.cpp
    Geometry/Circle.hpp
    Geometry/ConvexHull.cpp
    Geometry/ConvexHull.hpp
    Geometry/Curves.hpp
    Geometry/MedialAxis.cpp
    Geometry/MedialAxis.hpp
    Geometry/Voronoi.hpp
    Geometry/VoronoiOffset.cpp
    Geometry/VoronoiOffset.hpp
    Geometry/VoronoiUtils.hpp
    Geometry/VoronoiUtils.cpp
    Geometry/VoronoiVisualUtils.hpp
    Int128.hpp
    JumpPointSearch.cpp
    JumpPointSearch.hpp
    KDTreeIndirect.hpp
    Layer.cpp
    Layer.hpp
    LayerRegion.cpp
    LayerRegion.hpp
    libslic3r.h
    "${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h"
    Line.cpp
    Line.hpp
    Log.hpp
    Log.cpp
    BlacklistedLibraryCheck.cpp
    BlacklistedLibraryCheck.hpp
    LocalesUtils.cpp
    LocalesUtils.hpp
    CutUtils.cpp
    CutUtils.hpp
    Milling/MillingPostProcess.cpp
    Milling/MillingPostProcess.hpp
    Model.cpp
    Model.hpp
    ModelArrange.hpp
    ModelArrange.cpp
    MultiMaterialSegmentation.cpp
    MultiMaterialSegmentation.hpp
    MeshNormals.hpp
    MeshNormals.cpp
    Measure.hpp
    Measure.cpp
    MeasureUtils.hpp
    CustomGCode.cpp
    CustomGCode.hpp
    Arrange/Arrange.hpp
    Arrange/ArrangeImpl.hpp
    Arrange/Items/ArrangeItem.hpp
    Arrange/Items/ArrangeItem.cpp
    Arrange/Items/SimpleArrangeItem.hpp
    Arrange/Items/SimpleArrangeItem.cpp
    Arrange/Items/TrafoOnlyArrangeItem.hpp
    Arrange/Items/MutableItemTraits.hpp
    Arrange/Items/ArbitraryDataStore.hpp
    Arrange/ArrangeSettingsView.hpp
    Arrange/ArrangeSettingsDb_AppCfg.hpp
    Arrange/ArrangeSettingsDb_AppCfg.cpp
    Arrange/Scene.hpp
    Arrange/Scene.cpp
    Arrange/SceneBuilder.hpp
    Arrange/SceneBuilder.cpp
    Arrange/Tasks/ArrangeTask.hpp
    Arrange/Tasks/ArrangeTaskImpl.hpp
    Arrange/Tasks/FillBedTask.hpp
    Arrange/Tasks/FillBedTaskImpl.hpp
    Arrange/Tasks/MultiplySelectionTask.hpp
    Arrange/Tasks/MultiplySelectionTaskImpl.hpp
    Arrange/SegmentedRectangleBed.hpp
    Arrange/Core/ArrangeItemTraits.hpp
    Arrange/Core/DataStoreTraits.hpp
    Arrange/Core/ArrangeBase.hpp
    Arrange/Core/PackingContext.hpp
    Arrange/Core/ArrangeFirstFit.hpp
    Arrange/Core/Beds.hpp
    Arrange/Core/Beds.cpp
    Arrange/Core/NFP/NFP.hpp
    Arrange/Core/NFP/NFP.cpp
    Arrange/Core/NFP/NFPConcave_CGAL.hpp
    Arrange/Core/NFP/NFPConcave_CGAL.cpp
    Arrange/Core/NFP/NFPConcave_Tesselate.hpp
    Arrange/Core/NFP/NFPConcave_Tesselate.cpp
    Arrange/Core/NFP/EdgeCache.hpp
    Arrange/Core/NFP/EdgeCache.cpp
    Arrange/Core/NFP/CircularEdgeIterator.hpp
    Arrange/Core/NFP/NFPArrangeItemTraits.hpp
    Arrange/Core/NFP/PackStrategyNFP.hpp
    Arrange/Core/NFP/RectangleOverfitPackingStrategy.hpp
    Arrange/Core/NFP/Kernels/KernelTraits.hpp
    Arrange/Core/NFP/Kernels/GravityKernel.hpp
    Arrange/Core/NFP/Kernels/TMArrangeKernel.hpp
    Arrange/Core/NFP/Kernels/CompactifyKernel.hpp
    Arrange/Core/NFP/Kernels/RectangleOverfitKernelWrapper.hpp
    Arrange/Core/NFP/Kernels/SVGDebugOutputKernelWrapper.hpp
    Arrange/Core/NFP/Kernels/KernelUtils.hpp
    MultiPoint.cpp
    MultiPoint.hpp
    MutablePriorityQueue.hpp
    NormalUtils.cpp
    NormalUtils.hpp
    NumericUtils.hpp
    NSVGUtils.cpp
    NSVGUtils.hpp
    ObjectID.cpp
    ObjectID.hpp
    PerimeterGenerator.cpp
    PerimeterGenerator.hpp
    PlaceholderParser.cpp
    PlaceholderParser.hpp
    PluginProperty.cpp
    PluginProperty.hpp
    PropertyStorage.cpp
    PropertyStorage.hpp
    Platform.cpp
    Platform.hpp
    Point.cpp
    Point.hpp
    PointSerialize.hpp
    PointUtils.cpp
    PointUtils.hpp
    Polygon.cpp
    Polygon.hpp
    MutablePolygon.cpp
    MutablePolygon.hpp
    PolygonTrimmer.cpp
    PolygonTrimmer.hpp
    Polyline.cpp
    Polyline.hpp
    Config/Preset.cpp
    Config/Preset.hpp
    Config/PresetBundle.cpp
    Config/PresetBundle.hpp
    PrincipalComponents2D.hpp
    PrincipalComponents2D.cpp
    Config/AppConfig.cpp
    Config/AppConfig.hpp
    UiLayoutMerger.cpp
    UiLayoutMerger.hpp
    Print.cpp
    Print.hpp
    PrintApply.cpp
    PrintRecords.cpp
    PrintRecords.hpp
    PrintBase.cpp
    PrintBase.hpp
    Config/PrintConfig.cpp
    Config/PrintConfig.hpp
    PrintObject.cpp
    PrintObject.hpp
    PrintObjectRegion.cpp
    PrintObjectRegion.hpp
    PrintObjectSlice.cpp
    PrintRegion.cpp
    PrintRegion.hpp
    PrintSteps.cpp
    PrintSteps.hpp
    PointGrid.hpp
    PNGReadWrite.hpp
    PNGReadWrite.cpp
    QuadricEdgeCollapse.cpp
    QuadricEdgeCollapse.hpp
    RegionSettings.cpp
    RegionSettings.hpp
    Semver.cpp
    Semver.hpp
    ShortEdgeCollapse.cpp
    ShortEdgeCollapse.hpp
    ShortestPath.cpp
    ShortestPath.hpp
    SLAPrint.cpp
    SLAPrintSteps.cpp
    SLAPrintSteps.hpp
    SLAPrint.hpp
    Slicing.cpp
    Slicing.hpp
    SlicesToTriangleMesh.hpp
    SlicesToTriangleMesh.cpp
    SlicingAdaptive.cpp
    SlicingAdaptive.hpp
    Subdivide.cpp
    Subdivide.hpp
    Support/SupportCommon.cpp
    Support/SupportCommon.hpp
    Support/SupportDebug.cpp
    Support/SupportDebug.hpp
    Support/SupportLayer.hpp
    Support/SupportMaterial.cpp
    Support/SupportMaterial.hpp
    Support/SupportParameters.cpp
    Support/SupportParameters.hpp
    Support/OrganicSupport.cpp
    Support/OrganicSupport.hpp
    Support/TreeSupport.cpp
    Support/TreeSupport.hpp
    Support/TreeSupportCommon.cpp
    Support/TreeSupportCommon.hpp
    Support/TreeModelVolumes.cpp
    Support/TreeModelVolumes.hpp
    SupportSpotsGenerator.cpp
    SupportSpotsGenerator.hpp
    Surface.cpp
    Surface.hpp
    SurfaceCollection.cpp
    SurfaceCollection.hpp
    SurfaceMesh.hpp
    SVG.cpp
    SVG.hpp
    Technologies.hpp
    Tesselate.cpp
    Tesselate.hpp
    TextConfiguration.hpp
    TriangleMesh.cpp
    TriangleMesh.hpp
    TriangleMeshSlicer.cpp
    TriangleMeshSlicer.hpp
    MeshSplitImpl.hpp
    TriangulateWall.hpp
    utils.cpp
    Utils.hpp
    Time.cpp
    Time.hpp
    Timer.cpp
    Timer.hpp
    Thread.cpp
    Thread.hpp
    TriangleSelector.cpp
    TriangleSelector.hpp
    TriangleSetSampling.cpp
    TriangleSetSampling.hpp
    TriangleSelectorWrapper.cpp
    TriangleSelectorWrapper.hpp
    TypeTraits.hpp
    MTUtils.hpp
    Zipper.hpp
    Zipper.cpp
    Orient.cpp
    Orient.hpp
    MinAreaBoundingBox.hpp
    MinAreaBoundingBox.cpp
    miniz_extension.hpp
    miniz_extension.cpp
    MarchingSquares.hpp
    Execution/Execution.hpp
    Execution/ExecutionSeq.hpp
    Execution/ExecutionTBB.hpp
    Optimize/Optimizer.hpp
    Optimize/NLoptOptimizer.hpp
    Optimize/BruteforceOptimizer.hpp
    ${OpenVDBUtils_SOURCES}
    SLA/Pad.hpp
    SLA/Pad.cpp
    SLA/SupportTreeBuilder.hpp
    SLA/SupportTreeMesher.hpp
    SLA/SupportTreeMesher.cpp
    SLA/SupportTreeUtils.hpp
    SLA/SupportTreeUtilsLegacy.hpp
    SLA/SupportTreeBuilder.cpp
    SLA/SupportTree.hpp
    SLA/SupportTree.cpp
    SLA/SupportTreeStrategies.hpp
#    SLA/SupportTreeIGL.cpp
    SLA/Rotfinder.hpp
    SLA/Rotfinder.cpp
    SLA/SpatIndex.hpp
    SLA/SpatIndex.cpp
    SLA/RasterBase.hpp
    SLA/RasterBase.cpp
    SLA/AGGRaster.hpp
    SLA/RasterToPolygons.hpp
    SLA/RasterToPolygons.cpp
    SLA/SLAPrintConfig.cpp
    SLA/SLAPrintConfig.hpp
    SLA/ConcaveHull.hpp
    SLA/ConcaveHull.cpp
    SLA/DrainHole.hpp
    SLA/Hollowing.hpp
    SLA/Hollowing.cpp
    SLA/JobController.hpp
    SLA/SupportPoint.hpp
    SLA/SupportPointGenerator.hpp
    SLA/SupportPointGenerator.cpp
    SLA/Clustering.hpp
    SLA/Clustering.cpp
    SLA/ReprojectPointsOnMesh.hpp
    SLA/DefaultSupportTree.hpp
    SLA/DefaultSupportTree.cpp
    SLA/BranchingTreeSLA.hpp
    SLA/BranchingTreeSLA.cpp
    BranchingTree/BranchingTree.cpp
    BranchingTree/BranchingTree.hpp
    BranchingTree/PointCloud.cpp
    BranchingTree/PointCloud.hpp
    Arachne/BeadingStrategy/BeadingStrategy.hpp
    Arachne/BeadingStrategy/BeadingStrategy.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.hpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.cpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.hpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.cpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.hpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.cpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.hpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.cpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.hpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.cpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.hpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.cpp
    Arachne/utils/ExtrusionJunction.hpp
    Arachne/utils/ExtrusionJunction.cpp
    Arachne/utils/ExtrusionLine.hpp
    Arachne/utils/ExtrusionLine.cpp
    Arachne/utils/HalfEdge.hpp
    Arachne/utils/HalfEdgeGraph.hpp
    Arachne/utils/HalfEdgeNode.hpp
    Arachne/utils/SparseGrid.hpp
    Arachne/utils/SparsePointGrid.hpp
    Arachne/utils/SparseLineGrid.hpp
    Arachne/utils/SquareGrid.hpp
    Arachne/utils/SquareGrid.cpp
    Arachne/utils/PolygonsPointIndex.hpp
    Arachne/utils/PolygonsSegmentIndex.hpp
    Arachne/utils/PolylineStitcher.hpp
    Arachne/utils/PolylineStitcher.cpp
    Geometry/Voronoi.cpp
    Geometry/VoronoiUtils.hpp
    Geometry/VoronoiUtils.cpp
    Arachne/SkeletalTrapezoidation.hpp
    Arachne/SkeletalTrapezoidation.cpp
    Arachne/SkeletalTrapezoidationEdge.hpp
    Arachne/SkeletalTrapezoidationGraph.hpp
    Arachne/SkeletalTrapezoidationGraph.cpp
    Arachne/SkeletalTrapezoidationJoint.hpp
    Arachne/WallToolPaths.hpp
    Arachne/WallToolPaths.cpp
    StaticMap.hpp
)

add_library(libslic3r STATIC ${SLIC3R_SOURCES})

if (SLIC3R_BUILD_TESTS)
    target_compile_definitions(libslic3r PUBLIC SLIC3R_PLUGIN_REPOSITORY_TESTING)
    target_compile_definitions(libslic3r PRIVATE SLIC3R_FILESYSTEM_TRANSACTION_TESTING)
endif()

if (WIN32)
    target_compile_definitions(libslic3r PUBLIC NOMINMAX)
    target_compile_definitions(libslic3r PRIVATE SLIC3R_HOST_EXPORTS)
endif()

foreach(_source IN ITEMS ${SLIC3R_SOURCES})
    get_filename_component(_source_path "${_source}" PATH)
    string(REPLACE "/" "\\" _group_path "${_source_path}")
    source_group("${_group_path}" FILES "${_source}")
endforeach()

if (SLIC3R_STATIC)
    set(CGAL_Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif ()
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON CACHE BOOL "" FORCE)

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)
find_package(CGAL REQUIRED)
cmake_policy(POP)

add_library(libslic3r_cgal STATIC 
    CutSurface.hpp CutSurface.cpp
    Geometry/VoronoiUtilsCgal.hpp Geometry/VoronoiUtilsCgal.cpp
    IntersectionPoints.hpp IntersectionPoints.cpp
    MeshBoolean.hpp MeshBoolean.cpp
    TryCatchSignal.hpp TryCatchSignal.cpp
    Triangulation.hpp Triangulation.cpp
)
target_include_directories(libslic3r_cgal PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Reset compile options of libslic3r_cgal. Despite it being linked privately, CGAL options
# (-frounding-math) still propagate to dependent libs which is not desired.
get_target_property(_cgal_tgt CGAL::CGAL ALIASED_TARGET)
if (NOT TARGET ${_cgal_tgt})
    set (_cgal_tgt CGAL::CGAL)
endif ()
get_target_property(_opts ${_cgal_tgt} INTERFACE_COMPILE_OPTIONS)
if (_opts)
    set(_opts_bad "${_opts}")
    set(_opts_good "${_opts}")
    list(FILTER _opts_bad INCLUDE REGEX frounding-math)
    list(FILTER _opts_good EXCLUDE REGEX frounding-math)
    set_target_properties(${_cgal_tgt} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts_good}")
    target_compile_options(libslic3r_cgal PRIVATE "${_opts_bad}")
endif()

target_link_libraries(libslic3r_cgal PRIVATE ${_cgal_tgt} libigl)

if (MSVC AND "${CMAKE_SIZEOF_VOID_P}" STREQUAL "4") # 32 bit MSVC workaround
    target_compile_definitions(libslic3r_cgal PRIVATE CGAL_DO_NOT_USE_MPZF)
endif ()

encoding_check(libslic3r)

target_compile_definitions(libslic3r PUBLIC -DUSE_TBB -DTBB_USE_CAPTURED_EXCEPTION=0)
target_include_directories(libslic3r PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(libslic3r PUBLIC ${EXPAT_INCLUDE_DIRS})

find_package(JPEG REQUIRED)

target_link_libraries(libslic3r
    libnest2d
    admesh
    libcereal
    libigl
    miniz
    boost_libs
    clipper
    nowide
    libexpat
    glu-libtess
    qhull
    semver
    TBB::tbb
    TBB::tbbmalloc
    libslic3r_cgal
    ${CMAKE_DL_LIBS}
    PNG::PNG
    ZLIB::ZLIB
    JPEG::JPEG
    qoi
    LibBGCode::bgcode_convert
    quickdigest5
    libcurl
    )

if (APPLE)
    # TODO: we need to fix notarization with the separate shared library
    target_link_libraries(libslic3r OCCTWrapper)
endif ()

if (TARGET OpenVDB::openvdb)
    target_link_libraries(libslic3r OpenVDB::openvdb)
endif()

if(WIN32)
    target_link_libraries(libslic3r Psapi.lib)
endif()

if (APPLE)
    # This flag prevents the need for minimum SDK version 10.14
    # currently, PS targets v10.12
    target_compile_options(libslic3r PUBLIC "-fno-aligned-allocation")
    target_compile_options(libslic3r_cgal PUBLIC "-fno-aligned-allocation")
endif ()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r pchheader.hpp FORCEINCLUDE)
endif ()
