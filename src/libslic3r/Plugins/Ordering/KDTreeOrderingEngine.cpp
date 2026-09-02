///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
KD-tree-assisted item ordering
==============================

This implementation first flattens the candidates while retaining their input
item indices. Small problems use dynamic programming to find the globally
lowest-cost route. Large problems repeatedly select the cheapest candidate
reachable from the current exit using a KD-tree and remove all alternatives of
the selected item.

The KD-tree stores only estimated entry coordinates. Candidate-specific ratios
and penalties are evaluated by a custom visitor; conservative global minima
provide a geometric search radius without changing the public cost formula.
*/

#include "KDTreeOrderingEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/KDTreeIndirect.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

constexpr size_t EXACT_ITEM_LIMIT = 10;
constexpr size_t EXACT_CANDIDATE_LIMIT = 64;

struct CandidateReference
{
    uint32_t item_index = 0;
    uint32_t candidate_index = 0;
};

struct PreparedOrdering
{
    const std::vector<OrderingItem> &items;
    std::vector<CandidateReference> candidates;
    std::vector<std::vector<size_t>> candidates_by_item;
};

struct CandidateChoice
{
    size_t flat_index = size_t(-1);
    double cost = (std::numeric_limits<double>::infinity)();
};

/* Validate every public value and build the compact indices shared by both solvers. */
PreparedOrdering prepare_ordering(const std::vector<OrderingItem> &items);

/* Return the candidate selected as deterministic spatial anchor when no start is known. */
size_t spatial_anchor(const PreparedOrdering &prepared);

/* Solve a bounded generalized Hamiltonian path over items and candidate states. */
std::vector<OrderedItem> solve_exact(const PreparedOrdering &prepared,
                                     std::optional<c_point> initial_position);

/* Build a scalable route by repeatedly selecting the cheapest active destination. */
std::vector<OrderedItem> solve_spatial(const PreparedOrdering &prepared,
                                       std::optional<c_point> initial_position);

/* Convert one flat reference back to the public candidate supplied by its caller. */
const OrderingCandidate &candidate_at(const PreparedOrdering &prepared, size_t flat_index);

/* Measure the planar distance between scaled points and return millimetres. */
double distance_mm(c_point from, c_point to);

/* Evaluate the documented cost of entering destination from one physical point. */
double transition_cost(c_point from, const OrderingCandidate &destination);

/* Compare candidates deterministically after their computed costs are known. */
bool choice_is_better(const PreparedOrdering &prepared,
                      size_t candidate_index,
                      double candidate_cost,
                      const CandidateChoice &current);

/* Convert selected flat candidate indices to an owning public result. */
std::vector<OrderedItem> make_result(const PreparedOrdering &prepared,
                                     const std::vector<size_t> &selected_candidates);

PreparedOrdering prepare_ordering(const std::vector<OrderingItem> &items)
{
    if (items.size() > size_t((std::numeric_limits<uint32_t>::max)()))
        throw std::invalid_argument("OrderingEngine cannot index more than UINT32_MAX items.");

    PreparedOrdering prepared{items, {}, {}};
    prepared.candidates_by_item.resize(items.size());

    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        const OrderingItem &item = items[item_index];
        if (item.candidates.empty())
            throw std::invalid_argument("Every OrderingItem must provide at least one candidate.");
        if (item.candidates.size() > size_t((std::numeric_limits<uint32_t>::max)()))
            throw std::invalid_argument("OrderingEngine cannot index more than UINT32_MAX candidates per item.");

        prepared.candidates_by_item[item_index].reserve(item.candidates.size());
        for (size_t candidate_index = 0; candidate_index < item.candidates.size(); ++candidate_index) {
            const OrderingCandidate &candidate = item.candidates[candidate_index];
            if (!std::isfinite(candidate.distance_ratio) || candidate.distance_ratio < 0.0)
                throw std::invalid_argument("OrderingCandidate distance_ratio must be finite and non-negative.");
            if (!std::isfinite(candidate.penalty_mm) || candidate.penalty_mm < 0.0)
                throw std::invalid_argument("OrderingCandidate penalty_mm must be finite and non-negative.");

            const size_t flat_index = prepared.candidates.size();
            prepared.candidates.push_back(CandidateReference{
                uint32_t(item_index), uint32_t(candidate_index)});
            prepared.candidates_by_item[item_index].push_back(flat_index);
        }
    }

    return prepared;
}

