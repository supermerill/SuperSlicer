#include <catch2/catch.hpp>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/UiLayoutMerger.hpp"
#include "plugins_cpp/Polyholes/Polyholes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace Slic3r;

struct PolyholeOptionKeys
{
    const char *label;
    const char *enabled_key;
    const char *threshold_key;
    const char *twisted_key;
    const char *angle_start_key;
    const char *selector_plugin_id;
};

const PolyholeOptionKeys cpp_polyhole_keys = {
    "C++ Polyholes",
    "hole_to_polyhole",
    "hole_to_polyhole_threshold",
    "hole_to_polyhole_twisted",
    nullptr,
    nullptr
};

#ifdef SLIC3R_TEST_PYTHON_PLUGINS
const PolyholeOptionKeys python_polyhole_keys[] = {
    {
        "Python low-level Polyholes",
        "hole_to_polyhole",
        "hole_to_polyhole_threshold",
        "hole_to_polyhole_twisted",
        "hole_to_polyhole_angle_start",
        "python.polyholes"
    },
    {
        "Python high-level Polyholes",
        "hole_to_polyhole",
        "hole_to_polyhole_threshold",
        "hole_to_polyhole_twisted",
        nullptr,
        "python.polyholes.high_level"
    }
};
#endif

const char *k_polyholes_selector_key = "exclusive_group_300_polyholes_plugin";
const char *k_polyholes_settings_fragment_id = "polyholes_settings";

const char *python_polyholes_ui_fragment()
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:insert$afterline$Vertical Hole shrinking compensation:Convert round vertical holes to polyholes\n"
           "setting:label$_:hole_to_polyhole\n"
           "setting:sidetext_width$5:hole_to_polyhole_threshold\n"
           "setting:hole_to_polyhole_twisted\n"
           "end_line\n";
}

const char *python_polyholes_exclusive_group_ui_fragment()
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:insert$beforeline$Convert round vertical holes to polyholes:Polyholes plugin\n"
           "setting:exclusive_group_300_polyholes_plugin\n"
           "end_line\n";
}

const char *python_polyholes_angle_ui_fragment()
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:Convert round vertical holes to polyholes\n"
           "setting:insert$beforesetting$hole_to_polyhole_twisted:sidetext_width$5:hole_to_polyhole_angle_start\n"
           "end_line\n";
}

Point scaled_point(const double x, const double y)
{
    return Point(scale_i(x), scale_i(y));
}

Polygon make_square_contour(const double half_size_mm)
{
    Polygon contour({
        scaled_point(-half_size_mm, -half_size_mm),
        scaled_point( half_size_mm, -half_size_mm),
        scaled_point( half_size_mm,  half_size_mm),
        scaled_point(-half_size_mm,  half_size_mm)
    });
    contour.make_counter_clockwise();
    return contour;
}

Polygon make_clockwise_ellipse_hole(const double radius_x_mm,
                                    const double radius_y_mm,
                                    const size_t point_count)
{
    Polygon hole;
    hole.points.reserve(point_count);
    for (size_t point_idx = 0; point_idx < point_count; ++point_idx) {
        const double angle = -2. * PI * double(point_idx) / double(point_count);
        hole.points.push_back(scaled_point(radius_x_mm * std::cos(angle),
                                           radius_y_mm * std::sin(angle)));
    }
    hole.make_clockwise();
    return hole;
}

ExPolygons make_single_hole_slice(const double radius_x_mm,
                                  const double radius_y_mm,
                                  const size_t source_point_count)
{
    ExPolygon expolygon(make_square_contour(20.), make_clockwise_ellipse_hole(
        radius_x_mm, radius_y_mm, source_point_count));
    expolygon.assert_valid();
    return ExPolygons{std::move(expolygon)};
}

size_t expected_polyhole_edge_count(const double radius_mm, const double nozzle_diameter_mm)
{
    return size_t(std::max(3, int(std::round(4.0 * radius_mm * 0.4 / nozzle_diameter_mm))));
}

