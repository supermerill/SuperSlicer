#include <catch2/catch.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/EncodableState.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/DefaultExtruder.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/FanState.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/Gantry.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/HeaterState.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/Printer.hpp"
#include "libslic3r/PrintConfig.hpp"

/*
Generic G-code firmware state tests
===================================

These tests exercise state bookkeeping independently from PrintingPlan file
serialization. They verify requested and encoded values, including values
encoded by explicit wait instructions,
the domain components that apply offsets, and the machine objects that compose
those components into gantry, printer and extruder state.
*/

namespace {

using namespace Slic3r;
using namespace slic3r_api::GCodeGeneration;

TEST_CASE("Encodable firmware registers track requested and encoded values",
          "[plugins][gcode][firmware][state]")
{
    SECTION("plain register") {
        EncodableState<double> state;
        CHECK_FALSE(state.requested());
        CHECK_FALSE(state.encoded());
        CHECK_THROWS_AS(state.mark_encoded(), std::logic_error);

        state.request(12.5);
        CHECK(state.needs_encoding());
        state.mark_encoded();
        CHECK(state.encoded() == 12.5);
        CHECK_FALSE(state.needs_encoding());

        // A domain component may retain the raw request while recording a
        // corrected value that was actually encoded by the firmware.
        state.request(20.0);
        CHECK(state.needs_encoding_for(25.0));
        state.mark_encoded_as(25.0);
        CHECK(state.requested() == 20.0);
        CHECK(state.encoded() == 25.0);

        EncodableState<double> copy;
        copy.synchronize_runtime_from(state);
        CHECK(copy.requested() == 20.0);
        CHECK(copy.encoded() == 25.0);
        copy.clear_encoded();
        CHECK(copy.needs_encoding_for(25.0));
        copy.reset_runtime_state();
        CHECK_FALSE(copy.requested());
        CHECK_FALSE(copy.encoded());
    }

    SECTION("awaitable register") {
        AwaitableEncodableState<int16_t> state;
        CHECK_THROWS_AS(state.mark_encoded_with_wait(), std::logic_error);
        state.request(200);
        CHECK(state.needs_encoding());
        CHECK(state.needs_wait_encoding());
        state.mark_encoded();
        CHECK_FALSE(state.needs_encoding());
        CHECK(state.needs_wait_encoding());
        state.mark_encoded_with_wait();
        CHECK(state.encoded() == 200);
        CHECK(state.encoded_with_wait() == 200);
        CHECK_FALSE(state.needs_wait_encoding());

        AwaitableEncodableState<int16_t> copy;
        copy.synchronize_runtime_from(state);
        CHECK(copy.requested() == 200);
        CHECK(copy.encoded() == 200);
        CHECK(copy.encoded_with_wait() == 200);
        copy.clear_encoded();
        CHECK(copy.needs_encoding());
        CHECK(copy.encoded_with_wait() == 200);
        copy.clear_encoded_with_wait();
        CHECK(copy.needs_wait_encoding());
        CHECK_FALSE(copy.encoded());

        state.request(210);
        state.mark_encoded();
        CHECK(state.encoded() == 210);
        CHECK(state.encoded_with_wait() == 200);
        CHECK(state.needs_wait_encoding());
        state.reset_runtime_state();
        CHECK_FALSE(state.requested());
        CHECK_FALSE(state.encoded());
        CHECK_FALSE(state.encoded_with_wait());
    }
}

TEST_CASE("Generic firmware state keeps acceleration requests and encoded values independent",
          "[plugins][gcode][firmware][state]")
{
    Gantry gantry;
    gantry.request_print_acceleration(500);
    gantry.request_travel_acceleration(1200);
    REQUIRE(gantry.requested_print_acceleration() == 500);
    REQUIRE(gantry.requested_travel_acceleration() == 1200);
    CHECK(gantry.needs_print_acceleration_encoding());
    CHECK(gantry.needs_travel_acceleration_encoding());

    // A firmware may mark one independent register as encoded while the other
    // request remains pending for a later movement.
    gantry.mark_print_acceleration_encoded();
    CHECK(gantry.encoded_print_acceleration() == 500);
    CHECK_FALSE(gantry.needs_print_acceleration_encoding());
    CHECK(gantry.needs_travel_acceleration_encoding());

    gantry.mark_travel_acceleration_encoded();
    CHECK(gantry.encoded_travel_acceleration() == 1200);
    CHECK_FALSE(gantry.needs_travel_acceleration_encoding());
    gantry.clear_travel_acceleration_encoded();
    CHECK_FALSE(gantry.encoded_travel_acceleration());
    CHECK(gantry.requested_travel_acceleration() == 1200);
    CHECK(gantry.needs_travel_acceleration_encoding());
    gantry.mark_travel_acceleration_encoded();
    gantry.request_print_acceleration(700);
    CHECK(gantry.needs_print_acceleration_encoding());
    CHECK_FALSE(gantry.needs_travel_acceleration_encoding());

    gantry.reset_runtime_state();
    CHECK_FALSE(gantry.requested_print_acceleration());
    CHECK_FALSE(gantry.requested_travel_acceleration());
    CHECK_FALSE(gantry.encoded_print_acceleration());
    CHECK_FALSE(gantry.encoded_travel_acceleration());
}

TEST_CASE("Reusable heater and fan states preserve destination configuration",
          "[plugins][gcode][firmware][state]")
{
    HeaterState source_heater;
    source_heater.setup(5);
    source_heater.request_temperature(200);
    source_heater.mark_encoded_with_wait();
    source_heater.request_temperature(210);
    source_heater.mark_encoded();
    CHECK(source_heater.encoded_temperature() == 215);
    CHECK(source_heater.encoded_temperature_with_wait() == 205);

    HeaterState destination_heater;
    destination_heater.setup(-5);
    destination_heater.synchronize_runtime_from(source_heater);
    CHECK(destination_heater.requested_temperature() == 210);
    CHECK(destination_heater.encoded_temperature() == 215);
    CHECK(destination_heater.encoded_temperature_with_wait() == 205);
    CHECK(destination_heater.effective_temperature() == 205);
    CHECK(destination_heater.needs_encoding());

    FanState source_fan;
    source_fan.setup(10.0);
    source_fan.request_speed_percent(20.0);
    source_fan.mark_encoded();
    CHECK(source_fan.encoded_speed_percent() == 30.0);

    FanState destination_fan;
    destination_fan.setup(-5.0);
    destination_fan.synchronize_runtime_from(source_fan);
    CHECK(destination_fan.requested_speed_percent() == 20.0);
    CHECK(destination_fan.encoded_speed_percent() == 30.0);
    CHECK(destination_fan.effective_speed_percent() == 15.0);
    CHECK(destination_fan.needs_encoding());
}

TEST_CASE("Generic firmware state distinguishes normal and wait temperature encodings",
          "[plugins][gcode][firmware][state]")
{
    DefaultExtruder first_tool(0);
    DefaultExtruder second_tool(1);
    first_tool.heater().request_temperature(200);
    CHECK(first_tool.heater().needs_encoding());
    CHECK(first_tool.heater().needs_wait_encoding());
    first_tool.heater().mark_encoded();
    CHECK_FALSE(first_tool.heater().needs_encoding());
    CHECK(first_tool.heater().needs_wait_encoding());
    first_tool.heater().mark_encoded_with_wait();
    CHECK_FALSE(first_tool.heater().needs_wait_encoding());

    // A later non-blocking target changes the requested and encoded values
    // while preserving the previous target encoded with a wait instruction.
    first_tool.heater().request_temperature(210);
    first_tool.heater().mark_encoded();
    CHECK(first_tool.heater().requested_temperature() == 210);
    CHECK(first_tool.heater().encoded_temperature() == 210);
    CHECK(first_tool.heater().encoded_temperature_with_wait() == 200);
    CHECK(first_tool.heater().needs_wait_encoding());
    CHECK_FALSE(second_tool.heater().requested_temperature());

    first_tool.reset_runtime_state();
    CHECK_FALSE(first_tool.heater().requested_temperature());
    CHECK_FALSE(first_tool.heater().encoded_temperature());
    CHECK_FALSE(first_tool.heater().encoded_temperature_with_wait());

    Printer printer;
    printer.bed_heater().request_temperature(60);
    printer.bed_heater().mark_encoded();
    CHECK(printer.bed_heater().needs_wait_encoding());
    printer.bed_heater().mark_encoded_with_wait();
    CHECK_FALSE(printer.bed_heater().needs_wait_encoding());
    printer.bed_heater().request_temperature(70);
    printer.bed_heater().mark_encoded();
    CHECK(printer.bed_heater().encoded_temperature_with_wait() == 60);
    CHECK(printer.bed_heater().needs_wait_encoding());

    printer.chamber_heater().request_temperature(40);
    printer.chamber_heater().mark_encoded_with_wait();
    printer.chamber_fan().request_speed_percent(35.0);
    printer.chamber_fan().mark_encoded();
    CHECK(printer.chamber_heater().encoded_temperature_with_wait() == 40);
    CHECK(printer.chamber_fan().encoded_speed_percent() == 35.0);

    printer.reset_runtime_state();
    CHECK_FALSE(printer.bed_heater().requested_temperature());
    CHECK_FALSE(printer.chamber_heater().requested_temperature());
    CHECK_FALSE(printer.chamber_fan().requested_speed_percent());
}

TEST_CASE("High-level firmware states synchronize their complete runtime snapshot",
          "[plugins][gcode][firmware][state]")
{
    Gantry source_gantry;
    source_gantry.set_position({1.0, 2.0, 3.0});
    source_gantry.request_speed(25.0);
    source_gantry.mark_speed_encoded();
    source_gantry.request_speed(30.0);
    source_gantry.request_print_acceleration(500);
    source_gantry.mark_print_acceleration_encoded();
    source_gantry.request_travel_acceleration(1200);
    source_gantry.mark_travel_acceleration_encoded();

    Gantry destination_gantry;
    destination_gantry.synchronize_runtime_from(source_gantry);
    REQUIRE(destination_gantry.position());
    CHECK(destination_gantry.position()->x == Approx(1.0));
    CHECK(destination_gantry.position()->y == Approx(2.0));
    CHECK(destination_gantry.position()->z == Approx(3.0));
    CHECK(destination_gantry.requested_speed() == 30.0);
    CHECK(destination_gantry.encoded_speed() == 25.0);
    CHECK(destination_gantry.requested_print_acceleration() == 500);
    CHECK(destination_gantry.encoded_travel_acceleration() == 1200);

    Printer source_printer;
    source_printer.bed_heater().request_temperature(60);
    source_printer.bed_heater().mark_encoded_with_wait();
    source_printer.bed_heater().request_temperature(70);
    source_printer.bed_heater().mark_encoded();
    source_printer.chamber_heater().request_temperature(40);
    source_printer.chamber_heater().mark_encoded_with_wait();
    source_printer.chamber_fan().request_speed_percent(35.0);
    source_printer.chamber_fan().mark_encoded();
    source_printer.set_preview_enabled(false);

    Printer destination_printer;
    destination_printer.synchronize_runtime_from(source_printer);
    CHECK(destination_printer.bed_heater().requested_temperature() == 70);
    CHECK(destination_printer.bed_heater().encoded_temperature() == 70);
    CHECK(destination_printer.bed_heater().encoded_temperature_with_wait() == 60);
    CHECK(destination_printer.chamber_heater().encoded_temperature_with_wait() == 40);
    CHECK(destination_printer.chamber_fan().encoded_speed_percent() == 35.0);
    CHECK_FALSE(destination_printer.preview_enabled());

    DefaultExtruder source_tool(0);
    source_tool.heater().request_temperature(200);
    source_tool.heater().mark_encoded_with_wait();
    source_tool.heater().request_temperature(210);
    source_tool.heater().mark_encoded();
    source_tool.fan().request_speed_percent(30.0);
    source_tool.fan().mark_encoded();
    source_tool.fan().request_speed_percent(40.0);
    source_tool.pressure_advance().request(0.01);
    source_tool.pressure_advance().mark_encoded();
    source_tool.pressure_advance().request(0.02);
    source_tool.extrusion_axis().set_position(4.25);
    source_tool.extrusion_axis().set_retracted(1.5, 0.2);

    DefaultExtruder destination_tool(1);
    destination_tool.synchronize_runtime_from(source_tool);
    CHECK(destination_tool.id() == 1);
    CHECK(destination_tool.heater().requested_temperature() == 210);
    CHECK(destination_tool.heater().encoded_temperature() == 210);
    CHECK(destination_tool.heater().encoded_temperature_with_wait() == 200);
    CHECK(destination_tool.fan().requested_speed_percent() == 40.0);
    CHECK(destination_tool.fan().encoded_speed_percent() == 30.0);
    CHECK(destination_tool.pressure_advance().requested() == 0.02);
    CHECK(destination_tool.pressure_advance().encoded() == 0.01);
    CHECK(destination_tool.extrusion_axis().position() == Approx(4.25));
    CHECK(destination_tool.extrusion_axis().retracted() == Approx(1.5));
    CHECK(destination_tool.extrusion_axis().restart_extra() == Approx(0.2));
}

TEST_CASE("Extrusion axis state owns E quantization and its rounding remainder",
          "[plugins][gcode][firmware][state]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize("gcode_precision_e", "5");
    const slic3r_api::Config config_view(ApiHost::to_config_handle(&config));

    SECTION("absolute E keeps exact position and may explicitly emit zero") {
        DefaultExtruder tool(0);
        tool.setup(config_view);
        ExtrusionAxisState &axis = tool.extrusion_axis();

        CHECK_FALSE(axis.extrude(0.000004));
        CHECK(axis.position() == Approx(0.000004));
        CHECK(axis.extruded_dE_left() == Approx(0.000004));

        const std::optional<double> accumulated = axis.extrude(0.000004);
        REQUIRE(accumulated);
        CHECK(*accumulated == Approx(0.00001));
        CHECK(axis.position() == Approx(0.000008));
        CHECK(axis.extruded_dE_left() == Approx(-0.000002));

        const std::optional<double> zero = axis.extrude(-0.000008);
        REQUIRE(zero);
        CHECK(*zero == 0.0);
        CHECK(axis.position() == Approx(0.0));
        CHECK(axis.extruded_dE_left() == Approx(0.0));
    }

    SECTION("relative E carries sub-precision deltas between commands") {
        config.set_deserialize("use_relative_e_distances", "1");
        DefaultExtruder tool(0);
        tool.setup(config_view);
        ExtrusionAxisState &axis = tool.extrusion_axis();

        CHECK_FALSE(axis.extrude(0.000004));
        const std::optional<double> accumulated = axis.extrude(0.000004);
        REQUIRE(accumulated);
        CHECK(*accumulated == Approx(0.00001));
        CHECK(axis.position() == 0.0);
        CHECK(axis.extruded_dE_left() == Approx(-0.000002));
        CHECK_FALSE(axis.extrude(0.000002));
        CHECK(std::abs(axis.extruded_dE_left()) < 1e-12);
    }

    SECTION("external synchronization resets rounding only after a real state change") {
        DefaultExtruder tool(0);
        tool.setup(config_view);
        ExtrusionAxisState &axis = tool.extrusion_axis();

        CHECK_FALSE(axis.extrude(0.000004));
        REQUIRE(axis.extruded_dE_left() == Approx(0.000004));

        // A script which merely round-trips the current values must not discard
        // a sub-precision extrusion that belongs to the next generated command.
        CHECK_FALSE(axis.synchronize_after_external_gcode(
            axis.position(), axis.retracted(), axis.restart_extra()));
        CHECK(axis.extruded_dE_left() == Approx(0.000004));

        // Once external G-code really replaces E or retraction state, the old
        // remainder was computed from a stale coordinate and must be cleared.
        CHECK(axis.synchronize_after_external_gcode(2.5, 1.25, 0.2));
        CHECK(axis.position() == Approx(2.5));
        CHECK(axis.retracted() == Approx(1.25));
        CHECK(axis.restart_extra() == Approx(0.2));
        CHECK(axis.extruded_dE_left() == Approx(0.0));
    }
}

} // namespace
