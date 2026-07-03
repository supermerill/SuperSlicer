///|/ Copyright (c) Prusa Research 2016 - 2023 Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Oleksandra Iushchenko @YuSanka, Vojtěch Král @vojtechkral
///|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
///|/ Copyright (c) SuperSlicer 2022 Remi Durand @supermerill
///|/ Copyright (c) 2019 Bryan Smith
///|/ Copyright (c) 2017 Eyal Soha @eyal0
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2017 Joseph Lenox @lordofhyphens
///|/
///|/ ported from lib/Slic3r/Print.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv, Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2012 - 2013 Mark Hindess
///|/ Copyright (c) 2013 Devin Grady
///|/ Copyright (c) 2012 - 2013 Mike Sheldrake @mesheldrake
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/ Copyright (c) 2012 Michael Moon
///|/ Copyright (c) 2011 Richard Goodwin
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrintObject_hpp_
#define slic3r_PrintObject_hpp_

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

#include "DataTreeFwd.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Fill/FillAdaptive.hpp"
#include "Fill/FillLightning.hpp"
#include "libslic3r.h"
#include "Point.hpp"
#include "Polygon.hpp"
#include "PrintBase.hpp"
#include "FFFPrintConfig.hpp"
#include "PrintSteps.hpp"
#include "Surface.hpp"

namespace Slic3r {

class BoundingBox;
class GCodeGenerator;
class Print;
class PrintObject;
struct SlicingParameters;
namespace Steps { class StepPipeline; }
namespace ApiInternal { struct PrintObjectAccess; }

/**
* order:
*            m_objects[idx]->make_perimeters();
*                   -> slice()
*                   -> make_perimeters()
*            m_objects[idx]->infill();
*            m_objects[idx]->ironing();
*            obj->generate_support_spots();
*            psAlertWhenSupportsNeeded
*            obj.generate_support_material();
*            obj.estimate_curled_extrusions();
*            obj.calculate_overhanging_perimeters();
*            _make_wipe_tower();
*            _make_skirt();
*            make_brim();
*            simplify_extrusion_path();
* 
*           then export_gcode();
* */

// Single instance of a PrintObject.
// As multiple PrintObjects may be generated for a single ModelObject (their instances differ in rotation around Z),
// ModelObject's instancess will be distributed among these multiple PrintObjects.
struct PrintInstance
{
    // Parent PrintObject
    PrintObject 		*print_object;
    // Source ModelInstance of a ModelObject, for which this print_object was created.
	const ModelInstance *model_instance;
	// Shift of this instance's center into the world coordinates.
	Point 				 shift;
};

class PrintObject : public PrintObjectBaseWithState<Print, PrintObjectStep, posCount>, public PluginPropertyContainer
{
private: // Prevents erroneous use by other classes.
    typedef PrintObjectBaseWithState<Print, PrintObjectStep, posCount> Inherited;

public:
    // Size of an object: XYZ in scaled coordinates. The size might not be quite snug in XY plane.
    const Vec3crd&               size() const           { return m_size; }
    const PrintObjectConfig&     config() const         { return m_config; }
    const PrintRegionConfig&     default_region_config(const PrintRegionConfig &from_print) const;
    const Transform3d&           trafo() const          { return m_trafo; }
    // Trafo with the center_offset() applied after the transformation, to center the object in XY before slicing.
    Transform3d trafo_centered() const;
    const PrintInstances&        instances() const      { return m_instances; }

    // Bounding box is used to align the object infill patterns, and to calculate attractor for the rear seam.
    // The bounding box may not be quite snug.
    BoundingBox bounding_box() const;
    // Height is used for slicing, for sorting the objects by height for sequential printing and for checking vertical clearence in sequential print mode.
    // The height is snug.
    coord_t                     height() const         { return m_size.z(); }
    // Centering offset of the sliced mesh from the scaled and rotated mesh of the model.
    const Point&                center_offset() const  { return m_center_offset; }

    bool                         has_brim() const;
    Polygons                     get_brim_patch(ModelVolumeType brim_type, const PrintInstance *instance = nullptr) const;