DynamicPrintConfig polyhole_config(const PolyholeOptionKeys &keys, const bool twist)
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    std::vector<std::pair<std::string, std::string>> values = {
        {"first_layer_height", "1"},
        {keys.enabled_key, "1"},
        {keys.threshold_key, "0.1"},
        {keys.twisted_key, twist ? "1" : "0"},
        {"layer_height", "1"},
        {"nozzle_diameter", "0.4"},
        {"perimeters", "1"},
        {"resolution", "0.001"}
    };
    if (keys.angle_start_key != nullptr)
        values.emplace_back(keys.angle_start_key, "0");
    if (keys.selector_plugin_id != nullptr)
        values.emplace_back(k_polyholes_selector_key, keys.selector_plugin_id);
    for (const std::pair<std::string, std::string> &value : values)
        config.set_deserialize_strict(value.first, value.second);
    return config;
}

DynamicPrintConfig polyhole_config(const bool twist)
{
    return polyhole_config(cpp_polyhole_keys, twist);
}

Line polygon_segment_at(const Polygon &polygon, const size_t point_idx)
{
    return Line(polygon.points[point_idx], polygon.points[(point_idx + 1) % polygon.size()]);
}

bool contains_point(const Points &points, const Point &point)
{
    for (const Point &candidate : points)
        if (candidate == point)
            return true;
    return false;
}

Points unique_intersections(const Polygon &lhs, const Line &rhs)
{
    Points intersections;
    for (size_t point_idx = 0; point_idx < lhs.size(); ++point_idx) {
        Point intersection;
        if (polygon_segment_at(lhs, point_idx).intersection(rhs, &intersection) &&
            !contains_point(intersections, intersection))
            intersections.push_back(intersection);
    }
    return intersections;
}

bool polygon_fully_contains_polygon(const Polygon &outer, const Polygon &inner)
{
    for (const Point &point : inner.points)
        if (!outer.contains(point))
            return false;
    return true;
}

bool segment_cuts_polygon(const Line &segment, const Polygon &polygon)
{
    const Point midpoint = segment.midpoint();
    if (contains(polygon, midpoint, false))
        return true;

    return unique_intersections(polygon, segment).size() > 1;
}

bool polygon_segments_do_not_cut_polygon(const Polygon &outer, const Polygon &inner)
{
    for (size_t point_idx = 0; point_idx < outer.size(); ++point_idx)
        if (segment_cuts_polygon(polygon_segment_at(outer, point_idx), inner))
            return false;
    return true;
}

void assign_raw_slices_to_all_layers(PrintObject &object, const ExPolygons &slices)
{
    for (Layer &layer : object.layers()) {
        REQUIRE(layer.region_count() == 1);
        ApiInternal::LayerRegionAccess::slices_mutable(layer.region(0)) = slices;
        ApiInternal::LayerAccess::recompute_slices_from_layer_regions(layer);
    }
}

Polygon first_hole(const PrintObject &object, const size_t layer_idx)
{
    REQUIRE(layer_idx < object.layer_count());
    const ExPolygons &slices = object.layer(layer_idx).region(0).get_raw_slices();
    REQUIRE(slices.size() == 1);
    REQUIRE(slices.front().holes.size() == 1);
    return slices.front().holes.front();
}

struct PolyholeRunResult
{
    Polygon source_hole;
    std::vector<Polygon> layer_holes;
};

std::vector<size_t> point_counts(const PolyholeRunResult &result)
{
    std::vector<size_t> counts;
    counts.reserve(result.layer_holes.size());
    for (const Polygon &hole : result.layer_holes)
        counts.push_back(hole.size());
    return counts;
}

