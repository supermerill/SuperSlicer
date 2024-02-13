
//#define CATCH_CONFIG_DISABLE

#include <catch_main.hpp>
#include "test_data.hpp"
#include <libslic3r/GCode/FanMover.hpp>
#include <libslic3r/ExtrusionEntity.hpp>

#include <boost/algorithm/string.hpp>

using namespace Slic3r;
using namespace Slic3r::Geometry;
using namespace Slic3r::Test;

// note: F600: 10mm/s

TEST_CASE("Simple gcode") {

    GCodeWriter writer;
    //what's used from the writer:
    writer.config.gcode_flavor.value = gcfMarlinFirmware;
    writer.config.gcode_comments.value = false;
    writer.config.fan_percentage.value = false; //0 -> 255
    //writer.tool[0] = nullptr;
    assert(writer.tool() == nullptr);
    assert(writer.get_tool(0) == nullptr);
    
    const std::string gcode = []() {
            std::string gcode;
            gcode += "M107\n";
            gcode += "G1 X0 Y0 F600\n"; // shouldn't move
            gcode += "G1 X20 Y0 E20\n"; // 2 sec
            gcode += "M106 S153\n";     // activate at 60%
            gcode += "G1 X40 Y0 E20\n"; // 2sec
            gcode += "M107\n";          // disable
            gcode += "G1 X60 Y0 E20\n"; // 2sec
            return gcode;
        }();
    
    SECTION("disable evrything")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        REQUIRE(gcode == processed_gcode);
    }

    SECTION("only kickstart 1sec")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   1      // fan_kickstart.value));
        );
        const std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string wanted_gcode;
        wanted_gcode += "M107\n";
        wanted_gcode += "G1 X0 Y0 F600\n"; //shouldn't move
        wanted_gcode += "G1 X20 Y0 E20\n"; // 2 sec
        wanted_gcode += "M106 S255\n"; //activate with kickstart
        wanted_gcode += "G1 X26 Y0 E6\n"; // 1*0.6 = 0.6sec
        wanted_gcode += "M106 S153\n"; //go to good speed 60% of 1 second
        wanted_gcode += "G1 X40 Y0 E14\n"; // 2 - 1*0.6 = 1.4sec
        wanted_gcode += "M107\n"; //disable
        wanted_gcode += "G1 X60 Y0 E20\n"; // 2sec
        REQUIRE(wanted_gcode == processed_gcode);
    }
    
    SECTION("only speedup 1sec")
    {
        Slic3r::FanMover fan_mover(writer,
                                   1,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string wanted_gcode;
        wanted_gcode += "M107\n";
        wanted_gcode += "G1 X0 Y0 F600\n"; //shouldn't move
        wanted_gcode += "G1 X10 Y0 E10\n"; // 1 sec
        wanted_gcode += "M106 S153\n"; //activate
        wanted_gcode += "G1 X20 Y0 E10\n"; // 1 sec
        wanted_gcode += "G1 X40 Y0 E20\n"; // 2sec
        wanted_gcode += "M107\n"; //disable
        wanted_gcode += "G1 X60 Y0 E20\n"; // 2sec
        REQUIRE(wanted_gcode == processed_gcode);
    }
    
    SECTION("speedup deactivated on not-overhangs")
    {
        Slic3r::FanMover fan_mover(writer,
                                   1,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   true, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string type_not_overhang;
        SECTION("erNone") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erNone) + "\n"; }
        SECTION("erPerimeter") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erPerimeter) + "\n"; }
        SECTION("erExternalPerimeter") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erExternalPerimeter) + "\n"; }
        SECTION("erInternalInfill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erInternalInfill) + "\n"; }
        SECTION("erSolidInfill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erSolidInfill) + "\n"; }
        SECTION("erTopSolidInfill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erTopSolidInfill) + "\n"; }
        SECTION("erBridgeInfill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erBridgeInfill) + "\n"; } //note: bridge infill isn't overhang
        SECTION("erInternalBridgeInfill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erInternalBridgeInfill) + "\n"; }
        SECTION("erThinWall") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erThinWall) + "\n"; }
        SECTION("erGapFill") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erGapFill) + "\n"; }
        SECTION("erSkirt") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erSkirt) + "\n"; }
        SECTION("erSupportMaterial") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erSupportMaterial) + "\n"; }
        SECTION("erSupportMaterialInterface") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erSupportMaterialInterface) + "\n"; }
        SECTION("erWipeTower") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erWipeTower) + "\n"; }
        SECTION("erMilling") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erMilling) + "\n"; }
        SECTION("erCustom") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erCustom) + "\n"; }
        SECTION("erMixed") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erMixed) + "\n"; }
        SECTION("erTravel") { type_not_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erTravel) + "\n"; }
        std::string processed_gcode = fan_mover.process_gcode(type_not_overhang + gcode, true);
        REQUIRE(type_not_overhang + gcode == processed_gcode);
    }
    SECTION("speedup activated on overhangs")
    {
        Slic3r::FanMover fan_mover(writer,
                                   1,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   true, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string type_overhang;
        SECTION("erOverhangPerimeter") { type_overhang = std::string(";TYPE:") + ExtrusionEntity::role_to_string(ExtrusionRole::erOverhangPerimeter) + "\n"; }
        std::string processed_gcode = fan_mover.process_gcode(type_overhang + gcode, true);
        std::string wanted_gcode;
        wanted_gcode += "M107\n";
        wanted_gcode += "G1 X0 Y0 F600\n"; //shouldn't move
        wanted_gcode += "G1 X10 Y0 E10\n"; // 1 sec
        wanted_gcode += "M106 S153\n"; //activate
        wanted_gcode += "G1 X20 Y0 E10\n"; // 1 sec
        wanted_gcode += "G1 X40 Y0 E20\n"; // 2sec
        wanted_gcode += "M107\n"; //disable
        wanted_gcode += "G1 X60 Y0 E20\n"; // 2sec
        REQUIRE(type_overhang + wanted_gcode == processed_gcode);
    }
    
    SECTION("erase double M107")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string m107 = "M107\n";
        std::string processed_gcode = fan_mover.process_gcode(m107+gcode+m107, false);
        processed_gcode += fan_mover.process_gcode(gcode+m107, true);
        REQUIRE(gcode+gcode.substr(5) == processed_gcode); // substr to remove first m107
    }
    SECTION("erase M107-like M106 -> M107")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string m106 = "M106 S0\n";
        std::string processed_gcode = fan_mover.process_gcode(gcode+m106, false);
        processed_gcode += fan_mover.process_gcode(gcode, true);
        REQUIRE(gcode+gcode.substr(5) == processed_gcode); // substr to remove first m107
    }
    //TODO?
    //SECTION("erase M106 -> M107")
    //{
    //    Slic3r::FanMover fan_mover(writer,
    //                               0,     // fan_speedup_time.value,
    //                               false, // with_D_option
    //                               true,  // use_relative_e_distances.value,
    //                               false, // fan_speedup_overhangs.value,
    //                               0      // fan_kickstart.value));
    //    );
    //    std::string m106 = "M106 S125\n";
    //    std::string processed_gcode = fan_mover.process_gcode(m106+gcode+m106, false);
    //    processed_gcode += fan_mover.process_gcode(gcode, true);
    //    REQUIRE(gcode+m106 == processed_gcode); // substr to remove first m107
    //}
}