    // Whoever will get a non-const pointer to PrintObject will be able to modify its layers.
    size_t          layer_count() const { return m_layers.size(); }
    void            clear_layers();
    const Layer&    layer(size_t idx) const { return *m_layers[idx]; }
    Layer&          layer(size_t idx) 		{ return *m_layers[idx]; }
    LayerCRefs      layers() const         { return make_ref_view<Layer>(m_layers); }
    LayerRefs       layers()               { return make_ref_view<Layer>(m_layers); }
    LayerUPtrs&     mutable_layers()          { return m_layers; }
    // Get a layer exactly at print_z.
    const Layer*    get_layer_at_printz(coord_t print_z) const;
    Layer*          get_layer_at_printz(coord_t print_z);
    // Get a layer approximately at print_z.
    const Layer*    get_layer_at_printz(double print_z_mm, double epsilon) const;
    Layer*          get_layer_at_printz(double print_z_mm, double epsilon);
    // Get the first layer approximately bellow print_z.
    const Layer*    get_first_layer_below_printz(coord_t print_z) const;
    const Layer*    get_first_layer_below_printz(double print_z_mm, double epsilon) const;
    // For sparse infill, get the max spasing avaialable in this object (avaialable after prepare_infill)
    coord_t         get_sparse_max_spacing() const { return m_max_sparse_spacing; }

    // Auxiliary_layer are layers that aren't made by the 3D triangles.
    // They exist to hold brim, skirt, support, wipetower.
    size_t                  auxiliary_layer_count() const { return m_auxiliary_layers.size(); }
    void                    clear_auxiliary_layers();
    const Layer&            auxiliary_layer(size_t idx) const { return *m_auxiliary_layers[idx]; }
    Layer&                  auxiliary_layer(size_t idx) { return *m_auxiliary_layers[idx]; }
    LayerCRefs              auxiliary_layers() const { return make_ref_view<Layer>(m_auxiliary_layers); }
    LayerRefs               auxiliary_layers() { return make_ref_view<Layer>(m_auxiliary_layers); }
    LayerUPtrs&             mutable_auxiliary_layers()  { return m_auxiliary_layers; }
    Layer&                  add_auxiliary_layer(size_t id, coord_t height, coord_t print_z);
    LayerUPtrs::iterator    insert_auxiliary_layer(LayerUPtrs::const_iterator pos, size_t id, coord_t height, coord_t print_z, double slice_z);

    // This is the *total* layer count (including support layers)
    // this value is not supposed to be compared with Layer::id
    // since they have different semantics.
    size_t          total_layer_count() const { return this->layer_count() + this->auxiliary_layer_count(); }

    // Initialize the layer_height_profile from the model_object's layer_height_profile, from model_object's layer height table, or from slicing parameters.
    // Returns true, if the layer_height_profile was changed.
    static bool     update_layer_height_profile(const ModelObject &model_object, const SlicingParameters &slicing_parameters, std::vector<coordf_t> &layer_height_profile);
    const std::vector<coord_t>& layer_profile() const { return m_layer_profile; }

    // Collect the slicing parameters, to be used by variable layer thickness algorithm,
    // by the interactive layer height editor and by the printing process itself.
    // The slicing parameters are dependent on various configuration values
    // (layer height, first layer height, raft settings, print nozzle diameter etc).
    const SlicingParameters&                    slicing_parameters() const { return *m_slicing_params; }
    static std::shared_ptr<SlicingParameters>   slicing_parameters(const DynamicPrintConfig &full_config, const ModelObject &model_object, float object_max_z);

    size_t num_printing_regions() const throw();
    const PrintRegion &printing_region(size_t idx) const throw();
    //FIXME returing all possible regions before slicing, thus some of the regions may not be slicing at the end.
    std::vector<std::reference_wrapper<const PrintRegion>> all_regions() const;
    const PrintObjectRegions*   shared_regions()        const throw() { assert(m_shared_regions); return m_shared_regions.get(); }

