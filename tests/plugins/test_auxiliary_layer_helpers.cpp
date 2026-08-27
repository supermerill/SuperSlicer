#include <catch2/catch.hpp>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/cpp/AuxiliaryLayerHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config/ConfigOption.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <vector>

namespace {

using namespace Slic3r;

struct Box
{
    double min_x = 0.;
    double min_y = 0.;
    double min_z = 0.;
    double max_x = 0.;
    double max_y = 0.;
    double max_z = 0.;
};

/*
The tests use explicit boxes instead of the canned cube fixture because
auxiliary-layer masks are evaluated at one Z. Controlling the box extents keeps
the expected mask relationships obvious: a small modifier box is visibly inside
the large model-part box.
*/
TriangleMesh make_box(const Box &box)
{
    std::vector<Vec3f> vertices = {
        {float(box.min_x), float(box.min_y), float(box.min_z)},
        {float(box.max_x), float(box.min_y), float(box.min_z)},
        {float(box.max_x), float(box.max_y), float(box.min_z)},
        {float(box.min_x), float(box.max_y), float(box.min_z)},
        {float(box.min_x), float(box.min_y), float(box.max_z)},
        {float(box.max_x), float(box.min_y), float(box.max_z)},
        {float(box.max_x), float(box.max_y), float(box.max_z)},
        {float(box.min_x), float(box.max_y), float(box.max_z)}
    };
    std::vector<Vec3i32> faces = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7}
    };
    return TriangleMesh(std::move(vertices), std::move(faces));
}

DynamicPrintConfig print_config()
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "1"},
        {"first_layer_height", "1"},
        {"fill_density", "20%"},
        {"support_material", "0"}
    });
    return config;
}

ModelVolume *add_volume(ModelObject &object, TriangleMesh &&mesh, ModelVolumeType type)
{
    ModelVolume *volume = object.add_volume(std::move(mesh), type, false);
    volume->set_type(type);
    return volume;
}

DynamicPrintConfig fill_density_config(const char *value)
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config;
    config.set_deserialize_strict({{"fill_density", value}});
    return config;
}

Model make_auxiliary_region_model(bool part_has_region_override, bool with_modifier, bool with_negative)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "auxiliary_layer_regions.stl";

    ModelVolume *part = add_volume(*object, make_box({-10., -10., 0., 10., 10., 10.}), ModelVolumeType::MODEL_PART);
    if (part_has_region_override)
        part->config.assign_config(fill_density_config("80%"));

    if (with_modifier) {
        ModelVolume *modifier =
            add_volume(*object, make_box({-4., -4., 0., 4., 4., 10.}), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->config.assign_config(fill_density_config("90%"));
    }

    if (with_negative)
        add_volume(*object, make_box({-3., -3., 0., 3., 3., 10.}), ModelVolumeType::NEGATIVE_VOLUME);

    object->add_instance();
    return model;
}

void init_print_from_model(Model &model, Print &print)
{
    DynamicPrintConfig config = print_config();
    model.center_instances_around_point({100, 100});
    for (ModelObject &object : model.objects()) {
        object.ensure_on_bed();
        print.auto_assign_extruders(&object);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

void run_until_sliced(Print &print)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, print);
    Steps::StepSlicing::run_step(orchestrator, print);
    Steps::StepPostSlicing::run_step(orchestrator, print);
}

ExPolygon shifted_copy(const ExPolygon &source, coord_t dx)
{
    ExPolygon shifted = source;
    shifted.translate(Point(dx, 0));
    return shifted;
}

slic3r_api::Print print_view(Print &print)
{
    return slic3r_api::Print(reinterpret_cast<const print_handle *>(&print));
}

slic3r_api::Object object_view(PrintObject &object)
{
    return slic3r_api::Object(reinterpret_cast<const object_handle *>(&object));
}

slic3r_api::ExPolygonCollection expolygons_view(const ExPolygons &expolygons)
{
    return slic3r_api::ExPolygonCollection(reinterpret_cast<const expolygon_collection_handle *>(&expolygons));
}

double area_mm2(const ExPolygons &areas)
{
    return unscaled(unscaled(std::abs(area(areas))));
}

ExPolygon rectangle_expolygon(const double min_x, const double min_y, const double max_x, const double max_y)
{
    return ExPolygon(Polygon({
        Point(scale_i(min_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(max_y)),
        Point(scale_i(min_x), scale_i(max_y))
    }));
}

} // namespace

