
//#define CATCH_CONFIG_DISABLE
#include <catch_main.hpp>

#include "test_data.hpp"
#include <libslic3r/libslic3r.h>
#include <libslic3r/Layer.hpp>
#include <libslic3r/SVG.hpp>
#include <libslic3r/Format/3mf.hpp>
//#include <libslic3r/config.hpp>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;
using namespace std::literals;

namespace {

const PrintObject &print_object(const Print &print, size_t idx)
{
    return *print.objects()[idx];
}

template<class Fn>
void for_each_region_perimeter_collection(const LayerRegion *region, Fn &&fn)
{
    const Layer *layer = region->layer();
    for (const LayerSliceIslandPtr &island : layer->islands()) {
        for (const LayerRegionIslandPtr &region_island : island->regions_islands()) {
            if (region_island->regions().count(region) != 0 &&
                region_island->has_extrusion(LayerRegionIsland::PERIMETERS)) {
                fn(region_island->extrusion(LayerRegionIsland::PERIMETERS));
            }
        }
    }
}

size_t region_perimeter_entity_count(const LayerRegion *region)
{
    size_t count = 0;
    for_each_region_perimeter_collection(region, [&count](const ExtrusionEntityCollection &perimeters) {
        count += perimeters.entities().size();
    });
    return count;
}

size_t region_perimeter_items_count(const LayerRegion *region)
{
    size_t count = 0;
    for_each_region_perimeter_collection(region, [&count](const ExtrusionEntityCollection &perimeters) {
        count += perimeters.items_count();
    });
    return count;
}

template<class Visitor>
void visit_region_perimeters(const LayerRegion *region, Visitor &visitor)
{
    for_each_region_perimeter_collection(region, [&visitor](const ExtrusionEntityCollection &perimeters) {
        const_cast<ExtrusionEntityCollection &>(perimeters).visit(visitor);
    });
}

} // namespace

SCENARIO("PrintObject: Perimeter generation") {
    GIVEN("20mm cube and default config & 0.3 layer height") {
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        TestMesh m = TestMesh::cube_20x20x20;
        Model model{};
        config.set_key_value("fill_density", new ConfigOptionPercent(0));
        config.set_deserialize("nozzle_diameter", "0.4");
        config.set_deserialize("layer_height", "0.3");

        WHEN("make_perimeters() is called")  {
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            const PrintObject& object = print_object(print, 0);
            print.process();
            // there are 66.66666.... layers for 0.3mm in 20mm
            //slic3r is rounded (slice at half-layer), slic3rPE is less?
            //TODO: check the slic32r why it's not cut at half-layer
            THEN("67 layers exist in the model") {
                REQUIRE(object.layers().size() == 67);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers()) {
                    REQUIRE(region_perimeter_entity_count(layer->regions()[0]) == 1);
                }
            }
            THEN("Every layer (but top) in region 0 has 3 paths in its perimeters list.") {
                auto layers = object.layers();
                for (auto it_layer = layers.begin(); it_layer != layers.end() - 1; ++it_layer) {
                    REQUIRE(region_perimeter_items_count((*it_layer)->regions()[0]) == 3);
                }
            }
            THEN("Top layer in region 0 has 1 path in its perimeters list (only 1 perimeter on top).") {
                REQUIRE(region_perimeter_items_count(object.layers().back()->regions()[0]) == 1);
            }
        }
    }
}

SCENARIO("Print: Skirt generation") {
    GIVEN("20mm cube and default config") {
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        TestMesh m = TestMesh::cube_20x20x20;
        Slic3r::Model model{};
        config.set_key_value("skirt_height", new ConfigOptionInt(1));
        config.set_key_value("skirt_distance", new ConfigOptionFloat(1));
        WHEN("Skirts is set to 2 loops")  {
            config.set_key_value("skirts", new ConfigOptionInt(2));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten(false).entities().size() == 2);
            }
        }
    }
}

