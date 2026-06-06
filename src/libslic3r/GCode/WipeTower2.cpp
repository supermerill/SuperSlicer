///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ @enricoturri1966
///|/ Copyright (c) Prusa Research 2017 - 2023 Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Enrico Turri
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2019 Thomas Moore
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "WipeTower2.hpp"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PointUtils.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "GCodeProcessor.hpp"
#include "ToolOrdering.hpp"

namespace Slic3r {

coord_t WipeTower2::floatz_tolayer_coord(double z) {
    assert(z < 10000);
    assert(z >= 0);
    // round it via EPSILON/2
    coord_t coord_z = scale_i(z + EPSILON / 2);
    // remove epsilon part
    coord_z /= SCALED_EPSILON;
    coord_z *= SCALED_EPSILON;
    return coord_z;
}

void WipeTower2::set_config(const PrintConfig *config,
                            const PrintObjectConfig *object_config,
                            const PrintRegionConfig *region_config) {
    m_config = config;
    m_object_config = object_config;
    m_region_config = region_config;

    // get position
    m_position = Point::new_scale(m_object_config->wipe_tower_x.value, m_object_config->wipe_tower_y.value);
}

coord_t WipeTower2::width() const { return m_object_config ? scale_i(m_object_config->wipe_tower_width.value) : 0; }
Vec2d WipeTower2::position() const {
    return m_object_config ? Vec2d(m_object_config->wipe_tower_x.value, m_object_config->wipe_tower_y.value) :
                             unscale_p(m_position);
}
coord_t WipeTower2::extra_spacing() const {
    return m_object_config ? scale_i(m_object_config->wipe_tower_extra_spacing.value) : 0;
}
double WipeTower2::rotation_angle() const {
    return m_object_config ? m_object_config->wipe_tower_rotation_angle.value : 0;
}

//
// distf_t WipeTower2::get_max_length(coord_t z) {
//    double dist = 0;
//    // get the LayerData for this z
//    //for (auto &z_to_layer : m_layer_data) {
//    //    if (z < z_to_layer.second.z && z > z_to_layer.second.z - z_to_layer.second.extrusion_height) {
//    //        // for each of them, adppend their worst_needed_length
//    //        dist += worst_needed_length;
//    //    }
//    //}
//    auto it = m_layer_data.find(z);
//    if (it != m_layer_data.end()) {
//        dist = it->second.worst_needed_length_with_fused;
//    }
//    //divide by width
//    dist /= this->width();
//    return dist;
//}

int64_t WipeTower2::ObjectLayerData::compute_layer_key(coord_t z, coord_t height) {
    z *= 100000;
    height /= 100;
    return z + height;
}

std::vector<const Layer *> WipeTower2::WipeTowerLayerData::layers() const {
    std::vector<const Layer *> ret_layers;
    for (const ObjectLayerData *layer_data : fused_with) {
        append(ret_layers, layer_data->my_layers);
    }
    return ret_layers;
}
std::vector<const PrintObject *> WipeTower2::WipeTowerLayerData::objects() const {
    // std::vector<const Layer *> my_layers = this->layers();
    // std::vector<const PrintObject *> my_objects;
    std::set<const PrintObject *> all_objects;
    for (const ObjectLayerData *layers : fused_with) {
        for (const Layer *layer : layers->my_layers) {
            // my_objects.push_back(layer->object());
            all_objects.insert(layer->object());
        }
    }
    // assert(all_objects.size() == my_objects.size());
    std::vector<const PrintObject *> my_objects(all_objects.begin(), all_objects.end());
    return my_objects;
}

void WipeTower2::init(const Print *print, const SpanOfConstPtrs<PrintObject> &objects, ToolOrdering &ordering) {
    const coord_t min_wipe_tower_height = 0.1;
    coord_t max_line_width = 0;
    std::set<uint16_t> init_extruders;
    const ConfigOptionFloatOrPercent &line_width_config = m_object_config->wipe_tower_extrusion_width;

    // create a list of all layers ordered by Z
    std::vector<const Layer *> ordered_layers;
    // for each object
    for (const PrintObject *obj : objects) {
        // for each layer
        for (const Layer &layer : obj->layers()) {
            if (layer.has_extrusions()) { // layer_tools skip empty layers
                ordered_layers.push_back(&layer);
            }
        }
        for (const Layer &layer : obj->auxiliary_layers()) {
            // Support auxiliary layers are still normal Layer objects.
            if (layer.has_extrusions()) { // layer_tools skip empty layers
                ordered_layers.push_back(&layer);
            }
        }
    }
    std::sort(ordered_layers.begin(), ordered_layers.end(),
              [](const Layer *l1, const Layer *l2) { return l1->scaled_print_z() < l2->scaled_print_z(); });
    assert(ordered_layers.size() < 2 || ordered_layers[0]->scaled_print_z() <= ordered_layers[1]->scaled_print_z());

    // check that we have the same as ordering
    std::map<coord_t, std::vector<const Layer *>> ordered_layers_z;
    for (size_t i = 0; i < ordered_layers.size(); i++) {
        ordered_layers_z[ordered_layers[i]->scaled_print_z()].push_back(ordered_layers[i]);
    }
    // layers in ordered_layers can have no extrusion, and so aren't in layer_tools
    assert(ordering.layer_tools().size() == ordered_layers_z.size());
    for (size_t i = 0; i < ordering.layer_tools().size(); i++) {
        const LayerTools &ordering_l = ordering.layer_tools()[i];
        assert(ordered_layers_z.find(ordering_l._print_z) != ordered_layers_z.end());
    }

    // now create layer info
    int16_t last_extruder_id = -1;
    size_t idx_layer = 0;
    for (auto &ordered_layer_z : ordered_layers_z) {
        assert(idx_layer < ordering.layer_tools().size());
        const LayerTools &layer_tools = ordering.layer_tools()[idx_layer];
        assert(ordered_layer_z.first == layer_tools._print_z);
        assert(!ordered_layer_z.second.empty());
        const size_t layer_id = ordered_layer_z.second.front()->id();
        for (const Layer *layer : ordered_layer_z.second) {
            // init new extruders
            for (uint16_t extr_id : layer_tools.extruders) {
                if (init_extruders.find(extr_id) == init_extruders.end()) {
                    init_extruders.insert(extr_id);
                    // compute width
                    const double nozzle_diameter = m_config->nozzle_diameter.get_at(extr_id);
                    coord_t line_width = scale_i(line_width_config.get_effective_value(nozzle_diameter));
                    if (line_width == 0) {
                        line_width = scale_i(nozzle_diameter * 1.25);
                    }

                    // store min/max width
                    if (max_line_width < 0) {
                        max_line_width = (line_width);
                    } else {
                        max_line_width = std::max(max_line_width, (line_width));
                    }
                    if (min_line_width < 0) {
                        min_line_width = (line_width);
                    } else {
                        min_line_width = std::min(min_line_width, (line_width));
                    }

                    // compute filament change data
                    while (m_filament_change_data.size() <= extr_id) {
                        m_filament_change_data.emplace_back();
                    }
                    if (m_filament_change_data[extr_id].tool_id <= size_t(-1)) {
                        FilamentToolchangeInfo &fil = m_filament_change_data[extr_id];
                        fil.tool_id = extr_id;
                        // purge
                        fil.purge_volume = 0;
                        fil.purge_flow_mm3_sec = 0;
                        if (m_config->filament_multitool_ramming.get_at(extr_id)) {
                            fil.purge_flow_mm3_sec = m_config->filament_multitool_ramming_flow.get_at(extr_id);
                            fil.purge_volume = m_config->filament_multitool_ramming_volume.get_at(extr_id);
                        }
                        fil.purge_width = line_width;
                        fil.purge_spacing = m_object_config->wipe_tower_extra_spacing.get_effective_value(fil.purge_width);
                        // wipe
                        fil.wipe_speed = m_config->wipe_tower_speed.value;
                        if (m_config->filament_max_wipe_tower_speed.get_at(extr_id) > 0) {
                            fil.wipe_speed = std::min(fil.wipe_speed,
                                                      m_config->filament_max_wipe_tower_speed.get_at(extr_id));
                        }
                        assert(fil.wipe_speed > 0);
                        fil.wipe_width = line_width;
                        fil.wipe_volume_min = m_config->filament_minimal_purge_on_wipe_tower.get_at(extr_id);
                        fil.wipe_spacing = m_object_config->wipe_tower_extra_spacing.get_effective_value(fil.wipe_width);
                    }
                }
            }

            // check if m_layer_data exists
            coord_t layer_z = layer->scaled_print_z();
            coord_t layer_height = std::max(min_wipe_tower_height, layer->scaled_height());
            coord_t layer_max_bot = std::max(coord_t(0), layer_z - layer_height);
            int64_t layer_key = ObjectLayerData::compute_layer_key(layer_z, layer_height);

            // this layer is already here?
            auto it_layer = m_layer_data.find(layer_key);
            WipeTowerLayerData *wp_layer = nullptr;
            if (it_layer == m_layer_data.end()) {
                // add it
                // find our WTlayer
                auto search_wp_layer = m_printz_to_WTLayer_data.find(layer_z);
                if (search_wp_layer == m_printz_to_WTLayer_data.end()) {
                    // search for compatible layer
                    // as they are aordered by z, it has to be the last one (if okay)
                    if (!m_printz_to_WTLayer_data.empty()) {
                        const coord_t last_wp_layer_z = m_printz_to_WTLayer_data.rbegin()->second->extrusion_z;
                        if (layer_max_bot < last_wp_layer_z && last_wp_layer_z <= layer_z) {
                            wp_layer = m_printz_to_WTLayer_data.rbegin()->second;
                        }
                    }
                    if (wp_layer != nullptr) {
                        // register it for our z
                        m_printz_to_WTLayer_data[layer_z] = wp_layer;
                    }
                } else {
                    // found it
                    wp_layer = search_wp_layer->second;
                }
                if (wp_layer == nullptr) {
                    // create WipeTowerLayerData
                    m_WTLayer_data.emplace_back(new WipeTowerLayerData());
                    wp_layer = m_WTLayer_data.back().get();
                    wp_layer->extrusion_z = layer_z;
                    wp_layer->extrusion_height = layer_height;
                    m_printz_to_WTLayer_data[layer_z] = wp_layer;
                } else {
                    assert(m_printz_to_WTLayer_data.find(layer_z) != m_printz_to_WTLayer_data.end() &&
                           m_printz_to_WTLayer_data.find(layer_z)->second == wp_layer);
                    // fuse
                    assert(wp_layer->extrusion_z <= layer_z);
                    // do we increase layer height? => never
                    // if (wp_layer->extrusion_height < layer_height) {
                    //    // works because ordered_layers is ordered
                    //    wp_layer->extrusion_z += layer_height - wp_layer->extrusion_height;
                    //    wp_layer->extrusion_height = layer_height;
                    //}
                    // add it
                }
                // create LayerData
                std::unique_ptr<ObjectLayerData> &ld = m_layer_data[layer_key];
                assert(!ld);
                ld.reset(new ObjectLayerData());
                ld->real_z = layer_z;
                ld->real_height = layer_height;
                ld->my_layers.push_back(layer);
                wp_layer->fused_with.push_back(ld.get());
                it_layer = m_layer_data.find(layer_key);
            } else {
                // z already here. just add extruders
                assert(m_printz_to_WTLayer_data.find(layer_z) != m_printz_to_WTLayer_data.end());
                wp_layer = m_printz_to_WTLayer_data[layer_z];
                assert(std::find(it_layer->second->my_layers.begin(), it_layer->second->my_layers.end(), layer) ==
                       it_layer->second->my_layers.end());
                it_layer->second->my_layers.push_back(layer);
            }

            // set extruders for the WipeTowerLayerData
            ZLayerData &extruder_data = wp_layer->extruders_data[layer_z];
            extruder_data.real_z = layer_z;
            extruder_data.extruders = layer_tools.extruders;
            if (last_extruder_id >= 0 && last_extruder_id != layer_tools.extruders.front()) {
                // add previous extruder as the first to be changed.
                extruder_data.extruders.insert(extruder_data.extruders.begin(), last_extruder_id);
            }
            last_extruder_id = layer_tools.extruders.back();
        }
        // next item
        idx_layer++;
    }
    // it_layer->extruders.insert(extruders.begin(), extruders.end());

    // check if we have the same as ToolOrdering
    assert(m_printz_to_WTLayer_data.size() <= ordering.layer_tools().size());

    // compute estimated tower length for each layer
    uint16_t previous_tool_id = uint16_t(-1);
    for (auto &entry : m_printz_to_WTLayer_data) {
        WipeTowerLayerData &wp_layer = *entry.second;
        const size_t nb_toolchange = 0;
        coord_t total_wipe_tower_length = 0;
        for (auto &entry : wp_layer.extruders_data) {
            ZLayerData &extruder_data = entry.second;
            extruder_data.estimated_wipe_tower_length = 0;
            for (uint16_t tool_id : extruder_data.extruders) {
                // how many lines we need to reserve?
                if (tool_id != previous_tool_id && previous_tool_id < m_filament_change_data.size()) {
                    // unloading
                    const FilamentToolchangeInfo &fil_info = m_filament_change_data[previous_tool_id];
                    assert(fil_info.tool_id == previous_tool_id);
                    // also purge the nozzle before retracting.
                    distf_t filament_dist = scale_d(
                        fil_info.purge_volume /
                        (unscaled(fil_info.purge_width) * unscaled(wp_layer.extrusion_height)));
                    // count the lines
                    int nb_lines = 1 + filament_dist / (width() - EPSILON);
                    extruder_data.estimated_wipe_tower_length += nb_lines * fil_info.purge_spacing;
                }
                if (tool_id != previous_tool_id) {
                    // loading
                    const FilamentToolchangeInfo &fil_info = m_filament_change_data[tool_id];
                    assert(fil_info.tool_id == tool_id);
                    distf_t filament_dist = scale_d(
                        fil_info.wipe_volume_min /
                        (unscaled(fil_info.wipe_width) * unscaled(wp_layer.extrusion_height)));
                    // count the lines
                    int nb_lines = 1 + filament_dist / (width() - EPSILON);
                    extruder_data.estimated_wipe_tower_length += nb_lines * fil_info.wipe_spacing;
                }
                previous_tool_id = tool_id;
            }
            total_wipe_tower_length += extruder_data.estimated_wipe_tower_length;
        }
        wp_layer.estimated_wipe_tower_length = total_wipe_tower_length;
    }

    // ensure the estimated_wipe_tower_length doesn't shrink
    auto it_previous = m_printz_to_WTLayer_data.rbegin();
    auto it_current = it_previous;
    for (it_current++; it_current != m_printz_to_WTLayer_data.rend(); it_previous = it_current, it_current++) {
        WipeTowerLayerData &wp_layer_prev = *it_previous->second;
        WipeTowerLayerData &wp_layer_curr = *it_current->second;
        wp_layer_curr.estimated_wipe_tower_length = std::max(wp_layer_curr.estimated_wipe_tower_length,
                                                             wp_layer_prev.estimated_wipe_tower_length);
    }
}

std::map<coord_t, std::shared_ptr<WipeTowerLayer>> WipeTower2::create_layers() const {
    std::map<coord_t, std::shared_ptr<WipeTowerLayer>> layers;
    bool ydir_desc = true;
    coord_t max_wipetower_lenth = 0;
    size_t wt_layer_idx = 0;
    // create WipeTowerLayer for each WipeTowerLayerData
    for (auto &ptr : m_WTLayer_data) {
        WipeTowerLayerData &wp_layer = *ptr;
        std::shared_ptr<WipeTowerLayer> wipe_tower_layer = std::make_shared<WipeTowerLayer>(this);
        wipe_tower_layer->wipetower_layer_idx = wt_layer_idx++;
        wipe_tower_layer->y_down = ydir_desc;
        ydir_desc = !ydir_desc;
        wipe_tower_layer->extrusion_z = wp_layer.extrusion_z;
        assert(m_printz_to_WTLayer_data.find(wipe_tower_layer->extrusion_z) != m_printz_to_WTLayer_data.end());
        assert(m_printz_to_WTLayer_data.at(wipe_tower_layer->extrusion_z) == ptr.get());
        wipe_tower_layer->extrusion_height = wp_layer.extrusion_height;
        wipe_tower_layer->m_max_y_pos = wp_layer.estimated_wipe_tower_length;
        max_wipetower_lenth = std::max(max_wipetower_lenth, wp_layer.estimated_wipe_tower_length);
        wipe_tower_layer->m_wipetower_max_y_pos = max_wipetower_lenth;
        wipe_tower_layer->layers = wp_layer.layers();
        for (auto &entry : wp_layer.extruders_data) {
            wipe_tower_layer->uninitialized_z.push_back(entry.first);
        }
        for (ObjectLayerData *layer_data : wp_layer.fused_with) {
            wipe_tower_layer->layers_z.insert(layer_data->real_z);
            assert(layers.find(layer_data->real_z) == layers.end() || layers[layer_data->real_z] == wipe_tower_layer);
            layers[layer_data->real_z] = wipe_tower_layer;
        }
        assert(std::vector<coord_t>(wipe_tower_layer->layers_z.begin(), wipe_tower_layer->layers_z.end()) ==
               wipe_tower_layer->uninitialized_z);
        // m_object_config: if only one object, use its one, or then keep the print's default.
        std::vector<const PrintObject *> objects = wp_layer.objects();
        if (objects.size() == 1) {
            wipe_tower_layer->m_object_config = &objects.front()->config();
        }
    }
    return layers;
}

bool WipeTower2::has_toolchange() const {
    for (auto &WTLD_ptr : m_WTLayer_data) {
        for (auto &z_to_ZLD : WTLD_ptr->extruders_data) {
            if (z_to_ZLD.second.extruders.size() > 1) {
                return true;
            }
        }
    }
    return false;
}

// Returns extrusions to prime the nozzles at the front edge of the print bed.
ExtrusionEntityCollection WipeTower2::prime(
    // Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim
    // and the object.
    const std::vector<uint16_t> &tools,
    // print_z of the first layer, for the flow
    coord_t first_layer_height) {
    const size_t count_extruders = m_config->nozzle_diameter.size();
    // compute flows
    float max_width = 0.f;
    std::vector<Flow> tool_2_flow(count_extruders);
    for (uint16_t tool_id : tools) {
        assert(tool_id < count_extruders);
        // TODO: prime width, currently 200%
        double nz = m_config->nozzle_diameter.get_at(tool_id);
        tool_2_flow[tool_id] = Flow::new_from_width(nz * 2, nz, unscaled(first_layer_height), 1.f, false);
        max_width = std::max(max_width, tool_2_flow[tool_id].width());
    }

    if (max_width == 0) {
        assert(false);
        return {};
    }

    // compute polyline to follow
    Polyline path_around_bed;
    Polygon polygon_bed;
    {
        const std::vector<Vec2d> &bed_points = m_config->bed_shape.get_values();
        // convert to polygon
        assert(bed_points.size() >= 3);
        for (Vec2d pt : bed_points) {
            // Point contructor round the values.
            polygon_bed.points.emplace_back(scale_d(pt.x()), scale_d(pt.y()));
        }
        // TODO: if any "disabled bed area", remove them here.

        // offset the polygon to have enough width to prime.
        const Polygons offseted = offset(polygon_bed, -max_width);
        // take the biggest one
        if (offseted.size() == 0) {
            assert(false);
            return {};
        }
        polygon_bed = offseted.front();
        for (size_t i = 1; i < offseted.size(); ++i) {
            if (offseted[i].area() > polygon_bed.area()) {
                polygon_bed = offseted[i];
            }
        }
    }

    // TODO: compute min purge length: min length for loading, purge, unloading. Currently hardcoded to nozzle * 2,
    // nozzle*4, nozzle*2
    distf_t min_length_mm = 0;
    for (uint16_t tool_id : tools) {
        min_length_mm += scale_d(this->get_loading_volume(tool_id) / tool_2_flow[tool_id].mm3_per_mm());
        min_length_mm += scale_d(this->get_prime_volume(tool_id) / tool_2_flow[tool_id].mm3_per_mm());
        min_length_mm += scale_d(this->get_unloading_volume(tool_id) / tool_2_flow[tool_id].mm3_per_mm());
    }

    BoundingBox bb_bed(polygon_bed.points);
    if (m_config->priming_position.value == Vec2d(0, 0)) {
        path_around_bed = polygon_bed.split_at_first_point();
    } else if (m_config->priming_position.value.y() == 0 &&
               scale_i(m_config->priming_position.value.x()) > bb_bed.min.x() &&
               scale_i(m_config->priming_position.value.x()) < bb_bed.max.x()) {
        // move in x -> find the nearest y in the polyline
        BoundingBox bb_right = bb_bed;
        BoundingBox bb_left = bb_bed;
        bb_right.min.x() = scale_i(m_config->priming_position.value.x());
        bb_left.max.x() = scale_i(m_config->priming_position.value.x());
        Polygon result_right = ClipperUtils::clip_clipper_polygon_with_subject_bbox(polygon_bed, bb_right);
        Polygon result_left = ClipperUtils::clip_clipper_polygon_with_subject_bbox(polygon_bed, bb_left);
        distf_t length_right = result_right.length();
        distf_t length_left = result_left.length();
        if (min_length_mm < std::min(length_left, length_right)) {
            if (length_left > length_right) {
                int idx = result_left.closest_point_index(bb_right.min);
                result_left.make_counter_clockwise();
                path_around_bed = result_left.split_at_index(idx);
            } else {
                int idx = result_right.closest_point_index(bb_right.min);
                result_right.make_clockwise();
                path_around_bed = result_right.split_at_index(idx);
            }
        } else {
            int idx = result_right.closest_point_index(bb_right.min);
            result_right.make_clockwise();
            path_around_bed = result_right.split_at_index(idx);
        }
    } else if (m_config->priming_position.value.x() == 0 &&
               scale_i(m_config->priming_position.value.y()) > bb_bed.min.y() &&
               scale_i(m_config->priming_position.value.y()) < bb_bed.max.y()) {
        // move in x -> find the nearest y in the polyline
        BoundingBox bb_top = bb_bed;
        BoundingBox bb_bot = bb_bed;
        bb_top.min.y() = scale_i(m_config->priming_position.value.y());
        bb_bot.max.y() = scale_i(m_config->priming_position.value.y());
        Polygon result_top = ClipperUtils::clip_clipper_polygon_with_subject_bbox(polygon_bed, bb_top);
        Polygon result_bot = ClipperUtils::clip_clipper_polygon_with_subject_bbox(polygon_bed, bb_bot);
        distf_t length_top = result_top.length();
        distf_t length_bot = result_bot.length();
        if (min_length_mm < std::min(length_bot, length_top)) {
            if (length_top > length_bot) {
                int idx = result_top.closest_point_index(bb_top.min);
                result_top.make_counter_clockwise();
                path_around_bed = result_top.split_at_index(idx);
            } else {
                int idx = result_bot.closest_point_index(bb_top.min);
                result_bot.make_clockwise();
                path_around_bed = result_bot.split_at_index(idx);
            }
        } else {
            int idx = result_bot.closest_point_index(bb_top.min);
            result_bot.make_clockwise();
            path_around_bed = result_bot.split_at_index(idx);
        }
    } else if (m_config->priming_position.value.x() != 0 && m_config->priming_position.value.y() != 0) {
        // find nearest point
        BoundingBox bb_clip = bb_bed;
        bb_clip.min = Point::new_scale(m_config->priming_position.value.x(), m_config->priming_position.value.y());
        Polygon result_clip = ClipperUtils::clip_clipper_polygon_with_subject_bbox(polygon_bed, bb_clip);
        result_clip.make_counter_clockwise();
        int idx = result_clip.closest_point_index(bb_clip.min);
        int idx_before = (idx + result_clip.size() - 1) % result_clip.size();
        int idx_after = (idx + 1) % result_clip.size();
        // use longest side
        if (result_clip.points[idx].distance_to_square(result_clip.points[idx_before]) >
            result_clip.points[idx].distance_to_square(result_clip.points[idx_after])) {
            result_clip.make_clockwise();
        }
        path_around_bed = result_clip.split_at_index(result_clip.closest_point_index(bb_clip.min));
    } else {
        path_around_bed = polygon_bed.split_at_first_point();
    }
    assert(!path_around_bed.empty());

    // check path
    const distf_t max_length_mm = path_around_bed.length();
    const double ratio = std::min(max_length_mm, min_length_mm) / min_length_mm;

    // now we can create the extrusions
    ExtrusionEntityCollection tool_extrusions;
    // already done at the start of unload
    // tool_extrusions.append(ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::SAVE_AND_RESET_SPEED_RATIO,
    // 1.)));
    tool_extrusions.set_can_sort_reverse(false, false);
    tool_extrusions.append(
        ExtrusionNop(ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::COMMENT, "CP PRIMING START")));
    Point start_point = path_around_bed.front();
    size_t next_point_idx = 1;
    for (uint16_t tool_id : tools) {
        // move to this tool
        tool_extrusions.append(ExtrusionNop(
            ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::TOOLCHANGE, tool_id)));

