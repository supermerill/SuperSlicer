#include <catch2/catch.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/SeamPlacerViews.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "plugin_test_helpers.hpp"

/*
These tests exercise SEAM_PLACER as a real service rather than calling its C++
session directly. A small provider is registered once, selected through the
generic orchestrator API, and asked to publish an initialized session into the
service payload used by future ordering plugins.
*/

namespace {

using namespace Slic3r;

constexpr const char *SeamPluginId = "slic3r.test.seam_placer";
constexpr const char *SeamPluginGroup = "seam_placer_plugin";

struct SeamTestState;
class TestSeamSession;
class TestSeamPlugin;
class ActivePluginGuard;

/* Return a borrowed plugin API view for one core extrusion entity. */
static slic3r_api::ExtrusionEntity extrusion_view(const ExtrusionEntity &entity);

/* Build a fixed square loop made only of straight segments. */
static ExtrusionEntity make_square_loop();

/* Build a fixed loop made of two semicircular segments. */
static ExtrusionEntity make_arc_loop();

/* Register the process-lifetime test provider once. */
static void ensure_seam_provider_registered();

/* Select, execute, and take the test provider's published session. */
static slic3r_api::SeamPlacer execute_seam_provider(Print &print);

struct SeamTestState
{
    std::atomic<uint32_t> initialize_count { 0 };
    std::atomic<uint32_t> place_count { 0 };
    std::atomic<uint32_t> destroy_count { 0 };
    const printing_plan_handle *initialized_plan = nullptr;
    c_point result {};
    bool throw_initialize = false;
    bool throw_place = false;

    void reset()
    {
        initialize_count.store(0, std::memory_order_relaxed);
        place_count.store(0, std::memory_order_relaxed);
        destroy_count.store(0, std::memory_order_relaxed);
        initialized_plan = nullptr;
        result = {};
        throw_initialize = false;
        throw_place = false;
    }
};

SeamTestState g_seam_state;

class TestSeamSession final : public slic3r_api::SeamPlacerSession
{
public:
    explicit TestSeamSession(SeamTestState &state);
    ~TestSeamSession() override;