PolyholeRunResult run_polyhole_on_single_hole(const double radius_x_mm,
                                              const double radius_y_mm,
                                              const size_t source_point_count,
                                              const bool twist,
                                              const PolyholeOptionKeys &keys = cpp_polyhole_keys)
{
    DynamicPrintConfig config = polyhole_config(keys, twist);
    Model model;
    Print print;
    Slic3r::Test::init_print({Slic3r::Test::TestMesh::cube_20x20x20}, print, model, config);

    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepLayerHeightGeneration::run_step(orchestrator, print);
    Steps::StepSlicing::run_step(orchestrator, print);

    PrintObject &object = print.object(0);
    ExPolygons source_slices = make_single_hole_slice(radius_x_mm, radius_y_mm, source_point_count);
    Polygon source_hole = source_slices.front().holes.front();
    assign_raw_slices_to_all_layers(object, source_slices);

    Steps::StepPostSlicing::run_step(orchestrator, print);

    std::vector<Polygon> layer_holes;
    layer_holes.reserve(object.layer_count());
    for (size_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx)
        layer_holes.push_back(first_hole(object, layer_idx));
    return PolyholeRunResult{std::move(source_hole), std::move(layer_holes)};
}

std::string read_text_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

struct UiFragmentForTest
{
    std::string id;
    std::string content;
    int32_t priority = 0;
};

std::string merge_polyhole_fragments_for_test(const std::vector<UiFragmentForTest> &fragments)
{
    const std::string base = read_text_file(std::string(TEST_DATA_DIR) + "/../../resources/ui_layout/default/print.ui");
    UiLayoutMerger merger("print.ui");
    merger.set_base(base);

    // Orchestrator accepts only the first fragment for a target/id pair. This
    // local helper mirrors that rule so the test can check that C++ and Python
    // Polyholes produce the same final layout even if they are registered in a
    // different order.
    std::set<std::string> registered_ids;
    uint64_t order = 0;
    for (const UiFragmentForTest &fragment : fragments) {
        if (!registered_ids.insert(fragment.id).second)
            continue;
        merger.add_fragment(fragment.id, fragment.content, fragment.priority, order++);
    }

    return merger.merged();
}

size_t occurrence_count(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

const char *const k_no_dependencies[] = { nullptr };
const char *const k_duplicate_polyhole_key[] = { "hole_to_polyhole" };
const char *const k_duplicate_builtin_key[] = { "layer_height" };

struct DuplicateOptionPluginState
{
    const char *id = nullptr;
    const char *exclusive_group = nullptr;
    const char *const *defined_keys = nullptr;
    size_t defined_key_count = 0;
    bool create_polyhole_option = false;
    bool omit_used_keys = false;
    bool conflicting_used_types = false;
};

DuplicateOptionPluginState g_same_group_duplicate {
    "test.polyholes.duplicate.same_group",
    "polyholes",
    k_duplicate_polyhole_key,
    1,
    false
};

DuplicateOptionPluginState g_other_group_duplicate {
    "test.polyholes.duplicate.other_group",
    "other_polyholes",
    k_duplicate_polyhole_key,
    1,
    false
};

DuplicateOptionPluginState g_builtin_duplicate {
    "test.polyholes.duplicate.builtin",
    "polyholes",
    k_duplicate_builtin_key,
    1,
    false
};

DuplicateOptionPluginState g_same_group_initializer {
    "test.polyholes.initializer.same_group",
    "polyholes",
    k_duplicate_polyhole_key,
    1,
    true
};

DuplicateOptionPluginState g_other_group_initializer {
    "test.polyholes.initializer.other_group",
    "other_polyholes",
    k_duplicate_polyhole_key,
    1,
    true
};

const char *duplicate_plugin_id(void *plugin_ctx)
{
    return static_cast<DuplicateOptionPluginState *>(plugin_ctx)->id;
}

const char *duplicate_plugin_name(void *plugin_ctx)
{
    return static_cast<DuplicateOptionPluginState *>(plugin_ctx)->id;
}

const char *duplicate_plugin_description(void *)
{
    return "Test plugin for duplicate config option ownership.";
}

const char *duplicate_plugin_exclusive_group(void *plugin_ctx)
{
    return static_cast<DuplicateOptionPluginState *>(plugin_ctx)->exclusive_group;
}

const char *duplicate_plugin_exclusive_group_text(void *)
{
    return "";
}

slicing_step_t duplicate_plugin_step(void *)
{
    return STEP_POST_SLICING;
}

const_strings_t duplicate_plugin_dependencies(void *)
{
    const_strings_t out = {};
    out.items = k_no_dependencies;
    out.size = 0;
    return out;
}

int32_t duplicate_plugin_priority(void *)
{
    return 1000;
}

int32_t duplicate_plugin_used_keys(void *plugin_ctx, raw_used_config_key *keys)
{
    const DuplicateOptionPluginState &state = *static_cast<DuplicateOptionPluginState *>(plugin_ctx);
    if (state.omit_used_keys)
        return 0;

    const size_t key_count = state.defined_key_count * (state.conflicting_used_types ? 2 : 1);
    if (keys != nullptr) {
        for (size_t idx = 0; idx < state.defined_key_count; ++idx) {
            const raw_config_option_type type = std::string(state.defined_keys[idx]) == "layer_height" ?
                RAW_CO_FLOAT : RAW_CO_BOOL;
            keys[idx] = raw_used_config_key{
                state.defined_keys[idx], type, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT};
            if (state.conflicting_used_types)
                keys[state.defined_key_count + idx] = raw_used_config_key{
                    state.defined_keys[idx], RAW_CO_STRING, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT};
        }
    }
    return int32_t(key_count);
}

int32_t duplicate_plugin_defined_keys(void *plugin_ctx, const char **keys)
{
    const DuplicateOptionPluginState &state = *static_cast<DuplicateOptionPluginState *>(plugin_ctx);
    if (keys != nullptr)
        for (size_t idx = 0; idx < state.defined_key_count; ++idx)
            keys[idx] = state.defined_keys[idx];
    return int32_t(state.defined_key_count);
}

raw_config_option_def duplicate_polyhole_option_def()
{
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = "hole_to_polyhole";
    def.type = RAW_CO_BOOL;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Convert round holes to polyholes";
    def.full_label = "Convert round holes to polyholes";
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Search for almost-circular holes that span more than one layer and convert the geometry to polyholes."
        " Use the nozzle size and the (biggest) diameter to compute the polyhole."
        "\nSee http://hydraraptor.blogspot.com/2011/02/polyholes.html");
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "0";
    return def;
}

