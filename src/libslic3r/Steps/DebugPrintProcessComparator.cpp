///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DebugPrintProcessComparator.hpp"

#include "libslic3r/PrintObject.hpp"
#ifdef _DEBUG

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"

#include <array>
#include <sstream>
#include <vector>

namespace Slic3r::Steps {
namespace {

void append_path_error(std::string &out_error, const std::string &path, const char *what)
{
    out_error += path;
    out_error += ": ";
    out_error += what;
}

template<class T>
bool same_value(const T &lhs, const T &rhs, std::string &out_error, const std::string &path, const char *name)
{
    if (lhs == rhs)
        return true;

    std::ostringstream msg;
    msg << name << " mismatch";
    append_path_error(out_error, path, msg.str().c_str());
    return false;
}

bool same_expolygons(const ExPolygons &lhs, const ExPolygons &rhs, std::string &out_error, const std::string &path)
{
    if (lhs == rhs)
        return true;

    std::ostringstream msg;
    msg << "ExPolygons mismatch (lhs size=" << lhs.size() << ", rhs size=" << rhs.size() << ")";
    append_path_error(out_error, path, msg.str().c_str());
    return false;
}

bool same_surfaces(const Surface &lhs, const Surface &rhs, std::string &out_error, const std::string &path)
{
    if (!same_value(lhs.surface_type, rhs.surface_type, out_error, path, "surface_type"))
        return false;
    if (!same_value(lhs.expolygon, rhs.expolygon, out_error, path, "expolygon"))
        return false;
    if (!same_value(lhs.scaled_thickness(), rhs.scaled_thickness(), out_error, path, "scaled_thickness"))
        return false;
    if (!same_value(lhs.thickness_layers, rhs.thickness_layers, out_error, path, "thickness_layers"))
        return false;
    if (!same_value(lhs.bridge_angle, rhs.bridge_angle, out_error, path, "bridge_angle"))
        return false;
    if (!same_value(lhs.extra_perimeters, rhs.extra_perimeters, out_error, path, "extra_perimeters"))
        return false;
    if (!same_value(lhs.maxNbSolidLayersOnTop, rhs.maxNbSolidLayersOnTop, out_error, path, "maxNbSolidLayersOnTop"))
        return false;
    if (!same_value(lhs.priority, rhs.priority, out_error, path, "priority"))
        return false;
    return true;
}

bool same_surface_collection(const SurfaceCollection &lhs,
                             const SurfaceCollection &rhs,
                             std::string &out_error,
                             const std::string &path)
{
    if (lhs.size() != rhs.size()) {
        std::ostringstream msg;
        msg << "surface collection size mismatch (lhs=" << lhs.size() << ", rhs=" << rhs.size() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }

    for (size_t idx = 0; idx < lhs.size(); ++idx) {
        if (!same_surfaces(lhs.at(idx), rhs.at(idx), out_error, path + ".surface[" + std::to_string(idx) + "]"))
            return false;
    }
    return true;
}

std::vector<int> region_ids(const LayerRegionSetCPtrs &regions)
{
    std::vector<int> out;
    out.reserve(regions.size());
    for (const LayerRegion *region : regions)
        out.push_back(region == nullptr ? -1 : region->region().print_object_region_id());
    return out;
}

bool same_region_set(const LayerRegionSetCPtrs &lhs,
                     const LayerRegionSetCPtrs &rhs,
                     std::string &out_error,
                     const std::string &path)
{
    const std::vector<int> lhs_ids = region_ids(lhs);
    const std::vector<int> rhs_ids = region_ids(rhs);
    return same_value(lhs_ids, rhs_ids, out_error, path, "region set");
}

bool same_region_island(const LayerRegionIsland &lhs,
                        const LayerRegionIsland &rhs,
                        std::string &out_error,
                        const std::string &path)
{
    if (!same_value(lhs.extruder_id(), rhs.extruder_id(), out_error, path, "extruder_id"))
        return false;
    if (!same_region_set(lhs.regions(), rhs.regions(), out_error, path + ".regions"))
        return false;

    // Full polymorphic extrusion equality is intentionally not implemented here
    // yet. This catches structural divergence without pretending that pointer
    // identity inside ExtrusionEntityCollection is meaningful across cloned
    // Print trees.
    static constexpr std::array<ExtrusionRoleModifier, 7> roles = {
        ExtrusionRole::Perimeter,
        ExtrusionRole::GapFill,
        ExtrusionRole::InternalInfill,
        ExtrusionRole::Ironing,
        ExtrusionRole::Milling,
        ExtrusionRole::SupportMaterial,
        ExtrusionRole::SupportMaterialInterface
    };
    for (ExtrusionRoleModifier role : roles) {
        const bool lhs_has = lhs.has_extrusion(role);
        const bool rhs_has = rhs.has_extrusion(role);
        if (!same_value(lhs_has, rhs_has, out_error, path, "extrusion role presence"))
            return false;
        if (lhs_has && !same_value(lhs.extrusion(role).items_count(),
                                   rhs.extrusion(role).items_count(),
                                   out_error,
                                   path,
                                   "extrusion item count"))
            return false;
    }

    return true;
}

bool same_island(const LayerSliceIsland &lhs,
                 const LayerSliceIsland &rhs,
                 std::string &out_error,
                 const std::string &path)
{
    if (!same_value(lhs.get_slice(), rhs.get_slice(), out_error, path, "slice"))
        return false;
    if (!same_value(lhs.get_bounding_box(), rhs.get_bounding_box(), out_error, path, "bounding_box"))
        return false;
    if (!same_region_set(lhs.regions(), rhs.regions(), out_error, path + ".regions"))
        return false;
    if (!same_expolygons(lhs.infill_free_areas(), rhs.infill_free_areas(), out_error, path + ".infill_free_areas"))
        return false;
    if (!same_expolygons(lhs.infill_areas(), rhs.infill_areas(), out_error, path + ".infill_areas"))
        return false;
    if (!same_value(lhs.infill_areas_bboxes(), rhs.infill_areas_bboxes(), out_error, path, "infill_areas_bboxes"))
        return false;
    const ExPolygons &lhs_perimeter_slices = const_cast<LayerSliceIsland &>(lhs).get_perimeter_slices();
    const ExPolygons &rhs_perimeter_slices = const_cast<LayerSliceIsland &>(rhs).get_perimeter_slices();
    if (!same_expolygons(lhs_perimeter_slices, rhs_perimeter_slices, out_error, path + ".perimeter_slices"))
        return false;

    if (lhs.regions_islands().size() != rhs.regions_islands().size()) {
        std::ostringstream msg;
        msg << "region-island count mismatch (lhs=" << lhs.regions_islands().size()
            << ", rhs=" << rhs.regions_islands().size() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }

    for (size_t idx = 0; idx < lhs.regions_islands().size(); ++idx) {
        if (!same_region_island(lhs.regions_island(idx),
                                rhs.regions_island(idx),
                                out_error,
                                path + ".region_island[" + std::to_string(idx) + "]"))
            return false;
    }

    return true;
}

bool same_layer_region(const LayerRegion &lhs,
                       const LayerRegion &rhs,
                       std::string &out_error,
                       const std::string &path)
{
    if (!same_value(lhs.region().print_object_region_id(), rhs.region().print_object_region_id(), out_error, path, "print_object_region_id"))
        return false;
    if (!same_expolygons(lhs.get_raw_slices(), rhs.get_raw_slices(), out_error, path + ".raw_slices"))
        return false;
    if (!same_surface_collection(lhs.slices(), rhs.slices(), out_error, path + ".slices"))
        return false;
    if (!same_expolygons(lhs.fill_no_overlap_expolygons(), rhs.fill_no_overlap_expolygons(), out_error, path + ".fill_no_overlap_expolygons"))
        return false;
    if (!same_surface_collection(lhs.fill_surfaces(), rhs.fill_surfaces(), out_error, path + ".fill_surfaces"))
        return false;
    if (!same_expolygons(lhs.fill_expolygons(), rhs.fill_expolygons(), out_error, path + ".fill_expolygons"))
        return false;
    if (!same_value(lhs.unsupported_bridge_edges(), rhs.unsupported_bridge_edges(), out_error, path, "unsupported_bridge_edges"))
        return false;
    return true;
}

bool same_layer(const Layer &lhs, const Layer &rhs, std::string &out_error, const std::string &path)
{
    if (!same_value(lhs.id(), rhs.id(), out_error, path, "id"))
        return false;
    if (!same_value(lhs.scaled_height(), rhs.scaled_height(), out_error, path, "height"))
        return false;
    if (!same_value(lhs.scaled_print_z(), rhs.scaled_print_z(), out_error, path, "print_z"))
        return false;
    if (!same_value(lhs.slice_z, rhs.slice_z, out_error, path, "slice_z"))
        return false;
    if (!same_expolygons(lhs.lslices(), rhs.lslices(), out_error, path + ".lslices"))
        return false;

    if (lhs.region_count() != rhs.region_count()) {
        std::ostringstream msg;
        msg << "layer region count mismatch (lhs=" << lhs.region_count() << ", rhs=" << rhs.region_count() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }
    for (size_t idx = 0; idx < lhs.region_count(); ++idx) {
        if (!same_layer_region(lhs.region(idx),
                               rhs.region(idx),
                               out_error,
                               path + ".region[" + std::to_string(idx) + "]"))
            return false;
    }

    if (lhs.islands().size() != rhs.islands().size()) {
        std::ostringstream msg;
        msg << "island count mismatch (lhs=" << lhs.islands().size() << ", rhs=" << rhs.islands().size() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }
    for (size_t idx = 0; idx < lhs.islands().size(); ++idx) {
        if (!same_island(lhs.islands()[idx],
                         rhs.islands()[idx],
                         out_error,
                         path + ".island[" + std::to_string(idx) + "]"))
            return false;
    }

    return true;
}

bool same_object(const PrintObject &lhs, const PrintObject &rhs, std::string &out_error, const std::string &path)
{
    if (!same_value(lhs.size(), rhs.size(), out_error, path, "size"))
        return false;
    if (!same_value(lhs.config(), rhs.config(), out_error, path, "config"))
        return false;
    if (!same_value(lhs.layer_profile(), rhs.layer_profile(), out_error, path, "layer_profile"))
        return false;
    if (!same_value(lhs.num_printing_regions(), rhs.num_printing_regions(), out_error, path, "num_printing_regions"))
        return false;

    if (lhs.layer_count() != rhs.layer_count()) {
        std::ostringstream msg;
        msg << "layer count mismatch (lhs=" << lhs.layer_count() << ", rhs=" << rhs.layer_count() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }
    for (size_t idx = 0; idx < lhs.layer_count(); ++idx) {
        if (!same_layer(lhs.layer(idx), rhs.layer(idx), out_error, path + ".layer[" + std::to_string(idx) + "]"))
            return false;
    }

    if (lhs.auxiliary_layer_count() != rhs.auxiliary_layer_count()) {
        std::ostringstream msg;
        msg << "auxiliary layer count mismatch (lhs=" << lhs.auxiliary_layer_count()
            << ", rhs=" << rhs.auxiliary_layer_count() << ")";
        append_path_error(out_error, path, msg.str().c_str());
        return false;
    }
    for (size_t idx = 0; idx < lhs.auxiliary_layer_count(); ++idx) {
        if (!same_layer(lhs.auxiliary_layer(idx), rhs.auxiliary_layer(idx), out_error, path + ".auxiliary_layer[" + std::to_string(idx) + "]"))
            return false;
    }

    return true;
}

} // namespace

DebugPrintProcessComparator::DebugPrintProcessComparator(const Print &source)
{
    DynamicPrintConfig config = source.full_print_config();

    m_reference_print.physical_printer_config() = source.physical_printer_config();
    m_candidate_print.physical_printer_config() = source.physical_printer_config();

    m_reference_print.apply(source.model(), config);
    m_candidate_print.apply(source.model(), std::move(config));
}

bool DebugPrintProcessComparator::compare_tree(std::string &out_error) const
{
    if (!same_value(m_reference_print.config(), m_candidate_print.config(), out_error, "print", "config"))
        return false;
    if (!same_value(m_reference_print.num_print_regions(), m_candidate_print.num_print_regions(), out_error, "print", "num_print_regions"))
        return false;
    if (m_reference_print.objects().size() != m_candidate_print.objects().size()) {
        std::ostringstream msg;
        msg << "object count mismatch (lhs=" << m_reference_print.objects().size()
            << ", rhs=" << m_candidate_print.objects().size() << ")";
        append_path_error(out_error, "print", msg.str().c_str());
        return false;
    }

    for (size_t idx = 0; idx < m_reference_print.objects().size(); ++idx) {
        if (!same_object(m_reference_print.objects()[idx],
                         m_candidate_print.objects()[idx],
                         out_error,
                         "print.object[" + std::to_string(idx) + "]"))
            return false;
    }

    return true;
}

bool DebugPrintProcessComparator::compare_tree_after(const char *label, std::string &out_error) const
{
    std::string detail;
    if (compare_tree(detail))
        return true;

    if (label != nullptr && label[0] != '\0') {
        out_error += label;
        out_error += ": ";
    }
    out_error += detail;
    return false;
}

} // namespace Slic3r::Steps


#endif // _DEBUG
