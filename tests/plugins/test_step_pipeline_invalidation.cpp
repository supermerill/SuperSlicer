#include <catch2/catch.hpp>

#include <algorithm>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

namespace {

bool contains_step(const std::vector<slicing_step_t> &steps, slicing_step_t step)
{
    return std::find(steps.begin(), steps.end(), step) != steps.end();
}

} // namespace

TEST_CASE("Step pipeline DAG invalidates only skirt/brim dependents", "[plugins][step-pipeline][invalidation]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The new pipeline is not topologically sorted at runtime. This test
    // protects the central dependency table used by Print invalidation: changing
    // skirt/brim must re-run its finalization consumers, but it must not force
    // unrelated object geometry steps such as perimeter, surface or infill.
    const std::vector<slicing_step_t> dependents = Slic3r::Steps::dependent_steps_closure(STEP_SKIRT_BRIM);

    CHECK(contains_step(dependents, STEP_SUPPORT_DEMAND));
    CHECK(contains_step(dependents, STEP_SUPPORT));
    CHECK(contains_step(dependents, STEP_PRE_GCODE));
    CHECK(contains_step(dependents, STEP_ORDERING));
    CHECK(contains_step(dependents, STEP_WIPETOWER));
    CHECK(contains_step(dependents, STEP_GCODE));

    CHECK_FALSE(contains_step(dependents, STEP_PERIMETER));
    CHECK_FALSE(contains_step(dependents, STEP_SURFACE_GENERATION));
    CHECK_FALSE(contains_step(dependents, STEP_INFILL));
    CHECK_FALSE(contains_step(dependents, STEP_POST_INFILL));

    // A fresh Print starts with a full run requested. Marking every known step
    // as executed gives the same empty plan that a successful process() leaves
    // behind, then the single-step invalidation should rebuild only the
    // skirt/brim branch and its declared downstream consumers.
    Slic3r::Print print;
    for (slicing_step_t step : Slic3r::Steps::execution_order())
        print.mark_step_executed(step);

    print.mark_step_and_dependents_for_execution(STEP_SKIRT_BRIM);

    CHECK(print.should_execute_step(STEP_SKIRT_BRIM));
    CHECK(print.should_execute_step(STEP_PRE_GCODE));
    CHECK(print.should_execute_step(STEP_ORDERING));
    CHECK(print.should_execute_step(STEP_GCODE));

    CHECK_FALSE(print.should_execute_step(STEP_PERIMETER));
    CHECK_FALSE(print.should_execute_step(STEP_SURFACE_GENERATION));
    CHECK_FALSE(print.should_execute_step(STEP_INFILL));
    CHECK_FALSE(print.should_execute_step(STEP_POST_INFILL));
}

TEST_CASE("Step pipeline dependency order matches written execution order", "[plugins][step-pipeline][invalidation]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    // The dependency graph is only an invalidation graph. Runtime still follows
    // StepPipeline::execution_order(), so every edge must point forward in that
    // written order or a dependent step could be requested before its producer.
#ifdef _DEBUG
    CHECK(Slic3r::Steps::validate_execution_order_against_dependencies());
#else
    SUCCEED("Dependency order validation is a debug-only guard.");
#endif
}
