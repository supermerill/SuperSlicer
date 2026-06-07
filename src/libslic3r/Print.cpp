///|/ Copyright (c) Prusa Research 2016 - 2023 Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Roman Beránek @zavorka, David Kocík @kocikdav
///|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2019 Bryan Smith
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
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
#include "Print.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <limits>
#include <string>
#include <unordered_set>

#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>

#include <oneapi/tbb/parallel_for.h>

#include "BoundingBox.hpp"
#include "Api/internal/PrintAccess.hpp"
#include "Api/internal/PrintObjectAccess.hpp"
#include "Api/host/Orchestrator.hpp"
#include "Brim.hpp"
#include "BuildVolume.hpp"
#include <clipper/clipper_z.hpp>
#include "ClipperUtils.hpp"
#include "Exception.hpp"
#include "Extruder.hpp"
#include "ExtrusionEntityVisitors.hpp"
#include "Fill/FillBase.hpp"
#include "Flow.hpp"
#include "format.hpp"
#include "GCode.hpp"
#include "GCode/ConflictChecker.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "Geometry/ConvexHull.hpp"
#include "I18N.hpp"
#include "PrintObject.hpp"
#include "PrintObjectRegion.hpp"
#include "PrintRegion.hpp"
#include "Printing/PrintingPlan.hpp"
#include "ShortestPath.hpp"
#include "Steps/StepPipeline.hpp"
#include "Thread.hpp"
#include "Utils.hpp"

namespace Slic3r {

template class PrintState<PrintObjectStep, posCount>;

namespace {

using SlicingStepArray = std::array<slicing_step_t, 10>;
using PrintStepArray = std::array<slicing_step_t, 6>;

const SlicingStepArray& ordered_object_steps()
{
    static const SlicingStepArray steps {
        posSlice,
        posPerimeters,
        posPrepareInfill,
        posInfill,
        posIroning,
        posSupportSpotsSearch,
        posSupportMaterial,
        posEstimateCurledExtrusions,
        posCalculateOverhangingPerimeters,
        posSimplifyPath
    };
    return steps;
}

const PrintStepArray& ordered_print_steps()
{
    static const PrintStepArray steps {
        psAlertWhenSupportsNeeded,
        psSkirtBrim,
        psToolOrdering,
        psWipeTower,
        psCheckConflict,
        psGCodeExport
    };
    return steps;
}

size_t enabled_step_count(const SlicingStepArray &steps, int requested_step)
{
    if (requested_step == -1)
        return steps.size();
    SlicingStepArray::const_iterator it = std::find(steps.begin(), steps.end(), static_cast<slicing_step_t>(requested_step));
    return it == steps.end() ? steps.size() : size_t(std::distance(steps.begin(), it)) + 1;
}

PrintStateBase::StateWithTimeStamp aggregate_step_state(const PrintObjectUPtrs &objects, slicing_step_t step)
{
    PrintStateBase::StateWithTimeStamp result;
    if (objects.empty()) {
        result.enabled = false;
        return result;
    }

    bool all_done = true;
    bool any_started = false;
    bool any_canceled = false;
    bool any_invalidated = false;
    bool any_enabled = false;

    for (const PrintObjectUPtr &object : objects) {
        PrintStateBase::StateWithTimeStamp state = object->step_state_with_timestamp(step);
        result.timestamp = std::max(result.timestamp, state.timestamp);
        any_enabled |= state.enabled;
        all_done &= state.state == PrintStateBase::State::Done;
        any_started |= state.state == PrintStateBase::State::Started;
        any_canceled |= state.state == PrintStateBase::State::Canceled;
        any_invalidated |= state.state == PrintStateBase::State::Invalidated;
    }

    result.enabled = any_enabled;
    if (all_done)
        result.state = PrintStateBase::State::Done;
    else if (any_started)
        result.state = PrintStateBase::State::Started;
    else if (any_canceled)
        result.state = PrintStateBase::State::Canceled;
    else if (any_invalidated)
        result.state = PrintStateBase::State::Invalidated;
    return result;
}

void merge_warning(PrintStateBase::StateWithWarnings &state, const PrintStateBase::Warning &warning)
{
    std::vector<PrintStateBase::Warning>::iterator it = warning.message_id == 0 ?
        std::find_if(state.warnings.begin(), state.warnings.end(), [&warning](const PrintStateBase::Warning &existing) {
            return existing.message_id == 0 && existing.message == warning.message;
        }) :
        std::find_if(state.warnings.begin(), state.warnings.end(), [&warning](const PrintStateBase::Warning &existing) {
            return existing.message_id == warning.message_id;
        });

    if (it == state.warnings.end())
        state.warnings.emplace_back(warning);
    else {
        it->current |= warning.current;
        if (warning.level == PrintStateBase::WarningLevel::CRITICAL)
            it->level = warning.level;
        if (it->message != warning.message)
            it->message = warning.message;
    }
}

} // namespace

Print::Print()
{
    // Create config hierarchy.
    m_default_object_config.parent = &m_config;
    m_default_region_config.parent = &m_default_object_config;
    this->reset_step_execution_plan_all();
}

Print::~Print()
{
    this->clear();
}

Printing::PrintingPlan &Print::mutable_printing_plan()
{
    // STEP_ORDERING owns this work copy for the current process() run. Create
    // it lazily so tests and legacy code that do not run ordering do not pay
    // for an empty plan allocation.
    if (!m_printing_plan)
        m_printing_plan = std::make_unique<Printing::PrintingPlan>();
    return *m_printing_plan;
}

void Print::reset_printing_plan()
{
    m_printing_plan.reset();
}

void Print::set_task(const TaskParams &params)
{
    const SlicingStepArray &object_steps = ordered_object_steps();
    const PrintStepArray &print_steps = ordered_print_steps();

    std::scoped_lock<std::mutex> lock(this->state_mutex());

    size_t n_object_steps = enabled_step_count(object_steps, params.to_object_step);

    if (params.single_model_object.valid()) {
        PrintObject *print_object = nullptr;
        size_t idx_print_object = 0;
        for (; idx_print_object < m_objects.size(); ++idx_print_object)
            if (m_objects[idx_print_object]->model_object()->id() == params.single_model_object) {
                print_object = m_objects[idx_print_object].get();
                break;
            }
        assert(print_object != nullptr);

        bool running = false;
        for (size_t istep = 0; istep < n_object_steps; ++istep) {
            if (!print_object->is_step_enabled_unguarded(object_steps[istep]))
                break;
            if (print_object->is_step_started_unguarded(object_steps[istep])) {
                running = true;
                break;
            }
        }
        if (!running)
            this->call_cancel_callback();

        if (params.single_model_instance_only) {
            for (PrintObjectUPtr &object : m_objects)
                for (slicing_step_t step : object_steps)
                    object->enable_step_unguarded(step, false);
        } else if (!running && idx_print_object != 0)
            std::swap(m_objects.front(), m_objects[idx_print_object]);

        for (size_t istep = 0; istep < n_object_steps; ++istep)
            print_object->enable_step_unguarded(object_steps[istep], true);
        for (size_t istep = n_object_steps; istep < object_steps.size(); ++istep)
            print_object->enable_step_unguarded(object_steps[istep], false);
    } else {
        bool running = false;
        for (PrintObjectUPtr &object : m_objects) {
            for (size_t istep = 0; istep < n_object_steps; ++istep) {
                if (!object->is_step_enabled_unguarded(object_steps[istep]))
                    goto loop_end;
                if (object->is_step_started_unguarded(object_steps[istep])) {
                    running = true;
                    goto loop_end;
                }
            }
        }
    loop_end:
        if (!running)
            this->call_cancel_callback();
        for (PrintObjectUPtr &object : m_objects) {
            for (size_t istep = 0; istep < n_object_steps; ++istep)
                object->enable_step_unguarded(object_steps[istep], true);
            for (size_t istep = n_object_steps; istep < object_steps.size(); ++istep)
                object->enable_step_unguarded(object_steps[istep], false);
        }
    }

    if (params.to_object_step != -1) {
        for (slicing_step_t step : print_steps)
            for (PrintObjectUPtr &object : m_objects)
                object->enable_step_unguarded(step, false);
    } else if (params.to_print_step != -1) {
        PrintStepArray::const_iterator it = std::find(print_steps.begin(), print_steps.end(), static_cast<slicing_step_t>(params.to_print_step));
        if (it != print_steps.end()) {
            for (++it; it != print_steps.end(); ++it)
                for (PrintObjectUPtr &object : m_objects)
                    object->enable_step_unguarded(*it, false);
        }
    }
}

void Print::finalize()
{
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (PrintObjectUPtr &object : m_objects)
        object->finalize_impl();
}

void Print::clear() {
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
    m_objects.clear();
    m_print_regions.clear();
    m_model.clear_objects();
    this->reset_printing_plan();
    this->reset_step_execution_plan_all();
}

void Print::mark_step_for_execution(slicing_step_t step)
{
    if (step != STEP_NONE)
        m_steps_to_execute.insert(step);
}

void Print::mark_step_and_dependents_for_execution(slicing_step_t step)
{
    if (step == STEP_NONE)
        return;
    if (step == STEP_ANY) {
        this->reset_step_execution_plan_all();
        return;
    }

    this->mark_step_for_execution(step);
    for (slicing_step_t dependent : Steps::dependent_steps_closure(step))
        this->mark_step_for_execution(dependent);
}

bool Print::should_execute_step(slicing_step_t step) const
{
    return m_steps_to_execute.find(step) != m_steps_to_execute.end();
}

void Print::mark_step_executed(slicing_step_t step)
{
    m_steps_to_execute.erase(step);
}

void Print::reset_step_execution_plan_all()
{
    m_steps_to_execute.clear();
    for (slicing_step_t step : Steps::execution_order())
        m_steps_to_execute.insert(step);
}

const PrintObject *Print::get_print_object_by_model_object_id(ObjectID object_id) const {
    auto it = std::find_if(m_objects.begin(), m_objects.end(), [object_id](const PrintObjectUPtr &obj) {
        return obj->model_object()->id() == object_id;
    });
    return (it == m_objects.end()) ? nullptr : it->get();
}

const PrintObject *Print::get_object(ObjectID object_id) const {
    auto it = std::find_if(m_objects.begin(), m_objects.end(),
                           [object_id](const PrintObjectUPtr &obj) { return obj->id() == object_id; });
    return (it == m_objects.end()) ? nullptr : it->get();
}

// Called by Print::apply().
// This method only accepts PrintConfig option keys. Not PrintObjectConfig or PrintRegionConfig, go to PrintObject for these
bool Print::invalidate_state_by_config_options(const ConfigOptionResolver & /* new_config */,
                                               const std::vector<t_config_option_key> &opt_keys) {
    if (opt_keys.empty())
        return false;

    bool invalidated = false;
    for (const t_config_option_key &opt_key : opt_keys) {
        const ConfigOptionDef *def = PrintConfigDef::instance().get(opt_key);
        if (def == nullptr) {
            this->reset_step_execution_plan_all();
            invalidated = true;
            continue;
        }
        if (def->invalidates_step == STEP_NONE)
            continue;
        if (def->invalidates_step == STEP_ANY)
            this->reset_step_execution_plan_all();
        else
            this->mark_step_and_dependents_for_execution(def->invalidates_step);
        invalidated = true;
    }
    if (invalidated)
        m_timestamp_last_change = std::time(0);
    return invalidated;
}

bool Print::invalidate_step(slicing_step_t step)
{
    // Deprecated legacy invalidation. Keep it wired to the new execution plan
    // while old callers still invalidate historical PrintObject/PrintState
    // milestones directly.
    this->mark_step_and_dependents_for_execution(step);

    bool invalidated = false;
    if (is_print_object_step(step)) {
        for (PrintObjectUPtr &object : m_objects)
            invalidated |= object->invalidate_step(step);
        return invalidated;
    }

    for (PrintObjectUPtr &object : m_objects)
        invalidated |= object->invalidate_step_direct(step);
    // Propagate print-level steps downstream in the slicing pipeline.
    for (slicing_step_t print_step : ordered_print_steps())
        if (print_step > step)
            for (PrintObjectUPtr &object : m_objects)
                invalidated |= object->invalidate_step_direct(print_step);
    if (step != psGCodeExport)
        for (PrintObjectUPtr &object : m_objects)
            invalidated |= object->invalidate_step_direct(psGCodeExport);
    return invalidated;
}

bool Print::invalidate_steps(std::initializer_list<slicing_step_t> steps)
{
    bool invalidated = false;
    for (slicing_step_t step : steps)
        invalidated |= this->invalidate_step(step);
    return invalidated;
}

bool Print::invalidate_all_steps()
{
    // Deprecated legacy invalidation. It remains the compatibility entry point
    // for old code paths, but the new pipeline consumes m_steps_to_execute.
    this->reset_step_execution_plan_all();

    bool invalidated = false;
    for (PrintObjectUPtr &object : m_objects)
        invalidated |= object->invalidate_all_steps_direct();
    return invalidated;
}

bool Print::is_step_done(slicing_step_t step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const PrintObjectUPtr &object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

PrintStateBase::StateWithTimeStamp Print::step_state_with_timestamp(slicing_step_t step) const
{
    return aggregate_step_state(m_objects, step);
}

PrintStateBase::StateWithWarnings Print::step_state_with_warnings(slicing_step_t step) const
{
    PrintStateBase::StateWithWarnings result;
    PrintStateBase::StateWithTimeStamp state = aggregate_step_state(m_objects, step);
    result.state = state.state;
    result.timestamp = state.timestamp;
    result.enabled = state.enabled;

    for (const PrintObjectUPtr &object : m_objects) {
        PrintStateBase::StateWithWarnings object_state = object->step_state_with_warnings(step);
        for (const PrintStateBase::Warning &warning : object_state.warnings)
            merge_warning(result, warning);
    }
    return result;
}

bool Print::set_started(slicing_step_t step)
{
    if (m_objects.empty())
        return false;
    for (const PrintObjectUPtr &object : m_objects) {
        PrintStateBase::StateWithTimeStamp state = object->step_state_with_timestamp(step);
        if (!state.enabled || state.state == PrintStateBase::State::Done)
            return false;
    }

    bool started = false;
    for (PrintObjectUPtr &object : m_objects)
        started |= object->set_started(step);
    return started;
}

PrintStateBase::TimeStamp Print::set_done(slicing_step_t step)
{
    PrintStateBase::TimeStamp timestamp = 0;
    for (PrintObjectUPtr &object : m_objects) {
        PrintStateBase::StateWithTimeStamp state = object->step_state_with_timestamp(step);
        if (state.state == PrintStateBase::State::Started)
            timestamp = std::max(timestamp, object->set_done(step));
        else if (state.state == PrintStateBase::State::Done)
            timestamp = std::max(timestamp, state.timestamp);
    }
    return timestamp;
}

void Print::active_step_add_warning(PrintStateBase::WarningLevel warning_level, const std::string &message, int message_id)
{
    for (PrintObjectUPtr &object : m_objects) {
        bool has_active_step = false;
        for (slicing_step_t step : { psAlertWhenSupportsNeeded, psSkirtBrim, psWipeTower, psGCodeExport }) {
            PrintStateBase::StateWithTimeStamp state = object->step_state_with_timestamp(step);
            if (state.state == PrintStateBase::State::Started) {
                has_active_step = true;
                break;
            }
        }
        if (has_active_step)
            object->active_step_add_warning(warning_level, message, message_id);
    }
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::object_extruders(const PrintObjectPtrs &objects, coord_t z /*= -1*/) const
{
    std::set<uint16_t> extruders;
    for (const PrintObject *object : objects) {
        std::vector<std::reference_wrapper<const PrintRegion>> ok_regions;
        if (z < 0) {
            ok_regions = object->all_regions();
        } else {
            std::set<const PrintRegion*> region_set;
            for (const Layer &layer : object->layers()) {
                if ((layer.scaled_bottom_z()) <= z && z <= layer.scaled_print_z() ) {
                    for (const LayerSliceIsland &layer_island_ptr : layer.islands()) {
                        for (const LayerRegionIsland &region_island_ptr : layer_island_ptr.regions_islands()) {
                            if (region_island_ptr.has_extrusions()) {
                                for (const LayerRegion *lr : region_island_ptr.regions()) {
                                    region_set.insert(&lr->region());
                                }
                            }
                        }
                    }
                }
            }
            for (const PrintRegion *lr : region_set) {
                ok_regions.emplace_back(*lr);
            }
            assert(ok_regions.size() == region_set.size());
        }
        for (const PrintRegion &region : ok_regions) {
            region.collect_object_printing_extruders(*object->print(), extruders);
        }
    }
    return extruders;
}
std::set<uint16_t> Print::object_extruders(coord_t z /*= -1*/) const
{
    PrintObjectPtrs objects;
    objects.reserve(m_objects.size());
    for (const PrintObjectUPtr &object : m_objects)
        objects.emplace_back(object.get());
    return object_extruders(objects, z);
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::support_material_extruders(coord_t z /*= -1*/) const
{
    std::set<uint16_t> extruders;
    bool support_uses_current_extruder = false;
    auto num_extruders = (uint16_t)m_config.nozzle_diameter.size();

    for (const PrintObjectUPtr &object : m_objects) {
        if (object->has_support_material()) {
            bool has_support = true;
            bool has_support_interface = object->config().support_material_interface_layers > 0;
            if (z >= 0) {
                has_support = false;
                for (const Layer &suppl : object->auxiliary_layers()) {
                    if (suppl.get_property<LayerSupportProperty>() != nullptr &&
                        suppl.scaled_bottom_z() <= z &&
                        z <= suppl.scaled_print_z() &&
                        suppl.has_extrusions()) {
                        has_support = true;
                    }
                }
            }
            if (has_support) {
                assert(object->config().support_material_extruder >= 0);
                if (object->config().support_material_extruder == 0)
                    support_uses_current_extruder = true;
                else {
                    uint16_t i = (uint16_t) object->config().support_material_extruder - 1;
                    extruders.insert((i >= num_extruders) ? 0 : i);
                }
            }
            if (has_support && has_support_interface) {
                assert(object->config().support_material_interface_extruder >= 0);
                if (object->config().support_material_interface_extruder == 0)
                    support_uses_current_extruder = true;
                else {
                    uint16_t i = (uint16_t)object->config().support_material_interface_extruder - 1;
                    extruders.insert((i >= num_extruders) ? 0 : i);
                }
            }
        }
    }

    if (support_uses_current_extruder)
        // Add all object extruders to the support extruders as it is not know which one will be used to print supports.
        append(extruders, this->object_extruders());
    
    return extruders;
}

// returns 0-based indices of used extruders
std::set<uint16_t> Print::extruders(coord_t z /*= -1*/) const
{
    std::set<uint16_t> extruders = this->object_extruders(z);
    append(extruders, this->support_material_extruders(z));

    if (z < 0) {
        // The wipe tower extruder can also be set. When the wipe tower is enabled and it will be generated,
        // append its extruder into the list too.
        if (has_wipe_tower() && m_default_object_config.wipe_tower_extruder != 0 && extruders.size() > 1) {
            assert(m_default_object_config.wipe_tower_extruder > 0 &&
                   m_default_object_config.wipe_tower_extruder < int(m_config.nozzle_diameter.size()));
            extruders.insert(uint16_t(m_default_object_config.wipe_tower_extruder.value - 1)); // the config value is 1-based
        }
    }

    return extruders;
}

uint16_t Print::num_object_instances() const
{
    uint16_t instances = 0;
    for (const PrintObjectUPtr &print_object : m_objects)
        instances += (uint16_t)print_object->instances().size();
    return instances;
}

double Print::max_allowed_layer_height() const
{
    double nozzle_diameter_max = 0.;
    for (unsigned int extruder_id : this->extruders())
        nozzle_diameter_max = std::max(nozzle_diameter_max, m_config.nozzle_diameter.get_at(extruder_id));
    return nozzle_diameter_max;
}

std::vector<ObjectID> Print::print_object_ids() const 
{ 
    std::vector<ObjectID> out; 
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const PrintObjectUPtr &print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

bool Print::has_infinite_skirt() const
{
    return (m_config.draft_shield.value == dsEnabled && m_config.skirts > 0)/* || (m_config.ooze_prevention && this->extruders().size() > 1)*/;
}

bool Print::has_skirt() const
{
    return (m_config.skirt_height > 0 && m_config.skirts > 0) || this->has_infinite_skirt() || m_config.draft_shield.value != dsDisabled;
    // case dsLimited should only be taken into account when skirt_height and skirts are positive,
    // so it is covered by the first condition.
}

bool Print::has_brim() const
{
    return !this->m_brim.empty() || std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObjectUPtr &object) { return object->has_brim(); });
}

bool Print::sequential_print_horizontal_clearance_valid(const Print &print, Polygons* polygons)
{
    if (print.config().extruder_clearance_radius == 0) {
        return true;
    }
    Polygons convex_hulls_other;
    if (polygons != nullptr) {
        polygons->clear();
    }
    std::vector<size_t> intersecting_idxs;

	std::map<ObjectID, Polygon> map_model_object_to_convex_hull;
    const double dist_grow = min_object_distance(static_cast<const ConfigBase*>(&print.full_print_config()), 0);
	for (const PrintObject &print_object : print.objects()) {
        const double object_grow = (print.config().complete_objects && !print_object.config().brim_per_object) ? dist_grow : std::max(dist_grow, print_object.config().brim_width.value);
	    assert(! print_object.model_object()->instances.empty());
	    assert(! print_object.instances().empty());
	    ObjectID model_object_id = print_object.model_object()->id();
	    auto it_convex_hull = map_model_object_to_convex_hull.find(model_object_id);
        // Get convex hull of all printable volumes assigned to this print object.
        ModelInstance *model_instance0 = print_object.model_object()->instances.front();
	      if (it_convex_hull == map_model_object_to_convex_hull.end()) {
	          // Calculate the convex hull of a printable object. 
	          // Grow convex hull with the clearance margin.
	          // FIXME: Arrangement has different parameters for offsetting (jtMiter, limit 2)
	          // which causes that the warning will be showed after arrangement with the
	          // appropriate object distance. Even if I set this to jtMiter the warning still shows up.
            Geometry::Transformation trafo = model_instance0->get_transformation();
            trafo.set_offset(Vec3d{ 0.0, 0.0, model_instance0->get_offset().z() });
            Polygon ch2d = print_object.model_object()->convex_hull_2d(trafo.get_matrix());
            Polygons offs_ch2d = offset(ch2d,
                // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
                // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
                scale_d(0.5 * object_grow - BuildVolume::BedEpsilon), jtRound, scale_d(0.1));
            // for invalid geometries the vector returned by offset() may be empty
            if (!offs_ch2d.empty())
                it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, model_object_id, offs_ch2d.front());
        }
        if (it_convex_hull != map_model_object_to_convex_hull.end()) {
            // Make a copy, so it may be rotated for instances.
            //FIXME seems like the rotation isn't taken into account
            Polygon convex_hull0 = it_convex_hull->second;
            //this can create bugs in macos, for reasons.
            const double z_diff = Geometry::rotation_diff_z(model_instance0->get_matrix(), print_object.instances().front().model_instance->get_matrix());
            if (std::abs(z_diff) > EPSILON)
                convex_hull0.rotate(z_diff);
            // Now we check that no instance of convex_hull intersects any of the previously checked object instances.
            for (const PrintInstance& instance : print_object.instances()) {
                Polygon convex_hull = convex_hull0;
                // instance.shift is a position of a centered object, while model object may not be centered.
                // Convert the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
                convex_hull.translate(instance.shift - print_object.center_offset());
                // if output needed, collect indices (inside convex_hulls_other) of intersecting hulls
                for (size_t i = 0; i < convex_hulls_other.size(); ++i) {
                    if (!intersection(convex_hulls_other[i], convex_hull).empty()) {
                        if (polygons == nullptr)
                            return false;
                        else {
                            intersecting_idxs.emplace_back(i);
                            intersecting_idxs.emplace_back(convex_hulls_other.size());
                        }
                    }
                }
                convex_hulls_other.emplace_back(std::move(convex_hull));
            }
        }
    }

    if (!intersecting_idxs.empty()) {
        // use collected indices (inside convex_hulls_other) to update output
        std::sort(intersecting_idxs.begin(), intersecting_idxs.end());
        intersecting_idxs.erase(std::unique(intersecting_idxs.begin(), intersecting_idxs.end()), intersecting_idxs.end());
        for (size_t i : intersecting_idxs) {
            polygons->emplace_back(std::move(convex_hulls_other[i]));
        }
        return false;
    }
    return true;
}

static inline bool sequential_print_vertical_clearance_valid(const Print &print)
{
	std::vector<const PrintInstance*> print_instances_ordered = print.sort_object_instances_by_model_order();
	// Ignore the last instance printed.
	print_instances_ordered.pop_back();
	// Find the other highest instance.
	auto it = std::max_element(print_instances_ordered.begin(), print_instances_ordered.end(), [](auto l, auto r) {
		return l->print_object->height() < r->print_object->height();
	});
    return it == print_instances_ordered.end() || (*it)->print_object->height() <= scale_i(print.config().extruder_clearance_height.value);
}

coord_t Print::get_object_first_layer_height(const PrintObject& object) const {
    //get object first layer height
    coord_t object_first_layer_height = scale_to_layer_coord(object.config().first_layer_height.value);
    if (object.config().first_layer_height.percent) {
        std::set<uint16_t> object_extruders;
        for (const PrintRegion& region : object.all_regions()) {
            PrintRegion::collect_object_printing_extruders(config(), object.config(), region.config(), object_extruders);
        }
        object_first_layer_height = 1000000000;
        for (uint16_t extruder_id : object_extruders) {
            const double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
            const coord_t first_layer_height = scale_to_layer_coord(object.config().first_layer_height.get_effective_value(nozzle_diameter));
            object_first_layer_height = std::min(object_first_layer_height, first_layer_height);
        }
    }
    assert(object_first_layer_height < 1000000000);
    return object_first_layer_height;
}

coord_t Print::get_min_first_layer_height() const
{
    if (m_objects.empty())
        throw Slic3r::InvalidArgument("first_layer_height() can't be called without PrintObjects");

    coord_t min_layer_height = 10000000000;
    for(const PrintObjectUPtr &obj : m_objects)
        min_layer_height = std::min(min_layer_height, get_object_first_layer_height(*obj));

    if(min_layer_height == 10000000000)
        throw Slic3r::InvalidArgument("first_layer_height() can't be computed");

    return min_layer_height;
}

// Matches "G92 E0" with various forms of writing the zero and with an optional comment.
boost::regex regex_g92e0 { "^[ \\t]*[gG]92[ \\t]*[eE](0(\\.0*)?|\\.0+)[ \\t]*(;.*)?$" };

// Precondition: Print::validate() requires the Print::apply() to be called its invocation.
std::pair<PrintBase::PrintValidationError, std::string> Print::validate(std::vector<std::string>* warnings) const
{
    std::set<uint16_t> extruders = this->extruders();

    if (warnings) {
        for (auto it_a = extruders.begin(); it_a != extruders.end() ;++it_a)
            for (auto it_b = std::next(it_a); it_b != extruders.end() ;++it_b)
                if (std::abs(m_config.bed_temperature.get_at(*it_a) - m_config.bed_temperature.get_at(*it_b)) > 15
                 || std::abs(m_config.first_layer_bed_temperature.get_at(*it_a) - m_config.first_layer_bed_temperature.get_at(*it_b)) > 15) {
                    warnings->emplace_back("_BED_TEMPS_DIFFER");
                    goto DONE;
                }
        DONE:;
    }

    if (m_objects.empty())
        return { PrintBase::PrintValidationError::pveWrongPosition, _u8L("All objects are outside of the print volume.") };

    if (extruders.empty())
        return { PrintBase::PrintValidationError::pveNoPrint, _u8L("The supplied settings will cause an empty print.") };

    if (m_config.complete_objects /*|| m_config.parallel_objects_step > 0*/) {
    	if (! sequential_print_horizontal_clearance_valid(*this, const_cast<Polygons*>(&m_sequential_print_clearance_contours)))
            return { PrintBase::PrintValidationError::pveWrongPosition, _u8L("Some objects are too close; your extruder will collide with them.") };
        if (m_config.complete_objects && ! sequential_print_vertical_clearance_valid(*this))
            return { PrintBase::PrintValidationError::pveWrongPosition,_u8L("Some objects are too tall and cannot be printed without extruder collisions.") };
    }
    else
        const_cast<Polygons*>(&m_sequential_print_clearance_contours)->clear();

    if (m_config.avoid_crossing_perimeters && m_config.avoid_crossing_curled_overhangs) {
        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Avoid crossing perimeters option and avoid crossing curled overhangs option cannot be both enabled together.") };
    }    

    if (m_config.spiral_vase) {
        size_t total_copies_count = 0;
        for (const PrintObjectUPtr &object : m_objects)
            total_copies_count += object->instances().size();
        // #4043
        if (total_copies_count > 1 && ! m_config.complete_objects.value)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Only a single object may be printed at a time in Spiral Vase mode. "
                     "Either remove all but the last object, or enable sequential mode by \"complete_objects\".") };
        assert(m_objects.size() == 1 || config().complete_objects.value);
        if (m_objects.front()->all_regions().size() > 1)
            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Spiral Vase option can only be used when printing single material objects, and without any modifiers that may break the vase.") };
    }

