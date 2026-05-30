///|/ Copyright (c) SuperSlicer 2018 - 2025 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/
///|/ ported from lib/Slic3r/Layer.pm:
///|/ Copyright (c) Prusa Research 2016 - 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ SuperSlicer, PrusaSlicer, Slic3r are released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Layer_hpp_
#define slic3r_Layer_hpp_

#include "BoundingBox.hpp"
#include "DataTreeFwd.hpp"
#include "LayerRegion.hpp"
#include "libslic3r.h"
#include "Line.hpp"

namespace Slic3r {

class ExPolygon;
using ExPolygons = std::vector<ExPolygon>;
namespace ApiInternal { struct LayerAccess; }
namespace ApiInternal { struct LayerIslandAccess; }
namespace ApiInternal { struct LayerRegionAccess; }
namespace Steps { class StepPipeline; }


namespace FillAdaptive {
    struct Octree;
}

namespace FillLightning {
    class Generator;
};

// kind of similar as old's LayerSlice
class LayerSliceIsland : public PluginPropertyContainer
{
public:
    // only filled when Layer's LayerSliceIsland are locked.
    // used by supportspotgenerator (badly)
    struct Link
    {
        LayerSliceIsland* to;
        float area;
    };
    std::vector<Link> overlaps_above;
    std::vector<Link> overlaps_below;

    const ExPolygons &get_perimeter_slices() const { return m_perimeter_slices; }

protected:
    friend struct ApiInternal::LayerIslandAccess;

    ExPolygon m_slice;
    BoundingBox m_bbox;
    // only regions that are relevant for this island
    LayerRegionSetCPtrs m_regions;
    // storing unique_ptr because it's easier to have consistent objects while manipulating the vector.
    LayerRegionIslandUPtrs m_extrusions;
    // cache, can be accessed via m_regions. Only set after fill_regions.
    Layer *m_layer = nullptr;

    // Unspecified fill polygons, used for intersecting when we don't want the infill/perimeter encroaching
    // note: if empty, that means there is no overlap, so you don't need to intersect with it.
    ExPolygons                  m_infill_free_areas;
    ExPolygons                  m_infill_areas;
    BoundingBoxes               m_infill_areas_bboxes;
    // to get the boundary in avoid_crossing_perimeters. Filled by make_perimeters()
    ExPolygons                  m_perimeter_slices;

public:

    LayerSliceIsland(const ExPolygon &slice);
    void fill_regions(Layer& layer);

    const ExPolygon &get_slice() const { return m_slice; }
    const BoundingBox &get_bounding_box() const { return m_bbox; }
    const LayerRegionSetCPtrs &regions() const { return m_regions; }
    LayerRegionIslandCRefs regions_islands() const { return make_ref_view<LayerRegionIsland>(m_extrusions); }
    LayerRegionIslandRefs regions_islands() { return make_ref_view<LayerRegionIsland>(m_extrusions); }
    const LayerRegionIsland& regions_island(size_t idx) const { return *m_extrusions[idx]; }
    LayerRegionIsland& regions_island(size_t idx) { return *m_extrusions[idx]; }
    LayerRegionIslandUPtrs &mutable_regions_islands() { return m_extrusions; }
    const Layer *layer() const { return m_layer; }

    LayerRegionIsland& get_or_add_region_island(const LayerRegionSetCPtrs &regions, uint16_t extruder_id = uint16_t(-1));
    LayerRegionIsland& add_region_island(const LayerRegionSetCPtrs &regions, uint16_t extruder_id = uint16_t(-1));

        // Unspecified fill polygons, used for overhang detection ("ensure vertical wall thickness feature")
    // and for re-starting of infills.
    [[nodiscard]] const ExPolygons&                 infill_areas() const { return m_infill_areas; }
    // and their bounding boxes
    [[nodiscard]] const BoundingBoxes&              infill_areas_bboxes() const { return m_infill_areas_bboxes; }

    // return true if this expolygon is (inside) this island.
    // TODO remove when the fill surfaces will be linked to their islands (maybe moved here)
    bool is_expolygons_from_region(const ExPolygon &expolygon) const;

    // Is there any valid extrusion assigned to one LayerRegionIslandPtr?
    bool has_extrusions() const;

