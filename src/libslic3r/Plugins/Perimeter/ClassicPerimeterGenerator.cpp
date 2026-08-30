///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ClassicPerimeterGenerator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace ClassicPerimeterGeneratorPlugin {

namespace {

const char *k_classic_perimeter_generator_id = "perimeter.generator.classic";
const char *k_no_dependencies[] = { nullptr };

const char *k_perimeters_key = "perimeters";
const char *k_perimeter_extruder_key = "perimeter_extruder";
const char *k_external_perimeter_width_key = "external_perimeter_extrusion_width";
const char *k_perimeter_width_key = "perimeter_extrusion_width";
const char *k_overhangs_key = "overhangs";
const char *k_overhangs_flow_ratio_key = "overhangs_flow_ratio";
const char *k_overhangs_width_key = "overhangs_width";
const char *k_overhangs_extrusion_spacing_key = "overhangs_extrusion_spacing";
const char *k_thin_perimeters_key = "thin_perimeters";
const char *k_thin_perimeters_all_key = "thin_perimeters_all";
const char *k_perimeter_round_corners_key = "perimeter_round_corners";
const char *k_thin_walls_key = "thin_walls";
const char *k_thin_walls_min_width_key = "thin_walls_min_width";
const char *k_thin_walls_overlap_key = "thin_walls_overlap";
const char *k_thin_walls_merge_key = "thin_walls_merge";
const char *k_gap_fill_enabled_key = "gap_fill_enabled";
const char *k_gap_fill_last_key = "gap_fill_last";
const char *k_gap_fill_min_width_key = "gap_fill_min_width";
const char *k_gap_fill_max_width_key = "gap_fill_max_width";
const char *k_gap_fill_min_length_key = "gap_fill_min_length";
const char *k_gap_fill_min_area_key = "gap_fill_min_area";
const char *k_gap_fill_extension_key = "gap_fill_extension";
const char *k_gap_fill_overlap_key = "gap_fill_overlap";

// used_config_keys is the plugin declaration: it tells the host which existing
// configuration options this generator reads, and with which type. The island
// split itself is done later by RegionSettings; keeping both lists explicit
// makes it visible when a new setting is only read globally, or must also split
// the root geometry into separate region groups.
const raw_used_config_key k_used_config_keys[] = {
    { k_perimeters_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeter_extruder_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_external_perimeter_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeter_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_overhangs_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_overhangs_flow_ratio_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_overhangs_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_overhangs_extrusion_spacing_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_perimeters_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_perimeters_all_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeter_round_corners_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_walls_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_walls_min_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_walls_overlap_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_thin_walls_merge_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_enabled_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_last_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_min_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_max_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_min_length_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_min_area_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_extension_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_gap_fill_overlap_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

constexpr double k_inset_overlap_tolerance = 0.4;

struct ClassicGeneratorState
{
    explicit ClassicGeneratorState(storage_handle *storage) : overhang_area(storage) {}

    c_flow external_flow = {};
    c_flow perimeter_flow = {};
    StoredExPolygonCollection overhang_area;
    uint32_t perimeter_count = 1;
    coord_t overhang_spacing = 0;
    bool perimeter_round_corners = false;
    float external_thin_perimeter = 1.f;
    float internal_thin_perimeter = 1.f;
    bool is_overhangs = false;
};

StoredExPolygonCollection offset_area(storage_handle *storage, const ExPolygon &area, double delta)
{
    ClipperOperand subject(storage, area);
    StoredExPolygonCollection out = clipper_offset(subject, delta).to_expolygon_collection();
    out.ensure_valid();
    return out;
}

void append_classic_loop(StoredExtrusionEntity &dst,
                         const Polygon &polygon,
                         const c_flow &flow,
                         raw_extrusion_role role,
                         uint16_t perimeter_idx,
                         uint16_t perimeter_flags)
{
    if (!polygon.valid_polygon() || polygon.empty())
        return;

    std::vector<c_point> points = polygon.points();
    if (points.empty())
        return;
    points.push_back(points.front());

    StoredExtrusionEntity path(dst.storage(), points);
    EPropertyAttributes &attributes = path.get_or_add(EPropertyAttributes::key);
    attributes.extrusion_role(role)
        .mm3_per_mm(flow.mm3_per_mm)
        .width(float(unscaled(flow.width)))
        .height(float(unscaled(flow.height)));

    // TODO: we'll try with a loop with no children, as it's not useful right now to create a collection with 1 child.
    // It will make things a bit more difficult for algorithms taht want to split it, but we just need to add good helper function.
    //StoredExtrusionEntity loop(dst.storage());
    //loop.get_or_add(EPropertyPerimeter::key).shell_count(perimeter_idx).perimeter_role(loop_role);
    //loop.append_child_move(path.mutable_view());
    //loop.set_flags(RAW_EXTRUSION_FLAG_REVERSIBLE);

    path.set_flags(RAW_EXTRUSION_FLAG_REVERSIBLE);

    path.get_or_add(EPropertyPerimeter::key).shell_count(perimeter_idx).perimeter_flags(perimeter_flags);

    dst.append_child_move(path.mutable_view());
}

bool scaled_values_differ(coord_t lhs, coord_t rhs)
{
    return lhs > rhs ? lhs - rhs > SCALED_EPSILON : rhs - lhs > SCALED_EPSILON;
}

float normalized_thin_perimeter(float value)
{
    if (value < 0.f)
        value = -value;
    return value < 0.02f ? 0.f : value;
}

coord_t overhang_width_from_config(const RegionSettingsValue &values, const c_flow &reference_flow)
{
    if (!values.is_enabled(k_overhangs_flow_ratio_key))
        return 0;
    return scale_i(values.get_effective_value(unscaled(reference_flow.nozzle_diameter), k_overhangs_width_key));
}

coord_t overhang_spacing_from_config(const RegionSettingsValue &values, const c_flow &reference_flow)
{
    if (!values.is_enabled(k_overhangs_extrusion_spacing_key))
        return 0;
    return scale_i(values.get_effective_value(unscaled(reference_flow.nozzle_diameter),
                                             k_overhangs_extrusion_spacing_key));
}

StoredExPolygonCollection create_island_overhang_area(storage_handle *storage,
                                                      const LayerIsland &island,
                                                      const ExPolygon &root_area,
                                                      const RegionSettingsValue &values,
                                                      const c_flow &reference_flow)
{
    StoredExPolygonCollection empty(storage);
    const bool enabled = values.get_bool(k_overhangs_key) && values.is_enabled(k_overhangs_flow_ratio_key);
    if (!enabled || island.lower_islands().empty())
        return empty;
    
    ClipperContext clipper(storage);
    // Overhang area is computed against the union of lower islands touching the
    // current island. This is intentionally island-level: region splitting is
    // applied later by clipping the current root area.
    ClipperOperand lower_area = clipper.empty();
    for (LayerIsland lower_island : island.lower_islands())
        lower_area += clipper(lower_island.slice());
    lower_area = clipper_union(lower_area);

    const coord_t overhang_width = overhang_width_from_config(values, reference_flow);

    // First find the truly unsupported part of the current region group. The
    // small offset2 cleans boundary noise before the optional width expansion
    // below, matching the intent of the legacy process_classic preparation.
    ClipperOperand unsupported =
        clipper_diff_with_safety_offset(clipper(root_area), lower_area);
    unsupported = clipper_offset2(unsupported, double(SCALED_EPSILON * 10), double(SCALED_EPSILON * 10));

    if (unsupported.empty() || overhang_width <= 0)
        return unsupported.to_expolygon_collection();

    // The legacy code grows the unsupported area, clips it back to the current
    // island/region, then intersects the grown selector with the raw unsupported
    // area. With one already-split region group this reduces to a conservative
    // cleanup that keeps only overhang material belonging to this root area.
    ClipperOperand expanded = clipper_intersection(clipper_offset(unsupported, double(overhang_width)), clipper(root_area));
    ClipperOperand shrunk_selector = clipper_offset(expanded, double(overhang_width));
    return clipper_intersection(shrunk_selector, unsupported).to_expolygon_collection();
}

ClipperOperand move_overhangs(storage_handle *storage,
                              ClipperOperand &&last,
                              const ExPolygonCollection &last_overhang,
                              coord_t overhang_spacing,
                              coord_t perimeter_spacing)
{
    if (overhang_spacing <= 0 || last_overhang.empty() ||
        !scaled_values_differ(overhang_spacing, perimeter_spacing))
        return std::move(last);

    ClipperContext clipper(storage);
    ClipperOperand overhang = clipper_intersection(last, clipper(last_overhang));
    if (overhang.empty() || overhang_spacing >= perimeter_spacing)
        return std::move(last);

    // A narrower overhang extrusion needs more centerlines to cover the same
    // unsupported area. Grow only the overhang part before perimeter extraction
    // so the later inward offset creates those extra centerlines locally.
    const coord_t expand_value = perimeter_spacing - overhang_spacing;
    ClipperOperand contracted = clipper_offset(overhang, -double(expand_value));
    ClipperOperand expanded = clipper_offset(contracted, double(expand_value * 2), CLIPPER_JOIN_SQUARE, 0.0);
    return clipper_union2(last, expanded);
}

void create_gap_fill(std::vector<StoredExtrusionEntity> &gaps_extrusions,
                     storage_handle *storage,
                     StoredExPolygonCollection &&gaps,
                     const RegionSettingsValue &gap_fill_config,
                     const c_flow &perimeter_flow)
{
    if (gaps.empty() || perimeter_flow.width <= 0 || perimeter_flow.spacing <= 0)
        return;

    ClipperContext clipper(storage);
    StoredExPolygonCollection printable_gaps(storage);

    // The gap-fill medial axis works only on narrow printable slivers. The
    // legacy algorithm first rejects areas outside the configured width/area
    // window, then grows the surviving shrink-test polygons back to printable
    // gap areas before asking MedialAxis for centerlines.
    coordf_t min_width = std::max(0.2 * double(perimeter_flow.width) * (1.0 - k_inset_overlap_tolerance),
                                  double(SCALED_EPSILON));
    const coordf_t natural_max_width = 2.5 * double(perimeter_flow.spacing);
    const double reference_width = unscaled(double(perimeter_flow.width));

    const coordf_t configured_min_width = gap_fill_config.is_enabled(k_gap_fill_min_width_key) ?
        scale_d(gap_fill_config.get_effective_value(reference_width, k_gap_fill_min_width_key)) :
        0.0;
    const coordf_t configured_max_width = gap_fill_config.is_enabled(k_gap_fill_max_width_key) ?
        scale_d(gap_fill_config.get_effective_value(reference_width, k_gap_fill_max_width_key)) :
        0.0;
    const coord_t min_length = gap_fill_config.is_enabled(k_gap_fill_min_length_key) ?
        scale_i(gap_fill_config.get_effective_value(reference_width, k_gap_fill_min_length_key)) :
        0;
    const coord_t extension_length = gap_fill_config.is_enabled(k_gap_fill_extension_key) ?
        scale_i(gap_fill_config.get_effective_value(reference_width, k_gap_fill_extension_key)) :
        0;

    if (configured_min_width > 0)
        min_width = std::max(min_width, configured_min_width);

    coordf_t max_width = natural_max_width;
    if (configured_max_width > 0)
        max_width = std::min(max_width, configured_max_width);
    if (max_width <= min_width)
        return;

    // The area threshold is expressed in mm2 relative to perimeter width in the
    // legacy config. Two scale_d calls convert that physical area into scaled
    // coordinate squared units, matching ExPolygon::area().
    const double reference_area = reference_width * reference_width;
    const double min_area = gap_fill_config.is_enabled(k_gap_fill_min_area_key) ?
        scale_d(scale_d(gap_fill_config.get_effective_value(reference_area, k_gap_fill_min_area_key))) :
        0.0;

    ClipperOperand too_big = clipper_offset2(clipper(gaps), double(-max_width / 2.0), double(max_width / 2.0));
    if (!too_big.empty()) {
        gaps = clipper_diff_with_safety_offset(clipper(gaps), too_big).to_expolygon_collection();
    }

    // Each gap is shrink-tested before medial-axis generation. If a shrink
    // splits a gap into many tiny pieces, a stronger shrink is tried and kept
    // only when it significantly reduces the number of candidate islands.
    for (const ExPolygon &gap : gaps) {
        if (gap.area() <= min_area)
            continue;

        const coordf_t offset_test = min_width * 0.5;
        StoredExPolygonCollection shrunk =
            clipper_offset(clipper(gap), -double(offset_test)).to_expolygon_collection();

        if (shrunk.size() > 1) {
            for (int32_t idx = 0; idx < int32_t(shrunk.size()); ++idx) {
                if (shrunk[idx].area() < double(SCALED_EPSILON * SCALED_EPSILON * 4)) {
                    shrunk.erase(uint32_t(idx));
                    --idx;
                    continue;
                }

                StoredExPolygonCollection wider =
                    clipper_offset(clipper(shrunk[idx]), double(offset_test)).to_expolygon_collection();
                if (wider.empty() || wider.front().area() < min_area) {
                    shrunk.erase(uint32_t(idx));
                    --idx;
                }
            }

            const coordf_t stronger_offset_test = min_width * 0.8;
            StoredExPolygonCollection stronger_shrunk =
                clipper_offset(clipper(gap), -double(stronger_offset_test)).to_expolygon_collection();
            for (int32_t idx = 0; idx < int32_t(stronger_shrunk.size()); ++idx) {
                if (stronger_shrunk[idx].area() < double(SCALED_EPSILON * SCALED_EPSILON * 4)) {
                    stronger_shrunk.erase(uint32_t(idx));
                    --idx;
                    continue;
                }

                StoredExPolygonCollection wider =
                    clipper_offset(clipper(stronger_shrunk[idx]), double(stronger_offset_test)).to_expolygon_collection();
                if (wider.empty() || wider.front().area() < min_area) {
                    stronger_shrunk.erase(uint32_t(idx));
                    --idx;
                }
            }

            if (double(shrunk.size()) / 1.42 > double(stronger_shrunk.size())) {
                StoredExPolygonCollection restored =
                    clipper_offset(clipper(stronger_shrunk.readonly()), double(stronger_offset_test)).to_expolygon_collection();
                printable_gaps.append_move_from(restored);
            } else {
                StoredExPolygonCollection restored =
                    clipper_offset(clipper(shrunk.readonly()), double(offset_test)).to_expolygon_collection();
                printable_gaps.append_move_from(restored);
            }
        } else {
            StoredExPolygonCollection restored =
                clipper_offset(clipper(shrunk.readonly()), double(offset_test)).to_expolygon_collection();
            printable_gaps.append_move_from(restored);
        }
    }

    // The host medial-axis wrapper converts the gap areas directly into an
    // extrusion tree. It uses spacing_ratio to rebuild the Flow used by
    // thin_variable_width(), so the overlap setting is copied into that field.
    c_flow gap_fill_flow = perimeter_flow;
    gap_fill_flow.spacing_ratio = float(gap_fill_config.get_effective_value(1.0, k_gap_fill_overlap_key));

    for (const ExPolygon &gap : printable_gaps) {
        StoredExtrusionEntity gap_fill =
            medial_axis_gap_fill(gap_fill_flow)
                .medial_widths(coord_t(min_width), coord_t(natural_max_width))
                .extrusion_widths(coord_t(min_width), coord_t(max_width))
                .min_centerline_length(min_length)
                .endpoint_extension(extension_length)
                .can_reverse(true)
                .build(storage, gap);
        if (!gap_fill.empty())
            gaps_extrusions.emplace_back(std::move(gap_fill));
    }
}



void generate_external_perimeter(const PerimeterGenerationContextView &params,
                                 const ClassicGeneratorState &classic_params,
                                 PerimeterNodeView &node,
                                 expolygon_collection_handle *inner_areas_out,
                                 expolygon_collection_handle *inner_fill_areas_out) {
    storage_handle *storage = params.storage();
    ClipperContext clipper(storage);
    ClipperOperand next_onion(storage);
    ClipperOperand last = clipper(node.area());
    const c_flow external_flow = classic_params.external_flow;

    // allow this perimeter to overlap itself?
    float thin_perimeter = classic_params.external_thin_perimeter;
    bool allow_perimeter_anti_hysteresis = thin_perimeter >= 0;
    if (thin_perimeter < 0) {
        thin_perimeter = -thin_perimeter;
    }
    if (thin_perimeter < 0.02) { // can create artifacts
        thin_perimeter = 0;
    }

    // do overhangs_extrusion_width
    if (classic_params.is_overhangs && classic_params.overhang_spacing > 0) {
        last = move_overhangs(storage, std::move(last), classic_params.overhang_area.readonly(),
                              classic_params.overhang_spacing / 2, external_flow.width / 2);
    }

    // compute next onion
    // the minimum thickness of a single loop is:
    // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
    coordf_t good_spacing = external_flow.width / 2;
    coordf_t overlap_spacing = (1 - thin_perimeter) * external_flow.spacing / 2;
    if (thin_perimeter > 0.98) {
        next_onion = clipper_offset(last, -(float) (external_flow.width / 2), CLIPPER_JOIN_MITER, 3);
    } else {
        coordf_t good_spacing = external_flow.width / 2;
        coordf_t overlap_spacing = (1.f - thin_perimeter) * external_flow.spacing / 2;
        next_onion = clipper_offset2(last, -(float) (good_spacing + overlap_spacing - 1),
                                     +(float) (overlap_spacing - 1), CLIPPER_JOIN_MITER, 3);
    }
    if (thin_perimeter < 0.7) {
        // offset2_ex can create artifacts, if too big. see superslicer#2428
        next_onion = clipper_intersection(next_onion,
                                          clipper_offset(last, -(float) (external_flow.width / 2), CLIPPER_JOIN_MITER,
                                                         3));
    }

    RegionSettings thin_wall_settings = params.region_settings(
        {{k_thin_walls_key, k_thin_walls_min_width_key, k_thin_walls_overlap_key}});
    thin_wall_settings.segregate(node.area());

    // un-hysteresis for thin walls
    if (thin_wall_settings.has_many_config(k_thin_walls_key) ||
        thin_wall_settings.get_solo_config(k_thin_walls_key).get_bool()) {
        // detect edge case where a curve can be split in multiple small chunks.
        if (allow_perimeter_anti_hysteresis && next_onion.path_count() > 1) {
            // don't go too far, it's not possible to print thin wall after that
            std::vector<float> variations = {-.025f, .025f, -.05f, .05f, -.075f, .1f, .15f};
            const coordf_t good_spacing = external_flow.width / 2;
            const coordf_t overlap_spacing = (1 - thin_perimeter) * external_flow.spacing / 2;
            for (size_t idx_variations = 0; next_onion.path_count() > 1 && idx_variations < variations.size();
                 idx_variations++) {
                const coordf_t spacing_change = external_flow.spacing * variations[idx_variations];
                // don't go over 100% overlap
                if (overlap_spacing + spacing_change < 1) {
                    continue;
                }
                // use a sightly bigger spacing to try to drastically improve the split, that can lead to
                // very thick gapfill
                ClipperOperand next_onion_secondTry = clipper_offset2(last,
                                                                      -(float) (good_spacing + overlap_spacing +
                                                                                spacing_change - 1),
                                                                      +(float) (overlap_spacing + spacing_change) - 1);
                if (next_onion.path_count() > next_onion_secondTry.path_count() * 1.2 &&
                    next_onion.path_count() > next_onion_secondTry.path_count() + 2) {
                    next_onion = std::move(next_onion_secondTry);
                }
            }
        }
    }

    std::vector<StoredExtrusionEntity> thin_wall_extrusions;
    // look for thin walls
    for (auto const &[thin_walls_config, areas] : thin_wall_settings.get_areas(k_thin_walls_key)) {
        if (thin_walls_config.get_bool()) {
            StoredExPolygonCollection last_good_areas = areas.intersections(node.area());

            // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
            // (actually, something larger than that still may exist due to mitering or other causes)
            // coord_t min_width =
            // scale_i(params.config.thin_walls_min_width.get_effective_value(params.ext_perimeter_flow.nozzle_diameter()));
            coord_t min_width = scale_i(thin_walls_config.get_effective_value(unscaled(external_flow.nozzle_diameter),
                                                                              k_thin_walls_min_width_key));

            ClipperOperand no_thin_zone = clipper_offset(next_onion, double(external_flow.width / 2),
                                                         CLIPPER_JOIN_SQUARE);
            // medial axis requires non-overlapping geometry
            const ClipperOperand thin_zones = clipper_diff_with_safety_offset(clipper(last_good_areas), no_thin_zone);
            // don't use offset2_ex, because we don't want to merge the zones that have been separated.
            // a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
            ClipperOperand half_thins = clipper_offset(thin_zones, double(-min_width / 2));
            // half_thins.ensure_valid(external_flow.width / 20);
            //  we push the bits removed and put them into what we will use as our anchor
            if (!half_thins.empty()) {
                no_thin_zone = clipper_diff_with_safety_offset(clipper(last_good_areas),
                                                               clipper_offset(half_thins,
                                                                              double(min_width / 2 - SCALED_EPSILON)));
                no_thin_zone = clipper_offset2(no_thin_zone, -external_flow.width / 20, external_flow.width / 20);
                // no_thin_zone.ensure_valid(external_flow.width / 20);
            }
            StoredExPolygonCollection thins(storage);
            // compute a bit of overlap to anchor thin walls inside the print.
            for (const ExPolygon &half_thin : half_thins.to_expolygon_collection()) {
                // growing back the polygon
                StoredExPolygonCollection thin = clipper_offset(clipper(half_thin), double(min_width / 2))
                                                     .to_expolygon_collection();
                thin.ensure_valid(external_flow.width / 10);
                assert(thin.size() <= 1);
                if (thin.empty() || !thin.front().is_valid() ||
                    thin[0].area() <= min_width * (external_flow.width + external_flow.spacing)) {
                    continue;
                }
                thins.push_back(thin[0]);
                // const coord_t thin_walls_overlap =
                // scale_i(params.config.thin_walls_overlap.get_effective_value(params.ext_perimeter_flow.nozzle_diameter()));
                const coord_t thin_walls_overlap = scale_i(
                    thin_walls_config.get_effective_value(unscaled(external_flow.nozzle_diameter),
                                                          k_thin_walls_overlap_key));
                ClipperOperand full_thin_with_overlap = clipper_offset(clipper(half_thin),
                                                                       double(min_width / 2) +
                                                                           (float) (thin_walls_overlap),
                                                                       CLIPPER_JOIN_SQUARE);
                // clip no_thin_zone with bounding box from full_thin_with_overlap, as no_thin_zone can be huge.
                c_bounding_box bbox_full_thin_with_overlap = full_thin_with_overlap.bounding_box();
                ClipperOperand no_thin_zone_simplified =
                    clipper_clip_shapes_with_subject_bbox(storage, no_thin_zone, bbox_full_thin_with_overlap);

                ClipperOperand anchor = clipper_intersection_with_safety_offset(full_thin_with_overlap,
                                                                                no_thin_zone_simplified);
                ClipperOperand bounds = clipper(thin.readonly());
                bounds += anchor;
                bounds = clipper_union_with_safety_offset(bounds);
                for (ExPolygon bound : bounds.to_expolygon_collection()) {
                    if (!clipper_intersection(clipper(thin[0]), clipper(bound)).empty()) {
                        if (!bound.is_valid()) {
                            continue;
                        }
                        // the maximum thickness of our thin wall area is equal to the minimum thickness
                        // of a single loop (*1.2 because of circles approx. and enlrgment from 'div')
                        StoredExtrusionEntity thin_wall =
                            medial_axis_thin_wall(external_flow)
                                .medial_widths(min_width,
                                               coord_t((external_flow.width + external_flow.spacing) * 12 / 10))
                                .extrusion_widths(external_flow.nozzle_diameter, 0)
                                .extension_area(bound)
                                .endpoint_taper(thin_walls_overlap)
                                .min_centerline_length(external_flow.width + external_flow.spacing)
                                .build(storage, thin[0]);
                        if (!thin_wall.empty())
                            thin_wall_extrusions.emplace_back(std::move(thin_wall));
                        break;
                    }
                }
            }
            // use perimeters to extrude area that can't be printed by thin walls
            // it's a bit like re-add thin area into perimeter area.
            // it can over-extrude a bit, but it's for a better good.
            if (thin_perimeter > 0.98) {
                next_onion = clipper_union2(next_onion,
                                            clipper_offset(clipper_diff_with_safety_offset(last,
                                                                                           clipper(thins.readonly())),
                                                           -(float) (external_flow.width / 2), CLIPPER_JOIN_MITER, 3));
            } else if (thin_perimeter > 0.01) {
                next_onion =
                    clipper_union2(next_onion,
                                   clipper_offset2(clipper_diff_with_safety_offset(last, clipper(thins.readonly())),
                                                   -(float) ((external_flow.width / 2) +
                                                             ((1 - thin_perimeter) * external_flow.spacing / 4)),
                                                   (float) ((1 - thin_perimeter) * external_flow.spacing / 4),
                                                   CLIPPER_JOIN_MITER, 3));
            } else {
                next_onion =
                    clipper_union2(next_onion,
                                   clipper_offset2(clipper_diff_with_safety_offset(last, clipper(thins.readonly())),
                                                   -(float) ((external_flow.width / 2) + (external_flow.spacing / 4)),
                                                   (float) (external_flow.spacing / 4), CLIPPER_JOIN_MITER, 3));
            }
            // simplify the loop to avoid almost-0 segments
            // next_onion.ensure_valid(std::max<coord_t>(SCALED_EPSILON, external_flow.width / 20));
            // mask
            next_onion = clipper_intersection(next_onion, last);
        }
    }

    // create thin wall root
    StoredExtrusionEntity thin_wall_extrusion_root(storage);
    if (thin_wall_extrusions.size() == 1) {
        thin_wall_extrusion_root.move_from(thin_wall_extrusions.front().mutable_view());
    } else if (thin_wall_extrusions.size() > 1) {
        for (StoredExtrusionEntity &child : thin_wall_extrusions)
            thin_wall_extrusion_root.append_child_move(child.mutable_view());
    }

    // create perimeter extrusions
    StoredExtrusionEntity perimeter_extrusion_root(storage);
    StoredExPolygonCollection perimeter_centerlines = next_onion.to_expolygon_collection();
    for (ExPolygon area : perimeter_centerlines) {
        append_classic_loop(perimeter_extrusion_root, area.contour(), external_flow,
                            RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER, node.perimeter_idx(),
                            static_cast<uint16_t>(C_EXTRUSION_PERIMETER_FLAG_LOOP));
        for (Polygon hole : area.holes())
            append_classic_loop(perimeter_extrusion_root, hole, external_flow, RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER,
                                node.perimeter_idx(),
                                static_cast<uint16_t>(C_EXTRUSION_PERIMETER_FLAG_LOOP |
                                                      C_EXTRUSION_PERIMETER_FLAG_HOLE));
    };

    // create final root for this area
    StoredExtrusionEntity final_extrusion_root(storage);
    if (thin_wall_extrusion_root.empty()) {
        final_extrusion_root = std::move(perimeter_extrusion_root);
    } else if (perimeter_extrusion_root.empty()) {
        final_extrusion_root = std::move(thin_wall_extrusion_root);
    } else {
        final_extrusion_root.append_child_move(perimeter_extrusion_root.mutable_view());
        final_extrusion_root.append_child_move(thin_wall_extrusion_root.mutable_view());
        // this collection should not be sortable nor reversible
        final_extrusion_root.disable_sort().disable_reverse();
    }

    // give back the extrusions to the caller.
    extrusion_move_from(node.mutable_handle()->extrusions, final_extrusion_root.mutable_handle());

    // offset again to have the next & fill area

    // store maximum fill area (our centerline)
    expolygons_move(inner_fill_areas_out, perimeter_centerlines.mutable_handle());

    // put next onion as children
    StoredExPolygonCollection inner_fill_areas = clipper_offset(next_onion, -0.5 * double(external_flow.spacing))
                                                     .to_expolygon_collection();
    expolygons_move(inner_areas_out, inner_fill_areas.mutable_handle());
}

void generate_internal_perimeter(const PerimeterGenerationContextView &params,
                                 const ClassicGeneratorState &classic_params,
                                 PerimeterNodeView node,
                                 expolygon_collection_handle *inner_areas_out,
                                 expolygon_collection_handle *inner_fill_areas_out)
{
    storage_handle *storage = params.storage();
    ClipperContext clipper(storage);
    ClipperOperand next_onion(storage);
    ClipperOperand last = clipper(node.area());
    const c_flow internal_flow = classic_params.perimeter_flow;
    const c_flow previous_flow = node.perimeter_idx() == 1 ? classic_params.external_flow :
                                                               classic_params.perimeter_flow;

    // allow this perimeter to overlap itself?
    float thin_perimeter = 0;
    if (classic_params.external_thin_perimeter > 0) {
                thin_perimeter = classic_params.internal_thin_perimeter;
    }
    bool allow_perimeter_anti_hysteresis = thin_perimeter >= 0;
    if (thin_perimeter < 0) {
        thin_perimeter = -thin_perimeter;
    }
    if (thin_perimeter < 0.02) { // can create artifacts
        thin_perimeter = 0;
    }

    // do overhangs_extrusion_width
    if (classic_params.is_overhangs && classic_params.overhang_spacing > 0) {
        last = move_overhangs(storage, std::move(last), classic_params.overhang_area.readonly(),
                              classic_params.overhang_spacing / 2, previous_flow.spacing / 2);
    }

    // FIXME Is this offset correct if the line width of the inner perimeters differs
    //  from the line width of the infill?
    const coord_t half_spacing = internal_flow.spacing / 2;
    if (thin_perimeter <= 0.98) {
        const coordf_t overlap_spacing = (1 - thin_perimeter) * internal_flow.spacing / 2;
        // This path will ensure, that the perimeters do not overfill, as in
        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
        // excessively, creating gaps, which then need to be filled in by the not very
        // reliable gap fill algorithm.
        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
        // the original.
        next_onion = clipper_offset2(last, -(float) (half_spacing + overlap_spacing - 1),
                                     +(float) (overlap_spacing - 1),
                                     (classic_params.perimeter_round_corners ? CLIPPER_JOIN_ROUND :
                                                                               CLIPPER_JOIN_MITER),
                                     (classic_params.perimeter_round_corners ? internal_flow.width / 10 : 3));
        if (allow_perimeter_anti_hysteresis) {
            // now try with different min spacing if we fear some hysteresis
            // TODO, do that for each polygon from last, instead to do for all of them in one go.
            ClipperOperand no_thin_onion = clipper_offset(last, double(-half_spacing));
            double last_area = node.area().area();
            double new_area = 0;
            for (const ExPolygon &expoly : next_onion.to_expolygon_collection()) {
                new_area += expoly.area();
            }

            std::vector<float> variations = {.025f, .06f,
                                             .125f}; // don't over-extrude, so don't use negative variations
            for (size_t idx_variations = 0;
                 (next_onion.path_count() > no_thin_onion.path_count() || (new_area != 0 && last_area > new_area * 100)) &&
                 idx_variations < variations.size();
                 idx_variations++) {
                const coordf_t spacing_change = half_spacing * variations[idx_variations];
                // use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                ClipperOperand next_onion_secondTry =
                    clipper_offset2(last, -(float) (half_spacing + overlap_spacing + spacing_change - 1),
                                    +(float) (overlap_spacing + spacing_change - 1),
                                    (classic_params.perimeter_round_corners ? CLIPPER_JOIN_ROUND : CLIPPER_JOIN_MITER),
                                    (classic_params.perimeter_round_corners ? internal_flow.width / 10 : 3));
                if (next_onion.path_count() > next_onion_secondTry.path_count() * 1.2 &&
                    next_onion.path_count() > next_onion_secondTry.path_count() + 2) {
                    // don't get it if it creates too many
                    next_onion = std::move(next_onion_secondTry);
                } else if (next_onion.path_count() > next_onion_secondTry.path_count() || last_area > new_area * 100) {
                    // don't get it if it's too small
                    double area_new = 0;
                    for (const ExPolygon &expoly : next_onion_secondTry.to_expolygon_collection()) {
                        area_new += expoly.area();
                    }
                    if (last_area > area_new * 100 || new_area == 0) {
                        next_onion = std::move(next_onion_secondTry);
                    }
                }
            }
            last_area = new_area;
        }
    } else {
        // If "overlapping_perimeters" is enabled, this paths will be entered, which
        // leads to overflows, as in prusa3d/Slic3r GH #32
        next_onion = clipper_offset(last, double(-half_spacing),
                                     (classic_params.perimeter_round_corners ? CLIPPER_JOIN_ROUND :
                                                                               CLIPPER_JOIN_MITER),
                                     (classic_params.perimeter_round_corners ? internal_flow.width / 10 : 3));
    }
    // do overhangs_extrusion_width
    if (classic_params.is_overhangs && classic_params.overhang_spacing > 0) {
        next_onion = move_overhangs(storage, std::move(next_onion), classic_params.overhang_area.readonly(),
                              classic_params.overhang_spacing / 2, previous_flow.spacing / 2);
    }

    // look for gaps
    std::vector<StoredExtrusionEntity> gaps_extrusions;
    RegionSettings gap_fill_settings = params.region_settings({{k_gap_fill_enabled_key,
                                                                k_gap_fill_last_key,
                                                                k_gap_fill_min_width_key,
                                                                k_gap_fill_max_width_key,
                                                                k_gap_fill_min_length_key,
                                                                k_gap_fill_min_area_key,
                                                                k_gap_fill_extension_key,
                                                                k_gap_fill_overlap_key}});
    gap_fill_settings.segregate(node.area());
    if (gap_fill_settings.has_many_config(k_gap_fill_enabled_key) ||
        gap_fill_settings.get_solo_config(k_gap_fill_enabled_key).get_bool(k_gap_fill_enabled_key)) {
        const bool has_overhang_gap = classic_params.is_overhangs && !classic_params.overhang_area.empty();
        // not using safety offset here would "detect" very narrow gaps
        // (but still long enough to escape the area threshold) that gap fill
        // won't be able to fill but we'd still remove from infill area
        ClipperOperand no_last_gapfill = clipper_offset(next_onion, 0.5f * internal_flow.spacing + 30,
                                                        (classic_params.perimeter_round_corners ? CLIPPER_JOIN_ROUND :
                                                                                                  CLIPPER_JOIN_MITER),
                                                        (classic_params.perimeter_round_corners ? internal_flow.width / 10 : 3));
        StoredExPolygonCollection gapfill =
            clipper_diff(clipper_offset(last, -0.5f * previous_flow.spacing + 30),
                         no_last_gapfill).to_expolygon_collection();

        // RegionSettings gives one clip per compatible config value. A single
        // global config produces an accept-all clip, so this same loop covers
        // the fast path and the modifier/region-split path.
        for (const auto &[gap_fill_config, areas] : gap_fill_settings.get_areas(k_gap_fill_enabled_key)) {
            if (!gap_fill_config.get_bool(k_gap_fill_enabled_key))
                continue;

            // check if we are going to have an other perimeter
            if (!(node.perimeter_idx() < node.perimeter_needed() || has_overhang_gap || next_onion.empty() ||
                  (gap_fill_config.get_bool(k_gap_fill_last_key) && node.perimeter_idx() == node.perimeter_needed())))
                continue;

            create_gap_fill(gaps_extrusions, storage, std::move(areas.intersections(gapfill)), gap_fill_config, internal_flow);
        }
    }

    // create map fill root
    StoredExtrusionEntity gaps_extrusions_root(storage);
    if (gaps_extrusions.size() == 1) {
        gaps_extrusions_root.move_from(gaps_extrusions.front().mutable_view());
    } else if (gaps_extrusions.size() > 1) {
        for (StoredExtrusionEntity &child : gaps_extrusions)
            gaps_extrusions_root.append_child_move(child.mutable_view());
    }

    // create perimeter extrusions
    StoredExtrusionEntity perimeter_extrusion_root(storage);
    StoredExPolygonCollection perimeter_centerlines = next_onion.to_expolygon_collection();
    for(ExPolygon area : perimeter_centerlines){
        append_classic_loop(perimeter_extrusion_root, area.contour(), internal_flow,
                            RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER, node.perimeter_idx(),
                            static_cast<uint16_t>(C_EXTRUSION_PERIMETER_FLAG_LOOP |
                                                  C_EXTRUSION_PERIMETER_FLAG_INTERNAL));
        for (Polygon hole : area.holes())
            append_classic_loop(perimeter_extrusion_root, hole, internal_flow, RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER,
                                node.perimeter_idx(),
                                static_cast<uint16_t>(C_EXTRUSION_PERIMETER_FLAG_LOOP |
                                                      C_EXTRUSION_PERIMETER_FLAG_INTERNAL |
                                                      C_EXTRUSION_PERIMETER_FLAG_HOLE));
    };

    // create final root for this area
    StoredExtrusionEntity final_extrusion_root(storage);
    if (gaps_extrusions_root.empty()) {
        final_extrusion_root = std::move(perimeter_extrusion_root);
    } else if (perimeter_extrusion_root.empty()) {
        final_extrusion_root = std::move(gaps_extrusions_root);
    } else {
        final_extrusion_root.append_child_move(perimeter_extrusion_root.mutable_view());
        final_extrusion_root.append_child_move(gaps_extrusions_root.mutable_view());
        // this collection should not be sortable nor reversible
        final_extrusion_root.disable_sort().disable_reverse();
    }

    // give back the extrusions to the caller.
    extrusion_move_from(node.mutable_handle()->extrusions, final_extrusion_root.mutable_handle());

    // offset again to have the next & fill area

    // store maximum fill area (our centerline)
    expolygons_move(inner_fill_areas_out, perimeter_centerlines.mutable_handle());

    // put next onion as children
    StoredExPolygonCollection inner_fill_areas = clipper_offset(next_onion, -0.5 * double(internal_flow.spacing))
                                                     .to_expolygon_collection();
    expolygons_move(inner_areas_out, inner_fill_areas.mutable_handle());
}

int32_t generate_one_perimeter(void *generator_context,
                               perimeter_generation_context *context,
                               perimeter_node *node,
                               expolygon_collection_handle *inner_areas_out,
                               expolygon_collection_handle *inner_fill_areas_out)
{
    if (generator_context == nullptr || context == nullptr || context->run_ctx == nullptr ||
        context->run_ctx->plugin_storage == nullptr || node == nullptr ||
        inner_areas_out == nullptr || inner_fill_areas_out == nullptr)
        return 0;

    const ClassicGeneratorState &state = *reinterpret_cast<const ClassicGeneratorState *>(generator_context);
    storage_handle *storage = context->run_ctx->plugin_storage;
    PerimeterGenerationContextView context_view(context);
    PerimeterNodeView node_view(node);

    if (!node_view.needs_more_perimeters()) {
        StoredExPolygonCollection inner_areas(storage, node_view.area());
        expolygons_move(inner_areas_out, inner_areas.mutable_handle());

        StoredExPolygonCollection inner_fill_areas(storage, node_view.fill_area());
        expolygons_move(inner_fill_areas_out, inner_fill_areas.mutable_handle());
        return 1;
    }

    if (node_view.perimeter_idx() == 0)
        generate_external_perimeter(context_view, state, node_view, inner_areas_out, inner_fill_areas_out);
    else
        generate_internal_perimeter(context_view, state, node_view, inner_areas_out, inner_fill_areas_out);

    return 1;
}

c_flow external_perimeter_flow(const std::vector<LayerRegion> &regions)
{
    return regions.empty() ? c_flow{} : regions.front().flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
}

c_flow perimeter_flow(const std::vector<LayerRegion> &regions)
{
    return regions.empty() ? c_flow{} : regions.front().flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER);
}

std::vector<const layer_region_handle *> region_handles(const std::vector<LayerRegion> &regions)
{
    std::vector<const layer_region_handle *> out;
    out.reserve(regions.size());
    for (const LayerRegion &region : regions)
        out.push_back(region.handle());
    return out;
}

} // namespace

ClassicPerimeterGenerator &
ClassicPerimeterGenerator::instance(orchestrator_handle *orch)
{
    static ClassicPerimeterGenerator s_instance(orch);
    return s_instance;
}

const char *ClassicPerimeterGenerator::id_impl() const noexcept
{
    return k_classic_perimeter_generator_id;
}

const char *ClassicPerimeterGenerator::name_impl() const noexcept
{
    return "Classic perimeter generator";
}

const char *ClassicPerimeterGenerator::description_impl() const noexcept
{
    return "API-only skeleton for the future process_classic perimeter generator.";
}

slicing_step_t ClassicPerimeterGenerator::step_impl() const noexcept
{
    return STEP_PERIMETER;
}

const char *const *ClassicPerimeterGenerator::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ClassicPerimeterGenerator::priority_impl() const noexcept
{
    return 5;
}

int32_t ClassicPerimeterGenerator::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *ClassicPerimeterGenerator::progress_message_format_impl() const noexcept
{
    return "Classic perimeter generator: %u / %u islands";
}

void ClassicPerimeterGenerator::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_perimeter *ctx = plugin_ctx_as_generate_perimeter(run_ctx);
    if (ctx != nullptr && ctx->island != nullptr)
        progress().add_max(1);
}