    //if (m_config.machine_limits_usage == MachineLimitsUsage::EmitToGCode && m_config.gcode_flavor == gcfKlipper)
    //    return _u8L("Machine limits cannot be emitted to G-Code when Klipper firmware flavor is used. "
    //             "Change the value of machine_limits_usage.");

    // Cache of layer height profiles for checking:
    // 1) Whether all layers are synchronized if printing with wipe tower and / or unsynchronized supports.
    // 2) Whether layer height is constant for Organic supports.
    // 3) Whether build volume Z is not violated.
    std::vector<std::vector<coordf_t>> layer_height_profiles;
    auto layer_height_profile = [this, &layer_height_profiles](const size_t print_object_idx) -> const std::vector<coordf_t>& {
        const PrintObject       &print_object = *m_objects[print_object_idx];
        if (layer_height_profiles.empty())
            layer_height_profiles.assign(m_objects.size(), std::vector<coordf_t>());
        std::vector<coordf_t>   &profile      = layer_height_profiles[print_object_idx];
        if (profile.empty())
            PrintObject::update_layer_height_profile(*print_object.model_object(), print_object.slicing_parameters(), profile);
        return profile;
    };

    // Checks that the print does not exceed the max print height
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx) {
        const PrintObject &print_object = *m_objects[print_object_idx];
        //FIXME It is quite expensive to generate object layers just to get the print height!
        if (auto layers = generate_object_layers(print_object.slicing_parameters(), layer_height_profile(print_object_idx));
            ! layers.empty() && layers.back() > this->config().max_print_height + EPSILON) {
            return { PrintBase::PrintValidationError::pveWrongPosition, 
                // Test whether the last slicing plane is below or above the print volume.
                0.5 * (layers[layers.size() - 2] + layers.back()) > this->config().max_print_height + EPSILON ?
                format(_u8L("The object %1% exceeds the maximum build volume height."), print_object.model_object()->name) :
                format(_u8L("While the object %1% itself fits the build volume, its last layer exceeds the maximum build volume height."), print_object.model_object()->name) +
                " " + _u8L("You might want to reduce the size of your model or change current print settings and retry.") };
        }
    }

    // Some of the objects has variable layer height applied by painting or by a table.
    bool has_custom_layering = std::find_if(m_objects.begin(), m_objects.end(), 
        [](const PrintObjectUPtr &object) { return object->model_object()->has_custom_layering(); }) 
        != m_objects.end();

    // Custom layering is not allowed for tree supports as of now.
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx)
        if (const PrintObject &print_object = *m_objects[print_object_idx];
            print_object.has_support_material() && print_object.config().support_material_style.value == smsOrganic &&
            print_object.model_object()->has_custom_layering()) {
            if (const std::vector<coordf_t> &layers = layer_height_profile(print_object_idx); ! layers.empty())
                if (! check_object_layers_fixed(print_object.slicing_parameters(), layers))
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Variable layer height is not supported with Organic supports.") };
        }

    if (this->has_wipe_tower() && ! m_objects.empty()) {
        // Make sure all extruders use same diameter filament and have the same nozzle diameter
        // EPSILON comparison is used for nozzles and 10 % tolerance is used for filaments
        double first_nozzle_diam = m_config.nozzle_diameter.get_at(*extruders.begin());
        double first_filament_diam = m_config.filament_diameter.get_at(*extruders.begin());
        //for (const uint16_t& extruder_idx : extruders) {
        //    double nozzle_diam = m_config.nozzle_diameter.get_at(extruder_idx);
        //    double filament_diam = m_config.filament_diameter.get_at(extruder_idx);
        //    if (nozzle_diam - EPSILON > first_nozzle_diam || nozzle_diam + EPSILON < first_nozzle_diam
        //     || std::abs((filament_diam-first_filament_diam)/first_filament_diam) > 0.1)
        //        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The wipe tower is only supported if all extruders have the same nozzle diameter "
        //                 "and use filaments of the same diameter.") };
        //}

        //if (m_config.gcode_flavor != gcfRepRap 
        //    && m_config.gcode_flavor != gcfSprinter
        //    && m_config.gcode_flavor != gcfRepetier 
        //    && m_config.gcode_flavor != gcfMarlinLegacy
        //    && m_config.gcode_flavor != gcfMarlinFirmware
        //    && m_config.gcode_flavor != gcfKlipper )
        //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently only supported for the Marlin, Klipper, RepRap/Sprinter, Repetier and NematX G-code flavors.") };
        //if (! m_config.use_relative_e_distances)
            //return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently only supported with the relative extruder addressing (use_relative_e_distances=1).") };
        //if (m_config.ooze_prevention && m_config.single_extruder_multi_material)
        //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Ooze prevention is currently not supported with the wipe tower enabled.") };
        //if (m_config.use_volumetric_e)
        //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower currently does not support volumetric E (use_volumetric_e=0).") };
        //if (m_config.complete_objects && extruders.size() > 1)
        //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is currently not supported for multimaterial sequential prints.") };

        //if (m_objects.size() > 1) {
        //    const SlicingParameters     &slicing_params0       = m_objects.front()->slicing_parameters();
        //    size_t                       tallest_object_idx    = 0;
        //    for (size_t i = 1; i < m_objects.size(); ++ i) {
        //        const PrintObject       *object         = m_objects[i];
        //        const SlicingParameters &slicing_params = object->slicing_parameters();
        //        if (std::abs(slicing_params.first_print_layer_height - slicing_params0.first_print_layer_height) > EPSILON ||
        //            std::abs(slicing_params.layer_height             - slicing_params0.layer_height            ) > EPSILON)
        //            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they have equal layer heights") };
        //        if (slicing_params.raft_layers() != slicing_params0.raft_layers())
        //            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are printed over an equal number of raft layers") };
        //        if (object->config().support_material_contact_distance_type != m_objects.front()->config().support_material_contact_distance_type
        //            || object->config().support_material_contact_distance.value != m_objects.front()->config().support_material_contact_distance.value
        //            || object->config().support_material_bottom_contact_distance.value != m_objects.front()->config().support_material_bottom_contact_distance.value
        //            || slicing_params0.gap_object_support != slicing_params.gap_object_support
        //            || slicing_params0.gap_support_object != slicing_params.gap_support_object)
        //            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are printed with the same support_material_contact_distance") };
        //        if (! equal_layering(slicing_params, slicing_params0))
        //            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower is only supported for multiple objects if they are sliced equally.") };
        //        if (has_custom_layering) {
        //            auto &lh         = layer_height_profile(i);
        //            auto &lh_tallest = layer_height_profile(tallest_object_idx);
        //            if (*(lh.end()-2) > *(lh_tallest.end()-2))
        //                tallest_object_idx = i;
        //        }
        //    }

        //    if (has_custom_layering) {
        //        for (size_t idx_object = 0; idx_object < m_objects.size(); ++ idx_object) {
        //            if (idx_object == tallest_object_idx)
        //                continue;
        //            // Check that the layer height profiles are equal. This will happen when one object is
        //            // a copy of another, or when a layer height modifier is used the same way on both objects.
        //            // The latter case might create a floating point inaccuracy mismatch, so compare
        //            // element-wise using an epsilon check.
        //            size_t i = 0;
        //            const coordf_t eps = 0.5 * EPSILON; // layers closer than EPSILON will be merged later. Let's make
        //            // this check a bit more sensitive to make sure we never consider two different layers as one.
        //            while (i < layer_height_profiles[idx_object].size()
        //                && i < layer_height_profiles[tallest_object_idx].size()) {
        //                if (i%2 == 0 && layer_height_profiles[tallest_object_idx][i] > layer_height_profiles[idx_object][layer_height_profiles[idx_object].size() - 2 ])
        //                    break;
        //                if (std::abs(layer_height_profiles[idx_object][i] - layer_height_profiles[tallest_object_idx][i]) > eps)
        //                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe tower is only supported if all objects have the same variable layer height") };
        //                ++i;
        //            }
        //        }
        //    }
        //}
    }
    
	{
		// Find the smallest used nozzle diameter and the number of unique nozzle diameters.
		double min_nozzle_diameter = std::numeric_limits<double>::max();
		double max_nozzle_diameter = 0;
		for (uint16_t extruder_id : extruders) {
			double dmr = m_config.nozzle_diameter.get_at(extruder_id);
			min_nozzle_diameter = std::min(min_nozzle_diameter, dmr);
			max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
		}

#if 0
        // We currently allow one to assign extruders with a higher index than the number
        // of physical extruders the machine is equipped with, as the Printer::apply() clamps them.
        unsigned int total_extruders_count = m_config.nozzle_diameter.size();
        for (const auto& extruder_idx : extruders)
            if ( extruder_idx >= total_extruders_count )
                return _u8L("One or more object were assigned an extruder that the printer does not have.");
#endif

        const coord_t print_first_layer_height = get_min_first_layer_height();
        for (const PrintObjectUPtr &object : m_objects) {
            if (object->has_support_material()) {
                if ((object->config().support_material_extruder == 0 || object->config().support_material_interface_extruder == 0) && max_nozzle_diameter - min_nozzle_diameter > EPSILON) {
                    // The object has some form of support and either support_material_extruder or support_material_interface_extruder
                    // will be printed with the current tool without a forced tool change. Play safe, assert that all object nozzles
                    // are of the same diameter.
                    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Printing with multiple extruders of differing nozzle diameters. "
                           "If support is to be printed with the current extruder (support_material_extruder == 0 or support_material_interface_extruder == 0), "
                           "all nozzles have to be of the same diameter.") };
                }
                if (this->has_wipe_tower() && object->config().support_material_style != smsOrganic) {
                    if (object->config().support_material_contact_distance_type.value == zdNone) {
                        // Soluble interface
                        if (! object->config().support_material_synchronize_layers)
                            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("For the Wipe Tower to work with the soluble supports, the support layers need to be synchronized with the object layers.") };
                    }
                //  else {
                //        // Non-soluble interface
                //        if (object->config().support_material_extruder != 0 || object->config().support_material_interface_extruder != 0)
                //            return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("The Wipe Tower currently supports the non-soluble supports only if they are printed with the current extruder without triggering a tool change. "
                //                     "(both support_material_extruder and support_material_interface_extruder need to be set to 0).") };
                //    }
                }
                if (object->config().support_material_style.value == smsOrganic) {
                    float extrusion_width = std::min(
                        support_material_flow(object.get()).width(),
                        support_material_interface_flow(object.get()).width());
                    if (object->config().support_tree_tip_diameter < extrusion_width - EPSILON)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support tree tip diameter must not be smaller than support material extrusion width.") };
                    if (object->config().support_tree_branch_diameter < 2. * extrusion_width - EPSILON)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support branch diameter must not be smaller than 2x support material extrusion width.") };
                    if (object->config().support_tree_branch_diameter < object->config().support_tree_tip_diameter)
                        return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Organic support branch diameter must not be smaller than support tree tip diameter.") };
                }
            }

            // Do we have custom support data that would not be used?
            // Notify the user in that case.
            if (! object->has_support() && warnings) {
                for (const ModelVolume* mv : object->model_object()->volumes) {
                    bool has_enforcers = mv->is_support_enforcer() ||
                        (mv->is_model_part() && mv->supported_facets.has_facets(*mv, EnforcerBlockerType::ENFORCER));
                    if (has_enforcers) {
                        warnings->emplace_back("_SUPPORTS_OFF");
                        break;
                    }
                }
            }
            
            // validate layer_height for each region
            for (const PrintRegion& region : object->all_regions()) {
                std::set<uint16_t> object_extruders;
                PrintRegion::collect_object_printing_extruders(config(), object->config(), region.config(), object_extruders);
                const coord_t object_first_layer_height = get_object_first_layer_height(*object);
                const coord_t layer_height = scale_to_layer_coord(object->config().layer_height.value);
                for (uint16_t extruder_id : object_extruders) {
                    double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
                    const coord_t min_layer_height = scale_to_layer_coord(config().min_layer_height.get_effective_value(nozzle_diameter, extruder_id));
                    coord_t max_layer_height = scale_to_layer_coord(config().max_layer_height.get_effective_value(nozzle_diameter, extruder_id));
                    if (max_layer_height <= 0 || !config().max_layer_height.is_enabled()) {
                        max_layer_height = scale_to_layer_coord(nozzle_diameter * 0.75);
                    }
                    if (min_layer_height > max_layer_height) {
                        return {PrintBase::PrintValidationError::pveWrongSettings,
                                _u8L("Min layer height can't be greater than Max layer height")};
                    }
                    //if (max_layer_height > nozzle_diameter) return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Max layer height can't be greater than nozzle diameter") };
                    double skirt_width = Flow::new_from_config_width(frPerimeter,
                        *Flow::extrusion_width_option("skirt", m_default_region_config),
                        *Flow::extrusion_spacing_option("skirt", m_default_region_config),
                        (float)m_config.nozzle_diameter.get_at(extruder_id), 
                        unscaled(print_first_layer_height),
                        1,0 //don't care, all i want if width from width
                    ).width();
                    //check first layer layer_ranges
                    
                    if (object->shared_regions()->layer_ranges.front().layer_height_range_.first < object_first_layer_height) {
                        if (object_first_layer_height < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings,
                                format(_u8L("First layer height can't be lower than %s"), "min layer height") };
                        for (auto tuple : std::vector<std::pair<double, const char*>>{
                                {nozzle_diameter, "nozzle diameter"},
                                {unscaled(max_layer_height), "max layer height"},
                                {skirt_width, "skirt extrusion width"},
                                {object->config().support_material ?
                                    region.width(FlowRole::frSupportMaterial, true, *object) :
                                    unscaled(object_first_layer_height), "support material extrusion width"},
                                {region.width(FlowRole::frPerimeter, true, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frExternalPerimeter, true, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frInfill, true, *object), "infill extrusion width"},
                                {region.width(FlowRole::frSolidInfill, true, *object), "solid infill extrusion width"},
                                {region.width(FlowRole::frTopSolidInfill, true, *object), "top solid infill extrusion width"},
                            })
                            if (object_first_layer_height > scale_to_layer_coord(tuple.first))
                                return { PrintBase::PrintValidationError::pveWrongSettings,
                                    format(_u8L("First layer height can't be greater than %s"), tuple.second) };

                    }
                    //check not-first layer
                    if (object->shared_regions()->layer_ranges.front().layer_height_range_.second > layer_height) {
                        if (layer_height < min_layer_height)
                            return { PrintBase::PrintValidationError::pveWrongSettings,
                                format(_u8L("Layer height can't be lower than %s"), "min layer height") };
                        for (auto tuple : std::vector<std::pair<double, const char*>>{
                                {nozzle_diameter, "nozzle diameter"},
                                {unscaled(max_layer_height), "max layer height"},
                                {skirt_width, "skirt extrusion width"},
                                {object->config().support_material ?
                                    region.width(FlowRole::frSupportMaterial, false, *object) :
                                    unscaled(layer_height), "support material extrusion width"},
                                {region.width(FlowRole::frPerimeter, false, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frExternalPerimeter, false, *object), "perimeter extrusion width"},
                                {region.width(FlowRole::frInfill, false, *object), "infill extrusion width"},
                                {region.width(FlowRole::frSolidInfill, false, *object), "solid infill extrusion width"},
                                {region.width(FlowRole::frTopSolidInfill, false, *object), "top solid infill extrusion width"},
                            })
                            if (layer_height > scale_to_layer_coord(tuple.first)) {
                                return {PrintBase::PrintValidationError::pveWrongSettings,
                                        format(_u8L("Layer height can't be greater than %s"), tuple.second)};
                            }
                    }
                }
            }

        }
    }
    {
        bool before_layer_gcode_resets_extruder = boost::regex_search(m_config.before_layer_gcode.value, regex_g92e0);
        bool layer_gcode_resets_extruder        = boost::regex_search(m_config.layer_gcode.value, regex_g92e0);
        if (m_config.use_relative_e_distances) {
            // See GH issues #6336 #5073$
            //merill: should add it in gcode.cpp then!
            //if (! before_layer_gcode_resets_extruder && ! layer_gcode_resets_extruder)
            //    return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("Relative extruder addressing requires resetting the extruder position at each layer to prevent loss of floating point accuracy. Add \"G92 E0\" to layer_gcode.") };
        } else {
            if (before_layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("\"G92 E0\" was found in before_layer_gcode, which is incompatible with absolute extruder addressing.") };
            else if (layer_gcode_resets_extruder)
                return { PrintBase::PrintValidationError::pveWrongSettings, _u8L("\"G92 E0\" was found in layer_gcode, which is incompatible with absolute extruder addressing.") };
        }
    }

    return { PrintValidationError::pveNone, std::string() };
}