    void initialize(const slic3r_api::PrintingPlan &plan) override;
    c_point place_seam(const slic3r_api::ExtrusionEntity &loop,
                       c_point start_position) const override;

private:
    SeamTestState &m_state;
};

TestSeamSession::TestSeamSession(SeamTestState &state) : m_state(state) {}

TestSeamSession::~TestSeamSession()
{
    m_state.destroy_count.fetch_add(1, std::memory_order_relaxed);
}

void TestSeamSession::initialize(const slic3r_api::PrintingPlan &plan)
{
    m_state.initialize_count.fetch_add(1, std::memory_order_relaxed);
    m_state.initialized_plan = plan.handle();
    if (m_state.throw_initialize)
        throw std::runtime_error("intentional seam initialization failure");
}

c_point TestSeamSession::place_seam(const slic3r_api::ExtrusionEntity &loop,
                                    c_point start_position) const
{
    if (!loop.is_loop())
        throw std::invalid_argument("test provider received a non-loop");
    m_state.place_count.fetch_add(1, std::memory_order_relaxed);
    if (m_state.throw_place)
        throw std::runtime_error("seam failure " + std::to_string(start_position.x));
    return m_state.result;
}

class TestSeamPlugin final : public slic3r_api::PluginBase
{
public:
    explicit TestSeamPlugin(orchestrator_handle *orchestrator);

private:
    const char *id_impl() const noexcept override;
    const char *exclusive_group_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    void run_impl(const plugin_run_context *run_context) const override;
};

TestSeamPlugin::TestSeamPlugin(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

const char *TestSeamPlugin::id_impl() const noexcept
{
    return SeamPluginId;
}

const char *TestSeamPlugin::exclusive_group_impl() const noexcept
{
    return SeamPluginGroup;
}

slicing_step_t TestSeamPlugin::step_impl() const noexcept
{
    return SEAM_PLACER;
}

const char *const *TestSeamPlugin::dependencies_impl() const noexcept
{
    static const char *dependencies[] = { nullptr };
    return dependencies;
}

int32_t TestSeamPlugin::priority_impl() const noexcept
{
    return 0;
}

void TestSeamPlugin::run_impl(const plugin_run_context *run_context) const
{
    run_ctx_seam_placer *context = plugin_ctx_as_seam_placer(run_context);
    if (context == nullptr || context->plan == nullptr)
        throw std::invalid_argument("The seam placer test needs a PrintingPlan payload.");
    if (context->instance.session != nullptr)
        throw std::invalid_argument("The seam placer payload already owns a session.");

    context->instance = slic3r_api::make_seam_placer_instance(
        std::unique_ptr<slic3r_api::SeamPlacerSession>(new TestSeamSession(g_seam_state)),
        slic3r_api::PrintingPlan(context->plan));
}

class ActivePluginGuard
{
public:
    explicit ActivePluginGuard(Orchestrator &orchestrator);
    ~ActivePluginGuard();

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

ActivePluginGuard::ActivePluginGuard(Orchestrator &orchestrator) : m_orchestrator(orchestrator)
{
    for (Plugin *plugin : m_orchestrator.active_plugins())
        m_previous.push_back(plugin);
    m_orchestrator.clear_active_plugins();
    REQUIRE(m_orchestrator.set_plugin_active(SeamPluginId, true));
    m_orchestrator.reset_plugin_cancel();
}

ActivePluginGuard::~ActivePluginGuard()
{
    m_orchestrator.clear_active_plugins();
    for (Plugin *plugin : m_previous)
        m_orchestrator.set_plugin_active(plugin, true);
    m_orchestrator.reset_plugin_cancel();
    g_seam_state.reset();
}

static slic3r_api::ExtrusionEntity extrusion_view(const ExtrusionEntity &entity)
{
    return slic3r_api::ExtrusionEntity(
        reinterpret_cast<const extrusion_entity_handle *>(&entity));
}

static ExtrusionEntity make_square_loop()
{
    Points points {
        Point(0, 0),
        Point(1000, 0),
        Point(1000, 1000),
        Point(0, 1000),
        Point(0, 0)
    };
    return ExtrusionEntity(false, ArcPolyline(points));
}

static ExtrusionEntity make_arc_loop()
{
    Geometry::ArcWelder::Path path;
    path.emplace_back(Point(-1000, 0), 0.f, Geometry::ArcWelder::Orientation::Unknown);
    path.emplace_back(Point(1000, 0), 1000.f, Geometry::ArcWelder::Orientation::CCW);
    path.emplace_back(Point(-1000, 0), 1000.f, Geometry::ArcWelder::Orientation::CCW);
    return ExtrusionEntity(false, ArcPolyline(path));
}

static void ensure_seam_provider_registered()
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Orchestrator &orchestrator = Orchestrator::instance();
    if (orchestrator.get_plugin(SeamPluginId) != nullptr)
        return;

    static std::unique_ptr<TestSeamPlugin> plugin;
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    plugin.reset(new TestSeamPlugin(handle));
    REQUIRE(orchestrator.register_plugin(plugin->c_instance()));
}

static slic3r_api::SeamPlacer execute_seam_provider(Print &print)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    slic3r_api::OrchestratorView orchestrator_view(
        reinterpret_cast<orchestrator_handle *>(&orchestrator));
    const slic3r_api::PluginView provider =
        orchestrator_view.select_plugin(nullptr, SEAM_PLACER, SeamPluginGroup);
    REQUIRE(provider.valid());

    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_seam_placer context = {};
    context.plan = reinterpret_cast<const printing_plan_handle *>(&plan);
    REQUIRE(orchestrator_view.execute(
                provider,
                reinterpret_cast<const print_handle *>(&print),
                context) == RAW_PLUGIN_EXECUTION_SUCCESS);
    REQUIRE(context.instance.session != nullptr);
    return slic3r_api::SeamPlacer(context.instance);
}

} // namespace

TEST_CASE("A seam placer service publishes one initialized owned session",
          "[plugins][seam-placer][orchestrator]")
{
    ensure_seam_provider_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    ActivePluginGuard active(orchestrator);
    Print print;
    Printing::PrintingPlan &plan = print.mutable_printing_plan();

    {
        slic3r_api::SeamPlacer placer = execute_seam_provider(print);
        CHECK(placer.valid());
        CHECK(g_seam_state.initialize_count.load(std::memory_order_relaxed) == 1);
        CHECK(g_seam_state.initialized_plan ==
              reinterpret_cast<const printing_plan_handle *>(&plan));

        slic3r_api::SeamPlacer moved(std::move(placer));
        CHECK_FALSE(placer.valid());
        CHECK(moved.valid());
        CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 0);
    }
    CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Extrusion loop classification uses the shared fixed closed-path contract",
          "[plugins][seam-placer][extrusion]")
{
    ExtrusionEntity empty(false);
    ExtrusionEntity open(false, ArcPolyline(Points { Point(0, 0), Point(1000, 0) }));
    ExtrusionEntity square = make_square_loop();

    CHECK(extrusion_is_loop(nullptr) == 0);
    CHECK_FALSE(extrusion_view(empty).is_loop());
    CHECK_FALSE(extrusion_view(open).is_loop());
    CHECK(extrusion_view(square).is_loop());

    ExtrusionEntity::Children children;
    children.emplace_back(new ExtrusionEntity(
        false, ArcPolyline(Points { Point(0, 0), Point(1000, 0), Point(1000, 1000) })));
    children.emplace_back(new ExtrusionEntity(
        false, ArcPolyline(Points { Point(1000, 1000), Point(0, 1000), Point(0, 0) })));
    ExtrusionEntity sortable(std::move(children), true, false, false);
    CHECK_FALSE(extrusion_view(sortable).is_loop());
}

