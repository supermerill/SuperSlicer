#include <catch2/catch.hpp>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/PluginProperty.hpp"
#include "libslic3r/Surface.hpp"

namespace {
using namespace Slic3r;

struct TestSurfacePayload
{
    static plugin_property_type property_type;

    uint32_t priority = 0;
    uint32_t layer_count = 0;
};

plugin_property_type TestSurfacePayload::property_type = PLUGIN_PROPERTY_TYPE_INVALID;

void register_test_payload()
{
    TestSurfacePayload::property_type = orchestrator_register_property(
        nullptr,
        "tests.surface.payload",
        sizeof(TestSurfacePayload),
        alignof(TestSurfacePayload));
    REQUIRE(TestSurfacePayload::property_type != PLUGIN_PROPERTY_TYPE_INVALID);
}

ExPolygon rectangle_expolygon(double min_x, double min_y, double max_x, double max_y)
{
    return ExPolygon(Polygon({
        Point(scale_i(min_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(max_y)),
        Point(scale_i(min_x), scale_i(max_y))
    }));
}

} // namespace

TEST_CASE("PluginPropertyContainer stores small typed payloads safely",
          "[plugins][properties]")
{
    register_test_payload();

    PluginPropertyContainer container;

    // A new payload is zero-initialized, so plugins can fill only the fields
    // they care about without reading uninitialized bytes first.
    TestSurfacePayload &payload = container.get_or_add_property<TestSurfacePayload>();
    CHECK(payload.priority == 0);
    CHECK(payload.layer_count == 0);

    payload.priority = 7;
    payload.layer_count = 3;

    const TestSurfacePayload *readback = container.get_property<TestSurfacePayload>();
    REQUIRE(readback != nullptr);
    CHECK(readback->priority == 7);
    CHECK(readback->layer_count == 3);

    // A copied container owns its own bytes. Later mutations on the copy should
    // not change the original payload carried by the source object.
    PluginPropertyContainer copy = container;
    copy.get_or_add_property<TestSurfacePayload>().priority = 11;
    CHECK(container.get_property<TestSurfacePayload>()->priority == 7);
    CHECK(copy.get_property<TestSurfacePayload>()->priority == 11);

    // Reusing a type with a different layout is rejected. This catches the most
    // dangerous plugin mistake: two modules agreeing on a name but disagreeing
    // on the binary struct stored under that name.
    void *wrong_layout = container.get_or_add_property_data_mutable(
        TestSurfacePayload::property_type, sizeof(uint32_t), alignof(uint32_t));
    CHECK(wrong_layout == nullptr);
}

TEST_CASE("Surface plugin properties survive splits and protect merge checks",
          "[plugins][properties]")
{
    register_test_payload();

    Surface source(stPosInternal | stDensSparse, rectangle_expolygon(0., 0., 10., 10.));
    TestSurfacePayload &source_payload = source.get_or_add_property<TestSurfacePayload>();
    source_payload.priority = 4;
    source_payload.layer_count = 2;

    // Splitting a Surface creates new geometry but should keep the logical
    // metadata. A dense-infill plugin, for example, can attach a priority before
    // another plugin clips the area into several pieces.
    Surface split_piece(source, rectangle_expolygon(0., 0., 5., 10.));
    const TestSurfacePayload *split_payload = split_piece.get_property<TestSurfacePayload>();
    REQUIRE(split_payload != nullptr);
    CHECK(split_payload->priority == 4);
    CHECK(split_payload->layer_count == 2);

    // Surfaces may be merged only when plugin metadata matches too. Otherwise a
    // cleanup pass could accidentally fuse two areas that a later plugin expects
    // to process with different parameters.
    Surface same_metadata(source, rectangle_expolygon(5., 0., 10., 10.));
    CHECK(surfaces_could_merge(split_piece, same_metadata));

    same_metadata.get_or_add_property<TestSurfacePayload>().priority = 9;
    CHECK_FALSE(surfaces_could_merge(split_piece, same_metadata));
}
