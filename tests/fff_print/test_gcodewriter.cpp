#include <catch2/catch.hpp>

#include <memory>

#include "libslic3r/GCode/GCodeWriter.hpp"

using namespace Slic3r;

TEST_CASE("Wipe extrusion is counted before the remaining retraction", "[GCodeWriter][retraction-accounting]")
{
    const bool relative_e = GENERATE(false, true);
    const double before_wipe = GENERATE(0., 80.);
    const double wipe_fraction = GENERATE(0.5, 1.);
    CAPTURE(relative_e, before_wipe, wipe_fraction);

    PrintConfig config;
    config.use_relative_e_distances.value = relative_e;
    config.use_firmware_retraction.value = false;
    config.use_volumetric_e.value = false;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.retract_length.set_at(0.4, 0);
    config.retract_before_wipe.set_at(before_wipe, 0);
    config.retract_restart_extra.set_at(0., 0);
    config.retract_restart_extra_toolchange.set_at(0., 0);

    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_extruders({0});
    writer.set_tool(0);
    // Use the same writer calls as retract_and_wipe(), without mesh slicing.
    const std::string pre_retract = writer.retract(true);
    CHECK(pre_retract.empty() == (before_wipe == 0.));
    const double initial_retraction = 0.4 * before_wipe / 100.;
    const double wiped = (0.4 - initial_retraction) * wipe_fraction;
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(1., 0.), -wiped / 2.).empty());
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(2., 0.), -wiped / 2.).empty());
    CHECK(writer.tool()->retracted() == Approx(initial_retraction + wiped));

    const std::string remaining_retract = writer.retract();
    CHECK(remaining_retract.empty() == (wipe_fraction == 1.));
    CHECK(writer.tool()->retracted() == Approx(0.4));
    REQUIRE_FALSE(writer.unretract().empty());
    CHECK(writer.tool()->retracted() == 0.);
    CHECK(writer.tool()->used_filament() == Approx(0.).margin(1e-9));
    CHECK(writer.unretract().empty());
}

TEST_CASE("Priming through extrude consumes outstanding retraction", "[GCodeWriter][retraction-accounting]")
{
    const bool relative_e = GENERATE(false, true);
    GCodeConfig config;
    config.use_relative_e_distances.value = relative_e;
    config.use_volumetric_e.value = false;
    config.retract_restart_extra_toolchange.set_at(0., 0);
    Extruder extruder(0, config);

    // WipeTower priming uses extrude(), so a later unretract() must only
    // restore the outstanding amount, never repeat the already emitted E.
    REQUIRE(extruder.retract(0.4, 0., 0.).first == Approx(0.4));
    SECTION("Partial priming leaves only the remainder") {
        extruder.extrude(0.15);
        CHECK(extruder.retracted() == Approx(0.25));
        CHECK(extruder.unretract().first == Approx(0.25));
        CHECK(extruder.used_filament() == Approx(0.).margin(1e-9));
    }
    SECTION("Complete priming does not require another unretraction") {
        extruder.extrude(0.4);
        CHECK_FALSE(extruder.need_unretract());
        CHECK(extruder.unretract().first == 0.);
    }
    SECTION("Excess priming is deposited filament, not negative retraction") {
        extruder.extrude(0.6);
        CHECK(extruder.retracted() == 0.);
        CHECK_FALSE(extruder.need_unretract());
        CHECK(extruder.used_filament() == Approx(0.2));
    }
    SECTION("Priming preserves the configured restart extra") {
        extruder.set_retracted(0.4, 0.1);
        extruder.extrude(0.4);
        CHECK(extruder.need_unretract());
        CHECK(extruder.unretract().first == Approx(0.1));
        CHECK(extruder.used_filament() == Approx(0.1));
    }
    CHECK(extruder.retracted() == 0.);
    CHECK(extruder.unretract().first == 0.);
}

