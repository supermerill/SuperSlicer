///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultBrimGenerator.hpp"
#include "AdhesionLayerHelpers.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

#include "libslic3r/Api/internal/PrintObjectAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Brim.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ContainerUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"

/*
Default brim generator
======================

This plugin is the first STEP_SKIRT_BRIM implementation. It keeps the classic
brim geometry algorithm, but moves ownership of the output to the plugin step:
the plugin builds temporary ExtrusionEntityCollection trees, then publishes
them through the STEP_SKIRT_BRIM callbacks.

The brim algorithm needs to answer two questions:

1. Which objects can share one brim computation?
   Objects can share the same global brim only when the settings that affect
   brim geometry are identical. Otherwise each compatible group is processed
   separately.

2. Where is brim forbidden?
   A brim must not overlap object first-layer slices, negative brim patches, or
   other already-planned brim areas. The `unbrimmable` ExPolygons argument is
   the running "occupied" geometry passed to Brim.hpp helpers.

The plugin temporarily edits PrintObject::instances only for the legacy helper
calls that generate a single-object or single-instance brim pattern. The RAII
guard below restores the original instances even if a brim helper throws.
*/

namespace slic3r_api { namespace SkirtBrim { namespace DefaultBrimGeneratorPlugin {
namespace {

using namespace Slic3r;

const char *const k_no_dependencies[] = { nullptr };
const char *const k_group_id = "skirt_brim.brim";
const char *const k_settings_fragment_id = "skirt_brim.brim.default.settings";

const char *const k_defined_config_keys[] = {
    "brim_ears",
    "brim_ears_max_angle",
    "brim_ears_pattern",
    "brim_ears_detection_length",
    "brim_inside_holes"
};

const key_value_string_pair_t k_brim_ear_patterns[] = {
    { "concentric", "Concentric" },
    { "rectilinear", "Rectilinear" }
};

const raw_used_config_key k_used_config_keys[] = {
    { "brim_width", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_width_interior", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_separation", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_inside_holes", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_per_object", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_ears", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_ears_max_angle", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_ears_pattern", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_ears_detection_length", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_height", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "layer_height", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_direction", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "complete_objects", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "parallel_objects_step", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "arc_fitting", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "resolution_internal", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "raft_first_layer_density", RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_interface_spacing", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "filament_max_overlap", RAW_CO_VECTOR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_interface_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

/*
Temporarily replace one object's instances while a legacy brim helper computes
a reusable brim pattern. The original instances are restored in the destructor,
so every early return or exception leaves PrintObject in the same state it had
before generation started.
*/
class TemporaryObjectInstances
{
public:
    explicit TemporaryObjectInstances(PrintObject &object);
    ~TemporaryObjectInstances();

    void set_single_origin_instance();
    void set_single_instance(const PrintInstance &instance);

private:
    PrintObject &m_object;
    PrintInstances m_saved_instances;
};

/*
Values owned by this plugin and resolved from one object's dynamic config.
Keeping them together makes the grouping comparison and the geometry call use
the same snapshot of configuration.
*/
struct DefaultBrimSettings
{
    bool use_ears = false;
    BrimGenerationParameters geometry;
};

// Read the five settings owned by this plugin from an object's API config view.
DefaultBrimSettings read_default_brim_settings(const PrintObject &object);

// Register one GUI activation rule for a setting owned by this plugin.
void add_enable_rule(orchestrator_handle *orchestrator,
                     const char *target_key,
                     raw_gui_rule_condition condition,
                     const char *condition_key);

// Register the option definitions, layout fragment and activation rules.
void initialize_default_brim_settings(orchestrator_handle *orchestrator);

// Return true when an object has a model volume that explicitly adds/removes brim.
bool has_brim_patch(const PrintObject &object, ModelVolumeType brim_type);

// Return true when any object in a compatible group has the requested brim patch.
bool has_brim_patch(const std::vector<PrintObject *> &objects, ModelVolumeType brim_type);

// Group objects whose brim-affecting settings are identical.
std::vector<std::vector<PrintObject *>> group_objects_by_brim_settings(Slic3r::Print &print, bool &brim_per_object);

// Compare the optional first-layer extrusion width setting for grouping.
bool same_first_layer_extrusion_width(const PrintObjectConfig &lhs, const PrintObjectConfig &rhs);

// Compare all static and plugin-owned settings that change generated geometry.
bool same_brim_group_settings(const PrintObject &lhs, const PrintObject &rhs);

// Build the running occupied area used to keep generated brim away from objects and negative patches.
ExPolygons initial_unbrimmable_area(const std::vector<std::vector<PrintObject *>> &object_groups,
                                    bool brim_per_object);

// Convert centerline brim extrusions into the 2D subject owned by the auxiliary layer.
ExPolygons brim_subject_from_extrusion(const Slic3r::ExtrusionEntity &extrusion);

// Select the extrusion flow used by per-object brim generation.
Flow brim_flow_for_object(const Slic3r::Print &print, const PrintObject &object);

// Select the extrusion flow used by global brim generation.
Flow brim_flow_for_print(const Slic3r::Print &print);

// Add the configured outer, interior and patch brim into one temporary output collection.
void generate_brim_for_objects(const Slic3r::Print &print,
                               const Flow &flow,
                               const PrintObjectPtrs &objects,
                               ExPolygons &unbrimmable_area,
                               ExtrusionEntityCollection &out);

// Publish brim into an auxiliary layer tagged as adhesion.
bool publish_brim_to_auxiliary_layer(storage_handle *storage,
                                     orchestrator_handle *orchestrator,
                                     Slic3r::Print &print,
                                     const slic3r_api::Object &object,
                                     const Slic3r::Layer &reference_layer,
                                     Slic3r::ExtrusionEntity &brim);

// Publish object-owned brim into the object's auxiliary layers.
void publish_object_brim(storage_handle *storage,
                         orchestrator_handle *orchestrator,
                         Slic3r::Print &print,
                         PrintObject &object,
                         ExtrusionEntityCollection &brim);

// Generate the object-owned branch used when brim_per_object is enabled.
void generate_per_object_brim(storage_handle *storage,
                              orchestrator_handle *orchestrator,
                              Slic3r::Print &print,
                              PrintObject &object,
                              ExPolygons &unbrimmable_area);

// Generate the print-owned branch used when compatible objects share one global brim.
void generate_global_brim(storage_handle *storage,
                          orchestrator_handle *orchestrator,
                          Slic3r::Print &print,
                          const std::vector<PrintObject *> &object_group,
                          ExPolygons &unbrimmable_area);

// Generate all brim groups for one print.
void generate_default_brim(storage_handle *storage,
                           orchestrator_handle *orchestrator,
                           Slic3r::Print &print);

DefaultBrimSettings read_default_brim_settings(const PrintObject &object)
{
    const slic3r_api::Object object_view(reinterpret_cast<const object_handle *>(&object));
    const Config config = object_view.config();

    DefaultBrimSettings settings;
    settings.use_ears = config.bool_or_default("brim_ears", false);
    settings.geometry.fill_enclosed_holes = config.bool_or_default("brim_inside_holes", false);
    settings.geometry.ear_max_angle_degrees = config.float_or_default("brim_ears_max_angle", 125.0);
    settings.geometry.ear_detection_length_mm = config.float_or_default("brim_ears_detection_length", 1.0);
    settings.geometry.ear_pattern = config.enum_or_default("brim_ears_pattern", 0) == 1 ?
        BrimEarPattern::Rectilinear : BrimEarPattern::Concentric;
    return settings;
}

void add_enable_rule(orchestrator_handle *orchestrator,
                     const char *target_key,
                     const raw_gui_rule_condition condition,
                     const char *condition_key)
{
    raw_gui_rule rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE;
    rule.condition = condition;
    rule.target_key = target_key;
    rule.condition_key = condition_key;
    orchestrator_add_gui_rule(orchestrator, &rule);
}

void initialize_default_brim_settings(orchestrator_handle *orchestrator)
{
    /*
    These definitions deliberately keep the historic serialized keys and
    defaults. Existing profiles therefore load unchanged even though ownership
    moves from FFFPrintConfig to this plugin.
    */
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = "brim_inside_holes";
    def.type = RAW_CO_BOOL;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Brim inside holes";
    def.category = RAW_OPTION_CATEGORY_SKIRT_BRIM;
    def.tooltip = "Allow to create a brim over an island when it's inside a hole (or surrounded by an object)."
                  "\nIncompatible with brim_width_interior, as it enables it with brim_width width.";
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "0";
    orchestrator_create_option_def(orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "brim_ears";
    def.type = RAW_CO_BOOL;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Brim ears";
    def.full_label = "Brim ears";
    def.category = RAW_OPTION_CATEGORY_SKIRT_BRIM;
    def.tooltip = "Only draw brim over the sharp edges of the model.";
    def.mode = RAW_CONFIG_OPTION_MODE_SIM_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "0";
    orchestrator_create_option_def(orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "brim_ears_max_angle";
    def.type = RAW_CO_FLOAT;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Max angle";
    def.full_label = "Brim ear max angle";
    def.category = RAW_OPTION_CATEGORY_SKIRT_BRIM;
    def.tooltip = "Maximum angle to let a brim ear appear. \nIf set to 0, no brim will be created. "
                  "\nIf set to ~178, brim will be created on everything but straight sections.";
    def.sidetext = "°";
    def.has_min = 1;
    def.min_value = 0.0;
    def.has_max = 1;
    def.max_value = 180.0;
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "125";
    orchestrator_create_option_def(orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "brim_ears_detection_length";
    def.type = RAW_CO_FLOAT;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Detection radius";
    def.full_label = "Brim ear detection length";
    def.category = RAW_OPTION_CATEGORY_SKIRT_BRIM;
    def.tooltip = "The geometry will be decimated before dectecting sharp angles. This parameter indicates "
                  "the minimum length of the deviation for the decimation.\n0 to deactivate";
    def.sidetext = "mm";
    def.has_min = 1;
    def.min_value = 0.0;
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "1";
    orchestrator_create_option_def(orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "brim_ears_pattern";
    def.type = RAW_CO_ENUM;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.gui_type = RAW_GUI_TYPE_SELECT_CLOSE;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Pattern";
    def.full_label = "Ear pattern";
    def.category = RAW_OPTION_CATEGORY_INFILL;
    def.tooltip = "Pattern for the ear. The concentric is the default one. The rectilinear has a perimeter "
                  "around it, you can try it if the concentric has too many problems to stick to the build plate.";
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "concentric";
    def.enum_def.value_label_pairs.items = k_brim_ear_patterns;
    def.enum_def.value_label_pairs.count = uint32_t(std::size(k_brim_ear_patterns));
    orchestrator_create_option_def(orchestrator, &def);

    /*
    The fragment restores the same controls after they are removed from the
    static print.ui file. It also supplies the Simple-mode copy of brim_ears.
    */
    orchestrator_add_ui_fragment(
        orchestrator,
        "print.ui",
        k_settings_fragment_id,
        "page:Skirt & Brim\n"
        "group:Brim\n"
        "setting:insert$aftersetting$brim_width:brim_inside_holes\n"
        "line:insert$aftersetting$brim_width_interior:Brim ears\n"
        "setting:tags$Advanced$Expert$Prusa:label$_:sidetext_width$0:brim_ears\n"
        "setting:width$3:sidetext_width$1:brim_ears_max_angle\n"
        "setting:width$3:sidetext_width$3:brim_ears_detection_length\n"
        "setting:brim_ears_pattern\n"
        "end_line\n"
        "page:Support & Other\n"
        "group:Skirt & Brim\n"
        "setting:insert$aftersetting$brim_width:tags$Simple:sidetext_width$0:brim_ears\n",
        0);

    /*
    Multiple ENABLE rules for one target are combined with AND. The GUI thus
    mirrors the geometric preconditions without hardcoded option names in
    ConfigManipulation.
    */
    add_enable_rule(orchestrator, "brim_ears", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "brim_width");
    add_enable_rule(orchestrator, "brim_inside_holes", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "brim_width");
    add_enable_rule(orchestrator, "brim_inside_holes", RAW_GUI_RULE_CONDITION_BOOL_FALSE, "brim_width_interior");
    add_enable_rule(orchestrator, "brim_ears_max_angle", RAW_GUI_RULE_CONDITION_BOOL_TRUE, "brim_ears");
    add_enable_rule(orchestrator, "brim_ears_max_angle", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "brim_width");
    add_enable_rule(orchestrator, "brim_ears_detection_length", RAW_GUI_RULE_CONDITION_BOOL_TRUE, "brim_ears");
    add_enable_rule(orchestrator, "brim_ears_detection_length", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "brim_width");
    add_enable_rule(orchestrator, "brim_ears_pattern", RAW_GUI_RULE_CONDITION_BOOL_TRUE, "brim_ears");
    add_enable_rule(orchestrator, "brim_ears_pattern", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "brim_width");
}

TemporaryObjectInstances::TemporaryObjectInstances(PrintObject &object) :
    m_object(object),
    m_saved_instances(ApiInternal::PrintObjectAccess::mutable_instances(object))
{
}

TemporaryObjectInstances::~TemporaryObjectInstances()
{
    ApiInternal::PrintObjectAccess::mutable_instances(m_object) = std::move(m_saved_instances);
}

void TemporaryObjectInstances::set_single_origin_instance()
{
    PrintInstances &instances = ApiInternal::PrintObjectAccess::mutable_instances(m_object);
    instances.clear();
    instances.emplace_back();
}

void TemporaryObjectInstances::set_single_instance(const PrintInstance &instance)
{
    PrintInstances &instances = ApiInternal::PrintObjectAccess::mutable_instances(m_object);
    instances.clear();
    instances.push_back(instance);
}

bool has_brim_patch(const PrintObject &object, const ModelVolumeType brim_type)
{
    for (const ModelVolume *volume : object.model_object()->volumes) {
        assert(volume != nullptr);
        if (volume != nullptr && volume->type() == brim_type)
            return true;
    }
    return false;
}

bool has_brim_patch(const std::vector<PrintObject *> &objects, const ModelVolumeType brim_type)
{
    for (const PrintObject *object : objects)
        if (object != nullptr && has_brim_patch(*object, brim_type))
            return true;
    return false;
}

std::vector<std::vector<PrintObject *>> group_objects_by_brim_settings(Slic3r::Print &print, bool &brim_per_object)
{
    std::vector<std::vector<PrintObject *>> object_groups;
    brim_per_object = false;

    for (PrintObject &object : print.objects()) {
        brim_per_object = brim_per_object || object.config().brim_per_object.value;
        bool added = false;
        for (std::vector<PrintObject *> &object_group : object_groups) {
            assert(!object_group.empty());
            if (same_brim_group_settings(*object_group.front(), object)) {
                object_group.push_back(&object);
                added = true;
                break;
            }
        }
        if (!added)
            object_groups.push_back({ &object });
    }

    return object_groups;
}

bool same_first_layer_extrusion_width(const PrintObjectConfig &lhs, const PrintObjectConfig &rhs)
{
    if (lhs.first_layer_extrusion_width.is_enabled() != rhs.first_layer_extrusion_width.is_enabled())
        return false;
    if (!lhs.first_layer_extrusion_width.is_enabled())
        return true;
    return lhs.first_layer_extrusion_width.value == rhs.first_layer_extrusion_width.value;
}

bool same_brim_group_settings(const PrintObject &lhs, const PrintObject &rhs)
{
    const PrintObjectConfig &lhs_config = lhs.config();
    const PrintObjectConfig &rhs_config = rhs.config();
    const DefaultBrimSettings lhs_settings = read_default_brim_settings(lhs);
    const DefaultBrimSettings rhs_settings = read_default_brim_settings(rhs);

    return lhs_settings.use_ears == rhs_settings.use_ears &&
           lhs_settings.geometry.fill_enclosed_holes == rhs_settings.geometry.fill_enclosed_holes &&
           lhs_settings.geometry.ear_max_angle_degrees == rhs_settings.geometry.ear_max_angle_degrees &&
           lhs_settings.geometry.ear_detection_length_mm == rhs_settings.geometry.ear_detection_length_mm &&
           lhs_settings.geometry.ear_pattern == rhs_settings.geometry.ear_pattern &&
           lhs_config.brim_per_object.value == rhs_config.brim_per_object.value &&
           lhs_config.brim_separation.value == rhs_config.brim_separation.value &&
           lhs_config.brim_width.value == rhs_config.brim_width.value &&
           lhs_config.brim_width_interior.value == rhs_config.brim_width_interior.value &&
           same_first_layer_extrusion_width(lhs_config, rhs_config);
}

ExPolygons initial_unbrimmable_area(const std::vector<std::vector<PrintObject *>> &object_groups,
                                    const bool brim_per_object)
{
    ExPolygons brim_area;
    bool needs_global_occupied_area =
        object_groups.size() > 1 || brim_per_object;
    if (!needs_global_occupied_area && !object_groups.empty()) {
        for (const std::vector<PrintObject *> &object_group : object_groups) {
            needs_global_occupied_area = (has_brim_patch(object_group, ModelVolumeType::BRIM_PATCH) ||
                                          has_brim_patch(object_group, ModelVolumeType::BRIM_NEGATIVE));
            if (needs_global_occupied_area) {
                break;
            }
        }
    }
    if (!needs_global_occupied_area)
        return brim_area;

    /*
    When object groups cannot share one brim, each group must treat every first
    layer slice that already exists on the bed as occupied. The copies are
    translated by instance shift because layer slices are stored in object-local
    coordinates.
    */
    for (const std::vector<PrintObject *> &object_group : object_groups) {
        for (const PrintObject *object : object_group) {
            if (object == nullptr)
                continue;
            if (object->layer_count() > 0) {
                const Slic3r::Layer &first_layer = object->layers().front();
                for (const PrintInstance &instance : object->instances()) {
                    const size_t first_idx = brim_area.size();
                    brim_area.insert(brim_area.end(), first_layer.lslices().begin(), first_layer.lslices().end());
                    for (size_t idx = first_idx; idx < brim_area.size(); ++idx)
                        brim_area[idx].translate(instance.shift.x(), instance.shift.y());
                }
            }

            /*
            Negative brim patches behave like occupied bed area: generated brim
            must keep away from them even though they are not real object slices.
            */
            if (has_brim_patch(*object, ModelVolumeType::BRIM_NEGATIVE)) {
                for (Slic3r::Polygon &polygon : object->get_brim_patch(ModelVolumeType::BRIM_NEGATIVE))
                    brim_area.push_back(Slic3r::ExPolygon(std::move(polygon)));
            }
        }
    }

    return brim_area;
}

ExPolygons brim_subject_from_extrusion(const Slic3r::ExtrusionEntity &extrusion)
{
    /*
    Brim paths are stored as centerlines, but auxiliary layers own printable
    area. Inflate every leaf by the width carried in its extrusion attributes,
    then union the polygons so region/island reconstruction receives a clean
    subject instead of a set of overlapping path covers.
    */
    Polygons coverage = extrusion.polygons_covered_by_width(float(SCALED_EPSILON));
    return coverage.empty() ? ExPolygons{} : union_ex(coverage);
}

Flow brim_flow_for_object(const Slic3r::Print &print, const PrintObject &object)
{
    std::set<uint16_t> extruders = print.object_extruders(PrintObjectPtrs{ const_cast<PrintObject *>(&object) });
    append(extruders, print.support_material_extruders());
    const size_t extruder_id =
        extruders.empty() ? print.print_region(0).config().perimeter_extruder.value - 1 : *extruders.begin();
    return print.brim_flow(extruder_id, object.config());
}

Flow brim_flow_for_print(const Slic3r::Print &print)
{
    std::set<uint16_t> extruders = print.object_extruders();
    append(extruders, print.support_material_extruders());
    const size_t extruder_id =
        extruders.empty() ? print.print_region(0).config().perimeter_extruder.value - 1 : *extruders.begin();
    return print.brim_flow(extruder_id, print.default_object_config());
}

void generate_brim_for_objects(const Slic3r::Print &print,
                               const Flow &flow,
                               const PrintObjectPtrs &objects,
                               ExPolygons &unbrimmable_area,
                               ExtrusionEntityCollection &out)
{
    assert(!objects.empty());
    const PrintObjectConfig &brim_config = objects.front()->config();
    const DefaultBrimSettings brim_settings = read_default_brim_settings(*objects.front());

    if (brim_config.brim_width > 0) {
        if (brim_settings.use_ears)
            make_brim_ears(print, flow, objects, brim_settings.geometry, unbrimmable_area, out);
        else
            make_brim(print, flow, objects, brim_settings.geometry, unbrimmable_area, out);
    }

    if (brim_config.brim_width_interior > 0)
        make_brim_interior(print, flow, objects, unbrimmable_area, out);
}

void publish_object_brim(storage_handle *storage,
                         orchestrator_handle *orchestrator,
                         Slic3r::Print &print,
                         PrintObject &object,
                         ExtrusionEntityCollection &brim)
{
    if (brim.empty())
        return;

    const slic3r_api::Object object_view(reinterpret_cast<const object_handle *>(&object));
    if (object.layer_count() == 0 ||
        !publish_brim_to_auxiliary_layer(storage, orchestrator, print, object_view, object.layer(0), brim))
        throw RuntimeError("Default brim generator could not publish object brim auxiliary layer.");
}

bool publish_brim_to_auxiliary_layer(storage_handle *storage,
                                     orchestrator_handle *orchestrator,
                                     Slic3r::Print &print,
                                     const slic3r_api::Object &object,
                                     const Slic3r::Layer &reference_layer,
                                     Slic3r::ExtrusionEntity &brim)
{
    if (storage == nullptr || brim.empty())
        return false;

    ExPolygons subject = brim_subject_from_extrusion(brim);
    if (subject.empty())
        return false;

    const slic3r_api::Print print_view(reinterpret_cast<const print_handle *>(&print));
    const slic3r_api::ExPolygonCollection subject_view(
        reinterpret_cast<const expolygon_collection_handle *>(&subject));

    return publish_adhesion_extrusion_to_auxiliary_layer(
        storage,
        orchestrator,
        print_view,
        object,
        subject_view,
        reference_layer.scaled_height(),
        reference_layer.scaled_print_z(),
        scale_to_layer_coord(reference_layer.slice_z),
        RAW_LAYER_ADHESION_KIND_BRIM,
        0,
        slic3r_api::MutableExtrusionEntity(reinterpret_cast<extrusion_entity_handle *>(&brim)));
}

void generate_per_object_brim(storage_handle *storage,
                              orchestrator_handle *orchestrator,
                              Slic3r::Print &print,
                              PrintObject &object,
                              ExPolygons &unbrimmable_area)
{
    const PrintObjectConfig &brim_config = object.config();
    const Flow flow = brim_flow_for_object(print, object);

    if (print.config().complete_objects || print.config().parallel_objects_step.value > 0) {
        /*
        Complete-object mode prints one full object at a time. The brim stored
        on the object is therefore a reusable pattern around an origin instance;
        placement happens later with the object instance transform.
        */
        ExPolygons local_unbrimmable_area;
        ExtrusionEntityCollection object_brim;
        TemporaryObjectInstances instances_guard(object);
        instances_guard.set_single_origin_instance();
        generate_brim_for_objects(print, flow, PrintObjectPtrs{ &object }, local_unbrimmable_area, object_brim);
        make_brim_patch(
            print, flow, object.get_brim_patch(ModelVolumeType::BRIM_PATCH), local_unbrimmable_area, object_brim);
        publish_object_brim(storage, orchestrator, print, object, object_brim);
        return;
    }

    unbrimmable_area = union_ex(unbrimmable_area);
    const PrintInstances copies = object.instances();
    TemporaryObjectInstances instances_guard(object);

    /*
    In normal multi-object mode, every copy lives in bed coordinates and must
    avoid every other occupied area. Generate one temporary tree per copy, then
    publish it as an object-owned auxiliary brim layer plus legacy mirror.
    */
    for (const PrintInstance &instance : copies) {
        instances_guard.set_single_instance(instance);
        ExtrusionEntityCollection entity_brim;
        generate_brim_for_objects(print, flow, PrintObjectPtrs{ &object }, unbrimmable_area, entity_brim);
        make_brim_patch(
            print, flow, object.get_brim_patch(ModelVolumeType::BRIM_PATCH), unbrimmable_area, entity_brim);
        publish_object_brim(storage, orchestrator, print, object, entity_brim);
    }
}

void generate_global_brim(storage_handle *storage,
                          orchestrator_handle *orchestrator,
                          Slic3r::Print &print,
                          const std::vector<PrintObject *> &object_group,
                          ExPolygons &unbrimmable_area)
{
    unbrimmable_area = union_ex(unbrimmable_area);
    const Flow flow = brim_flow_for_print(print);
    ExtrusionEntityCollection global_brim;
    generate_brim_for_objects(print, flow, object_group, unbrimmable_area, global_brim);
    if (!global_brim.empty()) {
        const PrintObject *reference_object = nullptr;
        for (const PrintObject *object : object_group) {
            if (object != nullptr && object->layer_count() > 0) {
                reference_object = object;
                break;
            }
        }
        if (reference_object == nullptr)
            throw RuntimeError("Default brim generator could not find a first layer for global brim.");

        const slic3r_api::Print print_view(reinterpret_cast<const print_handle *>(&print));
        if (!publish_brim_to_auxiliary_layer(storage,
                                             orchestrator,
                                             print,
                                             print_view.auxiliary_object(),
                                             reference_object->layer(0),
                                             global_brim))
            throw RuntimeError("Default brim generator could not publish global brim auxiliary layer.");
    }

    /*
    Brim patch volumes are object-local features. Even when normal brim is
    shared globally, patch brim is generated per instance and stored with the
    object that owns the patch.
    */
    for (PrintObject *object : object_group) {
        assert(object != nullptr);
        if (object == nullptr)
            continue;
        for (const PrintInstance &instance : object->instances()) {
            ExtrusionEntityCollection patch_brim;
            make_brim_patch(
                print, flow, object->get_brim_patch(ModelVolumeType::BRIM_PATCH, &instance), unbrimmable_area, patch_brim);
            publish_object_brim(storage, orchestrator, print, *object, patch_brim);
        }
    }
}

void generate_default_brim(storage_handle *storage,
                           orchestrator_handle *orchestrator,
                           Slic3r::Print &print)
{
    bool brim_per_object = false;
    std::vector<std::vector<PrintObject *>> object_groups =
        group_objects_by_brim_settings(print, brim_per_object);
    ExPolygons unbrimmable_area = initial_unbrimmable_area(object_groups, brim_per_object);

    for (std::vector<PrintObject *> &object_group : object_groups) {
        assert(!object_group.empty());
        const PrintObjectConfig &brim_config = object_group.front()->config();
        const bool has_work =
            brim_config.brim_width > 0 ||
            brim_config.brim_width_interior > 0 ||
            has_brim_patch(object_group, ModelVolumeType::BRIM_PATCH);
        if (!has_work)
            continue;

        if (brim_config.brim_per_object) {
            for (PrintObject *object : object_group) {
                assert(object != nullptr);
                if (object != nullptr)
                    generate_per_object_brim(storage, orchestrator, print, *object, unbrimmable_area);
            }
        } else {
            generate_global_brim(storage, orchestrator, print, object_group, unbrimmable_area);
        }
    }
}

class DefaultBrimGenerator : public PluginBase
{
public:
    static DefaultBrimGenerator &instance(orchestrator_handle *orchestrator)
    {
        static DefaultBrimGenerator instance(orchestrator);
        return instance;
    }

    explicit DefaultBrimGenerator(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "skirt_brim.brim.default"; }
    const char *name_impl() const noexcept override { return "Default brim generator"; }
    const char *description_impl() const noexcept override
    {
        return "Generates the classic first-layer brim geometry.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_group_id; }
    const char *exclusive_group_label_impl() const noexcept override { return "Brim generator"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects which plugin generates brim extrusion around first-layer objects.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_SKIRT_BRIM; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 0; }
    const char *progress_message_format_impl() const noexcept override { return "Generating brim"; }

    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override
    {
        if (keys == nullptr)
            return int32_t(std::size(k_used_config_keys));
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
        return int32_t(std::size(k_used_config_keys));
    }

    int32_t defined_config_keys(const char **keys) const noexcept override
    {
        if (keys != nullptr)
            for (size_t idx = 0; idx < std::size(k_defined_config_keys); ++idx)
                keys[idx] = k_defined_config_keys[idx];
        return int32_t(std::size(k_defined_config_keys));
    }

    void inilialize_impl(storage_handle *) const override
    {
        initialize_default_brim_settings(m_orchestrator);
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_skirt_brim *ctx = plugin_ctx_as_skirt_brim(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->print != nullptr);
        if (ctx == nullptr || ctx->print == nullptr)
            return;

        generate_default_brim(
            run_ctx->plugin_storage,
            m_orchestrator,
            *reinterpret_cast<Slic3r::Print *>(ctx->print));
    }
};

} // namespace

void register_default_brim_generator_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, DefaultBrimGenerator::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::SkirtBrim::DefaultBrimGeneratorPlugin