void test_is_solid_infill(Print &p, size_t obj_id, size_t layer_id, bool check = true ) {
    const PrintObject& obj { print_object(p, obj_id) };
    const Layer& layer { *(obj.get_layer((int)layer_id)) };

    // iterate over all of the regions in the layer
    for (const LayerRegion* reg : layer.regions()) {
        // for each region, iterate over the fill surfaces
        for (const Surface& su : reg->fill_surfaces().surfaces) {
            CHECK(su.has_fill_solid() == check);
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        TestMesh m { TestMesh::cube_20x20x20 };
        config.set_key_value("top_solid_layers", new ConfigOptionInt(2));
        config.set_key_value("bottom_solid_layers", new ConfigOptionInt(1));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.5)); // get a known number of layers
        config.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent(0.5, false));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({1})); // has to be large enough for 0.5 layer height, the min/max lh depends on it
        config.set_key_value("enforce_full_fill_volume", new ConfigOptionBool(true)); 
        Slic3r::Model model;
        auto event_counter {0U};
        std::string stage;
        Print print{};
        Slic3r::Test::init_print(print, { m }, model, &config);
        print.process();
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
        THEN("20 mm with 0.5 layer height means 40 layers.")
        {
            REQUIRE(print.objects().front()->layer_count() == 40);
        }
        THEN("First layer is solid bottom layer.")
        {
            test_is_solid_infill(print, 0, 0); // should be solid
            test_is_solid_infill(print, 0, 1, false); // should not be solid
        }
        THEN("Two last layers are solid bottom layer.")
        {
            test_is_solid_infill(print, 0, 39); // should be solid
            test_is_solid_infill(print, 0, 38); // should be solid
            test_is_solid_infill(print, 0, 37, false); // should not be solid
        }
        WHEN("Model is re-sliced with top_solid_layers == 3") {
            ((ConfigOptionInt&)(print.get_print_region(0).config().top_solid_layers)).value = 3;
            REQUIRE(print.get_print_region(0).config().top_solid_layers.value == 3);
            DynamicPrintConfig useless;
            print.invalidate_state_by_config_options(useless, std::vector<Slic3r::t_config_option_key>{ "posPrepareInfill" });
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(print, 0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(print, 0, 39);
                test_is_solid_infill(print, 0, 38);
                test_is_solid_infill(print, 0, 37);
            }
        }
    }

}

SCENARIO("Print: Brim generation") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        TestMesh m{ TestMesh::cube_20x20x20 };
        Slic3r::Model model{};
        config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(1, false));
        WHEN("Brim is set to 3mm")  {
            config.set_key_value("brim_width", new ConfigOptionFloat(3));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            //{
            //    std::stringstream stri;
            //    stri << "20mm_cube_brim_test_" << ".svg";
            //    SVG svg(stri.str());
            //    svg.draw(print.brim().as_polylines(), "red");
            //    svg.Close();
            //}
            THEN("Brim Extrusion collection has 3 loops in it") {
                REQUIRE(print.brim().items_count() == 3);
            }
        }
        WHEN("Brim is set to 6mm") {
            config.set_key_value("brim_width", new ConfigOptionFloat(6));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            THEN("Brim Extrusion collection has 6 loops in it") {
                REQUIRE(print.brim().items_count() == 6);
            }
        }
        WHEN("Brim is set to 6mm with 1mm offset") {
            config.set_key_value("brim_width", new ConfigOptionFloat(6));
            config.set_key_value("brim_offset", new ConfigOptionFloat(1));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            THEN("Brim Extrusion collection has 6 loops in it, even with offset") {
                REQUIRE(print.brim().items_count() == 6);
            }
        }
        WHEN("Brim without first layer compensation") {
            config.set_key_value("brim_width", new ConfigOptionFloat(1));
            config.set_key_value("brim_offset", new ConfigOptionFloat(0));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            Flow brim_flow = print.brim_flow(0, print.default_object_config());
            THEN("First Brim Extrusion has a length of ~84") {
                REQUIRE(print.brim().entities().size() > 0);
                double dist = unscaled(ExtrusionLength{}.length(*print.brim().entities().front()));
                REQUIRE(dist < (20 + brim_flow.spacing()) * 4 ); // little lower because of the round edge at the corners
                REQUIRE(dist > (20 + brim_flow.spacing()) * 4 - 1);
            }
        }
        WHEN("Brim with 1mm first layer compensation") {
            config.set_key_value("brim_width", new ConfigOptionFloat(1));
            config.set_key_value("brim_offset", new ConfigOptionFloat(0));
            config.set_key_value("first_layer_size_compensation", new ConfigOptionFloat(-0.5));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            Flow brim_flow = print.brim_flow(0, print_object(print, 0).config());
            THEN("First Brim Extrusion has a length of ~80") {
                REQUIRE(print.brim().entities().size() > 0);
                double dist = unscaled(ExtrusionLength{}.length(*print.brim().entities().front()));
                REQUIRE(dist < (20 + brim_flow.spacing() - 1) * 4 );// little lower because of the round edge at the corners
                REQUIRE(dist > (20 + brim_flow.spacing() - 1) * 4 - 1);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
            config.set_key_value("brim_width", new ConfigOptionFloat(6));
            config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(0.5, false));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            double nbLoops = 6.0 / print.brim_flow(*print.extruders().begin(), print_object(print, 0).config()).spacing();
            THEN("Brim Extrusion collection has " + std::to_string(nbLoops) + " loops in it (flow=" +
                 std::to_string(print.brim_flow(*print.extruders().begin(), print_object(print, 0).config()).spacing()) + ")")
            {
                REQUIRE(print.brim().items_count() == floor(nbLoops));
            }
        }
        WHEN("Brim ears activated, 3mm") {
            config.set_key_value("brim_width", new ConfigOptionFloat(3));
            config.set_key_value("brim_ears", new ConfigOptionBool(true));
            Print print{};
            Slic3r::Test::init_print(print, { m }, model, &config);
            print.process();
            THEN("Brim ears Extrusion collection has 4 extrusions in it") {
                REQUIRE(print.brim().items_count() == 4);
            }
        }
    }
}