#if 0
// the bounding box of objects placed in copies position
// (without taking skirt/brim/support material into account)
BoundingBox Print::bounding_box() const
{
    BoundingBox bb;
    for (const PrintObject *object : m_objects)
        for (const PrintInstance &instance : object->instances()) {
            BoundingBox bb2(object->bounding_box());
            bb.merge(bb2.min + instance.shift);
            bb.merge(bb2.max + instance.shift);
        }
    return bb;
}

// the total bounding box of extrusions, including skirt/brim/support material
// this methods needs to be called even when no steps were processed, so it should
// only use configuration values
BoundingBox Print::total_bounding_box() const
{
    // get objects bounding box
    BoundingBox bb = this->bounding_box();
    
    // we need to offset the objects bounding box by at least half the perimeters extrusion width
    Flow perimeter_flow = m_objects.front()->layer(0).region(0).flow(frPerimeter);
    double extra = perimeter_flow.width/2;
    
    // consider support material
    if (this->has_support_material()) {
        extra = std::max(extra, SUPPORT_MATERIAL_MARGIN);
    }
    
    // consider brim and skirt
    if (m_config.brim_width.value > 0) {
        Flow brim_flow = this->brim_flow();
        extra = std::max(extra, m_config.brim_width.value + brim_flow.width/2);
    }
    if (this->has_skirt()) {
        int skirts = m_config.skirts.value + m_config.skirt_brim.value;
        if (skirts == 0 && this->has_infinite_skirt()) skirts = 1;
        double max_skirt_width = 0;
        double max_skirt_spacing = 0;
        for (unsigned int extruder_id : this->object_extruders()) {
            Flow   flow = this->skirt_flow(extruder_id);
            max_skirt_width = std::max(max_skirt_width, flow.width());
            max_skirt_spacing = std::max(max_skirt_spacing, flow.spacing());
        }
        if (m_config.skirt_distance_from_brim)
            extra += m_config.brim_width.value
                + m_config.skirt_distance.value
                + skirts * max_skirt_spacing
                + max_skirt_width / 2;
        else
            extra += std::max(
                m_config.brim_width.value,
                m_config.skirt_distance.value
                    + skirts * max_skirt_spacing
                    + max_skirt_width / 2);
    }
    
    if (extra > 0)
        bb.offset(scale_(extra));
    
    return bb;
}
#endif

