#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "libslic3r/Api/plugin/cpp/OrderingEngine.hpp"
#include "libslic3r/Plugins/Ordering/KDTreeOrderingEngine.hpp"
#include "libslic3r/Plugins/Ordering/NearestNeighborOrderingEngine.hpp"

namespace {

using slic3r_api::Ordering::OrderedItem;
using slic3r_api::Ordering::OrderingCandidate;
using slic3r_api::Ordering::OrderingEngine;
using slic3r_api::Ordering::OrderingItem;
using slic3r_api::Ordering::DefaultOrderingPlugin::KDTreeOrderingEngine;
using slic3r_api::Ordering::DefaultOrderingPlugin::NearestNeighborOrderingEngine;

/* Build a scaled API point from millimetre coordinates used by the tests. */
c_point point_mm(const double x, const double y)
{
    return c_point{scale_i(x), scale_i(y)};
}

/* Build one candidate with concise millimetre-based test syntax. */
OrderingCandidate candidate(const double entry_x,
                            const double entry_y,
                            const double exit_x,
                            const double exit_y,
                            const double distance_ratio = 1.0,
                            const double penalty_mm = 0.0)
{
    return OrderingCandidate{
        point_mm(entry_x, entry_y), point_mm(exit_x, exit_y),
        distance_ratio, penalty_mm};
}

/* Extract the original item permutation without hiding candidate assertions. */
std::vector<uint32_t> item_order(const std::vector<OrderedItem> &ordered)
{
    std::vector<uint32_t> result;
    result.reserve(ordered.size());
    for (const OrderedItem &item : ordered)
        result.push_back(item.item_index);
    return result;
}

class TestOrderingEngine final : public OrderingEngine
{
public:
    std::vector<OrderedItem> order(
        const std::vector<OrderingItem> &items,
        std::optional<c_point>) const override
    {
        std::vector<OrderedItem> result;
        for (size_t idx = 0; idx < items.size(); ++idx)
            result.push_back(OrderedItem{uint32_t(idx), items[idx].candidates.front()});
        return result;
    }
};

TEST_CASE("OrderingEngine is a replaceable public interface", "[plugins][ordering][engine]")
{
    TestOrderingEngine engine;
    const std::vector<OrderingItem> items = {
        OrderingItem{{candidate(0.0, 0.0, 1.0, 0.0)}}};

    const std::vector<OrderedItem> result = engine.order(items, std::nullopt);
    REQUIRE(result.size() == 1);
    CHECK(result.front().item_index == 0);
}

TEST_CASE("KDTreeOrderingEngine handles empty and single-item requests", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    CHECK(engine.order({}).empty());

    const OrderingCandidate only_candidate = candidate(2.0, 3.0, 4.0, 5.0, 0.5, 1.25);
    const std::vector<OrderedItem> result = engine.order(
        {OrderingItem{{only_candidate}}}, point_mm(0.0, 0.0));

    REQUIRE(result.size() == 1);
    CHECK(result.front().item_index == 0);
    CHECK(result.front().selected_candidate.estimated_entry.x == only_candidate.estimated_entry.x);
    CHECK(result.front().selected_candidate.estimated_entry.y == only_candidate.estimated_entry.y);
    CHECK(result.front().selected_candidate.estimated_exit.x == only_candidate.estimated_exit.x);
    CHECK(result.front().selected_candidate.estimated_exit.y == only_candidate.estimated_exit.y);
    CHECK(result.front().selected_candidate.distance_ratio == only_candidate.distance_ratio);
    CHECK(result.front().selected_candidate.penalty_mm == only_candidate.penalty_mm);
}

TEST_CASE("KDTreeOrderingEngine selects reversible orientations and loop seams", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    SECTION("a reversible open path exposes its two directions") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(0.0, 0.0, 10.0, 0.0),
                candidate(10.0, 0.0, 0.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(9.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(10.0));
        CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(0.0));
    }

    SECTION("a loop exposes one equal entry and exit per seam") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(0.0, 0.0, 0.0, 0.0),
                candidate(20.0, 0.0, 20.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(18.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(20.0));
        CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(20.0));
    }
}

TEST_CASE("KDTreeOrderingEngine evaluates ratios and penalties in millimetres", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    SECTION("a candidate-specific ratio weights incoming distance") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(10.0, 0.0, 10.0, 0.0, 1.0, 0.0),
                candidate(100.0, 0.0, 100.0, 0.0, 0.05, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(100.0));
    }

    SECTION("penalty_mm is added after weighted travel distance") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(1.0, 0.0, 1.0, 0.0, 1.0, 10.0),
                candidate(5.0, 0.0, 5.0, 0.0, 1.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(5.0));
    }
}