TEST_CASE("A seam placer accepts points on straight and arc loop segments",
          "[plugins][seam-placer][geometry]")
{
    ensure_seam_provider_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    ActivePluginGuard active(orchestrator);
    Print print;
    slic3r_api::SeamPlacer placer = execute_seam_provider(print);

    ExtrusionEntity square = make_square_loop();
    g_seam_state.result = { 500, 0 };
    const c_point straight = placer.place_seam(extrusion_view(square), { 0, 0 });
    CHECK(straight.x == 500);
    CHECK(straight.y == 0);

    ExtrusionEntity arcs = make_arc_loop();
    g_seam_state.result = { 0, -1000 };
    const c_point arc = placer.place_seam(extrusion_view(arcs), { -2000, 0 });
    CHECK(arc.x == 0);
    CHECK(arc.y == -1000);

    g_seam_state.result = { 500, 500 };
    CHECK_THROWS_WITH(
        placer.place_seam(extrusion_view(square), { 0, 0 }),
        "The seam placer returned a point outside the loop path.");

    ExtrusionEntity open(false, ArcPolyline(Points { Point(0, 0), Point(1000, 0) }));
    CHECK_THROWS_AS(
        placer.place_seam(extrusion_view(open), { 0, 0 }),
        std::invalid_argument);
}

TEST_CASE("Seam placer failures remain contained and diagnostics are thread local",
          "[plugins][seam-placer][errors]")
{
    ensure_seam_provider_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    ActivePluginGuard active(orchestrator);
    Print print;

    g_seam_state.throw_initialize = true;
    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_seam_placer failed_context = {};
    failed_context.plan = reinterpret_cast<const printing_plan_handle *>(&plan);
    slic3r_api::OrchestratorView orchestrator_view(
        reinterpret_cast<orchestrator_handle *>(&orchestrator));
    const slic3r_api::PluginView provider =
        orchestrator_view.select_plugin(nullptr, SEAM_PLACER, SeamPluginGroup);
    REQUIRE(provider.valid());
    CHECK(orchestrator_view.execute(
              provider,
              reinterpret_cast<const print_handle *>(&print),
              failed_context) == RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
    CHECK(failed_context.instance.session == nullptr);
    CHECK(g_seam_state.destroy_count.load(std::memory_order_relaxed) == 1);

    orchestrator.reset_plugin_cancel();
    g_seam_state.throw_initialize = false;
    slic3r_api::SeamPlacer placer = execute_seam_provider(print);
    ExtrusionEntity square = make_square_loop();
    const slic3r_api::ExtrusionEntity loop = extrusion_view(square);
    g_seam_state.throw_place = true;

    std::future<std::string> first = std::async(std::launch::async, [&placer, loop]() {
        try {
            placer.place_seam(loop, { 11, 0 });
        } catch (const std::exception &exception) {
            return std::string(exception.what());
        }
        return std::string();
    });
    std::future<std::string> second = std::async(std::launch::async, [&placer, loop]() {
        try {
            placer.place_seam(loop, { 22, 0 });
        } catch (const std::exception &exception) {
            return std::string(exception.what());
        }
        return std::string();
    });
    CHECK(first.get() == "seam failure 11");
    CHECK(second.get() == "seam failure 22");
}

TEST_CASE("SeamPlacer rejects incomplete instances without taking ownership",
          "[plugins][seam-placer][ownership]")
{
    slic3r_api::SeamPlacer empty;
    CHECK_FALSE(empty.valid());

    raw_seam_placer_instance incomplete = {};
    incomplete.struct_size = sizeof(incomplete);
    CHECK_THROWS_AS(slic3r_api::SeamPlacer(incomplete), std::invalid_argument);
    CHECK(incomplete.struct_size == sizeof(incomplete));

    int session_storage = 0;
    raw_seam_placer_vtable incomplete_vtable = {};
    incomplete_vtable.struct_size = sizeof(incomplete_vtable);
    incomplete_vtable.destroy = [](void *) {};

    incomplete.session = &session_storage;
    incomplete.vtable = &incomplete_vtable;
    CHECK_THROWS_AS(slic3r_api::SeamPlacer(incomplete), std::invalid_argument);
    CHECK(incomplete.session == &session_storage);
    CHECK(incomplete.vtable == &incomplete_vtable);
}