Flow Print::brim_flow(size_t extruder_id, const PrintObjectConfig& brim_config) const
{
    //use default region, but current object config.
    PrintRegionConfig tempConf = m_default_region_config;
    tempConf.parent = &brim_config;
    return Flow::new_from_config_width(
        frPerimeter,
        *Flow::extrusion_width_option("brim", tempConf),
        *Flow::extrusion_spacing_option("brim", tempConf),
        (float)m_config.nozzle_diameter.get_at(extruder_id),
        (float)unscaled(get_min_first_layer_height()),
        (extruder_id < m_config.nozzle_diameter.size()) ? brim_config.get_computed_value("filament_max_overlap", extruder_id) : 1
    );
}

Flow Print::skirt_flow(size_t extruder_id, bool first_layer/*=false*/) const
{
    if (m_objects.empty())
        throw Slic3r::InvalidArgument("skirt_first_layer_height() can't be called without PrintObjects");

    //get extruder used to compute first layer height
    double max_nozzle_diam = 0.f;
    for (const PrintObjectUPtr &pobject : m_objects) {
        PrintObject& object = *pobject;
        std::set<uint16_t> object_extruders;
        for (const PrintRegion& region : pobject->all_regions()) {
            PrintRegion::collect_object_printing_extruders(config(), object.config(), region.config(), object_extruders);
        }
        //get object first layer extruder diam
        for (uint16_t extruder_id : object_extruders) {
            double nozzle_diameter = config().nozzle_diameter.get_at(extruder_id);
            max_nozzle_diam = std::max(max_nozzle_diam, nozzle_diameter);
        }
    }

    
    //send m_default_object_config becasue it's the lowest config needed (extrusion_option need config from object & print)
    return Flow::new_from_config_width(
        frPerimeter,
        *Flow::extrusion_width_option("skirt", m_default_region_config),
        *Flow::extrusion_spacing_option("skirt", m_default_region_config),
        (float)max_nozzle_diam,
        (float)unscaled(get_min_first_layer_height()),
        1 // hard to say what extruder we have here(many) m_default_region_config.get_computed_value("filament_max_overlap", extruder -1),
    );
    
}

bool Print::has_support_material() const
{
    for (const PrintObjectUPtr &object : m_objects)
        if (object->has_support_material()) 
            return true;
    return false;
}

/*  This method assigns extruders to the volumes having a material
    but not having extruders set in the volume config. */
void Print::auto_assign_extruders(ModelObject* model_object) const
{
    // only assign extruders if object has more than one volume
    if (model_object->volumes.size() < 2)
        return;
    
//    size_t extruders = m_config.nozzle_diameter.size();
    for (size_t volume_id = 0; volume_id < model_object->volumes.size(); ++ volume_id) {
        ModelVolume *volume = model_object->volumes[volume_id];
        //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
        if ((volume->is_model_part() || volume->is_modifier()) && ! volume->material_id().empty() && ! volume->config.has("extruder"))
            volume->config.set("extruder", int(volume_id + 1));
    }
}

#ifdef _DEBUG
class CheckOrientation : public ExtrusionVisitorRecursiveConst
{
public:
    bool ccw;
    CheckOrientation(bool is_ccw) : ExtrusionVisitorRecursiveConst() {ccw = (is_ccw);}
    void default_use(const ExtrusionEntity &entity) override {
        if (!entity.is_leaf()) {
            ExtrusionVisitorRecursiveConst::default_use(entity);
            if (entity.is_loop()) {
                Polygon polygon(entity.as_polyline().to_polyline().points);
                assert(polygon.is_counter_clockwise() == ccw);
            }
        }
    }
};
#endif

#if 1
void Print::process() {
    m_timestamp_last_change = std::time(0);
    name_tbb_thread_pool_threads_set_locale();
    BOOST_LOG_TRIVIAL(info) << "Starting the step pipeline slicing process." << log_memory_info();
    secondary_status_counter_reset();

    // Print::process() is the GUI and CLI entry point for preparing all slicing
    // data. G-code export is now a separate Orchestrator pipeline phase because
    // it needs the final output path and should only run when the user exports.
    Orchestrator::instance().slice(*this);

    // bits not already moved by the new pipeline
    
    // Tool ordering
    if (this->set_started(psWipeTower)) {
        //m_ordering.clear();
        //if (this->config().complete_objects.value || config().parallel_objects_step.value > 0) {
        //    //an ordering per object
        //}
        //ml_ordering.

        //m_wipe_tower_data.clear();
        m_tool_orderings.clear();
        //if (this->has_wipe_tower()) {
        //    assert(!this->config().complete_objects.value && config().parallel_objects_step.value == 0);
        //    this->set_status(printstep_percent(psWipeTower), _u8L("Generating wipe tower"));
        //    // Let the Toolordering class know there will be initial priming extrusions at the start of the print.
        //    m_tool_orderings.emplace_back(*this, (uint16_t) -1, true);
        //    this->_make_wipe_tower();
        //    m_tool_orderings.back().assign_custom_gcodes(*this);
        //} else
        bool is_separate_objects = this->config().complete_objects.value || config().parallel_objects_step.value > 0;
        if (config().parallel_objects_step.value > 0 && config().parallel_islands.value && m_default_object_config.wipe_tower && m_objects.size() == 1) {
            is_separate_objects = false;
        }
        if (is_separate_objects) {
            //throw new std::exception();
            // FIXME: parallel_objects_step: end extruder on each pass isn't computed correctly.
            // TODO: add extruder-switch minimizing option.
            // Order object instances for sequential print.
            std::vector<const PrintInstance *> instances_ordering;
            if (config().complete_objects_sort.value == cosObject)
                instances_ordering = this->sort_object_instances_by_model_order();
            else if (config().complete_objects_sort.value == cosZ)
                instances_ordering = this->sort_object_instances_by_max_z();
            else if (config().complete_objects_sort.value == cosY)
                instances_ordering = this->sort_object_instances_by_max_y();
            else if (config().complete_objects_sort.value == cosNearest)
                instances_ordering = chain_print_object_instances(*this);
            // Find the 1st printing object, find its tool ordering and the initial extruder ID.
            uint16_t previous_final_extruder = -1;
            for (auto it = instances_ordering.begin(); it != instances_ordering.end(); ++it) {
                m_tool_orderings.emplace_back(*(*it)->print_object, previous_final_extruder);
                // last_extruder == -1 => nothign to print, so skip
                if (m_tool_orderings.back().last_extruder() != uint16_t(-1)) {
                    previous_final_extruder = m_tool_orderings.back().last_extruder();
                }
            }
            this->m_wipe_tower2.reset(new WipeTower2());
        } else {
            // Initialize the tool ordering, so it could be used by the G-code preview slider for planning tool
            // changes and filament switches.
            m_tool_orderings.emplace_back(*this, -1, /*prime_mmu*/ true /*false*/);
            if (m_tool_orderings.empty() || m_tool_orderings.back().last_extruder() == uint16_t(-1))
                throw Slic3r::SlicingError(
                    "The print is empty. The model is not printable with current print settings.");

            // add colorchange/pause/custom
            m_tool_orderings.back().assign_custom_gcodes(*this);

            // now create the wipe tower to move from extruder to the next one.
            this->m_wipe_tower2.reset(new WipeTower2());
            if (m_default_object_config.wipe_tower) {
                this->m_wipe_tower2->set_config(&this->config(), &this->default_object_config(),
                                                &this->default_region_config());

                assert(m_tool_orderings.size() == 1);
                PrintObjectPtrs objects;
                objects.reserve(m_objects.size());
                for (const PrintObjectUPtr &object : m_objects)
                    objects.emplace_back(object.get());
                this->m_wipe_tower2->init(this, objects, this->m_tool_orderings.back());

                this->set_done(psWipeTower);
                // fill wtdata
                {
                    std::scoped_lock<std::mutex> lock(m_wipe_tower_data_mutex);
                    m_wipe_tower_data.height = -1;
                    m_wipe_tower_data.z_and_depth_pairs.clear();
                    for (auto &wt_layer : this->m_wipe_tower2->m_WTLayer_data) {
                        m_wipe_tower_data.height = std::max(m_wipe_tower_data.height,
                                                            (float) unscaled(wt_layer->extrusion_z));
                        m_wipe_tower_data.z_and_depth_pairs.emplace_back((float) unscaled(wt_layer->extrusion_z),
                                                                         (float) unscaled(
                                                                             wt_layer->estimated_wipe_tower_length));
                    }
                    std::sort(m_wipe_tower_data.z_and_depth_pairs.begin(), m_wipe_tower_data.z_and_depth_pairs.end(),
                              [](std::pair<float, float> &e1, std::pair<float, float> &e2) {
                                  return e1.first < e2.first;
                              });
                }
            }
        }
    }
    
    /*
    STEP_SKIRT_BRIM is now part of Orchestrator::slice(). Do not call the
    legacy _make_skirt_brim() here, otherwise plugin-generated brim/skirt would
    be cleared or duplicated after the pipeline has already produced it.
    */
    secondary_status_counter_reset();

    m_timestamp_last_change = std::time(0);
    BOOST_LOG_TRIVIAL(info) << "Step pipeline slicing process finished." << log_memory_info();
    this->set_status(printstep_percent(psGCodeExport), L("Slicing done"), SlicingStatus::FlagBits::SLICING_ENDED);
}
#else
// Slicing process, running at a background thread.
void Print::process()
{
    m_timestamp_last_change = std::time(0);
    name_tbb_thread_pool_threads_set_locale();
    bool something_done = !this->is_step_done(psSkirtBrim);
    BOOST_LOG_TRIVIAL(info) << "Starting the slicing process." << log_memory_info();
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            m_objects[idx]->make_perimeters();
        }
    );
#ifdef _DEBUG
    for (const PrintObjectUPtr &obj : m_objects)
        for (const Layer &lay : obj->layers())
            for (const LayerSliceIsland &layer_island_ptr : lay.islands())
                for (const LayerRegionIsland &lri : layer_island_ptr.regions_islands())
                    if(lri.has_extrusion(LayerRegionIsland::PERIMETERS))
                        lri.extrusion(LayerRegionIsland::PERIMETERS).visit(ptvisitor);
#endif
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            m_objects[idx]->infill();
        }
    );
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            m_objects[idx]->ironing();
        }
    );

    // The following step writes to m_shared_regions, it should not run in parallel.
    //FIXME: only run it when the support is needed.
    secondary_status_counter_reset();
    for (PrintObjectUPtr &obj : m_objects)
        obj->generate_support_spots();
    // check data from previous step, format the error message(s) and send alert to ui
    // this also has to be done sequentially.
    alert_when_supports_needed();
    
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            m_objects[idx]->generate_support_material();
        }
    );
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            PrintObject &obj = *m_objects[idx];
            m_objects[idx]->estimate_curled_extrusions();
        }
    );
    secondary_status_counter_reset();
    Slic3r::parallel_for(size_t(0), m_objects.size(),
        [this](const size_t idx) {
            m_objects[idx]->calculate_overhanging_perimeters();
        }
    );

    // Tool ordering
    if (this->set_started(psWipeTower)) {
        //m_ordering.clear();
        //if (this->config().complete_objects.value || config().parallel_objects_step.value > 0) {
        //    //an ordering per object
        //}
        //ml_ordering.

        //m_wipe_tower_data.clear();
        m_tool_orderings.clear();
        //if (this->has_wipe_tower()) {
        //    assert(!this->config().complete_objects.value && config().parallel_objects_step.value == 0);
        //    this->set_status(printstep_percent(psWipeTower), _u8L("Generating wipe tower"));
        //    // Let the Toolordering class know there will be initial priming extrusions at the start of the print.
        //    m_tool_orderings.emplace_back(*this, (uint16_t) -1, true);
        //    this->_make_wipe_tower();
        //    m_tool_orderings.back().assign_custom_gcodes(*this);
        //} else
        bool is_separate_objects = this->config().complete_objects.value || config().parallel_objects_step.value > 0;
        if (config().parallel_objects_step.value > 0 && config().parallel_islands.value && m_default_object_config.wipe_tower && m_objects.size() == 1) {
            is_separate_objects = false;
        }
        if (is_separate_objects) {
            //throw new std::exception();
            // FIXME: parallel_objects_step: end extruder on each pass isn't computed correctly.
            // TODO: add extruder-switch minimizing option.
            // Order object instances for sequential print.
            std::vector<const PrintInstance *> instances_ordering;
            if (config().complete_objects_sort.value == cosObject)
                instances_ordering = this->sort_object_instances_by_model_order();
            else if (config().complete_objects_sort.value == cosZ)
                instances_ordering = this->sort_object_instances_by_max_z();
            else if (config().complete_objects_sort.value == cosY)
                instances_ordering = this->sort_object_instances_by_max_y();
            else if (config().complete_objects_sort.value == cosNearest)
                instances_ordering = chain_print_object_instances(*this);
            // Find the 1st printing object, find its tool ordering and the initial extruder ID.
            uint16_t previous_final_extruder = -1;
            for (auto it = instances_ordering.begin(); it != instances_ordering.end(); ++it) {
                m_tool_orderings.emplace_back(*(*it)->print_object, previous_final_extruder);
                // last_extruder == -1 => nothign to print, so skip
                if (m_tool_orderings.back().last_extruder() != uint16_t(-1)) {
                    previous_final_extruder = m_tool_orderings.back().last_extruder();
                }
            }
            this->m_wipe_tower2.reset(new WipeTower2());
        } else {
            // Initialize the tool ordering, so it could be used by the G-code preview slider for planning tool
            // changes and filament switches.
            m_tool_orderings.emplace_back(*this, -1, /*prime_mmu*/ true /*false*/);
            if (m_tool_orderings.empty() || m_tool_orderings.back().last_extruder() == uint16_t(-1))
                throw Slic3r::SlicingError(
                    "The print is empty. The model is not printable with current print settings.");

            // add colorchange/pause/custom
            m_tool_orderings.back().assign_custom_gcodes(*this);

            // now create the wipe tower to move from extruder to the next one.
            this->m_wipe_tower2.reset(new WipeTower2());
            if (m_default_object_config.wipe_tower) {
                this->m_wipe_tower2->set_config(&this->config(), &this->default_object_config(),
                                                &this->default_region_config());

                assert(m_tool_orderings.size() == 1);
                PrintObjectPtrs objects;
                objects.reserve(m_objects.size());
                for (const PrintObjectUPtr &object : m_objects)
                    objects.emplace_back(object.get());
                this->m_wipe_tower2->init(this, objects, this->m_tool_orderings.back());

                this->set_done(psWipeTower);
                // fill wtdata
                {
                    std::scoped_lock<std::mutex> lock(m_wipe_tower_data_mutex);
                    m_wipe_tower_data.height = -1;
                    m_wipe_tower_data.z_and_depth_pairs.clear();
                    for (auto &wt_layer : this->m_wipe_tower2->m_WTLayer_data) {
                        m_wipe_tower_data.height = std::max(m_wipe_tower_data.height,
                                                            (float) unscaled(wt_layer->extrusion_z));
                        m_wipe_tower_data.z_and_depth_pairs.emplace_back((float) unscaled(wt_layer->extrusion_z),
                                                                         (float) unscaled(
                                                                             wt_layer->estimated_wipe_tower_length));
                    }
                    std::sort(m_wipe_tower_data.z_and_depth_pairs.begin(), m_wipe_tower_data.z_and_depth_pairs.end(),
                              [](std::pair<float, float> &e1, std::pair<float, float> &e2) {
                                  return e1.first < e2.first;
                              });
                }
            }
        }
    }
    
    secondary_status_counter_reset();
    _make_skirt_brim();

    if (this->has_wipe_tower()) {
        // These values have to be updated here, not during wipe tower generation.
        // When the wipe tower is moved/rotated, it is not regenerated.
        m_wipe_tower_data.position = { m_default_object_config.wipe_tower_x, m_default_object_config.wipe_tower_y };
        m_wipe_tower_data.rotation_angle = m_default_object_config.wipe_tower_rotation_angle;
    }
    this->set_status(printstep_percent(psCheckConflict), _u8L("Checking line conflicts"));
    PrintObjectPtrs objects;
    objects.reserve(m_objects.size());
    for (const PrintObjectUPtr &object : m_objects)
        objects.emplace_back(object.get());
    auto conflictRes = ConflictChecker::find_inter_of_lines_in_diff_objs(objects, m_wipe_tower_data);

    m_conflict_result = conflictRes;
    if (conflictRes.has_value())
        BOOST_LOG_TRIVIAL(error) << boost::format("gcode path conflicts found between %1% and %2%") % conflictRes->_objName1 % conflictRes->_objName2;