TEST_CASE("KDTreeOrderingEngine chooses a deterministic anchor without a start", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;
    const std::vector<OrderingItem> items = {
        OrderingItem{{candidate(-100.0, 5.0, -100.0, 5.0)}},
        OrderingItem{{candidate(10.0, -2.0, 10.0, -2.0)}},
        OrderingItem{{candidate(-10.0, -2.0, -10.0, -2.0)}}};

    const std::vector<OrderedItem> result = engine.order(items);
    REQUIRE(result.size() == 3);
    CHECK(result.front().item_index == 2);
    CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(-10.0));
    CHECK(result.front().selected_candidate.estimated_entry.y == scale_i(-2.0));
}

TEST_CASE("KDTreeOrderingEngine exact solver optimizes the complete route", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    /*
    Item 0 may finish near either remaining item. The exact solver considers
    that exit while choosing its orientation, rather than selecting only the
    nearest entry and committing to a poor continuation.
    */
    const std::vector<OrderingItem> items = {
        OrderingItem{{
            candidate(1.0, 0.0, 100.0, 0.0),
            candidate(2.0, 0.0, 10.0, 0.0)}},
        OrderingItem{{candidate(11.0, 0.0, 11.0, 0.0)}},
        OrderingItem{{candidate(12.0, 0.0, 12.0, 0.0)}}};

    const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
    REQUIRE(result.size() == 3);
    CHECK(result.front().item_index == 0);
    CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(10.0));
    CHECK(item_order(result) == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("KDTreeOrderingEngine spatial solver orders large requests", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;
    std::vector<OrderingItem> items;

    /* Eleven items exceed the exact item threshold and exercise the KD-tree path. */
    for (uint32_t idx = 0; idx < 11; ++idx) {
        const double x = double(11 - idx) * 10.0;
        items.push_back(OrderingItem{{candidate(x, 0.0, x, 0.0)}});
    }

    const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
    REQUIRE(result.size() == items.size());
    CHECK(item_order(result) ==
          std::vector<uint32_t>{10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
}

TEST_CASE("KDTreeOrderingEngine spatial solver handles zero distance ratios", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;
    std::vector<OrderingItem> items;
    for (uint32_t idx = 0; idx < 10; ++idx) {
        const double x = double(idx + 1);
        items.push_back(OrderingItem{{candidate(x, 0.0, x, 0.0, 1.0, 0.0)}});
    }
    items.push_back(OrderingItem{{candidate(1000.0, 0.0, 1000.0, 0.0, 0.0, 0.25)}});

    const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
    REQUIRE(result.size() == items.size());
    CHECK(result.front().item_index == 10);
}

TEST_CASE("KDTreeOrderingEngine spatial solver honors weighted costs and ties",
          "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    SECTION("candidate ratios and penalties participate in indexed search") {
        std::vector<OrderingItem> items = {
            OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0, 1.0, 10.0)}},
            OrderingItem{{candidate(100.0, 0.0, 100.0, 0.0, 0.01, 0.0)}}};
        for (uint32_t idx = 0; idx < 9; ++idx) {
            const double x = 1000.0 + double(idx) * 10.0;
            items.push_back(OrderingItem{{candidate(x, 0.0, x, 0.0)}});
        }

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == items.size());
        CHECK(result.front().item_index == 1);
    }

    SECTION("equal costs are settled by original item index") {
        std::vector<OrderingItem> items;
        for (uint32_t idx = 0; idx < 11; ++idx)
            items.push_back(OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0)}});

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == items.size());
        CHECK(item_order(result) ==
              std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    }
}

TEST_CASE("KDTreeOrderingEngine returns every original item exactly once", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;
    const std::vector<OrderingItem> items = {
        OrderingItem{{candidate(30.0, 0.0, 30.0, 0.0), candidate(3.0, 0.0, 3.0, 0.0)}},
        OrderingItem{{candidate(2.0, 0.0, 2.0, 0.0)}},
        OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0)}}};

    const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
    std::vector<uint32_t> indices = item_order(result);
    std::sort(indices.begin(), indices.end());
    CHECK(indices == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("NearestNeighborOrderingEngine handles basic requests", "[plugins][ordering][engine]")
{
    const NearestNeighborOrderingEngine engine;

    CHECK(engine.order({}).empty());

    SECTION("a single item returns its cheapest candidate") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(1.0, 0.0, 2.0, 0.0, 1.0, 10.0),
                candidate(5.0, 0.0, 6.0, 0.0, 1.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().item_index == 0);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(5.0));
        CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(6.0));
    }

    SECTION("every original item is returned exactly once") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{candidate(3.0, 0.0, 3.0, 0.0)}},
            OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0)}},
            OrderingItem{{candidate(2.0, 0.0, 2.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        CHECK(item_order(result) == std::vector<uint32_t>{1, 2, 0});
    }
}

TEST_CASE("NearestNeighborOrderingEngine selects candidate alternatives",
          "[plugins][ordering][engine]")
{
    const NearestNeighborOrderingEngine engine;

    SECTION("entry and exit describe reversible path directions") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(0.0, 0.0, 10.0, 0.0),
                candidate(10.0, 0.0, 0.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(9.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(10.0));
        CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(0.0));
    }

    SECTION("multiple loop seams are ordinary alternatives") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(0.0, 0.0, 0.0, 0.0),
                candidate(20.0, 0.0, 20.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(18.0, 0.0));
        REQUIRE(result.size() == 1);
        CHECK(result.front().selected_candidate.estimated_entry.x == scale_i(20.0));
    }

    SECTION("ratio and penalty both participate in the greedy choice") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0, 1.0, 10.0)}},
            OrderingItem{{candidate(100.0, 0.0, 100.0, 0.0, 0.01, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 2);
        CHECK(result.front().item_index == 1);
    }

    SECTION("a zero ratio removes distance from the candidate cost") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{candidate(1.0, 0.0, 1.0, 0.0, 1.0, 1.0)}},
            OrderingItem{{candidate(1000.0, 0.0, 1000.0, 0.0, 0.0, 0.5)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 2);
        CHECK(result.front().item_index == 1);
    }
}