TEST_CASE("PrintApply keeps region zero as object default fallback",
          "[plugins][auxiliary-layer][regions]")
{
    Model model = make_auxiliary_region_model(true, false, false);
    Print print;
    init_print_from_model(model, print);

    const PrintObject &object = print.object(0);
    REQUIRE(object.num_printing_regions() >= 2);

    /*
    The only model part overrides fill_density, so older region creation could
    make that model-part region index zero. Auxiliary layers need index zero to
    stay the object default region because it receives subject pixels outside
    all model-part/modifier masks.
    */
    CHECK(object.printing_region(0).config().fill_density.value == Approx(20.));
}

TEST_CASE("Auxiliary layer C API creates mutable generic layers",
          "[plugins][auxiliary-layer][api]")
{
    Model model = make_auxiliary_region_model(false, false, false);
    Print print;
    init_print_from_model(model, print);
    PrintObject &object = print.object(0);
    object.clear_auxiliary_layers();

    const object_handle *object_handle_value = reinterpret_cast<const object_handle *>(&object);
    layer_handle *layer_handle_value =
        object_add_auxiliary_layer(object_handle_value, scale_i(1.), scale_i(1.), scale_i(0.5));

    REQUIRE(layer_handle_value != nullptr);
    REQUIRE(object_count_auxiliary_layer(object_handle_value) == 1);
    CHECK(object_get_auxiliary_layer(object_handle_value, 0) == layer_handle_value);

    /*
    Auxiliary layers are generic plugin work layers. They start with regions
    matching the object's PrintRegions but no raw slices; a later helper or
    plugin writes geometry and recomputes islands explicitly.
    */
    Layer *layer = reinterpret_cast<Layer *>(layer_handle_value);
    CHECK(layer->regions().size() == object.num_printing_regions());
    CHECK(layer->lslices().empty());

    CHECK(object_remove_auxiliary_layer(object_handle_value, layer_handle_value) != 0);
    CHECK(object_count_auxiliary_layer(object_handle_value) == 0);
}

TEST_CASE("Print auxiliary object owns print-level auxiliary layers outside object list",
          "[plugins][auxiliary-layer][api]")
{
    Print print;
    const print_handle *print_handle_value = reinterpret_cast<const print_handle *>(&print);
    CHECK(print_count_object(print_handle_value) == 0);
    CHECK(print.auxiliary_object() == nullptr);

    object_handle *first_auxiliary_object = print_get_auxiliary_object(print_handle_value);
    object_handle *second_auxiliary_object = print_get_auxiliary_object(print_handle_value);
    REQUIRE(first_auxiliary_object != nullptr);
    CHECK(first_auxiliary_object == second_auxiliary_object);
    CHECK(print_count_object(print_handle_value) == 0);

    /*
    The hidden object is a normal auxiliary-layer owner. It has no object layers
    and no model instances, but it has one synthetic print instance and the
    fallback PrintRegion needed by object_add_auxiliary_layer().
    */
    CHECK(object_count_layer(first_auxiliary_object) == 0);
    CHECK(object_count_instance(first_auxiliary_object) == 1);
    REQUIRE(object_count_region(first_auxiliary_object) >= 1);

    layer_handle *layer_handle_value =
        object_add_auxiliary_layer(first_auxiliary_object, scale_i(1.), scale_i(1.), scale_i(0.5));
    REQUIRE(layer_handle_value != nullptr);
    CHECK(object_count_auxiliary_layer(first_auxiliary_object) == 1);
    CHECK(object_get_auxiliary_layer(first_auxiliary_object, 0) == layer_handle_value);

    print.clear();
    CHECK(print.auxiliary_object() == nullptr);
}

TEST_CASE("Auxiliary layer helper builds regions on print-level auxiliary object",
          "[plugins][auxiliary-layer][regions]")
{
    Print print;
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);

    const ExPolygons subject{ rectangle_expolygon(10., 10., 30., 30.) };
    slic3r_api::AuxiliaryLayerBuildResult result =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            storage,
            print_view(print),
            print_view(print).auxiliary_object(),
            expolygons_view(subject),
            scale_i(1.),
            scale_i(1.),
            scale_i(0.5));

    REQUIRE(result.created);
    const object_handle *auxiliary_object = print_get_auxiliary_object(reinterpret_cast<const print_handle *>(&print));
    REQUIRE(auxiliary_object != nullptr);
    CHECK(object_count_auxiliary_layer(auxiliary_object) == 1);

    const Layer &layer = *reinterpret_cast<const Layer *>(result.layer.handle());
    REQUIRE(layer.regions().size() >= 1);
    REQUIRE(!layer.lslices().empty());
    REQUIRE(!layer.islands().empty());
    CHECK(area_mm2(layer.lslices()) == Approx(area_mm2(subject)).epsilon(0.001));
}

