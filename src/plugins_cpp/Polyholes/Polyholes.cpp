///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Polyholes.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_slicing.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

namespace slic3r_api { namespace PolyholesPlugin {

namespace {

const char *k_polyholes_id = "polyholes";
const char *k_polyholes_exclusive_group = "polyholes";
const char *k_polyholes_settings_fragment_id = "polyholes_settings";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "hole_to_polyhole", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "hole_to_polyhole_threshold", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "hole_to_polyhole_twisted", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT }
};
constexpr size_t k_used_config_key_count = sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]);
const char *k_defined_config_keys[] = {
    "hole_to_polyhole",
    "hole_to_polyhole_threshold",
    "hole_to_polyhole_twisted"
};
constexpr size_t k_defined_config_key_count = k_used_config_key_count;

enum ProgressPhase : uint32_t
{
    // The plugin is split into explicit phases because the first two phases can
    // be object/layer based, while the final phase is driven by the number of
    // through-holes discovered after grouping.
    ProgressSearchHoles = 0,
    ProgressGroupHoles = 1,
    ProgressConvertHoles = 2
};

// Candidate circular hole found in one layer-region slice. The polygon handle is
// borrowed from immutable slices; it is used only as an identity/template until
// the mutable raw slice is borrowed in the conversion phase.
struct HoleData
{
    c_point center;
    distf_t max_diameter;
    int16_t extruder_id;
    coord_t max_deviation;
    bool is_twist;
    Polygon hole_polygon;
    uint32_t hole_lregion_idx;
};

// One occurrence of a candidate hole in a specific layer and layer region.
struct LayerHole
{
    Polygon polygon;
    uint32_t layer_idx;
    uint32_t lregion_idx;
};


// A vertical group of matching LayerHole entries. One StoredPolygon replacement
// may be reused on several layers, optionally rotated for the "twisted" mode.
struct ThroughHole
{
    HoleData hole_data;
    std::vector<LayerHole> layers;
};

// Build one or more polygonal replacement holes around the detected circular
// center. The returned polygons are storage-owned because they will be copied
// into several borrowed raw slices during conversion.
std::vector<StoredPolygon> create_polyholes(storage_handle *storage,
                                            const c_point center,
                                            const coord_t radius,
                                            const coord_t nozzle_diameter,
                                            bool multiple) {
    // n = max(round(2 * d), 3); // equivalent rule from the legacy polyhole implementation.
    size_t nb_edges = (int)std::max(3, (int)std::round(4.0 * unscaled(radius) * 0.4 / unscaled(nozzle_diameter)));
    // Same geometry as OpenSCAD's cylinder(h = h, r = d / cos(180 / n), $fn = n).
    // Multiple polyholes are rotated variants used on alternating layers.
    int nb_polyhole = 1;
    float rotation = 0;
    if (multiple) {
        nb_polyhole = 5;
        rotation = 2 * float(PI) / (nb_edges * nb_polyhole);
    }
    std::vector<StoredPolygon> list;
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++)
        list.emplace_back(storage);
    for (int i_poly = 0; i_poly < nb_polyhole; i_poly++) {
        StoredPolygon& pts = (((i_poly % 2) == 0) ? list[i_poly / 2] : list[(nb_polyhole + 1) / 2 + i_poly / 2]);
        const coordf_t new_radius = coordf_t(radius) / std::cos(PI / double(nb_edges));
        for (size_t i_edge = 0; i_edge < nb_edges; ++i_edge) {
            const double angle = double(rotation) * double(i_poly) + (PI * 2. * double(i_edge)) / double(nb_edges);
            pts.push_back(c_point{coord_t(std::round(coordf_t(center.x) + new_radius * std::cos(angle))),
                                  coord_t(std::round(coordf_t(center.y) + new_radius * std::sin(angle)))});
        }
        pts.make_clockwise();
    }
    return list;
}

// Replace the matching hole contour inside one mutable ExPolygon. The match is
// done against point views so we do not depend on handle identity: raw slices and
// immutable slices are different containers that can hold the same geometry.
bool replace_matching_hole_points(expolygon_handle *expolygon, const Polygon &hole_to_replace, const StoredPolygon &replacement)
{
    assert(expolygon != nullptr);
    for (uint32_t hole_idx = 0; hole_idx < expolygon_hole_size(expolygon); ++hole_idx) {
        polygon_handle *hole_handle = expolygon_hole_at(expolygon, hole_idx);
        Polygon hole(hole_handle);
        if (equals(hole.view(), hole_to_replace.view())) {
            multipoint_copy(polygon_as_multipoint(hole_handle), replacement.multipoint_handle());
            return true;
        }
    }
    return false;
}

} // namespace