    bool                        has_support()           const { return m_config.support_material || m_config.support_material_enforce_layers > 0; }
    bool                        has_raft()              const { return m_config.raft_layers > 0; }
    bool                        has_support_material()  const { return this->has_support() || this->has_raft(); }
    // Checks if the model object is painted using the multi-material painting gizmo.
    bool is_mm_painted() const;

    // returns 0-based indices of extruders used to print the object (without brim, support and other helper extrusions)
    std::set<uint16_t>   object_extruders() const;
    double               get_first_layer_height() const;

    // Called by make_perimeters()
    void slice();

    // Helpers to slice support enforcer / blocker meshes by the support generator.
    std::vector<ExPolygons>     slice_support_volumes(const ModelVolumeType model_volume_type) const;
    std::vector<ExPolygons>     slice_support_blockers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_BLOCKER); }
    std::vector<ExPolygons>     slice_support_enforcers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_ENFORCER); }

    // Helpers to project custom facets on slices
    std::vector<Polygons> project_and_append_custom_facets(bool seam, EnforcerBlockerType type) const;
    std::vector<Polygons> project_and_append_custom_facets(const std::string &painting_key, EnforcerBlockerType type) const;

    /// skirts if done per copy and not per platter
    [[deprecated("brim/skirt are stored in auxiliary layers; this accessor materializes a compatibility cache.")]]
    const std::optional<ExtrusionEntityCollection>& skirt_first_layer() const;
    [[deprecated("brim/skirt are stored in auxiliary layers; this accessor materializes a compatibility cache.")]]
    const ExtrusionEntityCollection& skirt() const;
    [[deprecated("brim/skirt are stored in auxiliary layers; this accessor materializes a compatibility cache.")]]
    const ExtrusionEntityCollection& brim() const;

    // for unique_ptr
    ~PrintObject() override;
protected:
    // to be called from Print only.
    friend class Print;
    template<typename PrintStepEnumType, const size_t COUNT> friend class PrintBaseWithState;
    friend class Steps::StepPipeline;
    friend struct ApiInternal::PrintObjectAccess;

    PrintObject(Print* print, ModelObject* model_object, const Transform3d& trafo, PrintInstances&& instances);
    // Print-level auxiliary geometry uses a hidden PrintObject with no source
    // ModelObject. It owns only auxiliary layers and exists so plugins can use
    // the same Layer/LayerRegion/LayerIsland APIs for global print helpers as
    // they already use for object-local support, skirt or brim.
    PrintObject(Print* print, const Vec3crd &size, std::shared_ptr<PrintObjectRegions> shared_regions);
    // as Layers are linked to us via a pointer, we can't move ourselves, or the link is severed
    PrintObject(PrintObject&&) = delete;
    PrintObject& operator=(PrintObject&&) = delete;
    PrintObject(const PrintObject&) = delete;
    PrintObject& operator=(const PrintObject&) = delete;

    void                    config_apply(const ConfigBase &other, bool ignore_nonexistent = false) { m_config.apply(other, ignore_nonexistent); }
    void                    config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) { m_config.apply_only(other, keys, ignore_nonexistent); }
    PrintBase::ApplyStatus  set_instances(PrintInstances &&instances);
    // Invalidates the step, and its depending steps in PrintObject and Print.
    bool                    invalidate_step(slicing_step_t step);
    bool                    invalidate_step_direct(slicing_step_t step);
    bool                    invalidate_steps_direct(std::initializer_list<slicing_step_t> steps);
    // Invalidates all PrintObject and Print steps.
    bool                    invalidate_all_steps();
    bool                    invalidate_all_steps_direct();
    // Invalidate steps based on a set of parameters changed.
    // It may be called for both the PrintObjectConfig and PrintRegionConfig.
    bool                    invalidate_state_by_config_options(
        const ConfigOptionResolver &old_config, const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys);
    // If ! m_slicing_params.valid, recalculate.
    void                    update_slicing_parameters();

    // Called on main thread with stopped or paused background processing to let PrintObject release data for its milestones that were invalidated or canceled.
    void                    cleanup();

    static PrintObjectConfig object_config_from_model_object(const PrintObjectConfig &default_object_config, const ModelObject &object, size_t num_extruders);

