#include <catch2/catch.hpp>

#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepGenerateGcode.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

/*
These tests cover the temporary STEP_GCODE writer, not real G-code motion.

The writer is deliberately simple: it consumes a PrintingPlan and emits one
text line per PrintingExtrusion. The important contract is the plumbing around
it. A direct step call must receive a non-null plan and path, while
Orchestrator::export_gcode() must rebuild the export-side plan before running
the writer. The deterministic-order test protects the parallel formatting
section: workers may finish in any order, but the file must preserve the plan.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;

boost::filesystem::path temporary_gcode_path()
{
    return boost::filesystem::temp_directory_path() /
           boost::filesystem::unique_path("slic3r-printing-plan-writer-%%%%-%%%%.gcode");
}

std::string read_text_file(const boost::filesystem::path &path)
{
    std::ifstream file(path.string(), std::ios::in | std::ios::binary);
    REQUIRE(file.good());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void remove_output_pair(const boost::filesystem::path &path)
{
    boost::system::error_code ignored;
    boost::filesystem::remove(path, ignored);
    boost::filesystem::remove(path.string() + ".tmp", ignored);
}

void append_empty_extrusion(PrintingToolGroup &tool_group)
{
    /*
    The prototype writer only needs to observe the plan shape. An empty root is
    enough here because STEP_GCODE is being tested as a plan consumer, not as a
    geometry exporter.
    */
    PrintingExtrusion extrusion;
    extrusion.root = std::make_unique<ExtrusionEntity>(true);
    tool_group.extrusions.push_back(std::move(extrusion));
}

void select_printing_plan_writer(Print &print)
{
    /*
    Legacy is the default STEP_GCODE choice. These tests exercise the prototype
    writer, so they select it exactly like a preset/UI value would: by writing
    the serialized plugin id into the generated exclusive-group option.
    */
    DynamicPrintConfig &config = const_cast<DynamicPrintConfig &>(print.full_print_config());
    config.set_deserialize("step_gcode_plugin", "gcode.printing_plan_file_writer");
}

} // namespace

TEST_CASE("STEP_GCODE legacy selector is the default and owns the printer UI slot", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    Print print;

    const std::vector<Plugin *> gcode_plugins = orchestrator.get_active_plugins_for_step(STEP_GCODE);
    REQUIRE(gcode_plugins.size() >= 2);
    CHECK(gcode_plugins.front()->get_id() == "gcode.legacy");

    Plugin *selected = Steps::selected_or_active_plugin_for_step(orchestrator, STEP_GCODE, &print.full_print_config());
    REQUIRE(selected != nullptr);
    CHECK(selected->get_id() == "gcode.legacy");
    CHECK(PrintConfigDef::instance().get("step_gcode_plugin")->option_preset_type == RAW_PRESET_TYPE_FFF_PRINTER);
    CHECK(PrintConfigDef::instance().option_keys(RAW_PRESET_TYPE_FFF_PRINTER).count("step_gcode_plugin") == 1);

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

    /*
    The registered fragment is an insertion rule. Merge it against a tiny
    printer layout to make sure the selector really lands before gcode_flavor,
    which is where users expect output-backend choices to live.
    */
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

    /*
    The legacy entry is consumed by Orchestrator::export_gcode(), not by the
    STEP_GCODE plugin runner. A direct run should fail before creating a file,
    which catches accidental bypasses of the host legacy branch.
    */
    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string()),
                    RuntimeError);

    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}

TEST_CASE("STEP_GCODE PrintingPlan writer creates an empty file for an empty plan", "[plugins][gcode]")
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

TEST_CASE("STEP_GCODE fails when the writer reports an output error", "[plugins][gcode]")
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

    /*
    PluginBase converts the writer's C++ exception into report_error(), which
    requests plugin cancellation. STEP_GCODE must turn that cancellation back
    into a host-side failure so export_gcode() cannot report success after the
    plugin failed to create the file.
    */
    CHECK_THROWS_AS(Steps::StepGenerateGcode::run_step(orchestrator, print, output_path.string()),
                    RuntimeError);

    orchestrator.reset_plugin_cancel();
    CHECK_FALSE(boost::filesystem::exists(output_path));
    CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
}

TEST_CASE("Orchestrator export_gcode routes through ordering and STEP_GCODE", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
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
        /*
        The print is marked as already prepared, like it would be after
        Print::process(). Orchestrator::export_gcode() should then request only
        the export-side dependency chain, STEP_ORDERING should create a valid
        plan, and STEP_GCODE should receive the final path and create the file.
        */
        const std::string generated_path =
            Orchestrator::instance().export_gcode(print, output_path.string(), &result, nullptr);

        CHECK(generated_path == output_path.string());
        CHECK(result.filename == output_path.string());
        REQUIRE(boost::filesystem::exists(output_path));
        CHECK(read_text_file(output_path).empty());
        REQUIRE(print.printing_plan() != nullptr);
        CHECK(print.printing_plan()->groups.size() == 1);

        /*
        The GUI formats status messages with boost::format. Step labels are not
        format strings, so they must not receive the output path as a leftover
        argument; otherwise the plater crashes with boost::io::too_many_args.
        */
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

TEST_CASE("STEP_GCODE PrintingPlan writer preserves deterministic plan order", "[plugins][gcode]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    select_printing_plan_writer(print);
    PrintingPlan &plan = print.mutable_printing_plan();

    /*
    Each tool group formats its extrusions in parallel, but the writer must
    serialize those chunks back in PrintingPlan order. Two groups and several
    extrusion counts make the expected order visible without relying on any
    real sliced geometry.
    */
    PrintingGroup group;
    group.layers.emplace_back();
    group.layers.front().tool_groups.emplace_back();
    append_empty_extrusion(group.layers.front().tool_groups.front());
    append_empty_extrusion(group.layers.front().tool_groups.front());

    group.layers.front().tool_groups.emplace_back();
    append_empty_extrusion(group.layers.front().tool_groups.back());

    group.layers.emplace_back();
    group.layers.back().tool_groups.emplace_back();
    append_empty_extrusion(group.layers.back().tool_groups.front());
    append_empty_extrusion(group.layers.back().tool_groups.front());
    append_empty_extrusion(group.layers.back().tool_groups.front());

    plan.groups.push_back(std::move(group));

    const boost::filesystem::path output_path = temporary_gcode_path();
    remove_output_pair(output_path);

    try {
        Steps::StepGenerateGcode::run_step(Orchestrator::instance(), print, output_path.string());

        const std::string expected =
            "group 0 layer 0 tool 0 extrusion num0\n"
            "group 0 layer 0 tool 0 extrusion num1\n"
            "group 0 layer 0 tool 1 extrusion num0\n"
            "group 0 layer 1 tool 0 extrusion num0\n"
            "group 0 layer 1 tool 0 extrusion num1\n"
            "group 0 layer 1 tool 0 extrusion num2\n";
        CHECK(read_text_file(output_path) == expected);
        CHECK_FALSE(boost::filesystem::exists(output_path.string() + ".tmp"));
    } catch (...) {
        remove_output_pair(output_path);
        throw;
    }

    remove_output_pair(output_path);
}