void duplicate_plugin_initialize(void *plugin_ctx, storage_handle *)
{
    const DuplicateOptionPluginState &state = *static_cast<DuplicateOptionPluginState *>(plugin_ctx);
    if (!state.create_polyhole_option)
        return;

    raw_config_option_def def = duplicate_polyhole_option_def();
    orchestrator_create_option_def(reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()), &def);
}

void duplicate_plugin_setup(void *, const plugin_run_context *, uint32_t) {}
void duplicate_plugin_setup_run(void *, const plugin_run_context *) {}
void duplicate_plugin_run(void *, const plugin_run_context *) {}

const plugin_vtable *duplicate_option_plugin_vtable()
{
    static const plugin_vtable vt = {
        SLIC3R_PLUGIN_ABI_VERSION,
        &duplicate_plugin_id,
        &duplicate_plugin_name,
        &duplicate_plugin_description,
        &duplicate_plugin_exclusive_group,
        &duplicate_plugin_exclusive_group_text,
        &duplicate_plugin_exclusive_group_text,
        &duplicate_plugin_step,
        &duplicate_plugin_dependencies,
        &duplicate_plugin_priority,
        &duplicate_plugin_used_keys,
        &duplicate_plugin_defined_keys,
        &duplicate_plugin_initialize,
        &duplicate_plugin_setup,
        &duplicate_plugin_setup_run,
        &duplicate_plugin_run
    };
    return &vt;
}

void register_duplicate_option_plugin(DuplicateOptionPluginState &state)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    if (orchestrator.get_plugin(state.id) != nullptr)
        return;

    plugin_instance instance = {};
    instance.ctx = &state;
    instance.vt = duplicate_option_plugin_vtable();
    REQUIRE(orchestrator.register_plugin(instance));
}

void register_duplicate_option_plugins()
{
    register_duplicate_option_plugin(g_same_group_duplicate);
    register_duplicate_option_plugin(g_other_group_duplicate);
    register_duplicate_option_plugin(g_builtin_duplicate);
    register_duplicate_option_plugin(g_same_group_initializer);
    register_duplicate_option_plugin(g_other_group_initializer);
}