void ClassicPerimeterGenerator::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_perimeter *ctx = plugin_ctx_as_generate_perimeter(run_ctx);
    if (ctx == nullptr || ctx->island == nullptr || ctx->run_region_group == nullptr ||
        run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    throw_if_cancelled(run_ctx);

    const LayerIsland island(ctx->island);
    if (island.region_count() == 0)
        return;

    // Classic cannot safely share one perimeter tree between regions with
    // different perimeter counts, widths, overhang behavior or thin-perimeter
    // overlap. RegionSettings cuts the LayerIsland into the largest areas where
    // these values are identical, then each area gets its own root node.
    RegionSettings settings(run_ctx->plugin_storage,
                            island,
                            {{ k_perimeters_key,
                               k_perimeter_extruder_key,
                               k_external_perimeter_width_key,
                               k_perimeter_width_key,
                               k_overhangs_key,
                               k_overhangs_flow_ratio_key,
                               k_overhangs_width_key,
                               k_overhangs_extrusion_spacing_key,
                               k_perimeter_round_corners_key,
                               k_thin_perimeters_key,
                               k_thin_perimeters_all_key }});
    settings.segregate(island.slice());

    const RegionSettings::AreaMap &groups = settings.get_areas(k_perimeters_key);
    for (const auto &[values, clip] : groups) {
        const std::vector<LayerRegion> &regions = settings.get_regions(k_perimeters_key, values);
        if (regions.empty())
            continue;

        std::vector<const layer_region_handle *> handles = region_handles(regions);
        StoredExPolygonCollection root_areas = clip.intersections(island.slice());
        for (ExPolygon root_area : root_areas) {
            ClassicGeneratorState state(run_ctx->plugin_storage);
            state.external_flow = external_perimeter_flow(regions);
            state.perimeter_flow = perimeter_flow(regions);
            state.perimeter_round_corners = values.get_bool(k_perimeter_round_corners_key);
            state.perimeter_count = values.get_int(k_perimeters_key) <= 0 ?
                0 : uint32_t(values.get_int(k_perimeters_key));
            state.is_overhangs = values.get_bool(k_overhangs_key) && values.is_enabled(k_overhangs_flow_ratio_key);
            state.overhang_spacing = overhang_spacing_from_config(values, state.perimeter_flow);
            state.external_thin_perimeter =
                normalized_thin_perimeter(float(values.get_effective_value(1.0, k_thin_perimeters_key)));
            state.internal_thin_perimeter =
                normalized_thin_perimeter(float(values.get_effective_value(1.0, k_thin_perimeters_all_key)));
            state.overhang_area =
                create_island_overhang_area(run_ctx->plugin_storage, island, root_area, values, state.perimeter_flow);

            ctx->run_region_group(ctx,
                                  handles.empty() ? nullptr : handles.data(),
                                  uint32_t(handles.size()),
                                  root_area.handle(),
                                  &state,
                                  &generate_one_perimeter);
        }
    }

    progress().increment();
}

void register_classic_perimeter_generator_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ClassicPerimeterGenerator::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin

#ifdef CLASSIC_PERIMETER_GENERATOR_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin::register_classic_perimeter_generator_plugin(orch);
}
#endif // CLASSIC_PERIMETER_GENERATOR_PLUGIN_DLL