#ifdef _DEBUG
    for (const PrintObjectUPtr &obj : m_objects)
        for (const Layer &lay : obj->layers())
            for (const LayerSliceIsland &layer_island_ptr : lay.islands())
                for (const LayerRegionIsland &lri : layer_island_ptr.regions_islands())
                    if(lri.has_extrusion(LayerRegionIsland::PERIMETERS))
                        lri.extrusion(LayerRegionIsland::PERIMETERS).visit(ptvisitor);
#endif
    //simplify / make arc fitting
    {
        
#if _DEBUG
        class GetLoopsVisitor : public ExtrusionVisitorRecursive {
        public:
            using ExtrusionVisitorRecursive::use;
            std::vector<ExtrusionLoop*> loops;
            virtual void default_use(ExtrusionEntity& entity) override {
                if (ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(&entity))
                    loops.push_back(loop);
                else
                    ExtrusionVisitorRecursive::default_use(entity);
            }
        } get_loops;
#endif
        const bool spiral_mode = config().spiral_vase;
        const bool enable_arc_fitting = config().arc_fitting.value != ArcFittingType::Disabled && !spiral_mode;
        if (enable_arc_fitting) {
            this->set_status(objectstep_percent(posSimplifyPath), L("Creating arcs"));
        } else {
            this->set_status(objectstep_percent(posSimplifyPath), L("Simplifying paths"));
        }
        secondary_status_counter_reset();
        for (PrintObjectUPtr &obj : m_objects) {
            obj->simplify_extrusion_path();
        }
        //also simplify object skirt & brim
        if (enable_arc_fitting && (!this->m_skirt.empty() || !this->m_brim.empty())) {
            coordf_t scaled_resolution = scale_d(config().arc_fitting_resolution.get_effective_value(config().resolution.value));
            if (scaled_resolution == 0) scaled_resolution = SCALED_EPSILON * 2 ;
            const ConfigOptionFloatOrPercent& arc_fitting_tolerance = config().arc_fitting_tolerance;

            this->set_status(0, L("Optimizing skirt & brim %s%%"), { std::to_string(0) }, PrintBase::SlicingStatus::SECONDARY_STATE);
            std::atomic<int> atomic_count{ 0 };
            GetPathsVisitor visitor;
            visitor.traverse(this->m_skirt);
            visitor.traverse(this->m_brim);
#if _DEBUG
            this->m_skirt.visit(get_loops);
            for (auto loop : get_loops.loops) assert(loop->is_counter_clockwise());
#endif
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, visitor.paths.size()),
                [this, &visitor, scaled_resolution, &arc_fitting_tolerance, &atomic_count](const tbb::blocked_range<size_t>& range) {
                    assert(range.end() <= visitor.paths.size());
                    for (size_t path_idx = range.begin(); path_idx < range.end(); ++path_idx) {
                        const ExtrusionAttributes *attributes = visitor.paths[path_idx]->get_property<ExtrusionAttributes>();
                        assert(attributes != nullptr);
                        if (attributes != nullptr)
                            SimplifyVisitor::simplify(*visitor.paths[path_idx], scaled_resolution, config().arc_fitting.value, scale_d(arc_fitting_tolerance.get_effective_value(attributes->width)));
                        int nb_items_done = (++atomic_count);
                        this->set_status(int((nb_items_done * 100) / (visitor.paths.size())), L("Optimizing skirt & brim %s%%"), { std::to_string(int(100*nb_items_done / double(visitor.paths.size()))) }, PrintBase::SlicingStatus::SECONDARY_STATE);
                    }
                }
            );
#if _DEBUG
            get_loops.loops.clear();
            this->m_skirt.visit(get_loops);
            for (auto loop : get_loops.loops) {
                assert(loop->is_counter_clockwise());
            }
#endif
        }
    }
    
#if _DEBUG
    for (const PrintObjectUPtr &obj : m_objects) {
        for (const std::unique_ptr<Layer> &l : obj->m_layers) {
            for (const LayerSliceIsland &layer_island_ptr : l->islands())
                for (const LayerRegionIsland &lri : layer_island_ptr.regions_islands())
                    if (lri.has_extrusion(LayerRegionIsland::PERIMETERS)) {
                        LoopAssertVisitor().traverse(lri.extrusion(LayerRegionIsland::PERIMETERS));
                    }
        }
    }
#endif

    m_timestamp_last_change = std::time(0);
    BOOST_LOG_TRIVIAL(info) << "Slicing process finished." << log_memory_info();
    //notify gui that the slicing/preview structs are ready to be drawed
    if (something_done)
        this->set_status(printstep_percent(psGCodeExport), L("Slicing done"),
                         SlicingStatus::FlagBits::SLICING_ENDED);
}
#endif

bool has_brim_patch(const PrintObject &obj, ModelVolumeType brim_type)
{
    bool found = false;
    for (const ModelVolume *v : obj.model_object()->volumes) {
        assert(v);
        if (v->type() == brim_type) {
            found = true;
            break;
        }
    }
    return found;
}
bool has_brim_patch(const std::vector<PrintObject*> &objs_group, ModelVolumeType brim_type)
{
    bool found = false;
    for (const PrintObject *obj : objs_group) {
        if (has_brim_patch(*obj, brim_type)) {
            found = true;
            break;
        }
    }
    return found;
}

struct ExtrusionDirectionSetter : public ExtrusionVisitorRecursive {
    using ExtrusionVisitorRecursive::use;
    bool m_set_cw;
    ExtrusionDirectionSetter() : m_set_cw(true) {}
    ExtrusionDirectionSetter(bool set_cw) : m_set_cw(set_cw) {}
    virtual void default_use(ExtrusionEntity& entity) override {
        if (entity.is_loop()) {
            Polygon polygon(entity.as_polyline().to_polyline().points);
            if (polygon.is_counter_clockwise() == m_set_cw ) {
                entity.reverse();
            }
            return;
        }
        ExtrusionVisitorRecursive::default_use(entity);
    }
};

void append_extrusion_children_to_collection(ExtrusionEntityCollection &dst, ExtrusionEntity &src)
{
    if (src.is_nop())
        return;

    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(&src)) {
        /*
        The plugin root is a temporary transport object. Clone its children
        into the persistent Print tree, then clear the transport root. This is
        a little more conservative than moving unique_ptrs across the API
        boundary and keeps all long-lived adhesion extrusion owned by Print.
        If the collection has properties, it is a semantic subtree and must
        stay whole so descendants inherit them.
        */
        if (collection->has_properties()) {
            dst.append(src);
            src.clear_content();
            src.clear_properties();
            return;
        }
        for (const ExtrusionEntity *child : collection->entities())
            dst.append(*child);
        collection->clear();
        return;
    }

    if (src.is_leaf()) {
        dst.append(src);
        src.clear_content();
        src.clear_properties();
        return;
    }

    ExtrusionEntity::Children &children = src.children();
    for (const ExtrusionEntityUPtr &child : children)
        dst.append(*child);
    src.clear_content();
    src.clear_properties();
}

void ApiInternal::PrintAccess::clear_brim(Print &print)
{
    print.m_brim.clear();
    for (PrintObjectUPtr &object : print.m_objects) {
        ApiInternal::PrintObjectAccess::mutable_brim(*object).clear();
        ApiInternal::PrintObjectAccess::clear_brim_auxiliary_layers(*object);
    }
}

void ApiInternal::PrintAccess::clear_skirt(Print &print)
{
    print.m_skirt.clear();
    print.m_skirt_first_layer.reset();
    print.m_skirt_convex_hull.clear();
    for (PrintObjectUPtr &object : print.m_objects) {
        ApiInternal::PrintObjectAccess::mutable_skirt(*object).clear();
        ApiInternal::PrintObjectAccess::mutable_skirt_first_layer(*object).reset();
    }
}

bool ApiInternal::PrintAccess::append_brim_move(Print &print, ExtrusionEntity &extrusion)
{
    append_extrusion_children_to_collection(print.m_brim, extrusion);
    return true;
}

bool ApiInternal::PrintAccess::append_skirt_move(Print &print, ExtrusionEntity &extrusion)
{
    append_extrusion_children_to_collection(print.m_skirt, extrusion);
    return true;
}

bool ApiInternal::PrintAccess::append_skirt_first_layer_move(Print &print, ExtrusionEntity &extrusion)
{
    if (!print.m_skirt_first_layer)
        print.m_skirt_first_layer.emplace();
    append_extrusion_children_to_collection(*print.m_skirt_first_layer, extrusion);
    return true;
}

bool ApiInternal::PrintAccess::append_skirt_convex_hull_move(Print &print, Polygons &polygons)
{
    /*
    The incoming polygons live in plugin storage and the print hull lives until
    export finishes. Copying the point coordinates is intentionally boring and
    robust: the hull is small, and no persistent Print data keeps buffers that
    were allocated for a temporary plugin result.
    */
    for (const Polygon &polygon : polygons)
        append(print.m_skirt_convex_hull, polygon.points);
    polygons.clear();
    return true;
}

const ExtrusionEntity *ApiInternal::PrintAccess::skirt_first_layer(const Print &print)
{
    return print.m_skirt_first_layer ? &*print.m_skirt_first_layer : nullptr;
}

void ApiInternal::PrintAccess::normalize_skirt_brim_direction(Print &print)
{
    /*
    Skirt/brim output is stored partly on Print and partly on PrintObject.
    Normalize both places after plugins finish so each generator does not have
    to duplicate the perimeter-direction rule.
    */
    if (print.m_default_region_config.perimeter_direction.value == pdCW_CCW ||
        print.m_default_region_config.perimeter_direction.value == pdCW_CW) {
        ExtrusionDirectionSetter visitor(true);
        print.m_brim.visit(visitor);
        print.m_skirt.visit(visitor);
        if (print.m_skirt_first_layer)
            print.m_skirt_first_layer->visit(visitor);
        for (PrintObjectUPtr &object : print.m_objects) {
            ApiInternal::PrintObjectAccess::mutable_brim(*object).visit(visitor);
            ApiInternal::PrintObjectAccess::mutable_skirt(*object).visit(visitor);
            std::optional<ExtrusionEntityCollection> &first_layer =
                ApiInternal::PrintObjectAccess::mutable_skirt_first_layer(*object);
            if (first_layer)
                first_layer->visit(visitor);
        }
    }
}

void ApiInternal::PrintAccess::rebuild_first_layer_convex_hull_after_skirt_brim(Print &print)
{
    /*
    The hull is a print-level safety envelope used by bed-leveling, placeholders
    and wipe/purge placement. Build it from the stable final geometry: first
    layer islands plus any brim trees published by plugins. finalize_* then
    adds skirt and wipe tower corners using the existing host logic.
    */
    print.m_first_layer_convex_hull.points.clear();
    for (Polygon &polygon : print.first_layer_islands())
        append(print.m_first_layer_convex_hull.points, std::move(polygon.points));

    print.m_brim.collect_points(print.m_first_layer_convex_hull.points);
    print.m_skirt.collect_points(print.m_first_layer_convex_hull.points);
    if (print.m_skirt_first_layer)
        print.m_skirt_first_layer->collect_points(print.m_first_layer_convex_hull.points);
    for (PrintObjectUPtr &object : print.m_objects) {
        ApiInternal::PrintObjectAccess::mutable_brim(*object).collect_points(print.m_first_layer_convex_hull.points);
        ApiInternal::PrintObjectAccess::mutable_skirt(*object).collect_points(print.m_first_layer_convex_hull.points);
        std::optional<ExtrusionEntityCollection> &first_layer =
            ApiInternal::PrintObjectAccess::mutable_skirt_first_layer(*object);
        if (first_layer)
            first_layer->collect_points(print.m_first_layer_convex_hull.points);
    }

    print.finalize_first_layer_convex_hull();
}