    //// Unspecified fill polygons, used for intersecting when we don't want the infill/perimeter overlap
    //// note: if empty, that means there is no overlap, so you don't need to intersect with it.
    [[nodiscard]] const ExPolygons&                 infill_free_areas() const { return m_infill_free_areas; }

    void make_perimeters(LayerRegionIsland &region_island);
};
//static constexpr const size_t LayerIslandsStaticSize = 1;
//using LayerIslands =
//#ifdef NDEBUG
//    // To reduce memory allocation in release mode.
//    #include <boost/container/small_vector.hpp>
//    boost::container::small_vector<LayerIsland, LayerIslandsStaticSize>;
//#else // NDEBUG
//    // To ease debugging.
//    std::vector<LayerIsland>;
//#endif // NDEBUG

class Layer : public PluginPropertyContainer
{
    coord_t             m_height;        // layer height
    coord_t             m_print_z;       // Z used for printing
public:
    // Sequential index of this layer in PrintObject::m_layers.
    size_t              id() const          { return m_id; }
    void                set_id(size_t id)   { m_id = id; }
    PrintObject*        object()            { return m_object; }
    const PrintObject*  object() const      { return m_object; }

    Layer              *upper_layer;
    Layer              *lower_layer;
//    bool                slicing_errors;
    // heights
    double              slice_z;       // Z used for slicing, in unscaled coordinates
    coord_t             scaled_print_z() const { assert(scale_to_layer_coord(unscaled(m_print_z)) == m_print_z); return m_print_z; }
    double              unscaled_print_z() const { assert(scale_to_layer_coord(unscaled(m_print_z)) == m_print_z); return unscaled(m_print_z); }
    coord_t             scaled_height() const { return m_height; }
    double              unscaled_height() const { return unscaled(m_height); }
    coord_t             scaled_bottom_z() const { return this->m_print_z - this->m_height; }


    //Extrusions estimated to be seriously malformed, estimated during "Estimating curled extrusions" step. These lines should be avoided during fast travels.
    //TODO: put in island
    CurledLines         curled_lines;

protected:
    // geometric island.
    // Collection of expolygons generated by slicing the possibly multiple meshes of the source geometry 
    // (with possibly differing extruder ID and slicing parameters) and merged.
    // For the first layer, if the Elephant foot compensation is applied, this lslice is uncompensated, therefore
    // it includes the Elephant foot effect, thus it corresponds to the shape of the printed 1st layer.
    // These lslices are also used to detect overhangs and overlaps between successive layers, therefore it is important
    // that the lslice is not compensated by the Elephant foot compensation algorithm.
    ExPolygons              m_lslices; // now in LayerSliceIsland, here is just a cache for quicker lslices()
    // in unique_ptr to be sure the address doesn't change when updating the vector.
    LayerSliceIslandUPtrs m_islands;
    bool m_islands_locked = false;

public:
    // shortcut to get slices stored in islands
    const ExPolygons &      lslices() const { return m_lslices; }

    LayerSliceIslandCRefs islands() const { return make_ref_view<LayerSliceIsland>(m_islands); }
    LayerSliceIslandRefs  islands() { return make_ref_view<LayerSliceIsland>(m_islands); }
    const LayerSliceIsland& island(size_t idx) const { return *m_islands[idx]; }
    LayerSliceIsland&     island(size_t idx) { return *m_islands[idx]; }
    //LayerSliceIslandUPtrs &mutable_islands() { return m_islands; }
    //LayerSliceIslandUPtr& mutable_island(size_t idx) { return *m_islands[idx]; }
    // to be called after LayerRegion's m_slices are created (but still not separated)
    // create surfaces in layerregion, and fill regionislands.
    void add_regions_to_islands();