        ExtrusionMultiPath multipaths;
        assert(multipaths.empty());
        const double mm3_per_mm = tool_2_flow[tool_id].mm3_per_mm();
        // load
        distf_t dist_load = scale_d(this->get_loading_volume(tool_id) / mm3_per_mm);
        // prime
        distf_t dist_prime = scale_d(this->get_prime_volume(tool_id) / mm3_per_mm);
        // unload
        distf_t dist_unload = scale_d(this->get_unloading_volume(tool_id) / mm3_per_mm);
        distf_t total_dist = dist_load + dist_prime + dist_unload;
        // apply ratio
        if (ratio < 1) {
            dist_prime = ratio * total_dist - dist_load - dist_unload;
            if (dist_prime < 0) {
                double ratio2 = (dist_load + dist_unload + dist_prime) / (dist_load + dist_unload);
                dist_load *= ratio2;
                dist_unload *= ratio2;
            }
        }
        // speed
        ExtrusionPropertySpeed speed_property;
        speed_property.speed(this->get_loading_speed(tool_id))
            .acceleration(this->get_first_layer_acceleration(tool_id))
            .presure_advance(this->get_first_layer_pa(tool_id))
            .fan_speed(this->get_first_layer_fan_speed(tool_id));

        assert(path_around_bed.length() > dist_load);
        // create loading path
        Polyline priming_start = path_around_bed.split_at(dist_load);
        multipaths.paths().emplace_back(ArcPolyline(path_around_bed),
                                      ExtrusionAttributes{ExtrusionRole::WipeTower, tool_2_flow[tool_id]}, nullptr,
                                      false);
        multipaths.paths().back().add_property(
            ExtrusionPropertySpeed(this->get_loading_speed(tool_id), this->get_first_layer_acceleration(tool_id),
                                   this->get_first_layer_pa(tool_id), this->get_first_layer_fan_speed(tool_id)));