size_t spatial_anchor(const PreparedOrdering &prepared)
{
    size_t best_index = 0;
    for (size_t flat_index = 1; flat_index < prepared.candidates.size(); ++flat_index) {
        const OrderingCandidate &candidate = candidate_at(prepared, flat_index);
        const OrderingCandidate &best = candidate_at(prepared, best_index);
        const CandidateReference &reference = prepared.candidates[flat_index];
        const CandidateReference &best_reference = prepared.candidates[best_index];

        /*
        The anchor is deliberately independent of candidate cost: without a
        machine position there is no incoming travel to optimize. Y/X gives a
        stable spatial starting edge, and input indices settle exact ties.
        */
        if (candidate.estimated_entry.y < best.estimated_entry.y ||
            (candidate.estimated_entry.y == best.estimated_entry.y &&
             (candidate.estimated_entry.x < best.estimated_entry.x ||
              (candidate.estimated_entry.x == best.estimated_entry.x &&
               (reference.item_index < best_reference.item_index ||
                (reference.item_index == best_reference.item_index &&
                 reference.candidate_index < best_reference.candidate_index))))))
            best_index = flat_index;
    }
    return best_index;
}

std::vector<OrderedItem> solve_exact(const PreparedOrdering &prepared,
                                     const std::optional<c_point> initial_position)
{
    const size_t item_count = prepared.items.size();
    const size_t candidate_count = prepared.candidates.size();
    const size_t mask_count = size_t(1) << item_count;
    const double infinity = (std::numeric_limits<double>::infinity)();

    std::vector<double> costs(mask_count * candidate_count, infinity);
    std::vector<int32_t> predecessors(mask_count * candidate_count, -1);

    /*
    A known initial position permits every candidate to start the route. Without
    one, only the deterministic spatial anchor is seeded, so no fictitious
    travel from the origin influences the result.
    */
    if (initial_position) {
        for (size_t flat_index = 0; flat_index < candidate_count; ++flat_index) {
            const CandidateReference &reference = prepared.candidates[flat_index];
            const size_t mask = size_t(1) << reference.item_index;
            costs[mask * candidate_count + flat_index] =
                transition_cost(*initial_position, candidate_at(prepared, flat_index));
        }
    } else {
        const size_t anchor_index = spatial_anchor(prepared);
        const CandidateReference &anchor_reference = prepared.candidates[anchor_index];
        const size_t mask = size_t(1) << anchor_reference.item_index;
        costs[mask * candidate_count + anchor_index] =
            candidate_at(prepared, anchor_index).penalty_mm;
    }

    /*
    Each state stores the cheapest route for one visited-item mask and one final
    candidate. Advancing to an unvisited item evaluates all of that item's
    entry/exit alternatives without creating duplicate items.
    */
    for (size_t mask = 1; mask < mask_count; ++mask) {
        for (size_t last_index = 0; last_index < candidate_count; ++last_index) {
            const double current_cost = costs[mask * candidate_count + last_index];
            if (!std::isfinite(current_cost))
                continue;

            const OrderingCandidate &last_candidate = candidate_at(prepared, last_index);
            for (size_t next_item = 0; next_item < item_count; ++next_item) {
                const size_t next_bit = size_t(1) << next_item;
                if ((mask & next_bit) != 0)
                    continue;

                const size_t next_mask = mask | next_bit;
                for (size_t next_index : prepared.candidates_by_item[next_item]) {
                    const double next_cost = current_cost +
                        transition_cost(last_candidate.estimated_exit,
                                        candidate_at(prepared, next_index));
                    const size_t state_index = next_mask * candidate_count + next_index;

                    if (next_cost < costs[state_index] ||
                        (next_cost == costs[state_index] &&
                         (predecessors[state_index] < 0 ||
                          last_index < size_t(predecessors[state_index])))) {
                        costs[state_index] = next_cost;
                        predecessors[state_index] = int32_t(last_index);
                    }
                }
            }
        }
    }

    const size_t final_mask = mask_count - 1;
    CandidateChoice best;
    for (size_t flat_index = 0; flat_index < candidate_count; ++flat_index) {
        const double cost = costs[final_mask * candidate_count + flat_index];
        if (std::isfinite(cost) && choice_is_better(prepared, flat_index, cost, best)) {
            best.flat_index = flat_index;
            best.cost = cost;
        }
    }
    if (best.flat_index == size_t(-1))
        throw std::runtime_error("OrderingEngine could not build a complete exact route.");

    /* Follow predecessor states backward, removing the final item's bit at each step. */
    std::vector<size_t> selected_candidates;
    selected_candidates.reserve(item_count);
    size_t mask = final_mask;
    int32_t current_index = int32_t(best.flat_index);
    while (current_index >= 0) {
        selected_candidates.push_back(size_t(current_index));
        const CandidateReference &reference = prepared.candidates[size_t(current_index)];
        const int32_t predecessor = predecessors[mask * candidate_count + size_t(current_index)];
        mask &= ~(size_t(1) << reference.item_index);
        current_index = predecessor;
    }
    std::reverse(selected_candidates.begin(), selected_candidates.end());
    if (selected_candidates.size() != item_count)
        throw std::runtime_error("OrderingEngine exact route omitted an input item.");
    return make_result(prepared, selected_candidates);
}