private:
    void make_perimeters();
    void prepare_infill();
    void clear_fills();
    bool has_typed_slices() const;
    void restore_untyped_slices();
    void infill();
    void ironing();
    void generate_support_spots();
    void generate_support_material();
    void estimate_curled_extrusions();
    void calculate_overhanging_perimeters();
    void simplify_extrusion_path();

    void slice_volumes();
    // Has any support (not counting the raft).
    ExPolygons _shrink_contour_holes(double contour_delta, double default_delta, double convex_delta, const ExPolygons& input) const;
    void _transform_hole_to_polyholes();
    void _max_overhang_threshold();
    ExPolygons _smooth_curves(const ExPolygons &input, const PrintRegionConfig &conf) const;
    void detect_surfaces_type();
    void apply_solid_infill_below_layer_area();
    void process_external_surfaces(bool old);
    void discover_vertical_shells();
    void bridge_over_infill();
    void replaceSurfaceType(SurfaceType st_to_replace, SurfaceType st_replacement, SurfaceType st_under_it);
    // void clip_fill_surfaces(); //infill_only_where_needed
    void tag_under_bridge();
    void discover_horizontal_shells();
    void clean_surfaces();
    void combine_infill();
    void _generate_support_material();
    void _compute_max_sparse_spacing();
    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> prepare_adaptive_infill_data(
        const std::vector<std::pair<const Surface*, coord_t>>& surfaces_w_bottom_z) const;
    FillLightning::GeneratorPtr prepare_lightning_infill_data();

    // XYZ in scaled coordinates
    Vec3crd									m_size;
    PrintObjectConfig                       m_config;
    // Translation in Z + Rotation + Scaling / Mirroring.
    Transform3d                             m_trafo = Transform3d::Identity();
    // Slic3r::Point objects in scaled G-code coordinates
    std::vector<PrintInstance>              m_instances;
    // The mesh is being centered before thrown to Clipper, so that the Clipper's fixed coordinates require less bits.
    // This is the adjustment of the  the Object's coordinate system towards PrintObject's coordinate system.
    Point                                   m_center_offset;

    // Object split into layer ranges and regions with their associated configurations.
    // Shared among PrintObjects created for the same ModelObject.
    std::shared_ptr<PrintObjectRegions>     m_shared_regions;

    std::shared_ptr<SlicingParameters>      m_slicing_params;
    LayerUPtrs                               m_layers;
    LayerUPtrs                              m_auxiliary_layers;

    /*
    Deprecated read-through caches for legacy callers. Object-owned brim/skirt
    now lives in auxiliary layers; these collections are rebuilt from those
    layers when old accessors are called.
    */
    mutable std::optional<ExtrusionEntityCollection> m_legacy_skirt_first_layer_cache;
    mutable ExtrusionEntityCollection               m_legacy_skirt_cache;
    mutable ExtrusionEntityCollection               m_legacy_brim_cache;

    // this is set to true when LayerRegion->slices is split in top/internal/bottom
    // so that next call to make_perimeters() performs a union() before computing loops
    bool                                  m_typed_slices = false;

    //this setting allow fill_aligned_z to get the max sparse spacing spacing.
    coord_t                                 m_max_sparse_spacing = 0;

    // pair < adaptive , support>, filled by prepare_adaptive_infill_data() (in bridge_over_infill() in prepare_infill()) and used in infill()
    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> m_adaptive_fill_octrees;
    // filled by prepare_lightning_infill_data() (in bridge_over_infill() in prepare_infill()) and used in infill()
    FillLightning::GeneratorPtr m_lightning_generator;

    // Result of STEP_LAYER_HEIGHT. It stores explicit object-local
    // [layer_top_z, layer_height] pairs, one pair per object layer.
    // Raft/support layers are not part of this list.
    std::vector<coord_t> m_layer_profile;

};




} /* namespace Slic3r */

#endif // slic3r_PrintObject_hpp_