        // create priming path
        Polyline unloading_start;
        if (dist_prime > SCALED_EPSILON) {
            unloading_start = priming_start.split_at(dist_prime);
            multipaths.paths().emplace_back(ArcPolyline(priming_start),
                                          ExtrusionAttributes{ExtrusionRole::WipeTower, tool_2_flow[tool_id]},
                                          nullptr, false);
            multipaths.paths().back().add_property(
                ExtrusionPropertySpeed(this->get_prime_speed(tool_id), this->get_first_layer_acceleration(tool_id),
                                       this->get_first_layer_pa(tool_id), this->get_first_layer_fan_speed(tool_id)));
        } else {
            unloading_start = std::move(priming_start);
        }

        assert(unloading_start.length() >= dist_unload);
        // create unloading path
        // (give unsued part to next tool -> path_around_bed)
        path_around_bed = unloading_start.split_at(dist_unload);
        multipaths.paths().emplace_back(ArcPolyline(unloading_start),
                                      ExtrusionAttributes{ExtrusionRole::WipeTower, tool_2_flow[tool_id]}, nullptr,
                                      false);
        multipaths.paths().back().add_property(
            ExtrusionPropertySpeed(this->get_unloading_speed(tool_id), this->get_first_layer_acceleration(tool_id),
                                   this->get_first_layer_pa(tool_id), this->get_first_layer_fan_speed(tool_id)));
        tool_extrusions.append(std::move(multipaths));
    }
    tool_extrusions.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::RESTORE_SPEED_RATIO)));
    tool_extrusions.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::FLUSH_PLANNER_QUEUE)));
    tool_extrusions.append(
        ExtrusionNop(ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::COMMENT, "CP PRIMING END")));

    return tool_extrusions;
}

coord_t WipeTowerLayer::compute_y(coord_t raw_y) {
    coord_t delta = (m_wipetower_max_y_pos - m_max_y_pos) / 2;
    if ((int(unscaled(this->extrusion_z)) % 2) == 0) {
        return delta + raw_y;
    } else {
        return delta + m_max_y_pos - raw_y;
    }
}