struct PositiveCandidateCoordinate
{
    const PreparedOrdering *prepared = nullptr;

    double operator()(const size_t flat_index, const size_t dimension) const
    {
        const c_point point = candidate_at(*prepared, flat_index).estimated_entry;
        return dimension == 0 ? double(point.x) : double(point.y);
    }
};

using PositiveCandidateTree = Slic3r::KDTreeIndirect<2, double, PositiveCandidateCoordinate>;

struct PositiveTreeState
{
    std::unique_ptr<PositiveCandidateTree> tree;
    size_t indexed_count = 0;
    size_t active_count = 0;
    double minimum_ratio = (std::numeric_limits<double>::infinity)();
    double minimum_penalty = (std::numeric_limits<double>::infinity)();
};

struct ZeroCandidateKey
{
    double penalty = 0.0;
    uint32_t item_index = 0;
    uint32_t candidate_index = 0;
    size_t flat_index = 0;

    bool operator<(const ZeroCandidateKey &other) const
    {
        if (penalty != other.penalty)
            return penalty < other.penalty;
        if (item_index != other.item_index)
            return item_index < other.item_index;
        return candidate_index < other.candidate_index;
    }
};

/* Rebuild the positive-ratio KD-tree from candidates whose items remain active. */
void rebuild_positive_tree(const PreparedOrdering &prepared,
                           const std::vector<uint8_t> &selected_items,
                           PositiveTreeState &state);

/* Find the exact cheapest positive-ratio candidate under the weighted cost formula. */
CandidateChoice nearest_positive_candidate(const PreparedOrdering &prepared,
                                           const std::vector<uint8_t> &selected_items,
                                           const PositiveTreeState &state,
                                           c_point current_position,
                                           CandidateChoice initial_choice);

void rebuild_positive_tree(const PreparedOrdering &prepared,
                           const std::vector<uint8_t> &selected_items,
                           PositiveTreeState &state)
{
    std::vector<size_t> indices;
    state.minimum_ratio = (std::numeric_limits<double>::infinity)();
    state.minimum_penalty = (std::numeric_limits<double>::infinity)();

    for (size_t flat_index = 0; flat_index < prepared.candidates.size(); ++flat_index) {
        const CandidateReference &reference = prepared.candidates[flat_index];
        const OrderingCandidate &candidate = candidate_at(prepared, flat_index);
        if (selected_items[reference.item_index] != 0 || candidate.distance_ratio == 0.0)
            continue;

        indices.push_back(flat_index);
        state.minimum_ratio = std::min(state.minimum_ratio, candidate.distance_ratio);
        state.minimum_penalty = std::min(state.minimum_penalty, candidate.penalty_mm);
    }

    state.indexed_count = indices.size();
    state.active_count = indices.size();
    if (indices.empty()) {
        state.tree.reset();
        return;
    }

    state.tree = std::make_unique<PositiveCandidateTree>(PositiveCandidateCoordinate{&prepared});
    state.tree->build(indices);
}