class ScopedActivePluginSet
{
public:
    explicit ScopedActivePluginSet(std::initializer_list<const char *> plugin_ids)
        : m_orchestrator(Orchestrator::instance())
    {
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous_active_plugins.push_back(plugin);

        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids)
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
    }

    ~ScopedActivePluginSet()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous_active_plugins)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous_active_plugins;
};

} // namespace

TEST_CASE("Polyholes converts round holes to polygons with the expected point count", "[plugins][polyholes]")
{
    // The test feeds the post-slicing plugin a stack of raw slices containing a
    // single circular hole. The assertion observes the real output geometry:
    // the hole polygon in a non-first layer must have the same number of points
    // as the legacy polyhole formula predicts for each radius.
    const std::vector<double> radii_mm = {0.1,0.2,0.3,0.4, 0.5, 0.75, 1.0, 1.5, 2, 2.5, 3., 4., 5., 10.};
    for (const double radius_mm : radii_mm) {
        const std::vector<size_t> hole_point_counts = point_counts(run_polyhole_on_single_hole(
            radius_mm, radius_mm, 128, false));
        REQUIRE(hole_point_counts.size() > 1);
        CHECK(hole_point_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));
    }
}

TEST_CASE("Polyholes alternates rotated replacements when twisting is enabled", "[plugins][polyholes]")
{
    // Twisting still changes raw slice holes through the normal plugin path.
    // The rotated variants should keep the same point count, so checking two
    // consecutive layers catches the through-hole grouping and replacement.
    const double radius_mm = 2.5;
    const std::vector<size_t> hole_point_counts = point_counts(run_polyhole_on_single_hole(
        radius_mm, radius_mm, 128, true));

    REQUIRE(hole_point_counts.size() > 2);
    CHECK(hole_point_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));
    CHECK(hole_point_counts[2] == expected_polyhole_edge_count(radius_mm, 0.4));
}

TEST_CASE("Polyholes keeps the original round hole inside the replacement polygon", "[plugins][polyholes]")
{
    // The generated polygon is supposed to circumscribe the detected round
    // hole. The replacement boundary may touch the original boundary at
    // tangent points, but it must not cut through the original hole contour.
    const std::vector<double> radii_mm = {0.1, 0.2, 0.5, 1.0, 2.5, 5.0, 10.0};
    for (const double radius_mm : radii_mm) {
        const PolyholeRunResult result = run_polyhole_on_single_hole(radius_mm, radius_mm, 128, false);
        REQUIRE(result.layer_holes.size() > 1);

        const Polygon &replacement_hole = result.layer_holes[1];
        CHECK(polygon_fully_contains_polygon(replacement_hole, result.source_hole));
        CHECK(polygon_segments_do_not_cut_polygon(replacement_hole, result.source_hole));
    }
}

TEST_CASE("Polyholes rejects layer sections that are too oval", "[plugins][polyholes]")
{
    // A horizontal section of an inclined cylinder is an ellipse. A shallow
    // inclination is still inside the configured roundness margin and should be
    // converted, while a steeper one remains the original 128-point hole.
    const double radius_mm = 5.;
    const size_t source_point_count = 128;

    const double shallow_tilt_degrees = 10.;
    const std::vector<size_t> shallow_counts = point_counts(run_polyhole_on_single_hole(
        radius_mm / std::cos(shallow_tilt_degrees * PI / 180.),
        radius_mm,
        source_point_count,
        false));
    REQUIRE(shallow_counts.size() > 1);
    CHECK(shallow_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));

    const double steep_tilt_degrees = 20.;
    const std::vector<size_t> steep_counts = point_counts(run_polyhole_on_single_hole(
        radius_mm / std::cos(steep_tilt_degrees * PI / 180.),
        radius_mm,
        source_point_count,
        false));
    REQUIRE(steep_counts.size() > 1);
    CHECK(steep_counts[1] == source_point_count);
}