void WipeTowerLayer::init(const std::vector<const Layer *> layers,
                          const std::vector<uint16_t> &ordered_extruders,
                          const std::vector<uint16_t> &purges) {
    // create Toolchange & Purge
    assert(m_wipe_tower_info);
    assert(m_wipe_tower_info->m_printz_to_WTLayer_data.find(extrusion_z) !=
           m_wipe_tower_info->m_printz_to_WTLayer_data.end());
    const WipeTower2::WipeTowerLayerData &data = *m_wipe_tower_info->m_printz_to_WTLayer_data.at(extrusion_z);

    assert(!layers.empty());
    assert(!ordered_extruders.empty());
    auto it = std::find(this->uninitialized_z.begin(), this->uninitialized_z.end(), layers.front()->scaled_print_z());
    if (it == this->uninitialized_z.end()) {
        assert(this->uninitialized_z.empty()); //weird thing with islands
        return;
    }
    // m_is_init = true;
    assert(!this->uninitialized_z.empty());
    assert(this->uninitialized_z.front() == layers.front()->scaled_print_z());
    const coord_t objects_print_z = *it;
    initialized_z.push_back(objects_print_z);
    this->uninitialized_z.erase(it);

    if (data.estimated_wipe_tower_length <= 0) {
        assert(ordered_extruders.size() < 2);
        // wipe tower ended
        brim_done = true;
        perimeter_done = true;
        m_is_finished = true;
        return;
    }

    // check our layers are inside
    const std::vector<const Layer *> all_layers = data.layers();
    for (const Layer *layer : layers) {
        assert(std::find(all_layers.begin(), all_layers.end(), layer) != all_layers.end());
    }

    // ensure we have enough space
    assert(m_current_y_pos < data.estimated_wipe_tower_length);

    const coord_t wipe_tower_width = m_wipe_tower_info->width();
    const Point wipe_tower_pos(0, 0); // = Point::new_scale(m_wipe_tower_info->position());
    const Point wipe_tower_left_pos(wipe_tower_pos.x(),
                                    wipe_tower_pos.y() +
                                        std::max(compute_y(0), compute_y(data.estimated_wipe_tower_length)));
    const Point wipe_tower_right_pos(wipe_tower_pos.x() + wipe_tower_width,
                                     wipe_tower_pos.y() +
                                         std::max(compute_y(0), compute_y(data.estimated_wipe_tower_length)));
    const Point wipe_tower_left_bot_pos(wipe_tower_pos.x(),
                                        wipe_tower_pos.y() +
                                            std::min(compute_y(0), compute_y(data.estimated_wipe_tower_length)));
    const Point wipe_tower_right_bot_pos(wipe_tower_pos.x() + wipe_tower_width,
                                         wipe_tower_pos.y() +
                                             std::min(compute_y(0), compute_y(data.estimated_wipe_tower_length)));

    // create toolchanges
    for (size_t i = 1; i < ordered_extruders.size(); i++) {
        std::tuple<coord_t, uint16_t, uint16_t> key(layers.front()->scaled_print_z(), ordered_extruders[i - 1],
                                                    ordered_extruders[i]);
        assert(m_toolchanges.find(key) == m_toolchanges.end());
        Toolchange &toolchange = m_toolchanges[key];
        toolchange.layer = layers.front();
        toolchange.from_tool_id = ordered_extruders[i - 1];
        toolchange.to_tool_id = ordered_extruders[i];
        // assert(layers.size() == 1);
        const WipeTower2::FilamentToolchangeInfo &fil_info_prev =
            m_wipe_tower_info->m_filament_change_data[toolchange.from_tool_id];
        const WipeTower2::FilamentToolchangeInfo &fil_info_next = m_wipe_tower_info
                                                                      ->m_filament_change_data[toolchange.to_tool_id];
        // create polylines
        // purge
        if (fil_info_prev.purge_volume > 0) {
            toolchange.purge_flow = Flow::new_from_width(unscaled(fil_info_prev.purge_width),
                                                         m_config->nozzle_diameter.get_at(toolchange.from_tool_id),
                                                         unscaled(extrusion_height), 1.f, false);
            distf_t dist_purge = scale_d(fil_info_prev.purge_volume /
                                         (unscaled(fil_info_prev.purge_width) * unscaled(extrusion_height)));
            int nblines_purge = ((dist_purge - 1) / wipe_tower_width) + 1;
            for (size_t iline = 0; iline < nblines_purge; iline++) {
                m_current_y_pos += fil_info_prev.purge_spacing / 2;
                if (iline % 2 == 0) {
                    toolchange.purge_lines.points.push_back(
                        Point(wipe_tower_left_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                    toolchange.purge_lines.points.push_back(
                        Point(wipe_tower_right_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                } else {
                    toolchange.purge_lines.points.push_back(
                        Point(wipe_tower_right_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                    toolchange.purge_lines.points.push_back(
                        Point(wipe_tower_left_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                }
                m_current_y_pos += fil_info_prev.purge_spacing / 2;
            }
        }
        // wipe
        if (fil_info_next.wipe_volume_min > 0) {
            toolchange.wipe_flow = Flow::new_from_width(unscaled(fil_info_next.wipe_width),
                                                        m_config->nozzle_diameter.get_at(toolchange.to_tool_id),
                                                        unscaled(extrusion_height), 1.f, false);
            Polyline polyline_wipe;
            distf_t dist_wipe = scale_d(fil_info_next.wipe_volume_min /
                                        (unscaled(fil_info_next.wipe_width) * unscaled(extrusion_height)));
            int nblines_wipe = ((dist_wipe - 1) / wipe_tower_width) + 1;
            for (size_t iline = 0; iline < nblines_wipe; iline++) {
                m_current_y_pos += fil_info_next.wipe_spacing / 2;
                if (iline % 2 == 0) {
                    toolchange.wipe_lines.points.push_back(
                        Point(wipe_tower_left_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                    toolchange.wipe_lines.points.push_back(
                        Point(wipe_tower_right_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                } else {
                    toolchange.wipe_lines.points.push_back(
                        Point(wipe_tower_right_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                    toolchange.wipe_lines.points.push_back(
                        Point(wipe_tower_left_pos.x(), compute_y(wipe_tower_pos.y() + m_current_y_pos)));
                }
                m_current_y_pos += fil_info_next.wipe_spacing / 2;
            }
            if (wipetower_layer_idx % 2 == 1) {
                toolchange.wipe_lines.reverse();
            }
        }
    }

    // TODO create purges

    // create perimeter (from first extruder encountered)
    if (tower_perimeters.empty()) {
        perimeter_tool_idx = ordered_extruders.front();
        const WipeTower2::FilamentToolchangeInfo &fil_info_next =
            m_wipe_tower_info->m_filament_change_data[ordered_extruders.front()];
        tower_perimeter_flow = Flow::new_from_width(unscaled(fil_info_next.wipe_width),
                                                    m_config->nozzle_diameter.get_at(ordered_extruders.front()),
                                                    unscaled(extrusion_height), 1.f, false);
        const auto [R, support_scale] =
            m_wipe_tower_info->get_wipe_tower_cone_base(tower_perimeter_flow.width(), tower_perimeter_flow.height(),
                                                        unscaled(data.estimated_wipe_tower_length),
                                                        m_object_config->wipe_tower_cone_angle.value);

        coord_t wipe_tower_max_z = m_wipe_tower_info->m_printz_to_WTLayer_data.rbegin()->second->extrusion_z;
        // double r = std::tan(Geometry::deg2rad(m_wipe_tower_cone_angle/2.f)) * (m_wipe_tower_height - z);
        double rayon = std::tan(Geometry::deg2rad(m_object_config->wipe_tower_cone_angle.value / 2.f)) *
            unscaled(wipe_tower_max_z - extrusion_z);
        // Vec2f center = (wt_box.lu + wt_box.rd) / 2.;
        Vec2f center(unscaled(wipe_tower_width / 2), unscaled(m_wipetower_max_y_pos / 2));

        // First generate vector of annotated point which form the boundary.
        Polygon perimeter;
        perimeter.points.push_back(wipe_tower_left_pos);
        double length_mm = unscaled(m_max_y_pos);
        double alpha_start = std::asin((0.5 * length_mm) / rayon);
        if (!std::isnan(alpha_start) && rayon > 0.5 * length_mm + 0.01) {
            for (double alpha = alpha_start; alpha < M_PI - alpha_start + 0.001;
                 alpha += (M_PI - 2 * alpha_start) / 40.) {
                perimeter.points.push_back(Point::new_scale(center.x() - rayon * std::cos(alpha) / support_scale,
                                                            center.y() + rayon * std::sin(alpha)));
            }
        }
        perimeter.points.push_back(wipe_tower_right_pos);
        perimeter.points.push_back(wipe_tower_right_bot_pos);
        for (int i = int(perimeter.points.size()) - 3; i > 0; --i) {
            perimeter.points.emplace_back(perimeter.points[i].x(), m_wipetower_max_y_pos - perimeter.points[i].y());
        }
        perimeter.points.push_back(wipe_tower_left_bot_pos);
        perimeter.reverse(); // built as CW, nede to fix it to CCW.
        assert(perimeter.is_counter_clockwise());
        Polygons big_rectangle = offset(perimeter, (tower_perimeter_flow.scaled_width()) / 2);
        assert(!big_rectangle.empty());
        perimeter = big_rectangle.front();
        tower_perimeters.push_back(perimeter.split_at_first_point());

        // brim (first layer only)
        if (extrusion_z == extrusion_height) {
            // same as print::brimflow()
            PrintRegionConfig brim_region_config = *m_wipe_tower_info->m_region_config;
            brim_region_config.parent = m_object_config;
            double nozzle_diameter = m_config->nozzle_diameter.get_at(ordered_extruders.front());
            brim_flow = Flow::new_from_config_width(frPerimeter,
                                                    *Flow::extrusion_width_option("brim", brim_region_config),
                                                    *Flow::extrusion_spacing_option("brim", brim_region_config),
                                                    (float) nozzle_diameter, (float) unscaled(extrusion_height),
                                                    (ordered_extruders.front() < m_config->nozzle_diameter.size()) ?
                                                        m_object_config->get_computed_value("filament_max_overlap",
                                                                                            ordered_extruders.front()) :
                                                        1);
            const double spacing = brim_flow.spacing();
            // create wt brim only if settings allow it
            if (spacing > 0 && m_object_config->wipe_tower_brim_width.value > 0) {
                // How many perimeters shall the brim have?
                size_t loops_num = (m_object_config->wipe_tower_brim_width.get_effective_value(nozzle_diameter) +
                                    spacing / 2) /
                    spacing;

                // create
                for (size_t i = 0; i < loops_num; i++) {
                    Polygons polys = offset(perimeter, scale_d(spacing));
                    assert(polys.size() == 1);
                    perimeter = polys.front();
                    brim.push_back(perimeter.split_at_first_point());
                }
                std::reverse(brim.begin(), brim.end());
            }
        }

        //// infill the cone (on first layer)
        //if (extrusion_z == extrusion_height) {
        //    coord_t spacing = tower_perimeter_flow.scaled_spacing();
        //    ExPolygons infill_areas;
        //    ExPolygon wt_contour(tower_perimeters.back().points);
        //    ExPolygon wt_rectangle(
        //        Points{wipe_tower_right_pos, wipe_tower_right_bot_pos, wipe_tower_left_bot_pos, wipe_tower_left_pos});
        //    ExPolygons wt_rectangle_check = offset_ex(wt_rectangle, -spacing / 2.);
        //    ExPolygons wt_contour_check = offset_ex(wt_contour, -spacing / 2.);
        //    if (!wt_rectangle_check.empty() && !wt_contour_check.empty()) {
        //        wt_rectangle = wt_rectangle_check.front();
        //        wt_contour = wt_contour_check.front();
        //        infill_areas = diff_ex(wt_contour, wt_rectangle);
        //        if (infill_areas.size() == 2 || infill_areas.size() == 1) {
        //            ExPolygon &bottom_expoly = infill_areas.size() == 1 ? infill_areas[0] :
        //                infill_areas.front().contour.points.front().y() <
        //                    infill_areas.back().contour.points.front().y() ?
        //                                                                  infill_areas[0] :
        //                                                                  infill_areas[1];
        //            std::unique_ptr<Fill> filler(Fill::new_from_type(ipMonotonicLines));
        //            filler->angle = Geometry::deg2rad(45.f);
        //            FillParams params;
        //            params.role = ExtrusionRole::WipeTower;
        //            params.density = 1.f;
        //            Surface surface(stPosBottom | stDensSolid, bottom_expoly);
        //            filler->bounding_box = get_extents(bottom_expoly);
        //            filler->init_spacing(tower_perimeter_flow.spacing(), params);
        //            Polylines polylines = filler->fill_surface(&surface, params);
        //            if (!polylines.empty()) {
        //                if (polylines.front().points.front().x() > polylines.back().points.back().x()) {
        //                    std::reverse(polylines.begin(), polylines.end());
        //                    for (Polyline &p : polylines)
        //                        p.reverse();
        //                }
        //            }
        //            append(tower_perimeters, polylines);
        //        }
        //    }
        //}
    }
}

ExtrusionEntityCollection WipeTowerLayer::tool_change(const Layer *layer,
                                                      uint16_t old_tool,
                                                      uint16_t new_tool,
                                                      double de_retraction_new_tool) {
    assert(layer);
    assert(!initialized_z.empty());
    assert(initialized_z.back() == layer->scaled_print_z());
    assert(!m_is_finished || old_tool == new_tool);
    if (m_is_finished) {
        return {};
    }
    ExtrusionEntityCollection collection;
    collection.set_can_sort_reverse(false, false);
    assert(!collection.can_sort());
    assert(this->extrusion_z <= layer->scaled_print_z());
    bool need_move_into_wp = false;
    if (this->extrusion_z < layer->scaled_print_z()) {
        // extrude at the wipetower's z
        collection.add_property(ExtrusionPropertyZOffset(this->extrusion_z - layer->scaled_print_z()));
        need_move_into_wp = true;
    }

    if (old_tool == perimeter_tool_idx) {
        print_perimeter(collection, true);
    }

    if (old_tool != new_tool) {
        // find the toolchange
        std::tuple<coord_t, uint16_t, uint16_t> key(layer->scaled_print_z(), old_tool, new_tool);
        assert(m_toolchanges.find(key) != m_toolchanges.end());
        if (auto it = m_toolchanges.find(key); it != m_toolchanges.end()) {
            const Toolchange &toolchange = it->second;
            assert(layer == toolchange.layer);
            // unload can be made in-place. if a move is made, it's only in the wipetower and so the travel will take
            // care of the z offset
            bool moved_into_wp = toolchange_Unload(collection, toolchange.purge_lines, toolchange.from_tool_id,
                                                   toolchange.purge_flow);
            if (m_object_config->wipe_tower_rest_in_middle.value || (need_move_into_wp && !moved_into_wp)) // force move in middle to ooze inside the wp.
            {
                // move to center before toolchange, just in case it ooze
                const Point center_pos(m_wipe_tower_info->width() / 2, compute_y(m_current_y_pos));
                ExtrusionNop travel = ExtrusionNop();
                travel.position = center_pos;
                travel.set_role(ExtrusionRole::Travel);
                travel.add_property(ExtrusionPropertyModifier().set_toolchange_retraction().set_disable_lift());
                travel.add_property(ExtrusionPropertyCustomGcodeText("; inside wipe tower before toolchange"));
                collection.append(std::move(travel));
            } else {
                // travel a bit outside so the ooze won't do a mess in our wipetower.
                double nozzle_diameter_mm = m_config->nozzle_diameter.get_at(old_tool);
                coord_t brim_width = scale_i(m_object_config->wipe_tower_brim_width.get_effective_value(nozzle_diameter_mm));
                const Point center_pos(scale_i(-1) - brim_width / 2, compute_y(m_current_y_pos));
                ExtrusionNop travel = ExtrusionNop();
                travel.position = center_pos;
                travel.set_role(ExtrusionRole::Travel);
                travel.add_property(ExtrusionPropertyModifier().set_toolchange_retraction().set_disable_lift());
                travel.add_property(ExtrusionPropertyCustomGcodeText("; inside wipe tower before toolchange"));
                collection.append(std::move(travel));
            }
            toolchange_Change(collection, new_tool);
            toolchange_load(collection, toolchange.wipe_lines, toolchange.to_tool_id);
            toolchange_Wipe(collection, toolchange.wipe_lines, toolchange.wipe_flow, toolchange.to_tool_id,
                            de_retraction_new_tool);
        }
    }
    if (new_tool == perimeter_tool_idx) {
        print_perimeter(collection);
    }

    finish_layer(collection, new_tool);
    return collection;
}

Flow WipeTower2::get_ramming_flow(const uint16_t tool_id, const bool first_layer, const double layer_height) {
    double nozzle_diameter = m_config->nozzle_diameter.get_at(tool_id);
    double perimeter_width = m_object_config->wipe_tower_extrusion_width.get_effective_value(nozzle_diameter);
    // FIXME use PrintObject function to have first layer flow.
    if (first_layer && m_object_config->first_layer_extrusion_width.is_enabled()) {
        perimeter_width = std::max(perimeter_width,
                                   m_object_config->first_layer_extrusion_width.get_effective_value(nozzle_diameter));
    }
    // ramming_line_width_multiplicator comes from the ramming widget tool that is parsed afterwards
    // //TODO
    // perimeter_width *= m_filpar[m_current_tool].ramming_line_width_multiplicator; // desired ramming line thickness
    return Flow::new_from_width(perimeter_width, nozzle_diameter, layer_height,
                                m_region_config->perimeter_overlap.get_effective_value(1), false);
}

void wait_for_temp_with_fan(ExtrusionEntityCollection &coll, bool enable_fan, int fan_speed) {
    if (enable_fan) {
        ExtrusionEntityCollection wait_for_temp(false, false);
        // enable special fan for the temp wait.
        ExtrusionPropertySpeed &prop_fan = (ExtrusionPropertySpeed &) wait_for_temp.add_property(
            ExtrusionPropertySpeed());
        prop_fan.fan_speed(fan_speed);
        wait_for_temp.append(
            ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP)));
        coll.append(std::move(wait_for_temp));
    } else {
        coll.append(
            ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP)));
    }
}

Polyline load_move_x_advanced(
    ExtrusionEntityCollection &coll, Polyline polyline, float de, float e_speed, float max_x_speed = 100.f) {
    float time = std::abs(de / e_speed);  // time that the move must take
    float x_distance = polyline.length(); // max x-distance that we can travel
    float x_speed = x_distance / time;    // x-speed to do it in that time

    Polyline unused;
    if (x_speed > max_x_speed) {
        // Necessary x_speed is too high - we must shorten the distance to achieve max_x_speed and still respect the time.
        unused = polyline.split_at(x_speed * time);
        x_speed = max_x_speed;
    }
    ExtrusionPath extrusion_path(ArcPolyline(polyline),
                                 ExtrusionAttributes{ExtrusionRole::WipeTower, ExtrusionFlow{0, 0, 0}}, nullptr,
                                 false);
    extrusion_path.add_property(ExtrusionPropertySpeed(x_speed));
    // use ExtrusionPropertySpecialCommand::EXTRUSION instead of ExtrusionFlow to be sure i have the exact amount of
    // mm of extruder
    extrusion_path.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION, de));
    coll.append(extrusion_path);
    return unused;
}

void WipeTowerLayer::toolchange_load(ExtrusionEntityCollection &collection,
                                     const Polyline &loading_lines,
                                     const uint16_t tool_id) {
    ExtrusionEntityCollection load_collection(false, false);
    Polyline unused = loading_lines;
    const distf_t load_dist = loading_lines.length();
    collection.append(ExtrusionNop(ExtrusionPropertyCustomGcodeText(";CP TOOLCHANGE LOAD")));
    if (m_config->single_extruder_multi_material.value &&
        (m_config->parking_pos_retraction.value != 0 || m_config->extra_loading_move.value != 0)) {
        // Increase the extruder driver current to allow fast ramming.
        if (m_config->high_current_on_filament_swap.value) {
            load_collection.append(
                ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUDER_CURRENT,
                                                             750))); // TODO setting
        }

        // Load the filament while moving left / right, so the excess material will not create a blob at a single position.
        float edist = m_config->parking_pos_retraction + m_config->extra_loading_move;
        ExtrusionNop disable_preview{ExtrusionPropertySpecialCommand(
            ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW)};
        load_collection.append(std::move(disable_preview));

        ExtrusionNop extrusion = ExtrusionNop();
        extrusion.add_property(ExtrusionPropertySpeed(m_config->filament_loading_speed_start.get_at(tool_id)));
        extrusion.add_property(
            ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION, 0.2f * edist));
        load_collection.append(extrusion);
        // Fast phase
        unused = load_move_x_advanced(load_collection, unused, 0.7f * edist,
                                      m_config->filament_loading_speed.get_at(tool_id));
        // Super slow
        unused.append(reverse_polyline(loading_lines));
        unused = load_move_x_advanced(load_collection, unused, 0.1f * edist,
                                      0.1f * m_config->filament_loading_speed.get_at(tool_id));

        load_collection.append(
            ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::ENABLE_PREVIEW)));
        // Reset the extruder current to the normal value.
        if (m_config->high_current_on_filament_swap) {
            load_collection.append(
                ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUDER_CURRENT,
                                                             550))); // TODO setting
        }
    }

    collection.append(std::move(load_collection));
    collection.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::RESTORE_SPEED_RATIO)));
    collection.append(ExtrusionNop(ExtrusionPropertyCustomGcodeText("; CP TOOLCHANGE LOAD END\n")));
}

// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
bool WipeTowerLayer::toolchange_Unload(ExtrusionEntityCollection &collection,
                                       const Polyline &ramming_lines,
                                       const uint16_t tool_id,
                                       const Flow &ramming_flow) {
    assert(!collection.can_sort() && !collection.can_reverse());
    // cleaning_lines needs to be far apart enough so my flow doesn't create any issues. Caller's problem
    bool has_moved_into_wipetower = false;

    ExtrusionEntityCollection unload_collection(false, false);
    assert(!unload_collection.can_sort() && !unload_collection.can_reverse());
    // no pressure advance
    ExtrusionPropertySpeed &prop_unload = (ExtrusionPropertySpeed &) unload_collection.add_property(
        ExtrusionPropertySpeed());
    prop_unload.presure_advance(0);

    collection.append(ExtrusionNop(ExtrusionPropertyCustomGcodeText("; CP TOOLCHANGE UNLOAD\n")));
    collection.append(ExtrusionNop(
        ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::SAVE_AND_RESET_SPEED_RATIO, 1.)));
    Polyline lines_for_wipe = ramming_lines;
    // change_analyzer_line_width -> done by writer on the fly
    if (ramming_flow.width() != 0 && !ramming_lines.empty()) {
        // can set speed/pa via property or via role.
        ExtrusionPath ramming_path(ExtrusionAttributes(ExtrusionRole(ExtrusionRole::WipeTowerRamming), ramming_flow),
                                   nullptr);
        // the ramming distance should be exactly cleaning_lines length
        ramming_path.polyline() = ramming_lines;
        unload_collection.append(std::move(ramming_path));
        lines_for_wipe = reverse_polyline(ramming_lines);
        has_moved_into_wipetower = true;
    }

    // ask for forced retraction
    if (m_config->single_extruder_multi_material.value &&
        (m_config->cooling_tube_retraction.value != 0 || m_config->cooling_tube_length.value != 0)) {
        ExtrusionEntityCollection coll_retractions(false, false);
        coll_retractions.append(
            ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW)));
        // set toolchange temperature just prior to filament being extracted from melt zone and wait for set point
        //(SKINNYDIP--normal mode only)
        // TODO:m_filpar
        // if ((m_filpar[tool_id].filament_enable_toolchange_temp == true) &&
        //    (m_filpar[tool_id].filament_use_fast_skinnydip == false)) {
        //    ExtrusionPropertySpeed &prop_temp = coll_retractions.add_property(ExtrusionPropertySpeed());
        //    prop_temp.temperature(m_config->filament_toolchange_part_fan_speed.get_at(tool_id));
        //    wait_for_temp_with_fan(coll_retractions, m_config->filament_enable_toolchange_part_fan.get_at(tool_id),
        //                           m_config->filament_toolchange_part_fan_speed.get_at(tool_id));
        //}

        // disable preview, that retraction shouldn't be disaplyed or it will confuse users.
        // don't ask for a real retraction, do our custom one
        float total_retraction_distance = m_config->cooling_tube_retraction.value +
            m_config->cooling_tube_length.value / 2.f - 15.f; // the 15mm is reserved for the first part after ramming
        // 1st retraction: 15 mm
        ExtrusionNop retraction;
        // TODO
        // retraction.add_property(ExtrusionPropertySpeed(m_filpar[tool_id].unloading_speed_start));
        retraction.add_property(
            ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION, -15.f));
        coll_retractions.append(std::move(retraction));

        // 2nd retraction: 0.70f * total_retraction_distance
        retraction = ExtrusionNop();
        // TODO
        // retraction.add_property(ExtrusionPropertySpeed(m_filpar[tool_id].unloading_speed));
        retraction.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                                -0.70f * total_retraction_distance));
        coll_retractions.append(std::move(retraction));

        // 3rd retraction: 0.20f * total_retraction_distance
        retraction = ExtrusionNop();
        // TODO
        // retraction.add_property(ExtrusionPropertySpeed(0.5f * m_filpar[tool_id].unloading_speed));
        retraction.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                                -0.20f * total_retraction_distance));
        coll_retractions.append(std::move(retraction));

        // 4th retraction: 0.10f * total_retraction_distance
        retraction = ExtrusionNop();
        // TODO
        // retraction.add_property(ExtrusionPropertySpeed(0.3f * m_filpar[tool_id].unloading_speed));
        retraction.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                                -0.10f * total_retraction_distance));
        coll_retractions.append(std::move(retraction));
        // writer.suppress_preview()
        //     .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f) // feedrate 5000mm/min = 83mm/s
        //     .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
        //     .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
        //     .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
        //     .resume_preview();

        // reenable preview
        coll_retractions.append(
            ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::ENABLE_PREVIEW)));
        unload_collection.append(std::move(coll_retractions));
        // out of the coll_retractions, temperature will go back to normal
    }

    if (m_config->single_extruder_multi_material.value) {
        ExtrusionEntityCollection cooling_moves(false, false);
        ExtrusionPropertySpeed &prop_temp = (ExtrusionPropertySpeed &) cooling_moves.add_property(
            ExtrusionPropertySpeed());

        // Wipe tower should only change temperature with single extruder MM. Otherwise, all temperatures should
        // be already set and there is no need to change anything. Also, the temperature could be changed
        // for wrong extruder.
        // additionally, we are suppressing this temperature change if skinnydip fast mode is active because it will
        // happen later
        // if no toolchange temperatures are being used, just set the temperature of the next material.
        // TODO: m_filpar
        // if (m_config->single_extruder_multi_material.value && (m_filpar[tool_id].filament_enable_toolchange_temp ==
        // false)) {
        //    if (new_temperature != 0) { // Set the extruder temperature, but don't wait.
        //        prop_temp.temperature(m_config->filament_toolchange_part_fan_speed.get_at(tool_id));
        //    }
        //}
        // otherwise, if toolchange temperature changes are on and in normal mode, return to the previously set temperature

        // Cooling:
        // begin to cool extruder to toolchange temperature during cooling moves (only if using skinnydip fast mode)
        // TODO: m_filpar
        // if (m_filpar[tool_id].filament_enable_toolchange_temp == true) {
        //    if (m_filpar[tool_id].filament_use_fast_skinnydip == true) {
        //        prop_temp.temperature(m_config->filament_toolchange_temp.get_at(tool_id));
        //    }
        //} else
        {
            int new_temp = 0;
            // TODO: m_filpar
            // first_layer ? m_filpar[tool_id].first_layer_temperature :
            // m_filpar[tool_id].temperature;
            if (new_temp > 0) {
                prop_temp.temperature(new_temp);
            }
        }

        // Generate Cooling Moves
        const int number_of_moves = m_config->filament_cooling_moves.get_at(tool_id);
        if (number_of_moves > 0) {
            float initial_speed = float(m_config->filament_cooling_initial_speed.get_at(tool_id));
            float final_speed = float(m_config->filament_cooling_final_speed.get_at(tool_id));

            float speed_inc = (final_speed - initial_speed) / (2.f * number_of_moves - 1.f);

            cooling_moves.append(ExtrusionNop(
                ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW)));
            const distf_t max_length = lines_for_wipe.length();
            for (int i = 0; i < number_of_moves; ++i) {
                if (lines_for_wipe.length() < max_length) {
                    if (ramming_lines.front() == lines_for_wipe.back()) {
                        lines_for_wipe.append(ramming_lines);
                    } else {
                        lines_for_wipe.append(reverse_polyline(ramming_lines));
                    }
                }
                float speed = initial_speed + speed_inc * 2 * i;

                lines_for_wipe = load_move_x_advanced(cooling_moves, lines_for_wipe,
                                                      m_config->cooling_tube_length.value, speed);
                speed += speed_inc;
                lines_for_wipe = load_move_x_advanced(cooling_moves, lines_for_wipe,
                                                      -m_config->cooling_tube_length.value, speed);
            }
        }
        // BEGIN SKINNYDIP SECTION
        // wait for extruder to reach toolchange temperature after cooling moves complete (SKINNYDIP--fast mode only)
        if ((m_config->filament_enable_toolchange_temp.get_at(tool_id)) &&
            (m_config->filament_use_fast_skinnydip.get_at(tool_id))) {
            wait_for_temp_with_fan(cooling_moves, m_config->filament_enable_toolchange_part_fan.get_at(tool_id),
                                   m_config->filament_toolchange_part_fan_speed.get_at(tool_id));
        }

        // Generate a skinnydip move
        if (m_config->filament_use_skinnydip.get_at(tool_id)) {
            cooling_moves.append(ExtrusionNop(
                ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW)));

            ExtrusionNop extrusion = ExtrusionNop();
            extrusion.add_property(ExtrusionPropertySpeed(m_config->filament_dip_insertion_speed.get_at(tool_id)));
            extrusion.add_property(
                ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                m_config->filament_skinnydip_distance.get_at(tool_id)));
            extrusion.add_property(
                ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::COMMENT, "SKINNYDIP START"));
            cooling_moves.append(extrusion);

            ExtrusionNop pause = ExtrusionNop();
            pause.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::PAUSE,
                                                               m_config->filament_melt_zone_pause.get_at(tool_id)));
            cooling_moves.append(pause);

            ExtrusionNop retraction = ExtrusionNop();
            retraction.add_property(ExtrusionPropertySpeed(m_config->filament_dip_extraction_speed.get_at(tool_id)));
            retraction.add_property(
                ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                -m_config->filament_skinnydip_distance.get_at(tool_id)));
            cooling_moves.append(retraction);

            pause = ExtrusionNop();
            extrusion.add_property(
                ExtrusionPropertyCustomGcodeText(ExtrusionPropertyCustomGcodeText::Code::COMMENT, "SKINNYDIP END"));
            pause.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::PAUSE,
                                                               m_config->filament_cooling_zone_pause.get_at(tool_id)));
            cooling_moves.append(pause);

            cooling_moves.append(
                ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::ENABLE_PREVIEW)));
        }
        // restore temp

        // let's wait if necessary:
        ExtrusionNop pause = ExtrusionNop();
        pause.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::PAUSE,
                                                           m_config->filament_toolchange_delay.get_at(tool_id)));
        cooling_moves.append(pause);
        // we should be at the beginning of the cooling tube again - let's move to parking position:
        ExtrusionNop retraction = ExtrusionNop();
        retraction.add_property(ExtrusionPropertySpeed(33)); // speed=33mm/s => f=2000 mm/min
        retraction.add_property(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::EXTRUSION,
                                                                (-m_config->cooling_tube_length.value / 2.f +
                                                                 m_config->parking_pos_retraction.value -
                                                                 m_config->cooling_tube_retraction.value)));
        cooling_moves.append(retraction);

        unload_collection.append(std::move(cooling_moves));
    }

    // done at end of load
    //  unload_collection.append(ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::RESTORE_SPEED_RATIO)));
    unload_collection.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::FLUSH_PLANNER_QUEUE)));

    collection.append(std::move(unload_collection));
    collection.append(ExtrusionNop(ExtrusionPropertyCustomGcodeText("; CP TOOLCHANGE UNLOAD END\n")));
    return has_moved_into_wipetower;
}