CandidateChoice nearest_positive_candidate(const PreparedOrdering &prepared,
                                           const std::vector<uint8_t> &selected_items,
                                           const PositiveTreeState &state,
                                           const c_point current_position,
                                           CandidateChoice initial_choice)
{
    if (!state.tree)
        return initial_choice;

    struct Visitor
    {
        const PreparedOrdering &prepared;
        const std::vector<uint8_t> &selected_items;
        const PositiveTreeState &state;
        const c_point current_position;
        CandidateChoice best;

        unsigned int operator()(const size_t flat_index, const size_t dimension)
        {
            const CandidateReference &reference = prepared.candidates[flat_index];
            if (selected_items[reference.item_index] == 0) {
                const double cost = transition_cost(
                    current_position, candidate_at(prepared, flat_index));
                if (choice_is_better(prepared, flat_index, cost, best)) {
                    best.flat_index = flat_index;
                    best.cost = cost;
                }
            }

            /*
            Any unseen positive-ratio candidate costs at least
            distance*minimum_ratio + minimum_penalty. Converting the remaining
            affordable cost back to a scaled geometric radius lets the KD-tree
            prune safely despite candidate-specific weights.
            */
            double radius_squared = (std::numeric_limits<double>::infinity)();
            if (std::isfinite(best.cost)) {
                const double remaining_cost = best.cost - state.minimum_penalty;
                if (remaining_cost <= 0.0) {
                    radius_squared = 0.0;
                } else {
                    const double radius_mm = std::nextafter(
                        remaining_cost / state.minimum_ratio,
                        (std::numeric_limits<double>::infinity)());
                    const double radius_scaled = radius_mm * UNSCALING_FACTOR;
                    radius_squared = radius_scaled * radius_scaled;
                }
            }

            const double query_coordinate = dimension == 0 ?
                double(current_position.x) : double(current_position.y);
            return state.tree->descent_mask(
                query_coordinate, radius_squared, flat_index, dimension);
        }
    };

    Visitor visitor{prepared, selected_items, state, current_position, initial_choice};
    state.tree->visit(visitor);
    return visitor.best;
}