TEST_CASE("Simple gcode (absoute)")
{
    GCodeWriter writer;
    // what's used from the writer:
    writer.config.gcode_flavor.value   = gcfMarlinFirmware;
    writer.config.gcode_comments.value = false;
    writer.config.fan_percentage.value = false; // 0 -> 255
    // writer.tool[0] = nullptr;
    assert(writer.tool() == nullptr);
    assert(writer.get_tool(0) == nullptr);

    const std::string gcode = []() {
        std::string gcode;
        gcode += "M107\n";
        gcode += "G1 X0 Y0 F600\n"; // shouldn't move
        gcode += "G1 X20 Y0 E20\n"; // 2 sec
        gcode += "M106 S153\n";     // activate at 60%
        gcode += "G1 X40 Y0 E40\n"; // 2sec
        gcode += "M107\n";          // disable
        gcode += "G1 X60 Y0 E60\n"; // 2sec
        return gcode;
    }();

    
    SECTION("disable evrything")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   false,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        REQUIRE(gcode == processed_gcode);
    }

    SECTION("only kickstart 1sec")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   false,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   1      // fan_kickstart.value));
        );
        const std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string wanted_gcode;
        wanted_gcode += "M107\n";
        wanted_gcode += "G1 X0 Y0 F600\n"; //shouldn't move
        wanted_gcode += "G1 X20 Y0 E20\n"; // 2 sec
        wanted_gcode += "M106 S255\n"; //activate with kickstart
        wanted_gcode += "G1 X26 Y0 E26\n"; // 1*0.6 = 0.6sec
        wanted_gcode += "M106 S153\n"; //go to good speed 60% of 1 second
        wanted_gcode += "G1 X40 Y0 E40\n"; // 2 - 1*0.6 = 1.4sec
        wanted_gcode += "M107\n"; //disable
        wanted_gcode += "G1 X60 Y0 E60\n"; // 2sec
        REQUIRE(wanted_gcode == processed_gcode);
    }
    
    SECTION("only speedup 1sec")
    {
        Slic3r::FanMover fan_mover(writer,
                                   1,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   false,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string wanted_gcode;
        wanted_gcode += "M107\n";
        wanted_gcode += "G1 X0 Y0 F600\n"; //shouldn't move
        wanted_gcode += "G1 X10 Y0 E10\n"; // 1 sec
        wanted_gcode += "M106 S153\n"; //activate
        wanted_gcode += "G1 X20 Y0 E20\n"; // 1 sec
        wanted_gcode += "G1 X40 Y0 E40\n"; // 2sec
        wanted_gcode += "M107\n"; //disable
        wanted_gcode += "G1 X60 Y0 E60\n"; // 2sec
        REQUIRE(wanted_gcode == processed_gcode);
    }
}