// Wipe the newly loaded filament until the end of the assigned wipe area.
// TODO: if first layer, you may want to increase the flow a little bit (by 18% by default)
void WipeTowerLayer::toolchange_Wipe(ExtrusionEntityCollection &collection,
                                     Polyline wipe_lines,
                                     const Flow wipe_flow,
                                     const uint16_t tool_id,
                                     const double de_retraction_new_tool) {
    if (wipe_lines.size() < 2)
        return;

    // unretraction as wipe
    if (m_config->retract_length_toolchange.get_at(tool_id) &&
        m_config->retract_restart_wipe_toolchange.get_at(tool_id) > 0 && de_retraction_new_tool > 0) {
        // get speed & length
        const double wipe_e_length_mm =  m_config->retract_restart_wipe_toolchange.get_effective_value(de_retraction_new_tool, tool_id);
        //double unretract_speed_e_mm_per_s = m_config->deretract_speed.get_at(tool_id);
        //if (unretract_speed_e_mm_per_s == 0) {
        //    unretract_speed_e_mm_per_s = m_config->retract_speed.get_at(tool_id);
        //}
        //assert(unretract_speed_e_mm_per_s > 0);
        //double time_s = wipe_e_length_mm / unretract_speed_e_mm_per_s;
        //double xy_speed_mm_s = m_config->wipe_speed.get_at(tool_id);
        // if (xy_speed_mm_s == 0) {
        //     xy_speed_mm_s = m_config->travel_speed.value;
        // }
        //assert(xy_speed_mm_s > 0);
        //double dist_xy_mm = xy_speed_mm_s * time_s;



        // compute mm3
        double diam = m_config->filament_diameter.get_at(tool_id);
        double area = diam * diam * PI/4;
        double mm3_unretraction = wipe_e_length_mm * area;
        double mm_needed = mm3_unretraction / wipe_flow.mm3_per_mm();

        bool reverse = false;
        Polyline &base_lines = wipe_lines;
        //Polyline base_lines;
        //{
        //    std::unique_ptr<Fill> filler;
        //    Polygon wt_contour(tower_perimeters.back().points);
        //    assert(wt_contour.is_counter_clockwise());
        //    wt_contour.make_counter_clockwise();
        //    // we used the center line, so we need to go a bit away from it.
        //    ExPolygon offset_expoly = offset_ex(ExPolygon(wt_contour), -wipe_flow.scaled_spacing() / 3).front();
        //    assert(offset_expoly.is_counter_clockwise());
        //    Surface surface(stPosBottom | stDensSolid, ExPolygon(offset_expoly));
        //    filler.reset(Fill::new_from_type(ipRectilinear));
        //    filler->angle = Geometry::deg2rad(90.f);
        //    FillParams params;
        //    params.role = ExtrusionRole::WipeTower;
        //    params.flow = infill_flow;
        //    params.density = .25f;
        //    surface = Surface(stPosInternal | stDensSparse, ExPolygon(offset_expoly));
        //    filler->bounding_box = get_extents(surface.expolygon);
        //    filler->init_spacing(infill_flow.spacing(), params);
        //    Polylines lines = filler->fill_surface(&surface, params);
        //    while (lines.size() > 1) {
        //        if (lines[0].length() < lines[1].length()) {
        //            lines.erase(lines.begin());
        //        } else {
        //            lines.erase(lines.begin() + 1);
        //        }
        //    }
        //    if (lines.empty()) {
        //        base_lines = wipe_lines;
        //        assert(false);
        //    }
        //}
        Polyline base_lines_reversed = base_lines;
        base_lines_reversed.reverse();
        const double max_length_mm = unscaled(base_lines.length());
        double wipe_times = mm_needed / max_length_mm;
        Polyline unretract_lines;
        while (wipe_times > 1) {
            unretract_lines.append(reverse ? base_lines_reversed : base_lines);
            reverse = !reverse;
            wipe_times -= 1.;
        }
        // use base_lines_reversed as clipped end
        if (!reverse) {
            base_lines_reversed.reverse();
        }
        base_lines_reversed.clip_end(max_length_mm * (1-mm_needed));
        unretract_lines.append(base_lines_reversed);
        //if (max_length_mm > dist_xy_mm) {
        //    unretract_lines.clip_end(scale_d(max_length_mm - dist_xy_mm));
        //} else {
        //    dist_xy_mm = max_length_mm;
        //    dist_xy_mm = max_length_mm / time_s;
        //}
        unretract_lines.reverse();
        // double e_per_mm3 = m_config->extrusion_multiplier.get_at(tool_id);
        // double filament_diameter = m_config->filament_diameter.get_at(tool_id);
        // if (!m_config->use_volumetric_e)
        //     e_per_mm3 /= filament_diameter * filament_diameter * 0.25 * PI;
        // double e_per_mm = e_per_mm3 * wipe_flow.mm3_per_mm();
        //  height = -1 to force the mm3_per_mm to e_per_mm3
        ExtrusionFlow extrusion_flow(wipe_e_length_mm / unscaled(unretract_lines.length())/*dist_xy_mm*/, wipe_flow.width(), -2);
        // extrusion_flow.force_e_per_mm = true;
        ExtrusionAttributes extr_flow_attr(ExtrusionRole::WipeTowerWipe, std::move(extrusion_flow));
        ExtrusionPath path_unretract(unretract_lines, extr_flow_attr, nullptr, false);
        //path_unretract.add_property(ExtrusionPropertySpeed(dist_xy_mm));
        path_unretract.add_property(ExtrusionPropertyModifier().set_disable_retraction().set_disable_lift());
        path_unretract.add_property(ExtrusionPropertyCustomGcodeText("; unretract new tool via wipe"));
        if (m_config->retract_restart_wipe_toolchange.get_effective_value(1., tool_id) < 1.) {
            const double pre_uwipe_unretract_e = de_retraction_new_tool *
                (1. - m_config->retract_restart_wipe_toolchange.get_effective_value(1., tool_id));
            // extrusion_flow.force_e_per_mm = true;
            //ExtrusionAttributes travel_flow_attr(ExtrusionRole::Travel, ExtrusionFlow(0,0,0));
            //ExtrusionPath path_travel(Polyline{Points{unretract_lines.front()}}, travel_flow_attr, nullptr, false);
            //path_travel.add_property(ExtrusionPropertyModifier().set_disable_retraction());
            //path_travel.add_property(ExtrusionPropertyCustomGcodeText("; unretraction spot"));
            //collection.append(std::move(path_travel));
            int speed = int(floor(m_config->deretract_speed.get_at(tool_id) + 0.5));
            if (speed <= 0) {
                speed = int(floor(m_config->retract_speed.get_at(tool_id) + 0.5));
            }
            ExtrusionNop path_move = ExtrusionNop();
            if (m_config->retract_restart_toolchange_on_perimeter.get_at(tool_id)) {
                assert(total_length(tower_perimeters) > 0);
                coord_t dist = wipetower_layer_idx * scale_i(5);
                dist = dist % coord_t(total_length(tower_perimeters));
                for (size_t i = 0; i < tower_perimeters.size(); i++) {
                    if (dist >= coord_t(tower_perimeters[i].length())) {
                        dist -= coord_t(tower_perimeters[i].length());
                    } else if(dist > SCALED_EPSILON * 10) {
                        Polyline temp = tower_perimeters[i];
                        temp.split_at(distf_t(dist));
                        path_move.position = temp.back();
                        break;
                    } else {
                        path_move.position = tower_perimeters[i].front();
                        break;
                    }
                }
                assert(path_move.position != ExtrusionNop::NOT_A_POINT);
                if (path_move.position != ExtrusionNop::NOT_A_POINT) {
                    // shouldn't happen
                    path_move.position = tower_perimeters.front().front();
                }
            } else {
                path_move.position = unretract_lines.front();
            }
            path_move.set_role(ExtrusionRole::Travel);
            path_move.add_property(ExtrusionPropertyModifier().set_disable_retraction());
            collection.append(std::move(path_move));
            //assert(((ExtrusionNop*)collection.entities().back())->first_point() == unretract_lines.front());
            ExtrusionNop path_pre_unretract = ExtrusionNop();
            path_pre_unretract.set_role(ExtrusionRole::WipeTowerWipe);
            path_pre_unretract.add_property(ExtrusionPropertySpeed(speed));
            if (m_config->retract_restart_toolchange_on_perimeter.get_at(tool_id)) {
                path_pre_unretract.add_property(ExtrusionPropertyModifier().set_disable_lift().set_enforce_unlift());
            } else {
                path_pre_unretract.add_property(ExtrusionPropertyModifier().set_enforce_unlift());
            }
            path_pre_unretract.add_property(ExtrusionPropertyCustomGcodeText("; unretract new tool (static part)"));
            path_pre_unretract.add_property(
                ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::RETRACT,pre_uwipe_unretract_e));
            collection.append(std::move(path_pre_unretract));
            assert(((ExtrusionNop*)collection.entities().back())->role() == ExtrusionRole::WipeTowerWipe);
        }
        collection.append(std::move(path_unretract));
    }

    //.append("; CP TOOLCHANGE WIPE\n");
    float speed_factor = 1.f;

    // Speed override for the material. Go slow for flex and soluble materials.
    // speed_factor *= get_speed_reduction(); //TODO

    // wipe until the end of the assigned area.
    ExtrusionAttributes extr_flow_attr(ExtrusionRole::WipeTowerWipe,
                                       ExtrusionFlow{wipe_flow.mm3_per_mm(), wipe_flow.width(), wipe_flow.height()});

    // if there is less than 2.5*m_perimeter_width to the edge, advance straightaway (there is likely a blob anyway)
    if (wipe_lines.size() > 2 && wipe_lines.front().distance_to(wipe_lines[1]) < 2.5f * wipe_flow.width()) {
        wipe_lines.points.erase(wipe_lines.points.begin());
    }
    // now the wiping itself:
    ExtrusionPath path(wipe_lines, extr_flow_attr, nullptr, false);
    path.add_property(ExtrusionPropertyModifier().set_disable_retraction().set_disable_lift());
    collection.append(std::move(path));

    // We may be going back to the model - wipe the nozzle. If this is followed
    // by finish_layer, this wipe path will be overwritten.
    // writer.add_wipe_point(writer.x(), writer.y())
    //      .add_wipe_point(writer.x(), writer.y() - dy)
    //      .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y() - dy);

    // if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
    //     m_left_to_right = !m_left_to_right;
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTowerLayer::toolchange_Change(ExtrusionEntityCollection &collection, const uint16_t new_tool) {

    //if (true) {
    //    collection.append(
    //        ExtrusionNop(ExtrusionPropertyModifier().set_enforce_retraction()));
    //    const Point wipe_tower_pos(0, 0); // = Point::new_scale(m_wipe_tower_info->position());
    //    const Point wipe_tower_center(wipe_tower_pos.x() + m_wipe_tower_info->width() / 2,
    //                                  wipe_tower_pos.y() + m_max_y_pos / 2);
    //    ExtrusionAttributes travel_flow_attr(ExtrusionRole::Travel,
    //                                   ExtrusionFlow{0,0,0});
    //    collection.append(ExtrusionPath(ArcPolyline(Polyline{last_point, wipe_tower_center}),
    //                                    travel_flow_attr, nullptr, true));
    //}
    // todo in the writer: change the tool, ensure z is good. (for xy, it doesn't matter, it will travel somewhere)
    collection.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::TOOLCHANGE, new_tool)));
    collection.append(
        ExtrusionNop(ExtrusionPropertySpecialCommand(ExtrusionPropertySpecialCommand::Code::FLUSH_PLANNER_QUEUE)));
}

