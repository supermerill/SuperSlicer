#include <catch2/catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gcode_test_helpers.hpp"
#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Plugins/GCode/CustomGCodePerPrintZRecord.hpp"
#include "libslic3r/Plugins/GCode/Firmware/KlipperGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin1GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/Marlin2GCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/PrusaGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/RepRapGCodeFirmware.hpp"
#include "libslic3r/Plugins/GCode/Firmware/SprinterGCodeFirmware.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepExtrusionEdition.hpp"

/*
Custom height-marker plugin tests
=================================

These tests exercise the boundary between the host-owned PrintRecord and the
ordered PrintingPlan. The fixtures deliberately build the record independently
from Model so malformed producer data and plugin-side validation can be tested
without involving GUI persistence.
*/

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::GCode;
using namespace slic3r_api::GCodeGeneration::CustomGCodePerPrintZPlugin;

/* Publish a complete record table while preserving the row order supplied by the test. */
void publish_record(Print &print,
                    CustomGCode::Mode mode,
                    const std::vector<CustomGCode::Item> &items)
{
    DynamicConfig &record = print.records().get_or_add(CustomGCode::PrintRecordChannel);
    record.clear();

    std::vector<double> print_z;
    std::vector<int32_t> types;
    std::vector<int32_t> extruders;
    std::vector<std::string> colors;
    std::vector<std::string> extras;
    print_z.reserve(items.size());
    types.reserve(items.size());
    extruders.reserve(items.size());
    colors.reserve(items.size());
    extras.reserve(items.size());
    for (const CustomGCode::Item &item : items) {
        print_z.push_back(unscaled(item.print_z_));
        types.push_back(int32_t(item.type));
        extruders.push_back(item.extruder);
        colors.push_back(item.color);
        extras.push_back(item.extra);
    }

    record.set_key_value(CustomGCode::PrintRecordSizeKey,
                         new ConfigOptionInt(int32_t(items.size())));
    record.set_key_value(CustomGCode::PrintRecordModeKey,
                         new ConfigOptionInt(int32_t(mode)));
    record.set_key_value(CustomGCode::PrintRecordPrintZKey,
                         new ConfigOptionFloats(print_z));
    ConfigOptionInts *type_option = new ConfigOptionInts();
    type_option->set(types);
    record.set_key_value(CustomGCode::PrintRecordTypeKey, type_option);
    ConfigOptionInts *extruder_option = new ConfigOptionInts();
    extruder_option->set(extruders);
    record.set_key_value(CustomGCode::PrintRecordExtruderKey, extruder_option);
    record.set_key_value(CustomGCode::PrintRecordColorKey,
                         new ConfigOptionStrings(colors));
    record.set_key_value(CustomGCode::PrintRecordExtraKey,
                         new ConfigOptionStrings(extras));
}

/* Return the script child from the tag-plus-payload wrapper emitted by the plugin. */
const ExtrusionEntity &event_payload(const ExtrusionEntity &event)
{
    REQUIRE(event.child_count() == 2);
    return event.child(1);
}

/* Expose the protected fallback hook without changing the production API. */
template<class Session>
class EmptyScriptFallbackProbe : public Session
{
public:
    std::string fallback(gcode_script_type type) const
    {
        return Session::resolve_empty_script(type, nullptr);
    }
};

} // namespace

TEST_CASE("Custom height record reader validates the complete table",
          "[plugins][gcode][custom-per-print-z][record]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    configure_standard_firmware(print, 2);
    publish_record(print, CustomGCode::MultiExtruder, {
        {scale_to_layer_coord(0.2), CustomGCode::ColorChange, 1, "#ff0000", "first"},
        {scale_to_layer_coord(0.4), CustomGCode::PausePrint, 2, "", "pause"}
    });

    const slic3r_api::Print print_view(
        reinterpret_cast<const print_handle *>(&print));
    const std::optional<CustomGCodeTable> table = read_custom_gcode_per_print_z(print_view);
    REQUIRE(table.has_value());
    REQUIRE(table->rows.size() == 2);
    CHECK(table->mode == CustomGCode::MultiExtruder);
    CHECK(table->rows[0].print_z_mm == Approx(0.2));
    CHECK(table->rows[0].color == "#ff0000");
    CHECK(table->rows[1].type == CustomGCode::PausePrint);
    CHECK(table->rows[1].extra == "pause");

    /* A malformed column must reject the entire record instead of exposing a
       valid prefix to one of the three coordinated plugins. */
    DynamicConfig &record = print.records().get_or_add(CustomGCode::PrintRecordChannel);
    record.set_key_value(CustomGCode::PrintRecordExtraKey,
                         new ConfigOptionStrings({"only one row"}));
    CHECK_THROWS_AS(read_custom_gcode_per_print_z(print_view), std::runtime_error);
}

TEST_CASE("Custom height rows map to the first matching layer in model order",
          "[plugins][gcode][custom-per-print-z][mapping]")
{
    PrintingPlan native_plan;
    native_plan.groups.emplace_back();
    native_plan.groups.front().layers.emplace_back();
    native_plan.groups.front().layers.back().print_z = scale_i(0.2);
    native_plan.groups.front().layers.emplace_back();
    native_plan.groups.front().layers.back().print_z = scale_i(0.4);

    CustomGCodeTable table;
    table.rows = {
        {0.1, CustomGCode::PausePrint, 1, "", "first", 0},
        {0.2, CustomGCode::Custom, 1, "", "second", 1},
        {0.21, CustomGCode::Template, 1, "", "third", 2},
        {0.5, CustomGCode::PausePrint, 1, "", "above", 3}
    };

    const slic3r_api::PrintingPlan plan(
        reinterpret_cast<printing_plan_handle *>(&native_plan));
    const std::vector<std::vector<const CustomGCodeRow *>> mapped =
        rows_by_layer(table, plan.group(0));
    REQUIRE(mapped.size() == 2);
    REQUIRE(mapped[0].size() == 2);
    REQUIRE(mapped[1].size() == 1);
    CHECK(mapped[0][0]->source_row == 0);
    CHECK(mapped[0][1]->source_row == 1);
    CHECK(mapped[1][0]->source_row == 2);
}