TEST_CASE("NearestNeighborOrderingEngine uses deterministic starts and ties",
          "[plugins][ordering][engine]")
{
    const NearestNeighborOrderingEngine engine;

    SECTION("the missing initial position selects the lowest Y then X") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{candidate(-100.0, 5.0, -100.0, 5.0)}},
            OrderingItem{{candidate(10.0, -2.0, 10.0, -2.0)}},
            OrderingItem{{candidate(-10.0, -2.0, -10.0, -2.0)}}};

        const std::vector<OrderedItem> result = engine.order(items);
        REQUIRE(result.size() == 3);
        CHECK(result.front().item_index == 2);
    }

    SECTION("equal costs preserve item and candidate index ordering") {
        const std::vector<OrderingItem> items = {
            OrderingItem{{
                candidate(1.0, 0.0, 1.0, 0.0),
                candidate(1.0, 0.0, 2.0, 0.0)}},
            OrderingItem{{candidate(-1.0, 0.0, -1.0, 0.0)}}};

        const std::vector<OrderedItem> result = engine.order(items, point_mm(0.0, 0.0));
        REQUIRE(result.size() == 2);
        CHECK(result.front().item_index == 0);
        CHECK(result.front().selected_candidate.estimated_exit.x == scale_i(1.0));
    }
}

TEST_CASE("NearestNeighborOrderingEngine remains deliberately greedy",
          "[plugins][ordering][engine]")
{
    const NearestNeighborOrderingEngine nearest_engine;
    const KDTreeOrderingEngine optimized_engine;
    const std::vector<OrderingItem> items = {
        OrderingItem{{
            candidate(1.0, 0.0, 100.0, 0.0),
            candidate(2.0, 0.0, 10.0, 0.0)}},
        OrderingItem{{candidate(11.0, 0.0, 11.0, 0.0)}},
        OrderingItem{{candidate(12.0, 0.0, 12.0, 0.0)}}};

    const std::vector<OrderedItem> nearest =
        nearest_engine.order(items, point_mm(0.0, 0.0));
    const std::vector<OrderedItem> optimized =
        optimized_engine.order(items, point_mm(0.0, 0.0));

    REQUIRE(nearest.size() == 3);
    REQUIRE(optimized.size() == 3);
    CHECK(nearest.front().selected_candidate.estimated_exit.x == scale_i(100.0));
    CHECK(item_order(nearest) == std::vector<uint32_t>{0, 2, 1});
    CHECK(optimized.front().selected_candidate.estimated_exit.x == scale_i(10.0));
    CHECK(item_order(optimized) == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("NearestNeighborOrderingEngine rejects malformed candidates",
          "[plugins][ordering][engine]")
{
    const NearestNeighborOrderingEngine engine;

    CHECK_THROWS_AS(engine.order({OrderingItem{}}), std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(0.0, 0.0, 0.0, 0.0, -1.0, 0.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(0.0, 0.0, 0.0, 0.0, 1.0, -1.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(
            0.0, 0.0, 0.0, 0.0,
            (std::numeric_limits<double>::infinity)(), 0.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(
            0.0, 0.0, 0.0, 0.0, 1.0,
            (std::numeric_limits<double>::quiet_NaN)())}}}),
        std::invalid_argument);
}

TEST_CASE("KDTreeOrderingEngine rejects malformed candidates", "[plugins][ordering][engine]")
{
    const KDTreeOrderingEngine engine;

    CHECK_THROWS_AS(engine.order({OrderingItem{}}), std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(0.0, 0.0, 0.0, 0.0, -1.0, 0.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(0.0, 0.0, 0.0, 0.0, 1.0, -1.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(
            0.0, 0.0, 0.0, 0.0,
            (std::numeric_limits<double>::infinity)(), 0.0)}}}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        engine.order({OrderingItem{{candidate(
            0.0, 0.0, 0.0, 0.0, 1.0,
            (std::numeric_limits<double>::quiet_NaN)())}}}),
        std::invalid_argument);
}

} // namespace
