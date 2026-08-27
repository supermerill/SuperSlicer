#include <catch2/catch.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/filesystem.hpp>

#include "plugin_test_helpers.hpp"
#include "gcode_test_helpers.hpp"

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/GCodeScriptProcessor.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeScriptProcessorViews.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config/ConfigSnapshotSerialization.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityVisitors.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Plugins/GCode/Firmware/BuiltinGCodeFirmwares.hpp"
#include "libslic3r/Plugins/GCode/Firmware/KlipperGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin1GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin2GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/PrusaGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/RepRapGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/SprinterGCodeFirmware.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"
#include "libslic3r/Steps/StepGenerateGcode.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "test_data.hpp"

/*
PrintingPlan file-writer tests verify sequential scope traversal, firmware boundary calls and atomic publication.
Shared plan construction and output helpers live in gcode_test_helpers so this file keeps only component-specific behavior.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;

struct FirmwareRecorder
{
    std::vector<std::string> calls;
    uint32_t destruction_count = 0;
    bool throw_on_extrusion = false;
    std::vector<bool> roots;
    std::vector<bool> region_islands;
    std::vector<raw_extrusion_role> roles;
    std::vector<uint16_t> object_instance_indices;
};

class RecordingFirmwareSession final : public slic3r_api::GCodeFirmwareSession
{
public:
    explicit RecordingFirmwareSession(FirmwareRecorder &recorder) : m_recorder(recorder) {}
    ~RecordingFirmwareSession() override { ++m_recorder.destruction_count; }

    std::string begin_print(const slic3r_api::Print &) override
    {
        m_recorder.calls.emplace_back("begin_print");
        return "begin_print\n";
    }

    std::string begin_group(const slic3r_api::PrintingGroup &) override
    {
        m_recorder.calls.emplace_back("begin_group");
        return "begin_group\n";
    }

    std::string begin_layer(const slic3r_api::PrintingLayerGroup &) override
    {
        m_recorder.calls.emplace_back("begin_layer");
        return "begin_layer\n";
    }

    std::string begin_tool_group(const slic3r_api::PrintingToolGroup &) override
    {
        m_recorder.calls.emplace_back("begin_tool_group");
        return "begin_tool_group\n";
    }

    std::string write_extrusion(const slic3r_api::PrintingExtrusion &extrusion) override
    {
        if (m_recorder.throw_on_extrusion)
            throw std::runtime_error("test firmware extrusion failure");

        m_recorder.calls.emplace_back("write_extrusion");
        m_recorder.roots.push_back(extrusion.root().valid());
        m_recorder.region_islands.push_back(extrusion.region_island().valid());
        m_recorder.roles.push_back(extrusion.role());
        m_recorder.object_instance_indices.push_back(extrusion.object_instance_idx());
        return "write_extrusion\n";
    }

    std::string write_event(const slic3r_api::ExtrusionEntity &) override
    {
        m_recorder.calls.emplace_back("write_event");
        return "write_event\n";
    }

    std::string end_tool_group() override
    {
        m_recorder.calls.emplace_back("end_tool_group");
        return "end_tool_group\n";
    }

    std::string end_layer() override
    {
        m_recorder.calls.emplace_back("end_layer");
        return "end_layer\n";
    }

    std::string end_group() override
    {
        m_recorder.calls.emplace_back("end_group");
        return "end_group\n";
    }

    std::string end_print() override
    {
        m_recorder.calls.emplace_back("end_print");
        return "end_print\n";
    }

private:
    FirmwareRecorder &m_recorder;
};

} // namespace

TEST_CASE("STEP_GCODE legacy selector is the default and owns the printer UI slot", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Print print;

    const std::vector<Plugin *> gcode_plugins = orchestrator.get_active_plugins_for_step(STEP_GCODE);
    REQUIRE(gcode_plugins.size() >= 2);
    CHECK(gcode_plugins.front()->get_id() == "gcode.legacy");

    Plugin *selected = Steps::selected_or_active_plugin_for_step(
        orchestrator, STEP_GCODE, &print.full_print_config());
    REQUIRE(selected != nullptr);
    CHECK(selected->get_id() == "gcode.legacy");
    const ConfigOptionDef *definition = PrintConfigDef::instance().get("step_gcode_plugin");
    REQUIRE(definition != nullptr);
    CHECK(definition->option_preset_type == RAW_PRESET_TYPE_FFF_PRINTER);
    CHECK(PrintConfigDef::instance().option_keys(RAW_PRESET_TYPE_FFF_PRINTER).count(
              "step_gcode_plugin") == 1);

    const std::vector<Orchestrator::PluginUiFragment> printer_fragments =
        orchestrator.ui_fragments_for_file("printer_fff.ui");
    bool found_printer_fragment = false;
    for (const Orchestrator::PluginUiFragment &fragment : printer_fragments) {
        if (fragment.fragment_id != "step_gcode_plugin")
            continue;
        found_printer_fragment = true;
        CHECK(fragment.content.find("setting:insert$beforesetting$gcode_flavor:step_gcode_plugin") !=
              std::string::npos);
    }
    CHECK(found_printer_fragment);

    // Merge the insertion rule against a minimal printer page to verify the
    // user-visible order, not only the fragment registration.
    const std::string merged_printer_layout = orchestrator.merged_ui_layout(
        "printer_fff.ui",
        "page:General:printer\n"
        "group:Firmware\n"
        "\tsetting:gcode_flavor\n");
    const size_t gcode_selector_pos = merged_printer_layout.find("setting:step_gcode_plugin");
    const size_t flavor_pos = merged_printer_layout.find("setting:gcode_flavor");
    REQUIRE(gcode_selector_pos != std::string::npos);
    REQUIRE(flavor_pos != std::string::npos);
    CHECK(gcode_selector_pos < flavor_pos);

    const std::vector<Orchestrator::PluginUiFragment> print_fragments =
        orchestrator.ui_fragments_for_file("print.ui");
    for (const Orchestrator::PluginUiFragment &fragment : print_fragments)
        CHECK(fragment.fragment_id != "step_gcode_plugin");
}
TEST_CASE("STEP_GCODE legacy selector is not a direct plugin writer", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Print print;
    print.mutable_printing_plan();
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    // The orchestrator owns the legacy export path. Running the STEP_GCODE
    // plugin bridge directly must not turn that marker plugin into a writer.
    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(
                        orchestrator, print, output_path.string()), RuntimeError);
    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}
TEST_CASE("Firmware C++ adapter validates tables and destroys its session once",
          "[plugins][gcode][firmware]")
{
    FirmwareRecorder recorder;
    {
        FirmwareInstanceOwner owner(slic3r_api::make_gcode_firmware_instance(
            std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
        CHECK_NOTHROW(slic3r_api::GCodeFirmwareView(&owner.instance()));

        raw_gcode_firmware_instance truncated_instance = owner.instance();
        truncated_instance.struct_size = 0;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&truncated_instance), std::invalid_argument);

        raw_gcode_firmware_vtable truncated = *owner.instance().vtable;
        truncated.struct_size = 0;
        raw_gcode_firmware_instance invalid_size = owner.instance();
        invalid_size.vtable = &truncated;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&invalid_size), std::invalid_argument);

        raw_gcode_firmware_vtable missing_callback = *owner.instance().vtable;
        missing_callback.write_event = nullptr;
        raw_gcode_firmware_instance invalid_callback = owner.instance();
        invalid_callback.vtable = &missing_callback;
        CHECK_THROWS_AS(slic3r_api::GCodeFirmwareView(&invalid_callback), std::invalid_argument);
    }
    CHECK(recorder.destruction_count == 1);
}
TEST_CASE("PrintingPlan file writer serializes populated scope events inside their boundaries",
          "[plugins][gcode][firmware][printing][plan][events]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.front();
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.front();
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool_group = layer.tool_groups.front();
    const LayerRegionIsland *fake_region =
        reinterpret_cast<const LayerRegionIsland *>(uintptr_t(0x3456));
    append_empty_extrusion(tool_group, ExtrusionRole::None, 0, fake_region);

    // One child makes each fixed event root observable without depending on a
    // specific event property. The recording firmware marks every callback.
    ExtrusionEntity event(true);
    plan.events.append_before(event);
    plan.events.append_after(event);
    group.events.append_before(event);
    group.events.append_after(event);
    layer.events.append_before(event);
    layer.events.append_after(event);
    tool_group.events.append_before(event);
    tool_group.events.append_after(event);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());

        const std::vector<std::string> expected_calls{
            "begin_print", "write_event",
            "begin_group", "write_event",
            "begin_layer", "write_event",
            "begin_tool_group", "write_event",
            "write_extrusion",
            "write_event", "end_tool_group",
            "write_event", "end_layer",
            "write_event", "end_group",
            "write_event", "end_print"
        };
        CHECK(recorder.calls == expected_calls);
        CHECK(read_text_file(output_path) ==
              "begin_print\nwrite_event\n"
              "begin_group\nwrite_event\n"
              "begin_layer\nwrite_event\n"
              "begin_tool_group\nwrite_event\n"
              "write_extrusion\n"
              "write_event\nend_tool_group\n"
              "write_event\nend_layer\n"
              "write_event\nend_group\n"
              "write_event\nend_print\n");
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("PrintingPlan file writer serializes every firmware boundary in order",
          "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.front().layers.front();
    layer.tool_groups.emplace_back();
    layer.tool_groups.front().extruder_id = 2;
    const LayerRegionIsland *fake_region =
        reinterpret_cast<const LayerRegionIsland *>(uintptr_t(0x2345));
    append_empty_extrusion(
        layer.tool_groups.front(), ExtrusionRole::Perimeter, 7, fake_region);
    append_empty_extrusion(
        layer.tool_groups.front(), ExtrusionRole::None, 0, fake_region);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        run_file_writer_with_firmware(print, output_path, firmware.instance());

        const std::vector<std::string> expected_calls{
            "begin_print",
            "begin_group",
            "begin_layer",
            "begin_tool_group",
            "write_extrusion",
            "write_extrusion",
            "end_tool_group",
            "end_layer",
            "end_group",
            "end_print"
        };
        CHECK(recorder.calls == expected_calls);
        CHECK(read_text_file(output_path) ==
              "begin_print\n"
              "begin_group\n"
              "begin_layer\n"
              "begin_tool_group\n"
              "write_extrusion\n"
              "write_extrusion\n"
              "end_tool_group\n"
              "end_layer\n"
              "end_group\n"
              "end_print\n");
        REQUIRE(recorder.roots.size() == 2);
        CHECK(recorder.roots[0]);
        CHECK(recorder.roots[1]);
        REQUIRE(recorder.region_islands.size() == 2);
        CHECK(recorder.region_islands[0]);
        CHECK(recorder.region_islands[1]);
        REQUIRE(recorder.roles.size() == 2);
        CHECK(recorder.roles[0] == RAW_EXTRUSION_ROLE_PERIMETER);
        CHECK(recorder.roles[1] == RAW_EXTRUSION_ROLE_NONE);
        REQUIRE(recorder.object_instance_indices.size() == 2);
        CHECK(recorder.object_instance_indices[0] == 7);
        CHECK(recorder.object_instance_indices[1] == 0);
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("Default firmware selects the tool of an otherwise empty PrintingPlan", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    plan.groups.front().layers.front().tool_groups.front().extruder_id = 0;
    append_empty_extrusion(plan.groups.front().layers.front().tool_groups.front());
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path) == "T0\n");
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("Default firmware publishes an empty file for an empty PrintingPlan", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    print.mutable_printing_plan();
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path).empty());
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("PrintingPlan firmware executes host-owned scripts",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    configure_standard_firmware(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    ExtrusionNop script(ExtrusionPropertyCustomGcodeText(
        ExtrusionPropertyCustomGcodeText::Code::SCRIPT,
        "M117 X{position[0]}"));
    plan.events.append_before(script);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    try {
        Steps::StepGenerateGcode::run_step(
            Orchestrator::instance(), print, output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        const std::string output = read_text_file(output_path);
        CHECK(output.find("M117 X0") != std::string::npos);
        CHECK(output.find("{position") == std::string::npos);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
TEST_CASE("PrintingPlan scripts cannot replace a previously published file",
          "[plugins][gcode][firmware][custom-gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();
    ExtrusionNop script(ExtrusionPropertyCustomGcodeText(
        ExtrusionPropertyCustomGcodeText::Code::SCRIPT,
        "M117 {layer_num}"));
    plan.events.append_before(script);

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    write_text_file(output_path, "previous output\n");
    CHECK_THROWS_AS(
        Steps::StepGenerateGcode::run_step(
            Orchestrator::instance(), print, output_path.string()),
        RuntimeError);
    Orchestrator::instance().reset_plugin_cancel();
    REQUIRE(boost::filesystem::exists(output_path));
    CHECK(read_text_file(output_path) == "previous output\n");
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    remove_output_pair(output_path);
}
TEST_CASE("Firmware exceptions prevent final output publication", "[plugins][gcode][firmware]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    FirmwareRecorder recorder;
    recorder.throw_on_extrusion = true;
    FirmwareInstanceOwner firmware(slic3r_api::make_gcode_firmware_instance(
        std::unique_ptr<slic3r_api::GCodeFirmwareSession>(new RecordingFirmwareSession(recorder))));
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.front().layers.emplace_back();
    plan.groups.front().layers.front().tool_groups.emplace_back();
    append_empty_extrusion(plan.groups.front().layers.front().tool_groups.front());

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    write_text_file(output_path, "previous output\n");
    CHECK_THROWS_AS(run_file_writer_with_firmware(print, output_path, firmware.instance()), RuntimeError);
    REQUIRE(boost::filesystem::exists(output_path));
    CHECK(read_text_file(output_path) == "previous output\n");
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    remove_output_pair(output_path);
}
TEST_CASE("STEP_GCODE fails when the output directory is unavailable", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Print print;
    select_printing_plan_writer(print);
    print.mutable_printing_plan();
    const boost::filesystem::path missing_directory =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("slic3r-missing-output-dir-%%%%-%%%%");
    const boost::filesystem::path output_path = missing_directory / "out.gcode";

    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string()),
                    RuntimeError);
    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}
TEST_CASE("Orchestrator export_gcode still routes through ordering and STEP_GCODE", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.start_gcode.value.clear();
    config.end_gcode.value.clear();
    for (slicing_step_t step : Steps::execution_order())
        print.mark_step_executed(step);

    GCodeProcessorResult result;
    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);
    std::vector<PrintBase::SlicingStatus> statuses;
    print.set_status_callback([&statuses](const PrintBase::SlicingStatus &status) {
        statuses.push_back(status);
    });

    try {
        const std::string generated_path =
            Orchestrator::instance().export_gcode(print, output_path.string(), &result, nullptr);
        CHECK(generated_path == output_path.string());
        CHECK(result.filename == output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path).empty());
        REQUIRE(print.printing_plan() != nullptr);
        CHECK(print.printing_plan()->groups.size() == 1);

        // Step labels are not boost::format templates. A leftover path argument
        // would make the GUI throw boost::io::too_many_args while showing this
        // otherwise successful export.
        bool saw_ordering_status = false;
        for (const PrintBase::SlicingStatus &status : statuses) {
            if (status.main_text != "Ordering extrusions")
                continue;
            saw_ordering_status = true;
            CHECK(status.args.empty());
        }
        CHECK(saw_ordering_status);
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }
    remove_output_pair(output_path);
}
