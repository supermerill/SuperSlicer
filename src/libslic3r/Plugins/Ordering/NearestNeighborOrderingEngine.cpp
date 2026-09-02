///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Greedy nearest-neighbor item ordering
=====================================

This file provides the deliberately simple OrderingEngine implementation. It
keeps only one selected flag per input item. Starting from the supplied machine
position, it repeatedly scans all candidates of all unselected items, appends
the cheapest candidate, and continues from that candidate's estimated exit.

The implementation repeats a few small validation and cost helpers instead of
depending on the KD-tree engine. A developer can therefore read this file from
top to bottom to understand the complete algorithm and use it as a reference
when implementing another ordering strategy.
*/

#include "NearestNeighborOrderingEngine.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

struct CandidateChoice
{
    uint32_t item_index = (std::numeric_limits<uint32_t>::max)();
    uint32_t candidate_index = (std::numeric_limits<uint32_t>::max)();
    double cost = (std::numeric_limits<double>::infinity)();
};

/* Reject values that cannot participate safely in the documented cost formula. */
void validate_items(const std::vector<OrderingItem> &items);

/* Choose the deterministic Y/X starting candidate used when no machine position is known. */
CandidateChoice spatial_anchor(const std::vector<OrderingItem> &items);

/* Scan all candidates of unselected items and return the cheapest reachable destination. */
CandidateChoice nearest_candidate(const std::vector<OrderingItem> &items,
                                  const std::vector<uint8_t> &selected_items,
                                  c_point current_position);

/* Evaluate one destination using the public weighted-distance cost contract. */
double transition_cost(c_point from, const OrderingCandidate &destination);

/* Convert a planar distance between scaled API points to millimetres. */
double distance_mm(c_point from, c_point to);

/* Resolve equal costs deterministically by original item and candidate indices. */
bool choice_is_better(uint32_t item_index,
                      uint32_t candidate_index,
                      double cost,
                      const CandidateChoice &current);

void validate_items(const std::vector<OrderingItem> &items)
{
    if (items.size() > size_t((std::numeric_limits<uint32_t>::max)()))
        throw std::invalid_argument("OrderingEngine cannot index more than UINT32_MAX items.");

    for (const OrderingItem &item : items) {
        if (item.candidates.empty())
            throw std::invalid_argument("Every OrderingItem must provide at least one candidate.");
        if (item.candidates.size() > size_t((std::numeric_limits<uint32_t>::max)()))
            throw std::invalid_argument("OrderingEngine cannot index more than UINT32_MAX candidates per item.");

        for (const OrderingCandidate &candidate : item.candidates) {
            if (!std::isfinite(candidate.distance_ratio) || candidate.distance_ratio < 0.0)
                throw std::invalid_argument("OrderingCandidate distance_ratio must be finite and non-negative.");
            if (!std::isfinite(candidate.penalty_mm) || candidate.penalty_mm < 0.0)
                throw std::invalid_argument("OrderingCandidate penalty_mm must be finite and non-negative.");
        }
    }
}

CandidateChoice spatial_anchor(const std::vector<OrderingItem> &items)
{
    CandidateChoice best;
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        const OrderingItem &item = items[item_index];
        for (size_t candidate_index = 0; candidate_index < item.candidates.size(); ++candidate_index) {
            const OrderingCandidate &candidate = item.candidates[candidate_index];
            if (best.item_index == (std::numeric_limits<uint32_t>::max)()) {
                best.item_index = uint32_t(item_index);
                best.candidate_index = uint32_t(candidate_index);
                continue;
            }

            const OrderingCandidate &current =
                items[best.item_index].candidates[best.candidate_index];
            if (candidate.estimated_entry.y < current.estimated_entry.y ||
                (candidate.estimated_entry.y == current.estimated_entry.y &&
                 (candidate.estimated_entry.x < current.estimated_entry.x ||
                  (candidate.estimated_entry.x == current.estimated_entry.x &&
                   (item_index < best.item_index ||
                    (item_index == best.item_index &&
                     candidate_index < best.candidate_index)))))) {
                best.item_index = uint32_t(item_index);
                best.candidate_index = uint32_t(candidate_index);
            }
        }
    }
    return best;
}

CandidateChoice nearest_candidate(const std::vector<OrderingItem> &items,
                                  const std::vector<uint8_t> &selected_items,
                                  const c_point current_position)
{
    CandidateChoice best;
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        if (selected_items[item_index] != 0)
            continue;

        const OrderingItem &item = items[item_index];
        for (size_t candidate_index = 0; candidate_index < item.candidates.size(); ++candidate_index) {
            const double cost = transition_cost(current_position, item.candidates[candidate_index]);
            if (choice_is_better(
                    uint32_t(item_index), uint32_t(candidate_index), cost, best)) {
                best.item_index = uint32_t(item_index);
                best.candidate_index = uint32_t(candidate_index);
                best.cost = cost;
            }
        }
    }
    return best;
}

double transition_cost(const c_point from, const OrderingCandidate &destination)
{
    return distance_mm(from, destination.estimated_entry) *
               destination.distance_ratio +
           destination.penalty_mm;
}

double distance_mm(const c_point from, const c_point to)
{
    const double dx_mm = (double(to.x) - double(from.x)) * SCALING_FACTOR;
    const double dy_mm = (double(to.y) - double(from.y)) * SCALING_FACTOR;
    return std::hypot(dx_mm, dy_mm);
}

bool choice_is_better(const uint32_t item_index,
                      const uint32_t candidate_index,
                      const double cost,
                      const CandidateChoice &current)
{
    if (current.item_index == (std::numeric_limits<uint32_t>::max)() || cost < current.cost)
        return true;
    if (cost > current.cost)
        return false;
    return item_index < current.item_index ||
           (item_index == current.item_index &&
            candidate_index < current.candidate_index);
}

} // namespace

std::vector<OrderedItem> NearestNeighborOrderingEngine::order(
    const std::vector<OrderingItem> &items,
    const std::optional<c_point> initial_position) const
{
    validate_items(items);
    if (items.empty())
        return {};

    std::vector<uint8_t> selected_items(items.size(), uint8_t(0));
    std::vector<OrderedItem> result;
    result.reserve(items.size());
    std::optional<c_point> current_position = initial_position;

    /* Without a machine position, select the shared deterministic spatial anchor. */
    if (!current_position) {
        const CandidateChoice anchor = spatial_anchor(items);
        const OrderingCandidate &candidate =
            items[anchor.item_index].candidates[anchor.candidate_index];
        result.push_back(OrderedItem{anchor.item_index, candidate});
        selected_items[anchor.item_index] = 1;
        current_position = candidate.estimated_exit;
    }

    /* Each iteration permanently selects one item and discards its alternative candidates. */
    while (result.size() < items.size()) {
        const CandidateChoice choice = nearest_candidate(items, selected_items, *current_position);
        if (choice.item_index == (std::numeric_limits<uint32_t>::max)())
            throw std::runtime_error("OrderingEngine could not find an unselected candidate.");

        const OrderingCandidate &candidate =
            items[choice.item_index].candidates[choice.candidate_index];
        result.push_back(OrderedItem{choice.item_index, candidate});
        selected_items[choice.item_index] = 1;
        current_position = candidate.estimated_exit;
    }

    return result;
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