void Polyholes::run_impl(const plugin_run_context *run_ctx) const
{
    // POST_SLICING provides raw LayerRegion slices and layer island slices as
    // mutable borrowed handles. The plugin must keep both consistent by asking
    // the host to recompute slices/islands after raw slice edits.
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr)
        return;


    struct ThreadData {
        const run_ctx_post_slicing *ctx;
        Object object;
        std::vector<std::vector<HoleData>> layerid2center;
        PluginProgress *progress;
    } thread_data{ctx, Object(ctx->object), {}, &progress()};

    thread_data.layerid2center.resize(thread_data.object.layer_count());

    slic3r_parallel_for(0, thread_data.object.layer_count(), &thread_data, [](uint32_t layer_idx, void *user_data) {
        ThreadData *data = static_cast<ThreadData *>(user_data);
        Layer layer = data->object.layer(layer_idx);
        // Each worker writes only to the pre-sized layer slot matching its layer,
        // so no lock is needed during candidate collection.
        std::vector<HoleData> &layer_center_data = data->layerid2center[layer_idx];
        for (uint32_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
            LayerRegion lregion = layer.region(region_idx);
            Config region_config = lregion.print_region().config();
            ConfigOption setting_hole_to_polyhole = region_config.get("hole_to_polyhole");
            if (setting_hole_to_polyhole.get_bool()) {
                bool twist = region_config.get("hole_to_polyhole_twisted").get_bool();
                ExPolygonCollection region_slices = lregion.slices();
                for (ExPolygon surf_expoly : region_slices) {
                    for (Polygon hole : surf_expoly.holes()) {
                        // test if convex (as it's clockwise bc it's a hole, we have to do the opposite)
                        if (hole.convex_points_idx(0, PI).empty() && hole.size() > 8) {
                            // Estimate circularity from vertex radius and edge-midpoint radius. The midpoint check
                            // rejects rectangular holes whose vertices alone could look circle-like.
                            c_point center = hole.centroid();
                            distf_t diameter_min = std::numeric_limits<float>::max(), diameter_max = 0;
                            double diameter_sum = 0;
                            for (uint32_t i = 0; i < hole.size(); ++i) {
                                distf_t dist = norm(hole[i] - center);
                                diameter_min = std::min(diameter_min, dist);
                                diameter_max = std::max(diameter_max, dist);
                                diameter_sum += dist;
                            }
                            // also use center of lines to check it's not a rectangle
                            distf_t diameter_line_min = std::numeric_limits<float>::max(), diameter_line_max = 0;
                            c_point prev = hole.back();
                            for (uint32_t hole_line_a_idx = 0; hole_line_a_idx < hole.size(); ++hole_line_a_idx) {
                                c_point midline = (prev + hole[hole_line_a_idx]) / 2;
                                distf_t dist = norm(center - midline);
                                diameter_line_min = std::min(diameter_line_min, dist);
                                diameter_line_max = std::max(diameter_line_max, dist);
                                prev = hole[hole_line_a_idx];
                            }

                            // SCALED_EPSILON was a bit too harsh. Now using a config, as some may want some harsh
                            // setting and some don't.
                            ConfigOption setting_hole_to_polyhole_threshold = region_config.get("hole_to_polyhole_threshold");
                            coord_t max_variation = scale_i(setting_hole_to_polyhole_threshold.get_effective_value(
                                unscaled(diameter_sum / hole.size())));
                            max_variation = std::max(SCALED_EPSILON, max_variation);
                            if (diameter_max - diameter_min < max_variation * 2 &&
                                diameter_line_max - diameter_line_min < max_variation * 2) {
                                layer_center_data.push_back(
                                    HoleData{center, diameter_max,
                                             int16_t(region_config.get("perimeter_extruder").get_int() - 1),
                                             max_variation, twist, hole, region_idx});
                            }
                        }
                    }
                }
            }
        }
        data->progress->increment(ProgressSearchHoles);
    });
    progress().finish_run(ProgressSearchHoles);
    
    // Group layer-local candidates into through-holes. A match is intentionally
    // geometric, not handle-based, because each layer owns its own slice storage.
    std::vector<ThroughHole> id2layers2hole;
    const size_t min_nb_layers = 2;
    for (uint32_t layer_idx = 0; layer_idx < thread_data.object.layer_count(); ++layer_idx) {
        for (uint32_t hole_idx = 0; hole_idx < thread_data.layerid2center[layer_idx].size(); ++hole_idx) {
            //get all other same polygons
            const HoleData& main_hole_data = thread_data.layerid2center[layer_idx][hole_idx];
            coord_t max_z = thread_data.object.layer(layer_idx).print_z();
            std::vector<LayerHole> holes;
            holes.push_back(LayerHole{main_hole_data.hole_polygon, layer_idx, main_hole_data.hole_lregion_idx});
            for (uint32_t search_layer_idx = layer_idx + 1; search_layer_idx < thread_data.object.layer_count(); ++search_layer_idx) {
                Layer search_layer(thread_data.object.layer(search_layer_idx));
                if (search_layer.print_z() - search_layer.height() - max_z > 0) break;
                //search an other polygon with same main_hole_data
                std::vector<HoleData> &layer_center_data = thread_data.layerid2center[search_layer_idx];
                for (uint32_t search_hole_idx = 0; search_hole_idx < layer_center_data.size(); ++search_hole_idx) {
                    const HoleData& search_hole = layer_center_data[search_hole_idx];
                    if (main_hole_data.extruder_id == search_hole.extruder_id
                        && norm(main_hole_data.center - search_hole.center) < main_hole_data.max_deviation
                        && std::abs(main_hole_data.max_diameter - search_hole.max_diameter) < main_hole_data.max_deviation
                        ) {
                        max_z = search_layer.print_z();
                        holes.push_back(LayerHole{search_hole.hole_polygon, search_layer_idx, search_hole.hole_lregion_idx});
                        // this hole is now used, we can remove it from the search, to avoid finding it again for
                        // another hole.
                        layer_center_data.erase(layer_center_data.begin() + search_hole_idx);
                        search_hole_idx--;
                        break;
                    }
                }
            }
            // Convert holes spanning at least two layers. Single-layer holes are
            // accepted only on the first layer, where first-layer compensation can
            // make a through-hole appear short.
            if (holes.size() >= min_nb_layers || (holes.size() == 1 && holes[0].layer_idx == 0)) {
                id2layers2hole.push_back(ThroughHole{main_hole_data, std::move(holes)});
            }
        }
    }
    progress().add_max(ProgressConvertHoles, static_cast<uint32_t>(id2layers2hole.size()));
    progress().increment(ProgressGroupHoles);
    progress().finish_run(ProgressGroupHoles);

    // Replace every grouped hole in the mutable raw slices. The immutable views
    // collected above are never modified directly.
    std::set<uint32_t> modified_layer_idx;
    for (ThroughHole &through_hole : id2layers2hole) {
        ConfigOption setting_nozzle_diameter = Print(ctx->print).config().get("nozzle_diameter");
        std::vector<StoredPolygon> polyholes = create_polyholes(run_ctx->plugin_storage, through_hole.hole_data.center,
                                                                through_hole.hole_data.max_diameter,
                                                                scale_i(setting_nozzle_diameter.get_float(through_hole.hole_data.extruder_id)),
                                                                through_hole.hole_data.is_twist);
        for (LayerHole& poly_to_replace : through_hole.layers) {
            StoredPolygon &polyhole = polyholes[poly_to_replace.layer_idx % polyholes.size()];
            assert(polyhole.valid_polygon());

            // Borrow the mutable raw slices only at the last possible moment:
            // modifying them may invalidate read-only views collected earlier.
            int modified = 0;
            layer_handle *mut_layer = ctx->object_borrow_mutable_layer(ctx->object, poly_to_replace.layer_idx);
            layer_region_handle *mut_lregion = layer_get_region_mutable(mut_layer, poly_to_replace.lregion_idx);
            expolygon_collection_handle *mut_region_slices(ctx->layer_region_borrow_mutable_slices(mut_lregion));
            assert(expolygons_size(mut_region_slices) > 0);
            for (uint32_t expoly_idx = 0; expoly_idx < expolygons_size(mut_region_slices); ++expoly_idx) {
                expolygon_handle *lregion_slice = expolygons_at(mut_region_slices, expoly_idx);
                if (replace_matching_hole_points(lregion_slice, poly_to_replace.polygon, polyhole))
                    modified++;
            }
            assert(modified == 1);
            // The Layer cache and LayerSliceIsland list depend on raw slices, so
            // recompute them after all replacements for the layer are done.
            modified_layer_idx.insert(poly_to_replace.layer_idx);
        }
        progress().increment(ProgressConvertHoles);
    }
    progress().finish_run(ProgressConvertHoles);

    // Recompute once per modified layer instead of after every hole replacement.
    for (uint32_t layer_idx : modified_layer_idx) {
        layer_handle *mut_layer = ctx->object_borrow_mutable_layer(ctx->object, layer_idx);
        ctx->layer_recompute_slices_and_islands_from_layer_region(mut_layer);
    }

    assert(storage_size(run_ctx->plugin_storage) == 0);
    storage_clear(run_ctx->plugin_storage);
}

