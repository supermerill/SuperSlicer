///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SeamPlacerViews.hpp"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Point.hpp"

/*
This file implements both sides of the C++/C seam placer bridge. Provider
callbacks are contained here so exceptions and temporary diagnostics never
cross the C ABI. The consumer side validates the provider result against the
original arc-aware extrusion path before returning it to an ordering plugin.
*/

namespace slic3r_api {
namespace detail {

struct SeamPlacerSessionHolder;

/* Prepare a writable result before invoking provider code. */
static void initialize_result(raw_seam_placer_result *result) noexcept;

/* Delete the provider session owned by one published C instance. */
static void destroy_session(void *opaque) noexcept;

/* Invoke the C++ provider while containing all exceptions at the C boundary. */
static void place_seam_thunk(const void *opaque,
                             const extrusion_entity_handle *loop,
                             c_point start_position,
                             raw_seam_placer_result *result) noexcept;

/* Return the immutable callback table shared by all C++ provider sessions. */
static const raw_seam_placer_vtable &session_vtable();

/* Check the structural fields required before ownership can be transferred. */
static bool instance_is_complete(const raw_seam_placer_instance &instance) noexcept;

/* Verify a provider result against the complete line-and-arc loop path. */
static bool point_belongs_to_loop(const ExtrusionEntity &loop, c_point point);

struct SeamPlacerSessionHolder
{
    std::unique_ptr<SeamPlacerSession> session;
};

thread_local std::string g_seam_placer_error;

static void initialize_result(raw_seam_placer_result *result) noexcept
{
    result->status = RAW_SEAM_PLACER_STATUS_UNSET;
    result->point = {};
    result->error_message = nullptr;
}

static void destroy_session(void *opaque) noexcept
{
    delete static_cast<SeamPlacerSessionHolder *>(opaque);
}

static void place_seam_thunk(const void *opaque,
                             const extrusion_entity_handle *loop,
                             c_point start_position,
                             raw_seam_placer_result *result) noexcept
{
    // A short result structure cannot safely receive all output fields. Leave
    // it untouched so callers can detect that no valid result was produced.
    if (result == nullptr || result->struct_size < sizeof(raw_seam_placer_result))
        return;

    initialize_result(result);
    if (opaque == nullptr || loop == nullptr || extrusion_is_loop(loop) == 0) {
        result->status = RAW_SEAM_PLACER_STATUS_INVALID_ARGUMENT;
        return;
    }

    const SeamPlacerSessionHolder *holder =
        static_cast<const SeamPlacerSessionHolder *>(opaque);
    try {
        g_seam_placer_error.clear();
        result->point = holder->session->place_seam(ExtrusionEntity(loop), start_position);
        result->status = RAW_SEAM_PLACER_STATUS_SUCCESS;
    } catch (const std::exception &exception) {
        g_seam_placer_error = exception.what();
        result->status = RAW_SEAM_PLACER_STATUS_ERROR;
        result->error_message = g_seam_placer_error.c_str();
    } catch (...) {
        g_seam_placer_error = "Unknown exception in the seam placer session.";
        result->status = RAW_SEAM_PLACER_STATUS_ERROR;
        result->error_message = g_seam_placer_error.c_str();
    }
}

static const raw_seam_placer_vtable &session_vtable()
{
    static const raw_seam_placer_vtable vtable = {
        sizeof(raw_seam_placer_vtable),
        &destroy_session,
        &place_seam_thunk
    };
    return vtable;
}

static bool instance_is_complete(const raw_seam_placer_instance &instance) noexcept
{
    return instance.struct_size >= sizeof(raw_seam_placer_instance) &&
           instance.session != nullptr && instance.vtable != nullptr &&
           instance.vtable->struct_size >= sizeof(raw_seam_placer_vtable) &&
           instance.vtable->destroy != nullptr &&
           instance.vtable->place_seam != nullptr;
}

static bool point_belongs_to_loop(const ExtrusionEntity &loop, c_point point)
{
    // The host owns the opaque handle and may use its exact ArcPolyline path
    // here. This avoids flattening arcs, which could incorrectly reject a seam
    // lying between two stored arc endpoints.
    const Slic3r::ExtrusionEntity *core_loop =
        reinterpret_cast<const Slic3r::ExtrusionEntity *>(loop.handle());
    const Slic3r::ArcPolyline path = core_loop->as_polyline();
    if (path.empty())
        return false;

    const Slic3r::Point query(point.x, point.y);
    const Slic3r::Geometry::ArcWelder::PathSegmentProjection projection =
        Slic3r::Geometry::ArcWelder::point_to_path_projection(path.get_arc(), query);
    return projection.valid() &&
           projection.distance2 <= double(SCALED_EPSILON) * double(SCALED_EPSILON);
}

} // namespace detail

raw_seam_placer_instance make_seam_placer_instance(
    std::unique_ptr<SeamPlacerSession> session,
    const PrintingPlan &plan)
{
    if (!session)
        throw std::invalid_argument("A seam placer instance needs a session object.");
    if (!plan.valid())
        throw std::invalid_argument("A seam placer instance needs a valid PrintingPlan.");

    // Initialization happens before ownership crosses the C boundary. If it
    // throws, the local unique_ptr destroys the partially initialized session.
    session->initialize(plan);

    detail::SeamPlacerSessionHolder *holder = new detail::SeamPlacerSessionHolder();
    holder->session = std::move(session);

    raw_seam_placer_instance instance = {};
    instance.struct_size = sizeof(instance);
    instance.session = holder;
    instance.vtable = &detail::session_vtable();
    return instance;
}

SeamPlacer::SeamPlacer(raw_seam_placer_instance &instance)
{
    if (!detail::instance_is_complete(instance))
        throw std::invalid_argument("The seam placer instance is incomplete.");
    m_instance = instance;
    instance = {};
}

SeamPlacer::SeamPlacer(SeamPlacer &&other) noexcept : m_instance(other.m_instance)
{
    other.m_instance = {};
}

SeamPlacer &SeamPlacer::operator=(SeamPlacer &&other) noexcept
{
    if (this != &other) {
        reset();
        m_instance = other.m_instance;
        other.m_instance = {};
    }
    return *this;
}

SeamPlacer::~SeamPlacer()
{
    reset();
}

bool SeamPlacer::valid() const noexcept
{
    return detail::instance_is_complete(m_instance);
}

c_point SeamPlacer::place_seam(const ExtrusionEntity &loop,
                               c_point start_position) const
{
    if (!valid())
        throw std::logic_error("The seam placer instance is invalid.");
    if (!loop.is_loop())
        throw std::invalid_argument("Seam placement needs a non-empty fixed continuous closed loop.");

    raw_seam_placer_result result = {};
    result.struct_size = sizeof(result);
    m_instance.vtable->place_seam(
        m_instance.session, loop.handle(), start_position, &result);

    if (result.status != RAW_SEAM_PLACER_STATUS_SUCCESS) {
        const char *message = result.error_message != nullptr ?
            result.error_message : "The seam placer callback failed.";
        throw std::runtime_error(message);
    }
    if (!detail::point_belongs_to_loop(loop, result.point))
        throw std::runtime_error("The seam placer returned a point outside the loop path.");
    return result.point;
}

void SeamPlacer::reset() noexcept
{
    if (valid())
        m_instance.vtable->destroy(m_instance.session);
    m_instance = {};
}

} // namespace slic3r_api