TEST_CASE("Auxiliary layer helper splits subject into default, part and modifier regions",
          "[plugins][auxiliary-layer][regions]")
{
    Model model = make_auxiliary_region_model(true, true, false);
    Print print;
    init_print_from_model(model, print);
    run_until_sliced(print);

    PrintObject &object = print.object(0);
    REQUIRE(object.layer_count() > 0);
    const Layer &source_layer = object.layer(0);
    REQUIRE(!source_layer.lslices().empty());

    ExPolygons subject = source_layer.lslices();
    const ExPolygons part_area = subject;
    subject.push_back(shifted_copy(source_layer.lslices().front(), scale_i(80.)));

    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    slic3r_api::AuxiliaryLayerBuildResult result =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            storage,
            print_view(print),
            object_view(object),
            expolygons_view(subject),
            source_layer.scaled_height(),
            source_layer.scaled_print_z(),
            scale_i(source_layer.slice_z));

    REQUIRE(result.created);
    REQUIRE(object.auxiliary_layer_count() == 1);

    const Layer &auxiliary_layer = object.auxiliary_layer(0);
    REQUIRE(auxiliary_layer.regions().size() == object.num_printing_regions());
    CHECK(!auxiliary_layer.lslices().empty());
    CHECK(auxiliary_layer.islands().size() == auxiliary_layer.lslices().size());

    const double full_subject_area = area_mm2(subject);
    ExPolygons raw_union;
    for (const LayerRegion &region : auxiliary_layer.regions())
        append(raw_union, region.get_raw_slices());
    CHECK(area_mm2(union_ex(raw_union)) == Approx(full_subject_area).epsilon(0.001));

    /*
    The shifted copy is outside every model volume. It must remain in region
    zero, while the real model-part slice is stolen by the model-part region
    and its central modifier area is stolen again by the modifier region.
    */
    CHECK(area_mm2(auxiliary_layer.region(0).get_raw_slices()) > 1.);
    CHECK(area_mm2(auxiliary_layer.region(0).get_raw_slices()) < area_mm2(subject));

    bool has_part_region = false;
    bool has_modifier_region = false;
    const double part_area_mm2 = area_mm2(part_area);
    for (size_t region_idx = 1; region_idx < auxiliary_layer.regions().size(); ++region_idx) {
        const double region_area = area_mm2(auxiliary_layer.region(region_idx).get_raw_slices());
        has_part_region = has_part_region || (region_area > part_area_mm2 * 0.5);
        has_modifier_region = has_modifier_region || (region_area > 1. && region_area < part_area_mm2 * 0.5);
    }
    CHECK(has_part_region);
    CHECK(has_modifier_region);
}

TEST_CASE("Auxiliary layer helper ignores negative volumes by default",
          "[plugins][auxiliary-layer][regions]")
{
    Model model = make_auxiliary_region_model(false, false, true);
    Print print;
    init_print_from_model(model, print);
    run_until_sliced(print);

    PrintObject &object = print.object(0);
    REQUIRE(object.layer_count() > 0);
    const Layer &source_layer = object.layer(0);
    REQUIRE(!source_layer.lslices().empty());

    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    slic3r_api::AuxiliaryLayerBuildResult result =
        slic3r_api::build_auxiliary_layer_regions_from_subject(
            storage,
            print_view(print),
            object_view(object),
            expolygons_view(source_layer.lslices()),
            source_layer.scaled_height(),
            source_layer.scaled_print_z(),
            scale_i(source_layer.slice_z));

    REQUIRE(result.created);
    const Layer &auxiliary_layer = object.auxiliary_layer(0);

    /*
    The subject is already the geometry the plugin chose to print. A negative
    volume in the model may affect object slicing, but it must not punch a new
    hole in this auxiliary subject unless a future helper explicitly asks for
    that behavior.
    */
    ExPolygons raw_union;
    for (const LayerRegion &region : auxiliary_layer.regions())
        append(raw_union, region.get_raw_slices());
    CHECK(area_mm2(union_ex(raw_union)) == Approx(area_mm2(source_layer.lslices())).epsilon(0.001));
}