//TODO: complex gcode


////////////// issue 4061: G2/G3: currently, it can't split g2/G3 (TODO)

TEST_CASE("G2/G3 gcode")
{
    auto create_gcode =
        [](bool remove_useless, bool move_useful) -> std::string {
            std::string s = ";LAYER_CHANGE\n";
            s += ";Z:18.4\n";
            s += ";HEIGHT:0.199999\n";
            s += "G92 E0\n";
            s += "G10 ; retract\n";
            s += "; stop printing object Star_face.stl id:0 copy 0\n";
            s += "EXCLUDE_OBJECT_END NAME=Star_face_stl_id_0_copy_0\n";
            s += "M204 S10000\n";
            s += "G1 Z18.4 F1800\n";
            s += "G92 E0\n";
            s += "; printing object Star_face.stl id:0 copy 0\n";
            s += "EXCLUDE_OBJECT_START NAME=Star_face_stl_id_0_copy_0\n";
            s += "; acceleration to travel\n";
            s += "G1 X163.229 Y127.604 F18000\n";
            s += "G1 X146.967 Y135.013\n";
            s += "G1 X146.61 Y135.175\n";
            s += "G1 X123.351 Y145.771\n";
            s += "G1 X122.18 Y146.428\n";
            s += "; decel to extrusion\n";
            s += "M204 S5000\n";
            s += "G1 X118.537 Y148.471\n";
            s += "G1 X117.236 Y148.733\n";
            s += "G1 X116.744 Y148.781\n";
            s += "G1 X116.452 Y148.95\n";
            s += "; end travel\n";
            s += "G11 ; unretract\n";
            s += "G92 E0\n";
            s += ";TYPE:External perimeter\n";
            s += ";WIDTH:0.698815\n";
            s += "M106 S25.5\n";
            s += "G1 F4650\n";
            s += "G1 X115.957 Y148.975 E0.02567\n";
            s += "G3 X105.872 Y142.185 I-.066 J-10.787 E.69483\n";
            s += "G1 X105.745 Y141.815 E0.71508\n";
            s += "G3 X106.128 Y133.713 I10.705 J-3.555 E1.14496\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.728025\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X108.063 Y130.539 I11.385 J4.764 E1.34692\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.75984\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X112.6 Y127.602 I6.872 J5.643 E1.65789\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.789218\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X125.102 Y132.013 I3.494 J10.021 E2.50206\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.755247\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X126.771 Y137.462 I-9.904 J6.012 E2.82604\n";
            if(!remove_useless) s += "M106 S25.5\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 X126.775 Y137.806 E2.84539\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.727549\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X123.57 Y145.764 I-11.051 J.175 E3.32189\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.698815\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X116.606 Y148.952 I-7.679 J-7.576 E3.72755\n";
            s += "G1 X116.527 Y148.951 E3.73167\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIPE_START\n";
            s += "M204 S10000\n";
            s += "G1 X116.189 Y148.524 F18000\n";
            s += ";WIPE_END\n";
            s += "; acceleration to travel\n";
            s += "G1 X115.678 Y148.359\n";
            s += "; decel to extrusion\n";
            s += "M204 S5000\n";
            s += "G1 X115.422 Y148.276\n";
            s += "; end travel\n";
            s += ";WIDTH:0.720254\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 F4650\n";
            s += "G1 X116.001 Y148.314 E3.76269\n";
            s += "G2 X122.624 Y145.725 I-.12 J-10.072 E4.15158\n";
            s += "G2 X123.751 Y144.568 I-8.904 J-9.799 E4.23803\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.748162\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X126.066 Y137.801 I-7.967 J-6.506 E4.64496\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.77744\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X124.935 Y133.225 I-11.113 J.321 E4.92054\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.805275\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X119.271 Y128.291 I-8.81 J4.395 E5.38463\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.773006\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X113.961 Y127.995 I-3.206 J9.712 E5.69495\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.76111\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 X113.76 Y128.037 E5.70656\n";
            s += "G2 X110.376 Y129.391 I2.27 J10.575 E5.91434\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.728025\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X108.025 Y131.708 I5.15 J7.579 E6.09394\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.692664\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X105.869 Y139.349 I8.98 J6.658 E6.51053\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.720254\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X115.208 Y148.292 I10.012 J-1.107 E7.26171\n";
            s += "G1 X115.348 Y148.282 E7.26918\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIPE_START\n";
            s += "M204 S10000\n";
            s += "G1 X115.664 Y148.713 F18000\n";
            s += ";WIPE_END\n";
            s += "G92 E0\n";
            s += "G10 ; retract\n";
            s += "; acceleration to travel\n";
            s += "G1 X115.662 Y148.808\n";
            s += "G1 X120.932 Y147.524\n";
            s += "G1 X121.226 Y147.362\n";
            s += "G1 X150.16 Y140.336\n";
            s += "; decel to extrusion\n";
            s += "M204 S5000\n";
            s += "G1 X156.209 Y138.866\n";
            s += "G1 X156.22 Y138.976\n";
            s += "; end travel\n";
            s += "G11 ; unretract\n";
            s += ";WIDTH:0.649924\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 F4650\n";
            s += "G1 X155.801 Y139.034 E0.02026\n";
            s += "G3 X153.702 Y138.948 I-.587 J-11.382 E.12117\n";
            s += "G3 X152.849 Y138.822 I.823 J-8.513 E.16252\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.683858\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X145.878 Y134.51 I2.171 J-11.301 E.58686\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.711771\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X143.804 Y130.663 I9.809 J-7.768 E.81894\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.74297\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X148.219 Y117.616 I10.583 J-3.69 E1.63918\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.711031\n";
           if(!remove_useless)  s += "M106 S25.5\n";
            s += "G3 X152.826 Y115.811 I6.908 J10.85 E1.90195\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.680045\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X154.402 Y115.729 I1.097 J5.903 E1.9816\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.643995\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G3 X157.534 Y116.615 I-.943 J9.313 E2.13698\n";
            s += "G3 X165.073 Y124.528 I-4.996 J12.307 E2.67179\n";
            s += "G1 X165.155 Y124.808 E2.68563\n";
            s += "G3 X165.6 Y127.286 I-12.334 J3.496 E2.80535\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.649924\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 X165.609 Y127.779 E2.82901\n";
            s += "G3 X163.67 Y134.419 I-11.681 J.192 E3.1657\n";
            s += "G3 X161.835 Y136.929 I-6.919 J-3.132 E3.31587\n";
            s += "G3 X156.616 Y138.963 I-6.621 J-9.277 E3.58724\n";
            s += "G1 X156.295 Y138.974 E3.60263\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIPE_START\n";
            s += "M204 S10000\n";
            s += "G1 X155.927 Y138.571 F18000\n";
            s += ";WIPE_END\n";
            s += "M204 S5000\n";
            s += "G1 X155.863 Y138.403\n";
            s += ";WIDTH:0.638248\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 F4650\n";
            s += "G1 X155.909 Y138.419 E3.60495\n";
            s += "G2 X161.549 Y136.388 I-.727 J-10.866 E3.89052\n";
            s += "G2 X163.15 Y134.126 I-4.753 J-5.062 E4.02169\n";
            s += "G2 X164.82 Y125.892 I-9.372 J-6.188 E4.42665\n";
            s += "G1 X164.74 Y125.556 E4.4429\n";
            s += "G2 X159.719 Y118.49 I-11.601 J2.927 E4.86006\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.667068\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X154.678 Y116.365 I-7.59 J10.96 E5.13167\n";
            if (move_useful) {
                s += "G1 X154.511 Y116.36 E5.13992\n";
                s += "M106 S140.25\n";
            }
            s += "G1 X154.256 Y116.352 E5.15251\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.707201\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X152.007 Y116.669 I.061 J8.587 E5.27203\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.741124\n";
           if(!remove_useless)  s += "M106 S25.5\n";
            s += "G2 X143.911 Y126.283 I2.509 J10.329 E6.01375\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.713385\n";
           if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X144.261 Y129.843 I10.983 J.716 E6.20405\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.681374\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X146.03 Y133.648 I11.334 J-2.956 E6.41684\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.649924\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G2 X153.408 Y138.302 I9.116 J-6.276 E6.84676\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIDTH:0.638248\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += "G1 X153.561 Y138.322 E6.85401\n";
            s += "G2 X155.003 Y138.441 I1.621 J-10.769 E6.9221\n";
            s += "G1 X155.788 Y138.406 E6.95905\n";
            if(!remove_useless) s += "M106 S25.5\n";
            s += ";WIPE_START\n";
            s += "M204 S10000\n";
            s += "G1 X156.022 Y138.877 F18000\n";
            s += ";WIPE_END\n";
            s += "G92 E0\n";
            s += "G10 ; retract\n";
            s += "; acceleration to travel\n";
            s += "G1 X155.991 Y138.508\n";
            s += "G1 X163.054 Y127.686\n";
            s += "G1 X163.042 Y127.702\n";
            s += "; decel to extrusion\n";
            s += "M204 S500\n";
            s += "G1 X162.795 Y128.055\n";
            s += "; end travel\n";
            s += "G11 ; unretract\n";
            s += ";TYPE:Overhang perimeter\n";
            s += ";WIDTH:0.542919\n";
            s += ";HEIGHT:0.5\n";
            if(!move_useful) s += "M106 S140.25\n";
            s += "G1 F1800\n";
            s += "G3 X163.624 Y127.86 I.327 J-.467 E.10369\n";
            s += "G3 X163.258 Y128.142 I-.531 J-.312 E.1224\n";
            s += "G3 X162.86 Y128.094 I-.135 J-.555 E.13857\n";
            s += "M106 S25.5\n";
            s += "; acceleration to travel\n";
            s += "M204 S10000\n";
            s += "G1 X163.052 Y127.812 F18000\n";
            s += "; decel to extrusion\n";
            s += "M204 S5000\n";
            s += "G1 X163.2 Y127.595\n";
            s += "; end travel\n";
            s += ";TYPE:Internal perimeter\n";
            s += ";WIDTH:0.702708\n";
            s += ";HEIGHT:0.2\n";
           if(!remove_useless)  s += "M106 S25.5\n";
            s += "G1 F4650\n";
            s += "G3 X163.159 Y127.523 I-.077 J-.003 E.15938\n";
           if(!remove_useless)  s += "M106 S25.5\n";
            return s;
        };

    const std::string gcode = create_gcode(false, false);
    //"M106 S25.5\n";
    GCodeWriter       writer;
    // what's used from the writer:
    writer.config.gcode_flavor.value   = gcfMarlinFirmware;
    writer.config.gcode_comments.value = false;
    writer.config.fan_percentage.value = false; // 0 -> 255
    // writer.tool[0] = nullptr;
    assert(writer.tool() == nullptr);
    assert(writer.get_tool(0) == nullptr);

    SECTION("disable evrything")
    {
        Slic3r::FanMover fan_mover(writer,
                                   0,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   false,  // use_relative_e_distances.value,
                                   false, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string      processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string good_gcode = create_gcode(true, false);
        //remove 
        REQUIRE(good_gcode == processed_gcode);
    }

    SECTION("4061 bug")
    {
        Slic3r::FanMover fan_mover(writer,
                                   .486,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   false,  // use_relative_e_distances.value,
                                   true, // fan_speedup_overhangs.value,
                                   0      // fan_kickstart.value));
        );
        std::string      processed_gcode = fan_mover.process_gcode(gcode, true);
        std::string good_gcode = create_gcode(true, true);

        REQUIRE(good_gcode == processed_gcode);
    }
}

TEST_CASE("4113 bug (erase all fan command after a point)") {
    std::string gcode_input = read_to_string(std::string(TEST_DATA_DIR) + "/test_gcode/4113_fan_mover.gcode");
    std::string gcode_output = read_to_string(std::string(TEST_DATA_DIR) + "/test_gcode/4113_fan_mover_ok.gcode");
    
    //"M106 S25.5\n";
    GCodeWriter       writer;
    // what's used from the writer:
    writer.config.gcode_flavor.value   = gcfMarlinFirmware;
    writer.config.gcode_comments.value = false;
    writer.config.fan_percentage.value = false; // 0 -> 255
    // writer.tool[0] = nullptr;
    assert(writer.tool() == nullptr);
    assert(writer.get_tool(0) == nullptr);

    SECTION("disable evrything")
    {
        Slic3r::FanMover fan_mover(writer,
                                   1,     // fan_speedup_time.value,
                                   false, // with_D_option
                                   true,  // use_relative_e_distances.value,
                                   true, // fan_speedup_overhangs.value,
                                   0.5      // fan_kickstart.value));
        );
        std::string      processed_gcode = fan_mover.process_gcode(gcode_input, true);
        //remove
        //REQUIRE(good_gcode == processed_gcode);
        REQUIRE(processed_gcode.find("M106 S51") != std::string::npos);
        REQUIRE(gcode_output == processed_gcode);
    }

}