std::vector<OrderedItem> solve_spatial(const PreparedOrdering &prepared,
                                       const std::optional<c_point> initial_position)
{
    std::vector<uint8_t> selected_items(prepared.items.size(), uint8_t(0));
    std::set<ZeroCandidateKey> zero_candidates;
    for (size_t flat_index = 0; flat_index < prepared.candidates.size(); ++flat_index) {
        const CandidateReference &reference = prepared.candidates[flat_index];
        const OrderingCandidate &candidate = candidate_at(prepared, flat_index);
        if (candidate.distance_ratio == 0.0)
            zero_candidates.insert(ZeroCandidateKey{
                candidate.penalty_mm, reference.item_index,
                reference.candidate_index, flat_index});
    }

    PositiveTreeState positive_tree;
    rebuild_positive_tree(prepared, selected_items, positive_tree);

    std::vector<size_t> selected_candidates;
    selected_candidates.reserve(prepared.items.size());
    std::optional<c_point> current_position = initial_position;

    /* With no machine position, consume the fixed spatial anchor before nearest-neighbor search. */
    if (!current_position) {
        const size_t anchor_index = spatial_anchor(prepared);
        selected_candidates.push_back(anchor_index);
        const uint32_t anchor_item = prepared.candidates[anchor_index].item_index;
        selected_items[anchor_item] = 1;
        for (const size_t flat_index : prepared.candidates_by_item[anchor_item])
            if (candidate_at(prepared, flat_index).distance_ratio > 0.0)
                --positive_tree.active_count;
        current_position = candidate_at(prepared, anchor_index).estimated_exit;
    }

    while (selected_candidates.size() < prepared.items.size()) {
        /* Remove stale zero-ratio alternatives of items selected by an earlier iteration. */
        while (!zero_candidates.empty() &&
               selected_items[zero_candidates.begin()->item_index] != 0)
            zero_candidates.erase(zero_candidates.begin());

        CandidateChoice best;
        if (!zero_candidates.empty()) {
            best.flat_index = zero_candidates.begin()->flat_index;
            best.cost = zero_candidates.begin()->penalty;
        }
        best = nearest_positive_candidate(
            prepared, selected_items, positive_tree, *current_position, best);
        if (best.flat_index == size_t(-1))
            throw std::runtime_error("OrderingEngine could not find an active spatial candidate.");

        const uint32_t selected_item = prepared.candidates[best.flat_index].item_index;
        selected_candidates.push_back(best.flat_index);
        selected_items[selected_item] = 1;
        current_position = candidate_at(prepared, best.flat_index).estimated_exit;

        /*
        Count positive alternatives removed with this item. Rebuilding after
        inactive entries exceed half keeps filtered KD-tree searches bounded
        without paying a rebuild after every selection.
        */
        for (size_t flat_index : prepared.candidates_by_item[selected_item])
            if (candidate_at(prepared, flat_index).distance_ratio > 0.0)
                --positive_tree.active_count;
        if (positive_tree.active_count > 0 &&
            positive_tree.active_count * 2 < positive_tree.indexed_count)
            rebuild_positive_tree(prepared, selected_items, positive_tree);
    }

    return make_result(prepared, selected_candidates);
}

const OrderingCandidate &candidate_at(const PreparedOrdering &prepared, const size_t flat_index)
{
    const CandidateReference &reference = prepared.candidates[flat_index];
    return prepared.items[reference.item_index].candidates[reference.candidate_index];
}

double distance_mm(const c_point from, const c_point to)
{
    const double dx_mm = (double(to.x) - double(from.x)) * SCALING_FACTOR;
    const double dy_mm = (double(to.y) - double(from.y)) * SCALING_FACTOR;
    return std::hypot(dx_mm, dy_mm);
}

double transition_cost(const c_point from, const OrderingCandidate &destination)
{
    return distance_mm(from, destination.estimated_entry) *
               destination.distance_ratio +
           destination.penalty_mm;
}

bool choice_is_better(const PreparedOrdering &prepared,
                      const size_t candidate_index,
                      const double candidate_cost,
                      const CandidateChoice &current)
{
    if (current.flat_index == size_t(-1) || candidate_cost < current.cost)
        return true;
    if (candidate_cost > current.cost)
        return false;

    const CandidateReference &candidate = prepared.candidates[candidate_index];
    const CandidateReference &best = prepared.candidates[current.flat_index];
    return candidate.item_index < best.item_index ||
           (candidate.item_index == best.item_index &&
            candidate.candidate_index < best.candidate_index);
}

std::vector<OrderedItem> make_result(const PreparedOrdering &prepared,
                                     const std::vector<size_t> &selected_candidates)
{
    std::vector<OrderedItem> result;
    result.reserve(selected_candidates.size());
    for (const size_t flat_index : selected_candidates) {
        const CandidateReference &reference = prepared.candidates[flat_index];
        result.push_back(OrderedItem{
            reference.item_index, candidate_at(prepared, flat_index)});
    }
    return result;
}

} // namespace

std::vector<OrderedItem> KDTreeOrderingEngine::order(
    const std::vector<OrderingItem> &items,
    const std::optional<c_point> initial_position) const
{
    const PreparedOrdering prepared = prepare_ordering(items);
    if (prepared.items.empty())
        return {};

    if (prepared.items.size() <= EXACT_ITEM_LIMIT &&
        prepared.candidates.size() <= EXACT_CANDIDATE_LIMIT)
        return solve_exact(prepared, initial_position);
    return solve_spatial(prepared, initial_position);
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