void Print::_make_skirt_brim() {

    if (this->set_started(psSkirtBrim)) {
        this->set_status(printstep_percent(psSkirtBrim), L("Generating skirt and brim"));
        m_skirt.clear();
        m_skirt_first_layer.reset();
        //const bool draft_shield = config().draft_shield != dsDisabled;

        //first skirt. If it need the brim area, it will extrapolate it from config.
        m_skirt_convex_hull.clear();
        m_first_layer_convex_hull.points.clear();
        for (PrintObjectUPtr &obj : m_objects) {
            obj->m_skirt.clear();
            obj->m_skirt_first_layer.reset();
        }
        if (this->has_skirt()) {
            this->set_status(printstep_percent(psSkirtBrim), L("Generating skirt"));
            if (config().complete_objects && !config().complete_objects_one_skirt){
                for (PrintObjectUPtr &obj : m_objects) {
                    //create a skirt "pattern" (one per object)
                    const std::vector<PrintInstance> copies{obj->instances()};
                    obj->m_instances.clear();
                    obj->m_instances.emplace_back();
                    this->_make_skirt({ obj.get() }, obj->m_skirt, obj->m_skirt_first_layer);
                    obj->m_instances = copies;
                    DEBUG_VISIT(obj->m_skirt, CheckOrientation(true))
                    DEBUG_TREE_VISIT(obj->m_skirt, LoopAssertVisitor())
                }
            } else {
                PrintObjectPtrs objects;
                objects.reserve(m_objects.size());
                for (PrintObjectUPtr &object : m_objects)
                    objects.emplace_back(object.get());
                this->_make_skirt(objects, m_skirt, m_skirt_first_layer);
                DEBUG_VISIT(m_skirt, CheckOrientation(true))
                DEBUG_TREE_VISIT(m_skirt, LoopAssertVisitor())
            }
        }

        /*
        Brim is generated by STEP_SKIRT_BRIM plugins before this legacy skirt
        pass runs. Rebuild the print hull from the first-layer islands, plugin
        brim output, skirt and wipe-tower geometry so the old placeholders and
        placement code keep seeing one complete first-layer envelope.
        */
        ApiInternal::PrintAccess::rebuild_first_layer_convex_hull_after_skirt_brim(*this);

        // everything should be extruded ccw, so only chajnge dir if cw is requested
        if (this->m_default_region_config.perimeter_direction.value == pdCW_CCW ||
            this->m_default_region_config.perimeter_direction.value == pdCW_CW) {
            ExtrusionDirectionSetter visitor(true);
            this->m_skirt.visit(visitor);
            if (m_skirt_first_layer) {
                this->m_skirt_first_layer->visit(visitor);
            }
            this->m_brim.visit(visitor);
        }
        for (PrintObjectUPtr &object : m_objects) {
            const PrintRegionConfig &region_config = object->default_region_config(this->m_default_region_config);
            if (region_config.perimeter_direction.value == pdCW_CCW ||
                region_config.perimeter_direction.value == pdCW_CW) {
                ExtrusionDirectionSetter visitor(true);
                // using firend privilege. If you remove it, just create & call a printobject function.
                object->m_skirt.visit(visitor);
                if (object->m_skirt_first_layer) {
                    object->m_skirt_first_layer->visit(visitor);
                }
            }
            // only global setting is useful for brim
            if (this->m_default_region_config.perimeter_direction.value == pdCW_CCW ||
                this->m_default_region_config.perimeter_direction.value == pdCW_CW) {
                ExtrusionDirectionSetter visitor(true);
                object->m_brim.visit(visitor);
            }
        }

        // Brim depends on skirt (brim lines are trimmed by the skirt lines), therefore if
        // the skirt gets invalidated, brim gets invalidated as well and the following line is called.
        this->set_done(psSkirtBrim);
    }

}

void Print::_make_skirt(const PrintObjectPtrs &objects, ExtrusionEntityCollection &out, std::optional<ExtrusionEntityCollection>& out_first_layer)
{
    assert(out.empty());
    // First off we need to decide how tall the skirt must be.
    // The skirt_height option from config is expressed in layers, but our
    // object might have different layer heights, so we need to find the print_z
    // of the highest layer involved.
    // Note that unless has_infinite_skirt() == true
    // the actual skirt might not reach this $skirt_height_z value since the print
    // order of objects on each layer is not guaranteed and will not generally
    // include the thickest object first. It is just guaranteed that a skirt is
    // prepended to the first 'n' layers (with 'n' = skirt_height).
    // $skirt_height_z in this case is the highest possible skirt height for safety.
    coord_t skirt_height_z = 0.;
    for (const PrintObject *object : objects) {
        size_t skirt_layers = this->has_infinite_skirt() ?
            object->layer_count() : 
            std::min(size_t(m_config.skirt_height.value), object->layer_count());
        skirt_height_z = std::max(skirt_height_z, object->m_layers[skirt_layers-1]->scaled_print_z());
    }
    // Collect points from all layers contained in skirt height.
    Points points;
    for (const PrintObject *object : objects) {
        Points object_points;
        // Get object layers up to skirt_height_z.
        for (const std::unique_ptr<Layer> &layer : object->m_layers) {
            if (layer->scaled_print_z() > skirt_height_z)
                break;
            for (const ExPolygon &expoly : layer->lslices())
                // Collect the outer contour points only, ignore holes for the calculation of the convex hull.
                append(object_points, expoly.contour.points);
        }
        // simplify
        object_points = Slic3r::Geometry::convex_hull(object_points).points;
        // Get support layers up to skirt_height_z.
        for (const Layer &layer : object->auxiliary_layers()) {
            if (layer.get_property<LayerSupportProperty>() == nullptr)
                continue;
            if (layer.scaled_print_z() > skirt_height_z)
                break;
            for (const LayerSliceIsland &island : layer.islands()) {
                append(object_points, island.get_slice().contour.points);
            }
            // simplify
            object_points = Slic3r::Geometry::convex_hull(object_points).points;
        }
        // if brim, it superseed object & support for first layer
        if (config().skirt_distance_from_brim) {
            // get first layer support
            for (const Layer &support_layer : object->auxiliary_layers()) {
                if (support_layer.get_property<LayerSupportProperty>() == nullptr)
                    continue;
                if (support_layer.scaled_print_z() != object->m_layers[0]->scaled_print_z())
                    break;
                Points support_points;
                for (const LayerSliceIsland &island : support_layer.islands()) {
                    append(support_points, island.get_slice().contour.points);
                }
                const Polygon hull_support = Slic3r::Geometry::convex_hull(support_points);
                for (const Polygon& poly : offset(hull_support, scale_d(object->config().brim_width)))
                    append(object_points, poly.points);
                break;
            }
            // get object
            for (const ExPolygon& expoly : object->m_layers[0]->lslices())
                for (const Polygon& poly : offset(expoly.contour, scale_d(object->config().brim_width)))
                    append(object_points, poly.points);
            // get brim patchs
            if (has_brim_patch(*object, ModelVolumeType::BRIM_PATCH)) {
                assert(!object->instances().empty());
                for (Polygon &poly : object->get_brim_patch(ModelVolumeType::BRIM_PATCH, &object->instances().front())) {
                    // remove shift
                    for (Point &pt : poly.points)
                        pt -= object->instances().front().shift;
                    append(object_points, poly.points);
                }
            }
        }
        // simplify
        Polygon polygon = Slic3r::Geometry::convex_hull(object_points);
        coord_t scaled_resolution_internal_coarse = std::min(std::max(SCALED_EPSILON * 10,
                                                                      scale_i(this->config().resolution_internal)),
                                                             this->skirt_flow(0).scaled_width());
        if (!ensure_valid(polygon, scaled_resolution_internal_coarse)) {
            assert(false);
            return;
        }
        object_points = polygon.points;
        // Repeat points for each object copy.
        for (const PrintInstance &instance : object->instances()) {
            Points copy_points = object_points;
            for (Point &pt : copy_points)
                pt += instance.shift;
            append(points, copy_points);
        }
    }

    // Include the wipe tower.
    append(points, this->first_layer_wipe_tower_corners());

    // Unless draft shield is enabled, include all brims as well.
    if (config().draft_shield.value == dsDisabled)
        append(points, m_first_layer_convex_hull.points);

    if (points.size() < 3)
        // At least three points required for a convex hull.
        return;
    
    this->throw_if_canceled();
    Polygon convex_hull = Slic3r::Geometry::convex_hull(points);
    
    // Skirt may be printed on several layers, having distinct layer heights,
    // but loops must be aligned so can't vary width/spacing
    
    std::vector<size_t> extruders;
    std::vector<double> extruders_e_per_mm;
    {
        std::set<uint16_t> set_extruders = this->object_extruders(objects);
        append(set_extruders, this->support_material_extruders());
        extruders.reserve(set_extruders.size());
        extruders_e_per_mm.reserve(set_extruders.size());
        for (unsigned int extruder_id : set_extruders) {
            Flow   flow = this->skirt_flow(extruder_id);
            double mm3_per_mm = flow.mm3_per_mm();
            extruders.push_back(extruder_id);
            extruders_e_per_mm.push_back(Extruder((unsigned int)extruder_id, m_config).e_per_mm(mm3_per_mm));
        }
    }

    // Number of skirt loops per skirt layer.
    size_t n_skirts = m_config.skirts.value;
    size_t n_skirts_first_layer = n_skirts + m_config.skirt_brim.value;
    if (this->has_infinite_skirt() && n_skirts == 0)
        n_skirts = 1;
    if (m_config.skirt_brim.value > 0)
        out_first_layer.emplace();
    // Initial offset of the brim inner edge from the object (possible with a support & raft).
    // The skirt will touch the brim if the brim is extruded.
    float distance = float(scale_d(m_config.skirt_distance.value) - this->skirt_flow(extruders[extruders.size() - 1]).spacing() / 2.);


    size_t lines_per_extruder = (n_skirts + extruders.size() - 1) / extruders.size();
    size_t current_lines_per_extruder = n_skirts - lines_per_extruder * (extruders.size() - 1);

    // Draw outlines from outside to inside.
    // Loop while we have less skirts than required or any extruder hasn't reached the min length if any.
    std::vector<coordf_t> extruded_length(extruders.size(), 0.);
    for (size_t i = std::max(n_skirts, n_skirts_first_layer), extruder_idx = 0, nb_skirts = 1; i > 0; -- i) {
        bool first_layer_only = i <= (n_skirts_first_layer - n_skirts);
        Flow   flow = this->skirt_flow(extruders[extruders.size() - (1+ extruder_idx)]);
        float  spacing = flow.spacing();
        double mm3_per_mm = flow.mm3_per_mm();
        this->throw_if_canceled();
        // Offset the skirt outside.
        distance += float(scale_d(spacing/2));
        // Generate the skirt centerline.
        Polygon loop;
        {
            Polygons loops = offset(convex_hull, distance, ClipperLib::jtRound, float(flow.scaled_width() / 10));
            //make sure the skirt is simple enough
            Geometry::simplify_polygons(loops, flow.scaled_width() / 10, &loops);
			if (loops.empty())
				break;
            assert(loops.size() == 1);
			loop = loops.front();
        }
        distance += float(scale_d(spacing / 2));
        // Extrude the skirt loop.
        ExtrusionLoop eloop(elrSkirt);
        eloop.paths().emplace_back(
            ExtrusionAttributes{
                ExtrusionRole::Skirt,
                ExtrusionFlow{
                    float(mm3_per_mm),        // this will be overridden at G-code export time
                    flow.width(),
                    float(unscaled(get_min_first_layer_height())) // this will be overridden at G-code export time
                }
            }, nullptr,
            false
        );
        eloop.paths().back().polyline() = loop.split_at_first_point();
        //we make it counter-clowkwise, as loop aren't reversed
        //eloop.make_clockwise();
        if(eloop.is_clockwise())
            eloop.reverse();
        if(!first_layer_only)
            out.append(eloop);
        if(out_first_layer)
            out_first_layer->append(eloop);
        if (m_config.min_skirt_length.value > 0 && !first_layer_only) {
            // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
            extruded_length[extruder_idx] += unscaled(loop.length()) * extruders_e_per_mm[extruder_idx];
            if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                // Not extruded enough yet with the current extruder. Add another loop.
                if (i == 1 && extruded_length[extruder_idx] > 0)
                    ++ i;
            } else {
                assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                // Enough extruded with the current extruder. Extrude with the next one,
                // until the prescribed number of skirt loops is extruded.
                if (extruder_idx + 1 < extruders.size()) {
                    if (nb_skirts < current_lines_per_extruder) {
                        nb_skirts++;
                    } else {
                        current_lines_per_extruder = lines_per_extruder;
                        nb_skirts = 1;
                        ++extruder_idx;
                    }
                }
            }
        } else {
            // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
        }
    }
    DEBUG_VISIT(out, CheckOrientation(true))
    // Brims were generated inside out, reverse to print the outmost contour first.
    out.reverse();
    if (out_first_layer)
        out_first_layer->reverse();
    DEBUG_VISIT(out, CheckOrientation(true))

    // Remember the outer edge of the last skirt line extruded as m_skirt_convex_hull.
    for (Polygon &poly : offset(convex_hull, distance + 0.5f * float(this->skirt_flow(extruders[extruders.size() - 1]).scaled_spacing()), ClipperLib::jtRound, float(scale_d(0.1))))
        append(m_skirt_convex_hull, std::move(poly.points));
}



Polygons Print::first_layer_islands() const
{
    Polygons islands;
    for (const PrintObjectUPtr &object : m_objects) {
        Polygons object_islands;
        for (const ExPolygon &expoly : object->m_layers.front()->lslices())
            object_islands.push_back(expoly.contour);
        for (const Layer &support_layer : object->auxiliary_layers()) {
            if (support_layer.get_property<LayerSupportProperty>() == nullptr)
                continue;
            // was polygons_covered_by_spacing, but is it really important?
            for (const LayerSliceIsland &island : support_layer.islands()) {
                for (const LayerRegionIsland &region_island : island.regions_islands()) {
                    if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                               region_island.extrusion(LayerRegionIsland::SUPPORT)
                                   .polygons_covered_by_width(object_islands, float(SCALED_EPSILON));
                    }
                    if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                               region_island.extrusion(LayerRegionIsland::SUPPORT_INTERFACE)
                                   .polygons_covered_by_width(object_islands, float(SCALED_EPSILON));
                    }
                }
            }
            break;
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance &instance : object->instances())
            for (Polygon &poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift);
            }
    }
    return islands;
}

Points Print::first_layer_wipe_tower_corners() const
{
    Points pts_scaled;

    if (has_wipe_tower() && ! m_wipe_tower_data.tool_changes.empty()) {
        double width = m_default_object_config.wipe_tower_width + 2 * m_wipe_tower_data.brim_width;
        double depth = m_wipe_tower_data.depth + 2 * m_wipe_tower_data.brim_width;
        Vec2d pt0(-m_wipe_tower_data.brim_width, -m_wipe_tower_data.brim_width);
        
        // First the corners.
        std::vector<Vec2d> pts = { pt0,
                                   Vec2d(pt0.x()+width, pt0.y()),
                                   Vec2d(pt0.x()+width, pt0.y()+depth),
                                   Vec2d(pt0.x(),pt0.y()+depth)
                                 };

        // Now the stabilization cone.
        Vec2d center = (pts[0] + pts[2])/2.;
        const auto [cone_R, cone_x_scale] = WipeTower::get_wipe_tower_cone_base(m_default_object_config.wipe_tower_width,
                                                                                m_wipe_tower_data.height,
                                                                                m_wipe_tower_data.depth,
                                                                                m_default_object_config.wipe_tower_cone_angle);
        double r = cone_R + m_wipe_tower_data.brim_width;
        for (double alpha = 0.; alpha<2*M_PI; alpha += M_PI/20.)
            pts.emplace_back(center + r*Vec2d(std::cos(alpha)/cone_x_scale, std::sin(alpha)));

        for (Vec2d& pt : pts) {
            pt = Eigen::Rotation2Dd(Geometry::deg2rad(m_default_object_config.wipe_tower_rotation_angle.value)) * pt;
            pt += Vec2d(m_default_object_config.wipe_tower_x.value, m_default_object_config.wipe_tower_y.value);
            pts_scaled.emplace_back(Point(scale_i(pt.x()), scale_i(pt.y())));
        }
    }
    return pts_scaled;
}