struct GetFirst : ExtrusionVisitorRecursive
{
    ExtrusionLoop* first = nullptr;
    ExtrusionLoop* previous_last = nullptr;
    ExtrusionLoop* last = nullptr;
    void default_use(ExtrusionEntity &entity) override { assert(false); };
    void use(ExtrusionLoop& loop) override { if(!first) first = &loop; previous_last = last; last = &loop; }
};

SCENARIO("Print: perimeter generation : cube with hole, just enough space for two loops at a point")
{
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::Model       model{};
    config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(0.42, false));
    config.set_deserialize("nozzle_diameter", "0.4");
    config.set_deserialize("layer_height", "0.2");
    config.set_deserialize("first_layer_height", "0.2");
    config.set_key_value("only_one_perimeter_top", new ConfigOptionBool(false));

    auto facets = std::vector<Vec3i32>{Vec3i32(1, 4, 3),    Vec3i32(4, 1, 2),    Vec3i32(16, 12, 14),
                                       Vec3i32(16, 10, 12), Vec3i32(10, 4, 6),   Vec3i32(4, 10, 16),
                                       Vec3i32(8, 14, 12),  Vec3i32(8, 2, 14),   Vec3i32(6, 2, 8),
                                       Vec3i32(2, 6, 4),    Vec3i32(14, 15, 16), Vec3i32(15, 14, 13),
                                       Vec3i32(15, 4, 16),  Vec3i32(4, 15, 3),   Vec3i32(13, 11, 15),
                                       Vec3i32(13, 7, 11),  Vec3i32(7, 1, 5),    Vec3i32(1, 7, 13),
                                       Vec3i32(9, 15, 11),  Vec3i32(9, 3, 15),   Vec3i32(5, 3, 9),
                                       Vec3i32(3, 5, 1),    Vec3i32(1, 14, 2),   Vec3i32(14, 1, 13),
                                       Vec3i32(9, 12, 10),  Vec3i32(12, 9, 11),  Vec3i32(6, 9, 10),
                                       Vec3i32(9, 6, 5),    Vec3i32(8, 5, 6),    Vec3i32(5, 8, 7),
                                       Vec3i32(7, 12, 11),  Vec3i32(12, 7, 8)};
    for (Vec3i32 &vec : facets) vec -= Vec3i32(1, 1, 1);
    TriangleMesh tm = TriangleMesh{std::vector<Vec3f>{Vec3f(-5, -5, -0.1), Vec3f(-5, -5, 0.1), Vec3f(-5, 5, -0.1),
                                                      Vec3f(-5, 5, 0.1), Vec3f(-1.328430, 0, -0.1),
                                                      Vec3f(-1.328430, 0, 0.1), Vec3f(1.5, -2.828430, -0.1),
                                                      Vec3f(1.5, -2.828430, 0.1), Vec3f(1.5, 2.828430, -0.1),
                                                      Vec3f(1.5, 2.828430, 0.1), Vec3f(4.328430, 0, -0.1),
                                                      Vec3f(4.328430, 0, 0.1), Vec3f(5, -5, -0.1), Vec3f(5, -5, 0.1),
                                                      Vec3f(5, 5, -0.1), Vec3f(5, 5, 0.1)},
                                   facets};

    GIVEN("no brim")
    {
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        ExtrusionPrinter printer(/*mult=*/0.000001, /*trunc=*/100, /*json=*/true);
        //std::cout << "\n\n\nNO BRIM EXTUSIONS:\n";
        //print.objects()[0]->layers()[0]->regions()[0]->perimeters.visit(printer);
        //std::cout << "{" << printer.str() << "}";
        //Model model = print.model();
        //Slic3r::store_3mf("test_without_brim.3mf", &model, &print.full_print_config(), OptionStore3mf{});
        // see https://github.com/supermerill/SuperSlicer/issues/242, why the hole is after the contour inner
        THEN("hole perimeter should not be printed first, but still before external one")
        {
            GetFirst get_first_visitor;
            visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_first_visitor);
            REQUIRE(get_first_visitor.first != nullptr);
            REQUIRE(get_first_visitor.first->is_loop());
            // first inner contour peri
            REQUIRE((get_first_visitor.first->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE(get_first_visitor.first->role() == ExtrusionRole::Perimeter);
            // the external hole perimeter (because it's alone without inner ones, we don't want it to be used as a skirt)
            // note: here we may want to have the seam of this one near the next one instead of near our current pos,
            // if it's an external one, to avoid oozing before external.
            REQUIRE((get_first_visitor.previous_last->loop_role() & ExtrusionLoopRole::elrHole) != 0);
            REQUIRE(get_first_visitor.previous_last->role() == ExtrusionRole::ExternalPerimeter);
            // the external contour perimeter
            REQUIRE((get_first_visitor.last->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE(get_first_visitor.last->role() == ExtrusionRole::ExternalPerimeter);
        }
    }
    GIVEN("brim")
    {
        config.set_deserialize("brim_width", "0.2");
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        THEN("hole perimeter should not be printed first")
        {
            GetFirst get_first_visitor;
            visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_first_visitor);
            // the external contour perimeter
            REQUIRE((get_first_visitor.first->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE(get_first_visitor.first->role() == ExtrusionRole::ExternalPerimeter);
            // first inner contour peri
            REQUIRE((get_first_visitor.previous_last->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE(get_first_visitor.previous_last->role() == ExtrusionRole::Perimeter);
            // the external hole perimeter
            REQUIRE((get_first_visitor.last->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(get_first_visitor.last->role() == ExtrusionRole::ExternalPerimeter);
        }
    }
}


struct GetAll : ExtrusionVisitorRecursive
{
    std::vector<ExtrusionLoop*> loops;
    void default_use(ExtrusionEntity &entity) override { assert(false); };
    void use(ExtrusionLoop& loop) override { loops.push_back(&loop); }
};
SCENARIO("Print: perimeter generation : cube with hole in center") {
    DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::Model model{};
    config.set_key_value("first_layer_extrusion_width", new ConfigOptionFloatOrPercent(0.42, false));
    config.set_deserialize("nozzle_diameter", "0.4");
    config.set_deserialize("layer_height", "0.2");
    config.set_deserialize("first_layer_height", "0.2");
    config.set_key_value("only_one_perimeter_top", new ConfigOptionBool(false));

    std::vector<Vec3f>   v{{-10, 10, -0.1},   {-10, -10, -0.1},   {-10, 10, 0.1},    {-10, -10, 0.1},
                         {10, -10, -0.1},   {10, -10, 0.1},     {-2.5, 2.5, 0.1},  {-2.5, -2.5, 0.1},
                         {2.5, -2.5, 0.1},  {10, 10, 0.1},      {2.5, 2.5, 0.1},   {10, 10, -0.1},
                         {-2.5, 2.5, -0.1}, {-2.5, -2.5, -0.1}, {2.5, -2.5, -0.1}, {2.5, 2.5, -0.1}};
    std::vector<Vec3i32> i{{0, 1, 2},    {1, 3, 2},    {1, 4, 3},   {4, 5, 3},   {6, 2, 3},   {6, 3, 7},
                           {5, 8, 7},    {5, 7, 3},    {9, 6, 10},  {9, 10, 8},  {9, 8, 5},   {9, 2, 6},
                           {11, 0, 2},   {9, 11, 2},   {0, 12, 1},  {1, 12, 13}, {14, 4, 13}, {13, 4, 1},
                           {12, 11, 15}, {15, 11, 14}, {14, 11, 4}, {0, 11, 12}, {4, 11, 9},  {5, 4, 9},
                           {7, 13, 12},  {7, 12, 6},   {8, 14, 13}, {8, 13, 7},  {14, 8, 15}, {15, 8, 10},
                           {15, 10, 12}, {12, 10, 6}};
    TriangleMesh         tm = TriangleMesh(v, i);
    
    GIVEN("no brim")
    {
        config.set_deserialize("brim_width", "0");
        config.set_deserialize("brim_width_interior", "0");
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        // see https://github.com/supermerill/SuperSlicer/issues/242, why the hole is after the contour inner
        GetAll get_all_visitor;
        visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_all_visitor);
        auto &loops = get_all_visitor.loops;
        THEN("hole printed first, external last")
        {
            REQUIRE(loops.size() == 6);
            // first holes
            REQUIRE((loops[0]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[0]->role() == ExtrusionRole::Perimeter);
            REQUIRE((loops[2]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[2]->role() == ExtrusionRole::ExternalPerimeter);
        }
        THEN("contour printed last, external last")
        {
            //then contour
            REQUIRE( (loops[3]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[3]->role() == ExtrusionRole::Perimeter);
            REQUIRE( (loops[5]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[5]->role() == ExtrusionRole::ExternalPerimeter);
        }
    }
    GIVEN("contour brim")
    {
        config.set_deserialize("brim_width", "1");
        config.set_deserialize("brim_width_interior", "0");
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        ExtrusionPrinter printer(/*mult=*/0.000001, /*trunc=*/100, /*json=*/true);
        GetAll get_all_visitor;
        visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_all_visitor);
        auto &loops = get_all_visitor.loops;
        REQUIRE(loops.size() == 6);
        THEN("contour printed first, external first")
        {
            //then contour
            REQUIRE( (loops[0]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[0]->role() == ExtrusionRole::ExternalPerimeter);
            REQUIRE( (loops[2]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[2]->role() == ExtrusionRole::Perimeter);
        }
        THEN("hole printed last, external last as there is no hole brim")
        {
            // first holes
            REQUIRE((loops[3]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[3]->role() == ExtrusionRole::Perimeter);
            REQUIRE((loops[5]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[5]->role() == ExtrusionRole::ExternalPerimeter);
        }
        //TODO: thinwall after evrything
    }
    GIVEN("hole brim")
    {
        config.set_deserialize("brim_width", "0");
        config.set_deserialize("brim_width_interior", "1");
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        ExtrusionPrinter printer(/*mult=*/0.000001, /*trunc=*/100, /*json=*/true);
        GetAll get_all_visitor;
        visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_all_visitor);
        auto &loops = get_all_visitor.loops;
        REQUIRE(loops.size() == 6);
        THEN("hole printed first, external first")
        {
            // first holes
            REQUIRE((loops[0]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[0]->role() == ExtrusionRole::ExternalPerimeter);
            REQUIRE((loops[2]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[2]->role() == ExtrusionRole::Perimeter);
        }
        THEN("contour printed last, external last")
        {
            //then contour
            REQUIRE( (loops[3]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[3]->role() == ExtrusionRole::Perimeter);
            REQUIRE( (loops[5]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[5]->role() == ExtrusionRole::ExternalPerimeter);
        }
        //TODO: thinwall after evrything
    }
    GIVEN("both brim")
    {
        config.set_deserialize("brim_width", "1");
        config.set_deserialize("brim_width_interior", "1");
        Print print{};
        Slic3r::Test::init_print(print, {tm}, model, &config);
        print.process();
        ExtrusionPrinter printer(/*mult=*/0.000001, /*trunc=*/100, /*json=*/true);
        GetAll get_all_visitor;
        visit_region_perimeters(print.objects()[0]->layers()[0]->regions()[0], get_all_visitor);
        auto &loops = get_all_visitor.loops;
        REQUIRE(loops.size() == 6);
        THEN("contour printed first, external first")
        {
            //then contour
            REQUIRE( (loops[0]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[0]->role() == ExtrusionRole::ExternalPerimeter);
            REQUIRE( (loops[2]->loop_role() & ExtrusionLoopRole::elrHole) == 0);
            REQUIRE( loops[2]->role() == ExtrusionRole::Perimeter);
        }
        THEN("hole printed last, external first")
        {
            // first holes
            REQUIRE((loops[3]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[3]->role() == ExtrusionRole::ExternalPerimeter);
            REQUIRE((loops[5]->loop_role() & ExtrusionLoopRole::elrHole) == ExtrusionLoopRole::elrHole);
            REQUIRE(loops[5]->role() == ExtrusionRole::Perimeter);
        }
        //TODO: thinwall after evrything
    }
}