// note:
//  get_speed_reduction -> in writer, for wipetowerwipe speed

bool WipeTowerLayer::print_perimeter(ExtrusionEntityCollection &collection, bool for_toolchange) {
    if (!brim_done && !brim.empty()) {
        ExtrusionAttributes extr_flow_attr(ExtrusionRole::Skirt,
                                           ExtrusionFlow{tower_perimeter_flow.mm3_per_mm(),
                                                         tower_perimeter_flow.width(), tower_perimeter_flow.height()});
        ExtrusionEntityCollection brim_coll;
        brim_coll.set_can_sort_reverse(false, false);
        for (size_t i = 1; i < brim.size(); i++) {
            if (brim[i].front() == brim[i].back()) {
                ExtrusionLoop loop;
                loop.paths().emplace_back(ArcPolyline(brim[i]), extr_flow_attr, nullptr, true);
                brim_coll.append(std::move(loop));
            } else {
                brim_coll.append(ExtrusionPath(ArcPolyline(brim[i]), extr_flow_attr, nullptr, true));
            }
        }
        collection.append(std::move(brim_coll));
        brim_done = true;
    }
    if (perimeter_done || tower_perimeters.empty())
        return false;

    ExtrusionAttributes extr_flow_attr(ExtrusionRole::WipeTower,
                                       ExtrusionFlow{tower_perimeter_flow.mm3_per_mm(), tower_perimeter_flow.width(),
                                                     tower_perimeter_flow.height()});
    assert(tower_perimeters.front() == tower_perimeters.back());
    for (size_t i = 0; i < tower_perimeters.size(); i++) {
        if (tower_perimeters[i].front() == tower_perimeters[i].back()) {
            ExtrusionLoop loop;
            loop.paths().emplace_back(ArcPolyline(tower_perimeters[i]), extr_flow_attr, nullptr, true);
            collection.append(std::move(loop));
        } else {
            collection.append(ExtrusionPath(ArcPolyline(tower_perimeters[i]), extr_flow_attr, nullptr, true));
        }
    }
    last_point = tower_perimeters.back().back();
    perimeter_done = true;
    return true;
}