void Print::finalize_first_layer_convex_hull()
{
    append(m_first_layer_convex_hull.points, m_skirt_convex_hull);
    if (m_first_layer_convex_hull.empty()) {
        // Neither skirt nor brim was extruded. Collect points of printed objects from 1st layer.
        for (Polygon &poly : this->first_layer_islands())
            append(m_first_layer_convex_hull.points, std::move(poly.points));
    }
    append(m_first_layer_convex_hull.points, this->first_layer_wipe_tower_corners());
    m_first_layer_convex_hull = Geometry::convex_hull(m_first_layer_convex_hull.points);
}

void Print::alert_when_supports_needed()
{
    if (this->set_started(psAlertWhenSupportsNeeded)) {
        BOOST_LOG_TRIVIAL(debug) << "psAlertWhenSupportsNeeded - start";
        set_status(printstep_percent(psAlertWhenSupportsNeeded), L("Alert if supports needed"));

        auto issue_to_alert_message = [](SupportSpotsGenerator::SupportPointCause cause, bool critical) {
            std::string message;
            switch (cause) {
            //TRN Alert when support is needed. Describes that the model has long bridging extrusions which may print badly 
            case SupportSpotsGenerator::SupportPointCause::LongBridge: message = _u8L("Long bridging extrusions"); break;
            //TRN Alert when support is needed. Describes bridge anchors/turns in the air, which will definitely print badly
            case SupportSpotsGenerator::SupportPointCause::FloatingBridgeAnchor: message = _u8L("Floating bridge anchors"); break;
            case SupportSpotsGenerator::SupportPointCause::FloatingExtrusion:
                if (critical) {
                     //TRN Alert when support is needed. Describes that the print has large overhang area which will print badly or not print at all.
                    message = _u8L("Collapsing overhang");
                } else {
                    //TRN Alert when support is needed. Describes extrusions that are not supported enough and come out curled or loose.
                    message = _u8L("Loose extrusions");
                }
                break;
            //TRN Alert when support is needed. Describes that the print has low bed adhesion and may became loose.
            case SupportSpotsGenerator::SupportPointCause::SeparationFromBed: message = _u8L("Low bed adhesion"); break;
            //TRN Alert when support is needed. Describes that the object has part that is not connected to the bed and will not print at all without supports.
            case SupportSpotsGenerator::SupportPointCause::UnstableFloatingPart: message = _u8L("Floating object part"); break;
            //TRN Alert when support is needed. Describes that the object has thin part that may brake during printing 
            case SupportSpotsGenerator::SupportPointCause::WeakObjectPart: message = _u8L("Thin fragile part"); break;
            }

            return message;
        };

        // TRN this translation rule is used to translate lists of uknown size on single line. The first argument is element of the list,
        // the second argument may be element or rest of the list. For most languages, this does not need translation, but some use different 
        // separator than comma and some use blank space in front of the separator.
        auto single_line_list_rule = L("%1%, %2%");
        auto multiline_list_rule   = "%1%\n%2%";

        auto elements_to_translated_list = [](const std::vector<std::string> &translated_elements, std::string expansion_rule) {
            if (expansion_rule.find("%1%") == expansion_rule.npos || expansion_rule.find("%2%") == expansion_rule.npos) {
                BOOST_LOG_TRIVIAL(error) << "INCORRECT EXPANSION RULE FOR LIST TRANSLATION: " << expansion_rule
                                         << " - IT SHOULD CONTAIN %1% and %2%!";
                expansion_rule = "%1% %2%";
            }
            if (translated_elements.size() == 0) {
                return std::string{};
            }
            if (translated_elements.size() == 1) {
                return translated_elements.front();
            }

            std::string translated_list = expansion_rule;
            for (int i = 0; i < int(translated_elements.size()) - 1; ++ i) {
                auto first_elem = translated_list.find("%1%");
                assert(first_elem != translated_list.npos);
                translated_list.replace(first_elem, 3, translated_elements[i]);

                // expand the translated list by another application of the same rule
                auto second_elem = translated_list.find("%2%");
                assert(second_elem != translated_list.npos);
                if (i < int(translated_elements.size()) - 2) {
                    translated_list.replace(second_elem, 3, expansion_rule);
                } else {
                    translated_list.replace(second_elem, 3, translated_elements[i + 1]);
                }
            }

            return translated_list;
        };

        // vector of pairs of object and its issues, where each issue is a pair of type and critical flag
        std::vector<std::pair<const PrintObject *, std::vector<std::pair<SupportSpotsGenerator::SupportPointCause, bool>>>> objects_isssues;

        for (const PrintObjectUPtr &object : m_objects) {
            std::unordered_set<const ModelObject *> checked_model_objects;
            if (!object->has_support() && checked_model_objects.find(object->model_object()) == checked_model_objects.end()) {
                if (object->m_shared_regions->generated_support_points.has_value()) {
                    SupportSpotsGenerator::SupportPoints  supp_points = object->m_shared_regions->generated_support_points->support_points;
                    SupportSpotsGenerator::PartialObjects partial_objects = object->m_shared_regions->generated_support_points
                                                                                ->partial_objects;
                    auto issues = SupportSpotsGenerator::gather_issues(supp_points, partial_objects);
                    if (issues.size() > 0) {
                        objects_isssues.emplace_back(object.get(), issues);
                    }
                }
                checked_model_objects.emplace(object->model_object());
            }
        }

        bool                                                                                                  recommend_brim = false;
        std::map<std::pair<SupportSpotsGenerator::SupportPointCause, bool>, std::vector<const PrintObject *>> po_by_support_issues;
        for (const auto &obj : objects_isssues) {
            for (const auto &issue : obj.second) {
                po_by_support_issues[issue].push_back(obj.first);
                if (issue.first == SupportSpotsGenerator::SupportPointCause::SeparationFromBed && !obj.first->has_brim()) {
                    recommend_brim = true;
                }
            }
        }

        std::vector<std::pair<std::string, std::vector<std::string>>> message_elements;
        if (objects_isssues.size() > po_by_support_issues.size()) {
            // there are more objects than causes, group by issues
            for (const auto &issue : po_by_support_issues) {
                auto &pair = message_elements.emplace_back(issue_to_alert_message(issue.first.first, issue.first.second),
                                                           std::vector<std::string>{});
                for (const auto &obj : issue.second) {
                    pair.second.push_back(obj->m_model_object->name);
                }
            }
        } else {
            // more causes than objects, group by objects
            for (const auto &obj : objects_isssues) {
                auto &pair = message_elements.emplace_back(obj.first->model_object()->name,  std::vector<std::string>{});
                for (const auto &issue : obj.second) {
                    pair.second.push_back(issue_to_alert_message(issue.first, issue.second));
                }
            }
        }

        // first, gather sublements into single line list, store in first subelement
        for (auto &pair : message_elements) {
            pair.second.front() = elements_to_translated_list(pair.second, single_line_list_rule);
        }

        // then gather elements to create multiline list
        std::vector<std::string> lines = {};
        for (auto &pair : message_elements) {
            lines.push_back(""); // empty line for readability
            lines.push_back(pair.first);
            lines.push_back(pair.second.front());
        }

        lines.push_back("");
        lines.push_back(_u8L("Consider enabling supports."));
        if (recommend_brim) {
            lines.push_back(_u8L("Also consider enabling brim."));
        }

        // TRN Alert message for detected print issues. first argument is a list of detected issues.
        auto message = Slic3r::format(_u8L("Detected print stability issues:\n%1%"), elements_to_translated_list(lines, multiline_list_rule));

        if (objects_isssues.size() > 0) {
            this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, message);
        }

        BOOST_LOG_TRIVIAL(debug) << "psAlertWhenSupportsNeeded - end";
        this->set_done(psAlertWhenSupportsNeeded);
    }
}