TEST_CASE("Custom height markers become ordered typed plan events",
          "[plugins][gcode][custom-per-print-z][extrusion-edit]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    configure_standard_firmware(print);
    PrintConfig &config = const_cast<PrintConfig &>(print.config());
    config.color_change_gcode.value.clear();
    config.pause_print_gcode.value.clear();
    config.template_custom_gcode.value.clear();
    config.start_filament_gcode.set(std::vector<std::string>{"M117 filament start"});

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingGroup &group = plan.groups.back();
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = 0;

    publish_record(print, CustomGCode::MultiAsSingle, {
        {scale_to_layer_coord(0.2), CustomGCode::ColorChange, 1, "#112233", ""},
        {scale_to_layer_coord(0.2), CustomGCode::PausePrint, 1, "", "Inspect layer"},
        {scale_to_layer_coord(0.2), CustomGCode::ToolChange, 1, "#445566", ""},
        {scale_to_layer_coord(0.2), CustomGCode::Template, 1, "", ""},
        {scale_to_layer_coord(0.2), CustomGCode::Custom, 1, "", "G4 P25"}
    });

    Steps::StepExtrusionEdition::clean_and_prepare(print);
    Steps::StepExtrusionEdition::run_step(Orchestrator::instance(), print);

    const ExtrusionEntity &events = layer.tool_groups.front().events.before();
    REQUIRE(events.child_count() == 6);
    const ExtrusionPropertyCustomGcode *filament_start =
        events.child(0).get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(filament_start != nullptr);
    CHECK(filament_start->script_type == GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE);
    const ExtrusionEntity &color = event_payload(events.child(1));
    const ExtrusionEntity &pause = event_payload(events.child(2));
    const ExtrusionEntity &converted_tool = event_payload(events.child(3));
    const ExtrusionEntity &templated = event_payload(events.child(4));
    const ExtrusionEntity &custom = event_payload(events.child(5));

    const ExtrusionPropertyCustomGcode *color_property =
        color.get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *pause_property =
        pause.get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *tool_property =
        converted_tool.get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *template_property =
        templated.get_property<ExtrusionPropertyCustomGcode>();
    const ExtrusionPropertyCustomGcode *custom_property =
        custom.get_property<ExtrusionPropertyCustomGcode>();
    REQUIRE(color_property != nullptr);
    REQUIRE(pause_property != nullptr);
    REQUIRE(tool_property != nullptr);
    REQUIRE(template_property != nullptr);
    REQUIRE(custom_property != nullptr);

    CHECK(color_property->script_type == GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE);
    CHECK(color_property->processing_extruder_id == 0);
    CHECK(stored_script_int(color, *color_property, "color_change_extruder") == 0);
    CHECK(stored_script_string(color, *color_property, "next_color") == "#112233");
    CHECK(stored_script_string(color, *color_property, "next_colour") == "#112233");
    CHECK(pause_property->script_type == GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE);
    CHECK(stored_script_string(pause, *pause_property, "pause_message") == "Inspect layer");
    CHECK(tool_property->script_type == GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE);
    CHECK(stored_script_string(converted_tool, *tool_property, "next_color") == "#445566");
    CHECK(template_property->script_type == GCODE_SCRIPT_TYPE_TEMPLATE_CUSTOM_GCODE);
    CHECK(custom_property->kind == C_EXTRUSION_CUSTOM_GCODE_GCODE);
    CHECK(custom.custom_gcode_string(*custom_property) == "G4 P25");
}

TEST_CASE("Built-in firmwares resolve empty height-marker scripts",
          "[plugins][gcode][custom-per-print-z][firmware]")
{
    using namespace slic3r_api::GCodeGeneration::Firmware;

    EmptyScriptFallbackProbe<Marlin1GCodeFirmwareSession> marlin1;
    EmptyScriptFallbackProbe<Marlin2GCodeFirmwareSession> marlin2;
    EmptyScriptFallbackProbe<PrusaGCodeFirmwareSession> prusa;
    EmptyScriptFallbackProbe<RepRapGCodeFirmwareSession> reprap;
    EmptyScriptFallbackProbe<KlipperGCodeFirmwareSession> klipper;
    EmptyScriptFallbackProbe<SprinterGCodeFirmwareSession> sprinter;

    CHECK(marlin1.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE) == "M600\n");
    CHECK(marlin1.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) == "M0\n");
    CHECK(marlin2.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE) == "M600\n");
    CHECK(marlin2.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) == "M0\n");
    CHECK(prusa.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE) == "M600\n");
    CHECK(prusa.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) == "M601\n");
    CHECK(reprap.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE) == "M600\n");
    CHECK(reprap.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) == "M226\n");
    CHECK(klipper.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE) == "PAUSE\n");
    CHECK(klipper.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE) == "PAUSE\n");
    CHECK(sprinter.fallback(GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE).find("Unsupported") !=
          std::string::npos);
    CHECK(sprinter.fallback(GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE).find("Unsupported") !=
          std::string::npos);
}