Polyholes &Polyholes::instance(orchestrator_handle *orch)
{
    static Polyholes s_instance(orch);
    return s_instance;
}


const char *Polyholes::id_impl() const noexcept
{
    return k_polyholes_id;
}

const char *Polyholes::name_impl() const noexcept
{
    return "Polyholes";
}

const char *Polyholes::description_impl() const noexcept
{
    return "Convert round vertical holes to polygonal holes tuned for FDM extrusion.";
}

const char *Polyholes::exclusive_group_impl() const noexcept
{
    return k_polyholes_exclusive_group;
}

const char *Polyholes::exclusive_group_label_impl() const noexcept
{
    return "Polyholes plugin";
}

const char *Polyholes::exclusive_group_tooltip_impl() const noexcept
{
    return "Choose which active plugin converts round vertical holes to polyholes.";
}

slicing_step_t Polyholes::step_impl() const noexcept
{
    return STEP_POST_SLICING;
}

const char *const *Polyholes::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t Polyholes::priority_impl() const noexcept
{
    return 0;
}

int32_t Polyholes::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (size_t idx = 0; idx < k_used_config_key_count; ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(k_used_config_key_count);
}

int32_t Polyholes::defined_config_keys(const char **keys) const noexcept
{
    if (keys != nullptr)
        for (size_t idx = 0; idx < k_defined_config_key_count; ++idx)
            keys[idx] = k_defined_config_keys[idx];
    return int32_t(k_defined_config_key_count);
}