bool WipeTowerLayer::finish_layer(ExtrusionEntityCollection &collection, uint16_t current_extruder, bool force) {
    // check if last z to wipe
    if (!uninitialized_z.empty()) {
        return false;
    }

    // check if all toolchange & purge done
    if (!force) {
        bool is_finished = true;
        for (const auto &entry : m_toolchanges) {
            if (!entry.second.done) {
                is_finished = false;
                break;
            }
        }
        for (const Purge &purge : m_purges) {
            if (!purge.done) {
                is_finished = false;
                break;
            }
        }
        if (!is_finished) {
            return false;
        }
    }

    if (m_is_finished) {
        return false;
    }
    m_is_finished = true;

    uint16_t old_tool = current_extruder;

    const WipeTower2::FilamentToolchangeInfo &fil_info = m_wipe_tower_info->m_filament_change_data[current_extruder];
    Flow infill_flow = Flow::new_from_width(unscaled(fil_info.wipe_width),
                                            m_config->nozzle_diameter.get_at(current_extruder),
                                            unscaled(extrusion_height), 1.f, false);

    // just in case
    bool smthg_printed = print_perimeter(collection);
    ExtrusionAttributes extr_flow_attr(ExtrusionRole::WipeTower,
                                       ExtrusionFlow{infill_flow.mm3_per_mm(), infill_flow.width(),
                                                     infill_flow.height()});

    // infill the space that wasn't used for purge.

    // Slow down on the 1st layer.
    bool first_layer = extrusion_z == extrusion_height;
    float speed_factor = 60.f;
    float print_speed = m_object_config->support_material_speed;
    if (first_layer && m_object_config->first_layer_speed > 0)
        print_speed = m_object_config->first_layer_speed;
    if (print_speed <= 0)
        print_speed = m_wipe_tower_info->m_region_config->get_computed_value("perimeter_speed", current_extruder);
    if (print_speed <= 0) {
        assert(false);
        print_speed = 60.f;
    }

    // if nothing to fill
    if (m_current_y_pos + infill_flow.scaled_width() >= m_max_y_pos) {
        return smthg_printed;
    }

    const coord_t wipe_tower_width = m_wipe_tower_info->width();
    const Point wipe_tower_left_pos(0, std::max(compute_y(0), compute_y(m_current_y_pos)));
    const Point wipe_tower_right_pos(wipe_tower_width, std::max(compute_y(0), compute_y(m_current_y_pos)));
    const Point wipe_tower_left_bot_pos(0, std::min(compute_y(0), compute_y(m_current_y_pos)));
    const Point wipe_tower_right_bot_pos(wipe_tower_width,
                                         std::min(compute_y(0), compute_y(m_current_y_pos)));
    std::unique_ptr<Fill> filler;
    FillParams params;
    params.role = ExtrusionRole::WipeTower;
    params.flow = infill_flow;
    Polygon wt_used(
        Points{wipe_tower_right_pos, wipe_tower_left_pos, wipe_tower_left_bot_pos, wipe_tower_right_bot_pos});
    Polygon wt_contour(tower_perimeters.back().points);
    // BoundingBox bbox = get_extents(wt_contour.points);
    // bbox.offset(scale_(1.));
    // static int iii=0;
    //::Slic3r::SVG svg(debug_out_path("%d_before_overahngs_%d.svg", extrusion_z, ++iii).c_str(), bbox);
    ////svg.draw(to_polylines( diff_ex(ExPolygon(wt_contour), wt_used)), "green", scale_i(0.05));
    ////svg.draw(to_polylines(offset_ex(diff_ex(ExPolygon(wt_contour), wt_used), -infill_flow.scaled_spacing() / 3)),
    ///"teal", scale_i(0.02));
    // svg.draw(( diff_ex(ExPolygon(wt_contour), wt_used)), "green");
    // svg.draw((offset_ex(diff_ex(ExPolygon(wt_contour), wt_used), -infill_flow.scaled_spacing() / 3)), "teal");
    // svg.draw(to_polylines(wt_contour), "red", scale_i(0.09));
    // svg.draw(to_polylines(wt_used), "cyan", scale_i(0.07));
    // svg.Close();
    assert(wt_contour.is_counter_clockwise());
    wt_contour.make_counter_clockwise();
    for (ExPolygon &offset_expoly :
         diff_ex(offset_ex(wt_contour, -infill_flow.scaled_spacing() / 3), Polygons{wt_used})) {
        // we used the center line, so we need to go a bit away from it.
        //ExPolygon offset_expoly = offset_ex(expoly, ).front();
        Surface surface(stPosBottom | stDensSolid, ExPolygon(offset_expoly));
        if (extrusion_z == extrusion_height) {
            // infill
            filler.reset(Fill::new_from_type(ipMonotonicLines));
            filler->angle = Geometry::deg2rad(45.f);
            params.density = 1.f;
        } else {
            // sparse infill
            // TODO bridge flow if enough void below
            filler.reset(Fill::new_from_type(ipRectilinear));
            filler->angle = Geometry::deg2rad(45.f);
            params.density = .1f;
            surface = Surface(stPosInternal | stDensSparse, ExPolygon(offset_expoly));
        }
        filler->bounding_box = get_extents(surface.expolygon);
        filler->init_spacing(infill_flow.spacing(), params);
        filler->fill_surface_extrusion(&surface, params, collection);
    }
    return true;
}

// Static method to get the radius and x-scaling of the stabilizing cone base.
std::pair<double, double> WipeTower2::get_wipe_tower_cone_base(double width,
                                                               double height,
                                                               double depth,
                                                               double angle_deg) {
    double R = std::tan(Geometry::deg2rad(angle_deg / 2.)) * height;
    double fake_width = 0.66 * width;
    double diag = std::hypot(fake_width / 2., depth / 2.);
    double support_scale = 1.;
    if (R > diag) {
        double w = fake_width;
        double sin = 0.5 * depth / diag;
        double tan = depth / w;
        double t = (R - diag) * sin;
        support_scale = (w / 2. + t / tan + t * tan) / (w / 2.);
    }
    return std::make_pair(R, support_scale);
}

// Static method to extract wipe_volumes[from][to] from the configuration.
std::vector<std::vector<float>> WipeTower2::extract_wipe_volumes(const ConfigBase &config) {
    // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
    std::vector<float> wiping_matrix(
        cast<float>(config.option<ConfigOptionFloats>("wiping_volumes_matrix")->get_values()));

    // The values shall only be used when SEMM is enabled. The purging for other printers
    // is determined by filament_minimal_purge_on_wipe_tower.
    if (!config.option("single_extruder_multi_material")->get_bool())
        std::fill(wiping_matrix.begin(), wiping_matrix.end(), 0.f);

    // Extract purging volumes for each extruder pair:
    std::vector<std::vector<float>> wipe_volumes;
    const unsigned int number_of_extruders = (unsigned int) (sqrt(wiping_matrix.size()) + EPSILON);
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(wiping_matrix.begin() + i * number_of_extruders,
                                                  wiping_matrix.begin() + (i + 1) * number_of_extruders));

    // Also include filament_minimal_purge_on_wipe_tower. This is needed for the preview.
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        for (unsigned int j = 0; j < number_of_extruders; ++j)
            wipe_volumes[i][j] = std::max<float>(wipe_volumes[i][j],
                                                 config.option("filament_minimal_purge_on_wipe_tower")->get_float(j));

    return wipe_volumes;
}

} // namespace Slic3r