//note: erased by prusa, maybe it's wrong now?
SCENARIO("lift() is not ignored after unlift() at normal values of Z", "[GCodeWriter]") {
    GIVEN("A config from a file and a single extruder.") {
        GCodeWriter writer;
        GCodeConfig &config = writer.config;
        config.load(std::string(TEST_DATA_DIR) + "/fff_print_tests/test_gcodewriter/config_lift_unlift.ini", ForwardCompatibilitySubstitutionRule::Disable);

        std::vector<uint16_t> extruder_ids {0};
        writer.set_extruders(extruder_ids);
        writer.set_tool(0);

        WHEN("Z is set to 203") {
            double trouble_Z = 203;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.retract_lift.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift().size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 500003") {
            double trouble_Z = 500003;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.retract_lift.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift().size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 10.3") {
            double trouble_Z = 10.3;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift().size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.retract_lift.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift().size() > 0);
                        }
                    }
                }
            }
        }
		// The test above will fail for trouble_Z == 9007199254740992, where trouble_Z + 1.5 will be rounded to trouble_Z + 2.0 due to double mantisa overflow.
    }
}

SCENARIO("set_speed emits values with floating-point output, 8 significant digits.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 12345.678") {
            THEN("Output string is G1 12345.678") {
                REQUIRE_THAT(writer.set_speed(12345.678), Catch::Equals("G1 F12345.678\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 12345.200522") {
            THEN("Output string is G1 F12345.201") {
                REQUIRE_THAT(writer.set_speed(12345.200522), Catch::Equals("G1 F12345.201\n"));
            }
        }
    }
}

SCENARIO("set_fan rescales based on fan_percentage.", "[GCode][GCodeWriter]") {
    GIVEN("GCodeWriter instance with comments off and RepRap flavor") {
        GCodeWriter writer;
        writer.config.gcode_comments.value = false;
        writer.config.gcode_flavor.value = gcfRepRap;
        WHEN("set_fan is called to set speed to 100\% with fan_percentage = true") {
            writer.config.fan_percentage.value = true;
            THEN("Fan value is set to 100.") {
                REQUIRE_THAT(writer.set_fan(100, true), Catch::Equals("M106 S100\n"));
            }
            AND_WHEN("Fan value is set to 93\%") {
                THEN("Output string is 'M106 S93'") {
                    REQUIRE_THAT(writer.set_fan(93, true), Catch::Equals("M106 S93\n"));
                }
            }
            AND_WHEN("Fan value is set to 21\%") {
                THEN("Output string is 'M106 S21'") {
                    REQUIRE_THAT(writer.set_fan(21, true), Catch::Equals("M106 S21\n"));
                }
            }
        }
        WHEN("set_fan is called to set speed to 100\% with fan_percentage = false") {
            writer.config.fan_percentage.value = false;
            THEN("Output string is 'M106 S255'") {
                REQUIRE_THAT(writer.set_fan(100, true), Catch::Equals("M106 S255\n"));
            }
            AND_WHEN("Fan value is set to 93\%") {
                THEN("Output string is 'M106 S237'") {
                    REQUIRE_THAT(writer.set_fan(93, true), Catch::Equals("M106 S237.15\n"));
                }
            }
            AND_WHEN("Fan value is set to 21\%") {
                THEN("Output string is 'M106 S54'") {
                    REQUIRE_THAT(writer.set_fan(21, true), Catch::Equals("M106 S53.55\n"));
                }
            }
        }
    }
}
SCENARIO("set_fan saves state.", "[GCode][GCodeWriter]") {
    GIVEN("GCodeWriter instance with comments off and RepRap flavor") {
        GCodeWriter writer;
        writer.config.gcode_comments.value = false;
        writer.config.gcode_flavor.value = gcfRepRap;
        writer.config.fan_percentage.value = true;
        WHEN("set_fan is called to set speed to 100\%, saving") {
            THEN("Fan gcode is emitted.") {
                CHECK_THAT(writer.set_fan(100, false), Catch::Equals("M106 S100\n"));
            }
        }
        AND_WHEN("Another call is made to set_fan for 100\%") {
            THEN("No fan output gcode is emitted.") {
                REQUIRE_THAT(writer.set_fan(100, false), Catch::Equals(""));
            }
        }
        AND_WHEN("Another call is made to set_fan for 90\%") {
            THEN("Fan gcode is emitted.") {
                REQUIRE_THAT(writer.set_fan(90, false), Catch::Equals("M106 S90\n"));
            }
        }
    }
}
