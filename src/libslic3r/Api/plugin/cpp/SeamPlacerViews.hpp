///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_SeamPlacerViews_hpp_
#define slic3r_Api_plugin_cpp_SeamPlacerViews_hpp_

#include <memory>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Seam placer C++ service adapter
===============================

A provider derives SeamPlacerSession. During its SEAM_PLACER run(), it calls
make_seam_placer_instance() with the plan from run_ctx_seam_placer and stores
the returned C instance in that context.

A consumer selects and executes the provider through OrchestratorView, then
constructs SeamPlacer from the published instance. SeamPlacer owns only the
created session; the orchestrator continues to own the registered plugin.
*/

namespace slic3r_api {

class SeamPlacerSession
{
public:
    /* Allow a session implementation to release its provider-owned caches. */
    virtual ~SeamPlacerSession() = default;

    /*
    Prepare every immutable cache needed by seam placement.

    This method is called exactly once by make_seam_placer_instance(). It may
    perform expensive work and may organize its own parallelism. The borrowed
    plan must outlive the resulting session.
    */
    virtual void initialize(const PrintingPlan &plan) = 0;

    /*
    Select one point on loop when approaching it from start_position.

    Calls may execute concurrently after initialize() returns, so providers
    must not mutate shared state without synchronization. Repeating a call with
    the same loop and position must return the same point for this session.
    */
    virtual c_point place_seam(const ExtrusionEntity &loop,
                               c_point start_position) const = 0;
};

/*
Initialize a provider session and transfer it to the C service instance.

If initialization throws, the unique_ptr destroys the session and no instance
is published. The returned instance owns the session until its destroy callback
is invoked, normally by SeamPlacer.
*/
raw_seam_placer_instance make_seam_placer_instance(
    std::unique_ptr<SeamPlacerSession> session,
    const PrintingPlan &plan);

class SeamPlacer
{
public:
    /* Construct an invalid wrapper that owns no session. */
    SeamPlacer() = default;

    /*
    Take the instance published in run_ctx_seam_placer.

    The source is cleared after a successful transfer, preventing the context
    and this wrapper from destroying the same session. An incomplete instance
    is rejected without changing the source.
    */
    explicit SeamPlacer(raw_seam_placer_instance &instance);

    /* A session has unique ownership and therefore cannot be copied. */
    SeamPlacer(const SeamPlacer &) = delete;
    SeamPlacer &operator=(const SeamPlacer &) = delete;

    /* Transfer session ownership without invoking the provider destructor. */
    SeamPlacer(SeamPlacer &&other) noexcept;
    SeamPlacer &operator=(SeamPlacer &&other) noexcept;

    /* Destroy the owned provider session, when present. */
    ~SeamPlacer();

    /* Return whether this wrapper owns a complete callable session. */
    bool valid() const noexcept;

    /*
    Ask the provider for a seam and validate its service contract.

    loop must be a non-empty, fixed, continuous closed path. The returned point
    must lie on one of its straight or arc segments within SCALED_EPSILON. A
    provider error or invalid point raises an exception; no projection is made.
    The loop's membership in the initialized plan is a caller precondition.
    */
    c_point place_seam(const ExtrusionEntity &loop,
                       c_point start_position) const;

private:
    /* Destroy the owned session and restore the invalid state. */
    void reset() noexcept;

    raw_seam_placer_instance m_instance = {};
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_SeamPlacerViews_hpp_