TEST_CASE("Polyhole UI fragments are stable when C++ and Python register the same line", "[plugins][polyholes][ui]")
{
    // All Polyholes variants provide the same exclusive-group selector
    // fragment id, so only one selector is kept. They also share the same
    // settings line fragment id; the Python-only angle setting is merged into
    // that line afterwards.
    const std::string cpp_then_python = merge_polyhole_fragments_for_test({
        {"polyholes", slic3r_api::PolyholesPlugin::Polyholes::exclusive_group_ui_fragment(), 1},
        {k_polyholes_settings_fragment_id, slic3r_api::PolyholesPlugin::Polyholes::print_ui_fragment(), 0},
        {"polyholes", python_polyholes_exclusive_group_ui_fragment(), 1},
        {k_polyholes_settings_fragment_id, python_polyholes_ui_fragment(), 0},
        {"polyholes_angle_start", python_polyholes_angle_ui_fragment(), 1}
    });

    const std::string python_then_cpp = merge_polyhole_fragments_for_test({
        {"polyholes", python_polyholes_exclusive_group_ui_fragment(), 1},
        {k_polyholes_settings_fragment_id, python_polyholes_ui_fragment(), 0},
        {"polyholes_angle_start", python_polyholes_angle_ui_fragment(), 1},
        {"polyholes", slic3r_api::PolyholesPlugin::Polyholes::exclusive_group_ui_fragment(), 1},
        {k_polyholes_settings_fragment_id, slic3r_api::PolyholesPlugin::Polyholes::print_ui_fragment(), 0}
    });

    CHECK(cpp_then_python == python_then_cpp);
    CHECK(occurrence_count(cpp_then_python, "setting:label$_:hole_to_polyhole") == 1);
    CHECK(occurrence_count(cpp_then_python, "hole_to_polyhole_angle_start") == 1);
    CHECK(occurrence_count(cpp_then_python, k_polyholes_selector_key) == 1);

    const size_t selector_pos = cpp_then_python.find(k_polyholes_selector_key);
    const size_t angle_pos = cpp_then_python.find("hole_to_polyhole_angle_start");
    const size_t twist_pos = cpp_then_python.find("hole_to_polyhole_twisted");
    const size_t polyholes_pos = cpp_then_python.find("setting:label$_:hole_to_polyhole");
    REQUIRE(selector_pos != std::string::npos);
    REQUIRE(angle_pos != std::string::npos);
    REQUIRE(twist_pos != std::string::npos);
    REQUIRE(polyholes_pos != std::string::npos);
    CHECK(selector_pos < polyholes_pos);
    CHECK(angle_pos < twist_pos);
}

TEST_CASE("Exclusive-group option ownership validates plugin activation early", "[plugins][polyholes][config]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    register_duplicate_option_plugins();

    std::string error_message;
    CHECK(Orchestrator::instance().validate_plugin_activation(
        {g_same_group_duplicate.id},
        error_message));

    // A plugin may not claim a setting that is already owned by another
    // exclusive group, even if the full definition would otherwise be
    // compatible. This is the cheap GUI-side check before a restart.
    error_message.clear();
    CHECK_FALSE(Orchestrator::instance().validate_plugin_activation(
        {g_other_group_duplicate.id},
        error_message));
    CHECK(error_message.find("hole_to_polyhole") != std::string::npos);

    // Existing application settings have no plugin owner. A plugin must define
    // its own setting key instead of silently attaching itself to them.
    error_message.clear();
    CHECK_FALSE(Orchestrator::instance().validate_plugin_activation(
        {g_builtin_duplicate.id},
        error_message));
    CHECK(error_message.find("layer_height") != std::string::npos);
}

TEST_CASE("Defined plugin settings obtain their type from used settings", "[plugins][config][metadata]")
{
    DuplicateOptionPluginState state {
        "test.config.metadata",
        "test.config.metadata",
        k_duplicate_polyhole_key,
        1,
        false
    };
    plugin_instance instance = {};
    instance.ctx = &state;
    instance.vt = duplicate_option_plugin_vtable();

    Plugin plugin(instance, std::string(), std::string());
    REQUIRE(plugin.get_defined_config_keys().size() == 1);
    CHECK(plugin.get_defined_config_keys().front().key == "hole_to_polyhole");
    CHECK(plugin.get_defined_config_keys().front().type == RAW_CO_BOOL);
}