    size_t                  region_count() const { return m_regions.size(); }
    const LayerRegion&      region(size_t idx) const { return *m_regions[idx]; }
    LayerRegion&            region(size_t idx) { return *m_regions[idx]; }
    const LayerRegionCRefs  regions() const { return make_ref_view<LayerRegion>(m_regions); }
    LayerRegionRefs         regions() { return make_ref_view<LayerRegion>(m_regions); }
    // Test whether whether there are any slices assigned to this layer.
    bool                    empty() const;
    // After creating the slices on all layers, chain the islands overlapping in Z.
    static void             build_up_down_graph(Layer &below, Layer &above);
    // erase LayerRegion's 'slices' surfaces and recreate one from raw
    void                    restore_untyped_slices();
    // Slices merged into islands, to be used by the elephant foot compensation to trim the individual surfaces with the shrunk merged slices.
    ExPolygons              merged(coordf_t offset_scaled = 0) const;
    void                    make_perimeters();
    void                    make_milling_post_process();
    void                    make_fills(FillAdaptive::Octree     *adaptive_fill_octree,
                                       FillAdaptive::Octree     *support_fill_octree,
                                       FillLightning::Generator *lightning_generator);
    void                    make_ironing();
    Polylines               generate_sparse_infill_polylines_for_anchoring(FillAdaptive::Octree *adaptive_fill_octree,
                                                                           FillAdaptive::Octree *support_fill_octree,
                                                                           FillLightning::Generator* lightning_generator) const;


protected:
    void _make_fills(LayerSliceIsland &island,
                     FillAdaptive::Octree *adaptive_fill_octree,
                     FillAdaptive::Octree *support_fill_octree,
                     FillLightning::Generator *lightning_generator);
    void                    _make_ironing(LayerSliceIsland &island);
    Polylines               _generate_sparse_infill_polylines_for_anchoring(const LayerSliceIsland &island,
                                                                           FillAdaptive::Octree *adaptive_fill_octree,
                                                                           FillAdaptive::Octree *support_fill_octree,
                                                                           FillLightning::Generator* lightning_generator) const;

public:

    void                    export_region_slices_to_svg(const char *path) const;
    void                    export_region_fill_surfaces_to_svg(const char *path) const;
    // Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
    void                    export_region_slices_to_svg_debug(const char *name) const;
    void                    export_region_fill_surfaces_to_svg_debug(const char *name) const;

    // Is there any valid extrusion assigned to any island-region?
    bool            has_extrusions() const;

    void simplify_extrusion_path();

    //need public destructor for unique_ptr
    virtual ~Layer();
protected:
    friend class PrintObject;
    friend class Steps::StepPipeline;
    friend LayerUPtrs new_layers(PrintObject*, const std::vector<coordf_t>&);
    friend struct ApiInternal::LayerAccess;

    Layer(size_t id, PrintObject *object, coord_t height, coord_t print_z, double slice_z, bool scaledok);
    // Clear fill extrusions, remove them from layer islands.
    void clear_fills();
    //Deprecated, for  legacy slicing in printobject
    void make_slices();
private:
    // Sequential index of layer, 0-based, offsetted by number of raft layers.
    size_t              m_id;
    PrintObject        *m_object;
    LayerRegionUPtrs     m_regions;
};

class SupportLayer : public Layer 
{
public:

    // Zero based index of an interface layer, used for alternating direction of interface / contact layers.
    size_t                      interface_id() const { return m_interface_id; }

    ExtrusionRole role() const;

    void simplify_support_extrusion_path();
    virtual ~SupportLayer() = default;
protected:
    friend class PrintObject;

    // The constructor has been made public to be able to insert additional support layers for the skirt or a wipe tower
    // between the raft and the object first layer.
    SupportLayer(size_t id, size_t interface_id, PrintObject *object, coord_t height, coord_t print_z, double slice_z, bool scaledok);

    size_t m_interface_id;
};

inline const Layer& layer_ref(const Layer &layer) { return layer; }
inline const Layer& layer_ref(const Layer *layer) { return *layer; }
inline const Layer& layer_ref(const std::unique_ptr<Layer> &layer) { return *layer; }

template<typename LayerContainer>
inline std::vector<float> slice_z_from_layers(const LayerContainer &layers)
{
    std::vector<float> zs;
    zs.reserve(layers.size());
    for (const auto &layer : layers)
        zs.emplace_back((float)layer_ref(layer).slice_z);
    return zs;
}

extern BoundingBox get_extents(const LayerRegion &layer_region);
extern BoundingBox get_extents(const LayerRegionRefs &layer_regions);

}

#endif