const char *Polyholes::progress_message_format_impl() const noexcept
{
    return "Searching holes: %u / %u layers";
}

const char *Polyholes::exclusive_group_ui_fragment() noexcept
{
    // The selector setting is generated by the host from STEP_POST_SLICING and
    // the "polyholes" exclusive group. Every Polyholes implementation registers
    // this same fragment id so the UI keeps exactly one implementation selector.
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:insert$beforeline$Convert round vertical holes to polyholes:Polyholes plugin\n"
           "setting:exclusive_group_300_polyholes_plugin\n"
           "end_line\n";
}

const char *Polyholes::print_ui_fragment() noexcept
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:insert$afterline$Vertical Hole shrinking compensation:Convert round vertical holes to polyholes\n"
           "setting:label$_:hole_to_polyhole\n"
           "setting:sidetext_width$5:hole_to_polyhole_threshold\n"
           "setting:hole_to_polyhole_twisted\n"
           "end_line\n";
}

void Polyholes::inilialize_impl(storage_handle *storage) const {

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
    orchestrator_create_option_def(m_orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "hole_to_polyhole_threshold";
    def.type = RAW_CO_FLOAT_OR_PERCENT;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = ("Roundness margin");
    def.full_label = ("Polyhole detection margin");
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Maximum deflection of a point to the estimated radius of the circle."
        "\nAs cylinders are often exported as triangles of varying size, points may not be on the circle circumference."
        " This setting allows you some leeway to broaden the detection."
        "\nIn mm or in % of the radius.");
    def.sidetext = ("mm or %");
    def.has_max_literal = true;
    def.max_literal_value = 10;
    def.max_literal_is_percent = false;
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "0.01";
    orchestrator_create_option_def(m_orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "hole_to_polyhole_twisted";
    def.type = RAW_CO_BOOL;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = ("Twisting");
    def.full_label = ("Polyhole twist");
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Rotate the polyhole every layer.");
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "1";
    orchestrator_create_option_def(m_orchestrator, &def);

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_polyholes_exclusive_group,
                                 Polyholes::exclusive_group_ui_fragment(),
                                 1);

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_polyholes_settings_fragment_id,
                                 Polyholes::print_ui_fragment(),
                                 0);

    raw_gui_rule rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE;
    rule.condition = RAW_GUI_RULE_CONDITION_BOOL_TRUE;
    rule.condition_key = "hole_to_polyhole";
    rule.target_key = "hole_to_polyhole_threshold";
    orchestrator_add_gui_rule(m_orchestrator, &rule);
    rule.target_key = "hole_to_polyhole_twisted";
    orchestrator_add_gui_rule(m_orchestrator, &rule);
}

void Polyholes::setup_impl(const plugin_run_context *, uint32_t) const
{
    // Phase-specific formats make the shared PluginProgress bar readable even
    // though each phase counts a different kind of work item.
    progress().set_phase_format(ProgressSearchHoles, "Searching holes: %u / %u layers");
    progress().set_phase_format(ProgressGroupHoles, "Grouping holes: %u / %u objects");
    progress().set_phase_format(ProgressConvertHoles, "convertion des trous en polyholes: %u / %u holes");
}

void Polyholes::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    progress().add_max(ProgressSearchHoles, Object(ctx->object).layer_count());
    progress().add_max(ProgressGroupHoles, 1);
}

void register_polyholes_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, Polyholes::instance(orch).c_instance());
}

}} // namespace slic3r_api::PolyholesPlugin

#ifdef POLYHOLES_PLUGIN_DLL
SLIC3R_PLUGIN_DECLARE_ABI_VERSION()

extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::PolyholesPlugin::register_polyholes_plugin(orch);
}

#endif //POLYHOLES_PLUGIN_DLL