TEST_CASE("Defined plugin settings require a matching typed used setting", "[plugins][config][metadata]")
{
    DuplicateOptionPluginState state {
        "test.config.metadata.missing",
        "test.config.metadata.missing",
        k_duplicate_polyhole_key,
        1,
        false
    };
    state.omit_used_keys = true;
    plugin_instance instance = {};
    instance.ctx = &state;
    instance.vt = duplicate_option_plugin_vtable();

    try {
        Plugin plugin(instance, std::string(), std::string());
        (void) plugin;
        FAIL("Plugin registration should reject a defined key missing from used_config_keys.");
    } catch (const std::runtime_error &error) {
        CHECK(std::string(error.what()).find("without a matching used_config_keys entry") != std::string::npos);
    }
}

TEST_CASE("Defined plugin settings reject conflicting used setting types", "[plugins][config][metadata]")
{
    DuplicateOptionPluginState state {
        "test.config.metadata.conflict",
        "test.config.metadata.conflict",
        k_duplicate_polyhole_key,
        1,
        false
    };
    state.conflicting_used_types = true;
    plugin_instance instance = {};
    instance.ctx = &state;
    instance.vt = duplicate_option_plugin_vtable();

    try {
        Plugin plugin(instance, std::string(), std::string());
        (void) plugin;
        FAIL("Plugin registration should reject conflicting declarations for one setting.");
    } catch (const std::runtime_error &error) {
        CHECK(std::string(error.what()).find("conflicting used_config_keys types") != std::string::npos);
    }
}

TEST_CASE("Exclusive-group option ownership rejects wrong-group definitions during initialization", "[plugins][polyholes][config]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    register_duplicate_option_plugins();
    Orchestrator &orchestrator = Orchestrator::instance();

    {
        // The duplicate definition is accepted because the plugin declares the
        // same exclusive group as the original Polyholes implementation.
        ScopedActivePluginSet active({g_same_group_initializer.id});
        orchestrator.initialize_plugins();
        CHECK(orchestrator.is_plugin_active(g_same_group_initializer.id));
    }

    {
        // The full create_new_print_config() validation is still authoritative:
        // if a plugin lies or was enabled without the GUI precheck, it is
        // disabled as soon as it tries to register a foreign setting key.
        ScopedActivePluginSet active({g_other_group_initializer.id});
        orchestrator.initialize_plugins();
        CHECK_FALSE(orchestrator.is_plugin_active(g_other_group_initializer.id));
    }
}

#ifdef SLIC3R_TEST_PYTHON_PLUGINS

TEST_CASE("Python low-level Polyholes shares C++ option definitions and adds only its own angle option", "[plugins][polyholes][python][ui]")
{
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());

    CHECK(PrintConfigDef::instance().get("hole_to_polyhole") != nullptr);
    CHECK(PrintConfigDef::instance().get("hole_to_polyhole_threshold") != nullptr);
    CHECK(PrintConfigDef::instance().get("hole_to_polyhole_twisted") != nullptr);
    CHECK(PrintConfigDef::instance().get("hole_to_polyhole_angle_start") != nullptr);
    CHECK(PrintConfigDef::instance().get("python_hole_to_polyhole") == nullptr);
    CHECK(PrintConfigDef::instance().get("python_hole_to_polyhole_threshold") == nullptr);
    CHECK(PrintConfigDef::instance().get("python_hole_to_polyhole_twisted") == nullptr);
    CHECK(PrintConfigDef::instance().get("python_high_level_hole_to_polyhole") == nullptr);
    CHECK(PrintConfigDef::instance().get("python_high_level_hole_to_polyhole_threshold") == nullptr);
    CHECK(PrintConfigDef::instance().get("python_high_level_hole_to_polyhole_twisted") == nullptr);

    const std::string base = read_text_file(std::string(TEST_DATA_DIR) + "/../../resources/ui_layout/default/print.ui");
    const std::string merged = Orchestrator::instance().merged_ui_layout("print.ui", base);
    CHECK(occurrence_count(merged, "setting:label$_:hole_to_polyhole") == 1);
    CHECK(occurrence_count(merged, "hole_to_polyhole_angle_start") == 1);
    CHECK(occurrence_count(merged, k_polyholes_selector_key) == 1);

    const size_t selector_pos = merged.find(k_polyholes_selector_key);
    const size_t polyholes_pos = merged.find("setting:label$_:hole_to_polyhole");
    REQUIRE(selector_pos != std::string::npos);
    REQUIRE(polyholes_pos != std::string::npos);
    CHECK(selector_pos < polyholes_pos);
}