bool Print::has_wipe_tower() const {
    if (config().nozzle_diameter.size() <= 1 || !m_default_object_config.wipe_tower || config().complete_objects ||
        config().spiral_vase.value) {
        return false;
    }
    bool has_parallel_objects_step = config().parallel_objects_step.value > 0 && !config().parallel_islands.value;
    if (has_parallel_objects_step && config().parallel_objects_step.value > 0) {
        // check if the print has multiple extruders below has_parallel_objects_step_max_z
        const coord_t max_z = scale_to_layer_coord(config().parallel_objects_step.value);
        bool can_wipe_tower = true;
        int extruder = -1;
        auto check_extruder = [&extruder, &can_wipe_tower](int extr) -> bool {
            if (extr <= 0) {
                return false;
            }
            if (extruder == -1) {
                extruder = extr;
                return false;
            }
            can_wipe_tower = extruder == extr;
            return !can_wipe_tower;
        };
        for (const PrintObject &obj : this->objects()) {
            for (const Layer &layer : obj.layers()) {
                if (layer.scaled_print_z() > max_z)
                    continue;
                for (const LayerSliceIsland &layer_island_ptr : layer.islands()) {
                    for (const LayerRegionIsland &lri : layer_island_ptr.regions_islands()) {
                        if (lri.has_extrusions()) {
                            if (lri.has_extrusion(LayerRegionIsland::PERIMETERS)) {
                                for (const LayerRegion *lr : lri.regions()) {
                                    if (check_extruder(lr->region().config().perimeter_extruder.value)) {
                                        goto finish_search;
                                    }
                                }
                            }
                            if (lri.has_extrusion(LayerRegionIsland::INFILLS) ||
                                lri.has_extrusion(LayerRegionIsland::IRONINGS) ||
                                lri.has_extrusion(LayerRegionIsland::GAP_FILLS)) {
                                for (const LayerRegion *lr : lri.regions()) {
                                    if (check_extruder(lr->region().config().infill_extruder.value) ||
                                        check_extruder(lr->region().config().solid_infill_extruder.value)) {
                                        goto finish_search;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        for (const PrintObject &obj : this->objects()) {
            for (const Layer &slayer : obj.auxiliary_layers()) {
                if (slayer.get_property<LayerSupportProperty>() == nullptr)
                    continue;
                if (slayer.scaled_height() > max_z)
                    continue;
                if (slayer.has_extrusions() &&
                    (check_extruder(obj.config().support_material_extruder.value) ||
                     check_extruder(obj.config().support_material_interface_extruder.value))) {
                    goto finish_search; // !can_wipe_tower
                }
            }
        }
    finish_search:;
        has_parallel_objects_step = !can_wipe_tower;
    }
    return !has_parallel_objects_step;
}

const WipeTowerData& Print::wipe_tower_data(const ConfigBase* config, double nozzle_diameter) const
{
    std::scoped_lock<std::mutex> lock(m_wipe_tower_data_mutex);
    // If the wipe tower wasn't created yet, make sure the depth and brim_width members are set to default.
    if (! is_step_done(psWipeTower) && config != &this->m_config) {
        size_t extruders_cnt = config->option("nozzle_diameter")->size();

        // Calculating depth should take into account currently set wiping volumes.
        // For a long time, the initial preview would just use 900/width per toolchange (15mm on a 60mm wide tower)
        // and it worked well enough. Let's try to do slightly better by accounting for the purging volumes.
        std::vector<std::vector<float>> wipe_volumes = WipeTower::extract_wipe_volumes(*config);
        std::vector<float> max_wipe_volumes;
        for (const std::vector<float>& v : wipe_volumes)
            max_wipe_volumes.emplace_back(*std::max_element(v.begin(), v.end()));
        float maximum = std::accumulate(max_wipe_volumes.begin(), max_wipe_volumes.end(), 0.f);
        maximum = maximum * extruders_cnt / max_wipe_volumes.size();

        float unscaled_brim_width = config->option<ConfigOptionFloatOrPercent>("wipe_tower_brim_width")->get_effective_value(nozzle_diameter);
        // use min layer height, as it's what wil disctate the wipe tower width.
        float first_layer_height = 0;
        //if (m_objects.empty()) {
            // if no objects, then no extruder selected: use the first one.
            //first_layer_height = default_object_config().first_layer_height.get_effective_value(config->option("nozzle_diameter")->get_float(0));
            first_layer_height = config->option<ConfigOptionFloatOrPercent>("first_layer_height")->get_effective_value(config->option("nozzle_diameter")->get_float(0));
        //} else {
        //    first_layer_height = unscaled(get_min_first_layer_height());
        //}
        // FIXME: get layer height from layers instead of config.
        //float layer_height = std::min(first_layer_height, float(default_object_config().layer_height.value));
        float layer_height = float(config->option("layer_height")->get_float());
        if (first_layer_height > 0 && first_layer_height < layer_height) {
            layer_height = first_layer_height;
        }
        
        const_cast<Print*>(this)->m_wipe_tower_data.position = Vec2d{config->option("wipe_tower_x")->get_float(), config->option("wipe_tower_y")->get_float()};
        const_cast<Print*>(this)->m_wipe_tower_data.width = float(config->option("wipe_tower_width")->get_float());
        const_cast<Print*>(this)->m_wipe_tower_data.rotation_angle = float(config->option("wipe_tower_rotation_angle")->get_float());
        const_cast<Print*>(this)->m_wipe_tower_data.depth = (maximum/layer_height)/this->m_wipe_tower_data.width;
        const_cast<Print*>(this)->m_wipe_tower_data.brim_width = unscaled_brim_width;
        const_cast<Print*>(this)->m_wipe_tower_data.cone_angle = float(config->option("wipe_tower_cone_angle")->get_float());
        const_cast<Print*>(this)->m_wipe_tower_data.first_layer_height = first_layer_height;
        const_cast<Print*>(this)->m_wipe_tower_data.height = -1.f; // unknown yet
        const_cast<Print*>(this)->m_wipe_tower_data.z_and_depth_pairs.clear(); // unknown yet
    }

    return this->m_wipe_tower_data;
}
//
//void Print::_make_wipe_tower()
//{
//    m_wipe_tower_data.clear();
//    if (! this->has_wipe_tower())
//        return;
//
//    std::vector<std::vector<float>> wipe_volumes = WipeTower::extract_wipe_volumes(m_config);
//
//    if (! m_wipe_tower_data.print->tool_orderings().front().has_wipe_tower())
//        // Don't generate any wipe tower.
//        return;
//
//    assert(m_tool_orderings.size() == 1);
//
//    // Check whether there are any layers in m_tool_orderings, which are marked with has_wipe_tower,
//    // they print neither object, nor support. These layers are above the raft and below the object, and they
//    // shall be added to the support layers to be printed.
//    // see https://github.com/prusa3d/PrusaSlicer/issues/607
//    {
//        size_t idx_begin = size_t(-1);
//        size_t idx_end   = m_tool_orderings.front().layer_tools().size();
//        // Find the first wipe tower layer, which does not have a counterpart in an object or a support layer.
//        for (size_t i = 0; i < idx_end; ++ i) {
//            const LayerTools &lt = m_tool_orderings.front().layer_tools()[i];
//            if (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support) {
//                idx_begin = i;
//                break;
//            }
//        }
//        if (idx_begin != size_t(-1)) {
//            // Find the position in m_objects.first()->support_layers to insert these new support layers.
//            coord_t wipe_tower_new_layer_print_z_first = m_tool_orderings.front().layer_tools()[idx_begin]._print_z;
//            LayerUPtrs::const_iterator it_layer = m_objects.front()->mutable_auxiliary_layers().begin();
//            for (; it_layer != m_objects.front()->mutable_auxiliary_layers().end() && (*it_layer)->scaled_print_z() <= wipe_tower_new_layer_print_z_first; ++ it_layer);
//            // Find the stopper of the sequence of wipe tower layers, which do not have a counterpart in an object or a support layer.
//            for (size_t i = idx_begin; i < idx_end; ++ i) {
//                LayerTools &lt = const_cast<LayerTools&>(m_tool_orderings.front().layer_tools()[i]);
//                if (! (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support))
//                    break;
//                lt.has_support = true;
//                // Insert the new support layer.
//                coord_t height = lt._print_z - (i == 0 ? 0. : m_tool_orderings.front().layer_tools()[i-1]._print_z);
//                //FIXME the support layer ID is set to -1, as Vojtech hopes it is not being used anyway.
//                it_layer = m_objects.front()->insert_auxiliary_layer(it_layer, -1, height, lt._print_z, unscaled(lt._print_z - height / 2));
//                ++ it_layer;
//            }
//        }
//    }
//    this->throw_if_canceled();
//
//    // Initialize the wipe tower.
//    WipeTower wipe_tower(m_config, m_default_object_config, m_default_region_config, wipe_volumes, m_tool_orderings.front().first_extruder());
//
//    // Set the extruder & material properties at the wipe tower object.
//    for (size_t i = 0; i < m_config.nozzle_diameter.size(); ++ i)
//        wipe_tower.set_extruder(i);
//
//    m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
//        wipe_tower.prime((float)unscaled(get_min_first_layer_height()), m_tool_orderings.front().all_extruders(), false));
//
//    // Lets go through the wipe tower layers and determine pairs of extruder changes for each
//    // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
//    {
//        unsigned int current_extruder_id = m_tool_orderings.front().all_extruders().back();
//        for (LayerTools &layer_tools : m_tool_orderings.front().layer_tools()) { // for all layers
//            if (!layer_tools.has_wipe_tower) continue;
//            bool first_layer = &layer_tools == &m_tool_orderings.front().front();
//            wipe_tower.plan_toolchange((float)unscaled(layer_tools._print_z), (float)unscaled(layer_tools.wipe_tower_layer_height), current_extruder_id, current_extruder_id, false);
//            for (const auto extruder_id : layer_tools.extruders) {
//                if ((first_layer && extruder_id == m_tool_orderings.front().all_extruders().back()) || extruder_id != current_extruder_id) {
//                    double volume_to_wipe = wipe_volumes[current_extruder_id][extruder_id];             // total volume to wipe after this toolchange
//                    
//                    // START filament_wipe_advanced_pigment
//                    if (m_config.wipe_advanced) {
//                        volume_to_wipe = m_config.wipe_advanced_nozzle_melted_volume;
//                        float pigmentBef = m_config.filament_wipe_advanced_pigment.get_at(current_extruder_id);
//                        float pigmentAft = m_config.filament_wipe_advanced_pigment.get_at(extruder_id);
//                        if (m_config.wipe_advanced_algo.value == waLinear) {
//                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft);
//                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (lin) ";
//                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
//                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
//                                << " * ( " << pigmentBef << " - " << pigmentAft << " )\n";
//                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value* (pigmentBef - pigmentAft)) << "\n";
//                        } else if (m_config.wipe_advanced_algo.value == waQuadra) {
//                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft)
//                                + m_config.wipe_advanced_multiplier.value * (pigmentBef - pigmentAft) * (pigmentBef - pigmentAft) * (pigmentBef - pigmentAft);
//                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (quadra) ";
//                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
//                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
//                                << " * ( " << pigmentBef << " - " << pigmentAft << " ) + " << m_config.wipe_advanced_multiplier.value
//                                << " * ( " << pigmentBef << " - " << pigmentAft << " ) ^3 \n";
//                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value* (pigmentBef - pigmentAft))
//                                << " + " << (m_config.wipe_advanced_multiplier.value*(pigmentBef - pigmentAft)*(pigmentBef - pigmentAft)*(pigmentBef - pigmentAft))<<"\n";
//                        } else if (m_config.wipe_advanced_algo.value == waHyper) {
//                            volume_to_wipe += m_config.wipe_advanced_multiplier.value * (0.5 + pigmentBef) / (0.5 + pigmentAft);
//                            BOOST_LOG_TRIVIAL(info) << "advanced wiping (hyper) ";
//                            BOOST_LOG_TRIVIAL(info) << current_extruder_id << " -> " << extruder_id << " will use " << volume_to_wipe << " mm3\n";
//                            BOOST_LOG_TRIVIAL(info) << " calculus : " << m_config.wipe_advanced_nozzle_melted_volume << " + " << m_config.wipe_advanced_multiplier.value
//                                << " * ( 0.5 + " << pigmentBef << " ) / ( 0.5 + " << pigmentAft << " )\n";
//                            BOOST_LOG_TRIVIAL(info) << "    = " << m_config.wipe_advanced_nozzle_melted_volume << " + " << (m_config.wipe_advanced_multiplier.value * (0.5 + pigmentBef) / (0.5 + pigmentAft)) << "\n";
//                        }
//                    }
//                    // END filament_wipe_advanced_pigment
//                    
//                    // Not all of that can be used for infill purging:
//                    volume_to_wipe -= (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);
//
//                    // try to assign some infills/objects for the wiping:
//                    volume_to_wipe = layer_tools.wiping_extrusions_nonconst().mark_wiping_extrusions(*this, layer_tools, current_extruder_id, extruder_id, volume_to_wipe);
//
//                    // add back the minimal amount toforce on the wipe tower:
//                    volume_to_wipe += (float)m_config.filament_minimal_purge_on_wipe_tower.get_at(extruder_id);
//
//                    // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
//                    wipe_tower.plan_toolchange((float)unscaled(layer_tools._print_z), (float)unscaled(layer_tools.wipe_tower_layer_height),
//                                               current_extruder_id, extruder_id, volume_to_wipe);
//                    current_extruder_id = extruder_id;
//                }
//            }
//            layer_tools.wiping_extrusions_nonconst().ensure_perimeters_infills_order(*this, layer_tools);
//            if (&layer_tools == &m_tool_orderings.front().back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
//                break;
//        }
//    }
//
//    // Generate the wipe tower layers.
//    m_wipe_tower_data.tool_changes.reserve(m_tool_orderings.front().layer_tools().size());
//    wipe_tower.generate(m_wipe_tower_data.tool_changes);
//    m_wipe_tower_data.depth = wipe_tower.get_depth();
//    m_wipe_tower_data.z_and_depth_pairs = wipe_tower.get_z_and_depth_pairs();
//    m_wipe_tower_data.brim_width = wipe_tower.get_brim_width();
//    m_wipe_tower_data.height = wipe_tower.get_wipe_tower_height();
//
//    // Unload the current filament over the purge tower.
//    double layer_height = m_objects.front()->config().layer_height.value;
//    if (m_tool_orderings.front().back().wipe_tower_partitions > 0) {
//        // The wipe tower goes up to the last layer of the print.
//        if (wipe_tower.layer_finished()) {
//            // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
//            // Lift Z to the next layer.
//            wipe_tower.set_layer(float(unscaled(m_tool_orderings.front().back()._print_z) + layer_height), float(layer_height), 0, false, true);
//        } else {
//            // There is yet enough space at this layer of the wipe tower for the final purge.
//        }
//    } else {
//        // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
//        assert(m_tool_orderings.front().back().wipe_tower_partitions == 0);
//        wipe_tower.set_layer(float(unscaled(m_tool_orderings.front().back()._print_z)), float(layer_height), 0, false, true);
//    }
//    m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(
//        wipe_tower.tool_change((unsigned int)(-1)));
//
//    m_wipe_tower_data.used_filament_until_layer = wipe_tower.get_used_filament_until_layer();
//    m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
//    m_wipe_tower_data.width = wipe_tower.width();
//    m_wipe_tower_data.first_layer_height = unscaled(get_min_first_layer_height());
//    m_wipe_tower_data.cone_angle = m_default_object_config.wipe_tower_cone_angle;
//}

// Generate a recommended G-code output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before G-code is finalized).
std::string Print::output_filename(const std::string &filename_base) const 
{ 
    // Set the placeholders for the data know first after the G-code export is finished.
    // These values will be just propagated into the output file name.
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    config.set_key_value("num_extruders", new ConfigOptionInt((int)m_config.nozzle_diameter.size()));
    config.set_key_value("extruders_count", new ConfigOptionInt((int)m_config.nozzle_diameter.size()));
    config.set_key_value("num_milling", new ConfigOptionInt((int)m_config.milling_diameter.size()));
    config.set_key_value("milling_count", new ConfigOptionInt((int)m_config.milling_diameter.size()));
    config.set_key_value("default_output_extension", new ConfigOptionString(".gcode"));

    // Handle output_filename_format. There is a hack related to binary G-codes: gcode / bgcode substitution.
    std::string output_filename_format = m_config.output_filename_format.value;
    if (m_config.binary_gcode && boost::iends_with(output_filename_format, ".gcode"))
        output_filename_format.insert(output_filename_format.end()-5, 'b');
    if (! m_config.binary_gcode && boost::iends_with(output_filename_format, ".bgcode"))
        output_filename_format.erase(output_filename_format.end()-6);

    return this->PrintBase::output_filename(output_filename_format, ".gcode", filename_base, &config);
}

// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
std::vector<const PrintInstance*> Print::sort_object_instances_by_max_z() const
{
    std::vector<const PrintObject*> objects;
    objects.reserve(this->objects().size());
    for (const PrintObject &object : this->objects())
        objects.emplace_back(&object);
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject* object : objects)
        for (size_t i = 0; i < object->instances().size(); ++i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}

// Sort the PrintObjects by their increasing Y, likely useful for avoiding colisions on printer with a x-bar during sequential prints.
std::vector<const PrintInstance*> Print::sort_object_instances_by_max_y() const
{
    std::vector<const PrintObject*> objects;
    objects.reserve(this->objects().size());
    for (const PrintObject &object : this->objects())
        objects.emplace_back(&object);
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    std::map<const PrintInstance*, coord_t> map_min_y;
    for (const PrintObject* object : objects) {
        for (size_t i = 0; i < object->instances().size(); ++i) {
            instances.emplace_back(&object->instances()[i]);
            // Calculate the convex hull of a printable object. 
            Polygon poly = object->model_object()->convex_hull_2d(
                object->trafo()
                // already in object->trafo()
                //* Geometry::assemble_transform(Vec3d::Zero(),
                //    object->instances()[i].model_instance->get_rotation(), 
                //    object->instances()[i].model_instance->get_scaling_factor(), 
                //    object->instances()[i].model_instance->get_mirror())
            );
            BoundingBox bb(poly.points);
            Vec2crd offset = object->instances()[i].shift - object->center_offset();
            bb.translate(offset.x(), offset.y());
            map_min_y[instances.back()] = bb.min.y();
        }
    }
    std::sort(instances.begin(), instances.end(), [&map_min_y](const PrintInstance* po1, const PrintInstance* po2) { return map_min_y[po1] < map_min_y[po2]; });
    return instances;
}

// Produce a vector of PrintObjects in the order of their respective ModelObjects in this->model().
std::vector<const PrintInstance*> Print::sort_object_instances_by_model_order() const
{
    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(this->num_object_instances());
    for (const PrintObject &print_object : this->objects())
        for (const PrintInstance &print_instance : print_object.instances())
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first < r.first; });

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject &model_object : this->model().objects())
        for (const ModelInstance *model_instance : model_object.instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first < r.first; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    return instances;
}

const std::string PrintStatistics::FilamentUsedG     = "filament used [g]";
const std::string PrintStatistics::FilamentUsedGMask = "; filament used [g] =";

const std::string PrintStatistics::TotalFilamentUsedG          = "total filament used [g]";
const std::string PrintStatistics::TotalFilamentUsedGMask      = "; total filament used [g] =";
const std::string PrintStatistics::TotalFilamentUsedGValueMask = "; total filament used [g] = %.2lf\n";

const std::string PrintStatistics::FilamentUsedCm3     = "filament used [cm3]";
const std::string PrintStatistics::FilamentUsedCm3Mask = "; filament used [cm3] =";

const std::string PrintStatistics::FilamentUsedMm     = "filament used [mm]";
const std::string PrintStatistics::FilamentUsedMmMask = "; filament used [mm] =";

const std::string PrintStatistics::FilamentCost     = "filament cost";
const std::string PrintStatistics::FilamentCostMask = "; filament cost =";

const std::string PrintStatistics::TotalFilamentCost          = "total filament cost";
const std::string PrintStatistics::TotalFilamentCostMask      = "; total filament cost =";
const std::string PrintStatistics::TotalFilamentCostValueMask = "; total filament cost = %.2lf\n";

const std::string PrintStatistics::TotalFilamentUsedWipeTower     = "total filament used for wipe tower [g]";
const std::string PrintStatistics::TotalFilamentUsedWipeTowerValueMask = "; total filament used for wipe tower [g] = %.2lf\n";



DynamicConfig PrintStatistics::config() const
{
    DynamicConfig config;
    if (this->estimated_print_time_str.find(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)) !=
        this->estimated_print_time_str.end()) {
        std::string normal_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)));
        config.set_key_value("print_time", new ConfigOptionString(normal_print_time));
        config.set_key_value("normal_print_time", new ConfigOptionString(normal_print_time));
    } else if (this->estimated_print_time_str.find(static_cast<uint8_t>(
                   PrintEstimatedStatistics::ETimeMode::Stealth)) != this->estimated_print_time_str.end()) {
        std::string silent_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)));
        config.set_key_value("print_time", new ConfigOptionString(silent_print_time));
    }
    if (this->estimated_print_time_str.find(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)) !=
        this->estimated_print_time_str.end()) {
        std::string silent_print_time = short_time(
            this->estimated_print_time_str.at(static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)));
        config.set_key_value("silent_print_time", new ConfigOptionString(silent_print_time));
    }
    config.set_key_value("used_filament",             new ConfigOptionFloat(this->total_used_filament / 1000.));
    config.set_key_value("extruded_volume",           new ConfigOptionFloat(this->total_extruded_volume));
    config.set_key_value("total_cost",                new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_toolchanges",         new ConfigOptionInt(this->total_toolchanges));
    config.set_key_value("total_weight",              new ConfigOptionFloat(this->total_weight));
    config.set_key_value("total_wipe_tower_cost",     new ConfigOptionFloat(this->total_wipe_tower_cost));
    config.set_key_value("total_wipe_tower_filament", new ConfigOptionFloat(this->total_wipe_tower_filament));
    config.set_key_value("initial_tool",              new ConfigOptionInt(int(this->initial_extruder_id)));
    config.set_key_value("initial_extruder",          new ConfigOptionInt(int(this->initial_extruder_id)));
    config.set_key_value("initial_filament_type",     new ConfigOptionString(this->initial_filament_type));
    config.set_key_value("printing_filament_types",   new ConfigOptionString(this->printing_filament_types));
    config.set_key_value("num_printing_extruders",    new ConfigOptionInt(int(this->printing_extruders.size())));
//    config.set_key_value("printing_extruders",        new ConfigOptionInts(std::vector<int>(this->printing_extruders.begin(), this->printing_extruders.end())));
    
    return config;
}

DynamicConfig PrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const char *key : { 
        "print_time", "normal_print_time", "silent_print_time", 
        "used_filament", "extruded_volume", "total_cost", "total_weight", 
        "total_toolchanges", "total_wipe_tower_cost", "total_wipe_tower_filament",
        "initial_tool", "initial_extruder", "initial_filament_type", "printing_filament_types", "num_printing_extruders" })
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));
    return config;
}

std::string PrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}


} // namespace Slic3r
