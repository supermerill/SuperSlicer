#ifndef slic3r_tests_plugins_perimeter_test_helpers_hpp_
#define slic3r_tests_plugins_perimeter_test_helpers_hpp_

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/SurfaceCollection.hpp"

namespace Slic3r::Test::PerimeterPluginTests {

extern const char *const SIMPLE_PERIMETER_GENERATOR;
extern const char *const PYTHON_SIMPLE_PERIMETER_GENERATOR;
extern const char *const ARACHNE_PERIMETER_GENERATOR;
extern const char *const CLASSIC_PERIMETER_GENERATOR;
extern const char *const DENSE_INFILL_SURFACE_MARKER;
extern const char *const DENSE_INFILL_RECIPE_MODIFIER;
extern const char *const DENSE_INFILL_POST_INFILL_ORDER;
extern const char *const EXTRA_PERIMETER_COUNT;
extern const char *const EXTRA_PERIMETER_BELOW_AREA;
extern const char *const EXTRA_PERIMETER_ODD_LAYER;
extern const char *const ONLY_ONE_PERIMETER_FIRST_LAYER;
extern const char *const ONLY_ONE_PERIMETER_ON_TOP;
extern const char *const SEPARATE_HOLE_CONTOUR;
extern const char *const REMOVE_GAP_FILL_ON_OVERHANGS;
extern const char *const MARK_FIRST_LOOP;
extern const char *const EXTRA_PERIMETERS_ON_OVERHANGS;
extern const char *const EXTRA_PERIMETER_OVERHANG_WAVE;
extern const char *const DETECT_OVERHANG;
extern const char *const FUZZY_SKIN;
extern const char *const INITIAL_TYPED_SURFACE_BUILDER;
extern const char *const SOLID_SHELLS;
extern const char *const TOP_SURFACE_EXPANSION;
extern const char *const CLEAN_INFILL_SURFACES;
extern const char *const INFILL_REGION_COMPATIBILITY_SPLITTER;

struct PerimeterRunCapture
{
    ExtrusionEntityCollection external_perimeters;
    SurfaceCollection fill_surfaces;
    SurfaceCollection fill_no_overlap_surfaces;
    coord_t external_perimeter_width = 0;
    coord_t external_perimeter_spacing = 0;
    double external_perimeter_mm3_per_mm = 0.;
    float external_perimeter_width_mm = 0.f;
    float external_perimeter_height_mm = 0.f;
};

struct PerimeterMultiIslandRunCapture
{
    std::vector<PerimeterRunCapture> islands;
};

struct VerticalSplitCounts
{
    size_t left_only = 0;
    size_t right_only = 0;
    size_t crossing = 0;
};

struct SeparateHoleContourDirectResult
{
    size_t total = 0;
    size_t contours = 0;
    size_t holes = 0;
    size_t children = 0;
    uint32_t perimeter_needed = 0;
};

struct PreparedPerimeterPrint
{
    Model model;
    Print print;
    std::vector<std::unique_ptr<PrintRegion>> extra_regions;
};

struct PerimeterRegionOverride
{
    ExPolygon area;
    std::vector<std::pair<std::string, std::string>> settings;
};

struct GenericFacetPaintingOverride
{
    std::string key;
    EnforcerBlockerType type;
    std::vector<int> facets;
};

ExPolygon rectangle_expolygon(double min_x, double min_y, double max_x, double max_y);
ExPolygon rectangle_with_hole_expolygon();
DynamicPrintConfig perimeter_config(std::initializer_list<std::pair<std::string, std::string>> overrides);
void prepare_cube_print(PreparedPerimeterPrint &prepared, const DynamicPrintConfig &config);
size_t layer_index_for_top(const PrintObject &object);
size_t layer_index_for_odd_layer(const PrintObject &object);

PerimeterRunCapture run_perimeter_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> active_plugins,
    const ExPolygon &area,
    size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides = {},
    const ExPolygon *region_area = nullptr);

PerimeterRunCapture run_perimeter_and_post_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides = {},
    const ExPolygon *region_area = nullptr);

PerimeterRunCapture run_perimeter_and_post_case_with_regions(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    size_t layer_idx,
    const std::vector<PerimeterRegionOverride> &region_overrides);

PerimeterRunCapture run_perimeter_and_post_case_with_lower_area(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    const ExPolygon &lower_area,
    size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides = {},
    const ExPolygon *region_area = nullptr);

PerimeterRunCapture run_perimeter_and_post_case_with_generic_facet_painting(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    size_t layer_idx,
    const std::vector<GenericFacetPaintingOverride> &paintings);

PerimeterMultiIslandRunCapture run_perimeter_multi_island_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> active_plugins,
    const ExPolygons &areas,
    size_t layer_idx);

size_t external_perimeter_count(const PerimeterRunCapture &capture);
const ExtrusionEntityCollection &external_perimeters(const PerimeterRunCapture &capture);
double extrusion_length(const ExtrusionEntity &entity);
size_t total_polyline_points(const ExtrusionEntity &entity);
size_t count_loops_with_role(const ExtrusionEntity &entity, ExtrusionLoopRole role_mask);
VerticalSplitCounts vertical_split_counts(const ExtrusionEntity &entity, coord_t split_x);
void require_leaf_fill_area_consistency(const PerimeterRunCapture &capture);
void require_simple_generator_first_child_area_partition(const PerimeterRunCapture &capture, const ExPolygon &parent_area);

size_t run_remove_gap_fill_module(const DynamicPrintConfig &config,
                                  bool use_region_override,
                                  double *length_out = nullptr,
                                  size_t layer_idx = 0);

SeparateHoleContourDirectResult run_separate_hole_contour_module_direct(
    const DynamicPrintConfig &config,
    uint32_t perimeter_idx,
    uint32_t perimeter_needed,
    uint32_t contour_loop_count,
    uint32_t hole_loop_count,
    bool add_open_polyline = false,
    bool add_unclassified_closed_loop = false);

} // namespace Slic3r::Test::PerimeterPluginTests

#endif // slic3r_tests_plugins_perimeter_test_helpers_hpp_