TEST_CASE("Python Polyholes converts round holes to polygons with the expected point count", "[plugins][polyholes][python]")
{
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());

    const std::vector<double> radii_mm = {0.1, 0.2, 0.3, 0.4, 0.5, 0.75, 1.0, 1.5, 2., 2.5, 3., 4., 5., 10.};
    for (const PolyholeOptionKeys &keys : python_polyhole_keys) {
        INFO(keys.label);
        for (const double radius_mm : radii_mm) {
            const std::vector<size_t> hole_point_counts = point_counts(run_polyhole_on_single_hole(
                radius_mm, radius_mm, 128, false, keys));
            REQUIRE(hole_point_counts.size() > 1);
            CHECK(hole_point_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));
        }
    }
}

TEST_CASE("Python Polyholes alternates rotated replacements when twisting is enabled", "[plugins][polyholes][python]")
{
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());

    const double radius_mm = 2.5;
    for (const PolyholeOptionKeys &keys : python_polyhole_keys) {
        INFO(keys.label);
        const std::vector<size_t> hole_point_counts = point_counts(run_polyhole_on_single_hole(
            radius_mm, radius_mm, 128, true, keys));

        REQUIRE(hole_point_counts.size() > 2);
        CHECK(hole_point_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));
        CHECK(hole_point_counts[2] == expected_polyhole_edge_count(radius_mm, 0.4));
    }
}

TEST_CASE("Python Polyholes keeps the original round hole inside the replacement polygon", "[plugins][polyholes][python]")
{
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());

    const std::vector<double> radii_mm = {0.1, 0.2, 0.5, 1.0, 2.5, 5.0, 10.0};
    for (const PolyholeOptionKeys &keys : python_polyhole_keys) {
        INFO(keys.label);
        for (const double radius_mm : radii_mm) {
            const PolyholeRunResult result = run_polyhole_on_single_hole(radius_mm, radius_mm, 128, false, keys);
            REQUIRE(result.layer_holes.size() > 1);

            const Polygon &replacement_hole = result.layer_holes[1];
            CHECK(polygon_fully_contains_polygon(replacement_hole, result.source_hole));
            CHECK(polygon_segments_do_not_cut_polygon(replacement_hole, result.source_hole));
        }
    }
}

TEST_CASE("Python Polyholes rejects layer sections that are too oval", "[plugins][polyholes][python]")
{
    REQUIRE(Slic3r::Test::Plugins::python_plugin_test_runtime_available());

    const double radius_mm = 5.;
    const size_t source_point_count = 128;
    const double shallow_tilt_degrees = 10.;
    const double steep_tilt_degrees = 20.;
    for (const PolyholeOptionKeys &keys : python_polyhole_keys) {
        INFO(keys.label);
        const std::vector<size_t> shallow_counts = point_counts(run_polyhole_on_single_hole(
            radius_mm / std::cos(shallow_tilt_degrees * PI / 180.),
            radius_mm,
            source_point_count,
            false,
            keys));
        REQUIRE(shallow_counts.size() > 1);
        CHECK(shallow_counts[1] == expected_polyhole_edge_count(radius_mm, 0.4));

        const std::vector<size_t> steep_counts = point_counts(run_polyhole_on_single_hole(
            radius_mm / std::cos(steep_tilt_degrees * PI / 180.),
            radius_mm,
            source_point_count,
            false,
            keys));
        REQUIRE(steep_counts.size() > 1);
        CHECK(steep_counts[1] == source_point_count);
    }
}

#endif
