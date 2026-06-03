///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultOrdering.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api { namespace Ordering { namespace DefaultOrderingPlugin {
namespace {

/*
DefaultToolGroupOrdering is the second plugin in the ordering chain.

The plan builder has already grouped work by print group, print Z and tool. This
plugin does not inspect extrusion geometry. It only reorders tool sections
inside each layer group so consecutive layers can reuse the same tool whenever
that does not disturb the layer's existing relative order too much.

The algorithm treats each layer group as a small "which tool starts / which tool
ends" problem:
  - generate all legal first/last extruder pairs for every printable layer;
  - use dynamic programming to minimize changes between previous.last and
    current.first;
  - move tool groups in each layer so the chosen first and last tools are in the
    requested positions.

The plugin does not split tool groups and does not move extrusions between
tools. Those operations belong to other ordering or post-processing plugins.
*/

const char *k_no_dependencies[] = { nullptr };
const char *k_group_tools = "ordering.tool_groups";

struct ToolOrderCandidate
{
    uint16_t first_extruder = uint16_t(-1);
    uint16_t last_extruder = uint16_t(-1);
    bool valid = false;
};

struct ToolOrderScore
{
    unsigned int tool_changes = (std::numeric_limits<unsigned int>::max)();
    unsigned int initial_penalty = (std::numeric_limits<unsigned int>::max)();
};

/*
Order all tool groups inside one PrintingGroup.

The function chooses one first/last extruder pair per layer, then applies those
choices to the layer groups. It is intentionally local to this plugin so another
tool-ordering plugin can replace the strategy without relying on host callbacks.
*/
void order_tool_groups_in_plugin(const PrintingGroup &printing_group, uint16_t first_extruder = uint16_t(-1));

/*
Return unique extruder ids in their current layer order.

One layer may contain several tool groups with the same extruder after a plugin
splits a tool visit into several sections. Tool-change optimization only needs
each extruder id once; the later reordering keeps same-extruder groups stable.
*/
std::vector<uint16_t> unique_layer_extruders(const PrintingLayerGroup &layer_group);

/*
Generate every first/last tool choice for one printable layer.

The ordering inside the layer is still mostly stable. The candidate only says
which extruder should appear first and which one should appear last so the next
layer can start with fewer tool changes.
*/
std::vector<ToolOrderCandidate> tool_order_candidates_for_extruders(const std::vector<uint16_t> &extruders);

/* Return true when lhs is a better dynamic-programming score than rhs. */
bool tool_order_score_less(const ToolOrderScore &lhs, const ToolOrderScore &rhs);

/*
Select one tool-order candidate per layer with a small dynamic program.

The primary cost is the number of tool changes between consecutive layers.
first_extruder is only a tie-break for the first printable layer; it should not
force a worse global sequence.
*/
std::vector<ToolOrderCandidate> select_tool_order_candidates(const PrintingGroup &printing_group,
                                                             uint16_t first_extruder);

/*
Apply a selected first/last tool choice by moving tool groups in place.

This is a stable category reorder: all groups for the first extruder move to
the front, groups for the last extruder move to the back, and everything else
keeps its relative order in the middle.
*/
void apply_tool_order_candidate(const PrintingLayerGroup &layer_group, const ToolOrderCandidate &candidate);

void order_tool_groups_in_plugin(const PrintingGroup &printing_group, const uint16_t first_extruder)
{
    const std::vector<ToolOrderCandidate> candidates =
        select_tool_order_candidates(printing_group, first_extruder);
    assert(candidates.size() == printing_group.layer_group_count());

    /*
    Candidate selection is computed for the whole PrintingGroup before any
    layer is moved. Applying moves afterwards keeps the DP input stable.
    */
    for (uint32_t layer_idx = 0; layer_idx < printing_group.layer_group_count(); ++layer_idx)
        apply_tool_order_candidate(printing_group.layer_group(layer_idx), candidates[layer_idx]);
}

std::vector<uint16_t> unique_layer_extruders(const PrintingLayerGroup &layer_group)
{
    std::vector<uint16_t> out;
    for (uint32_t idx = 0; idx < layer_group.tool_group_count(); ++idx) {
        const uint16_t extruder_id = layer_group.tool_group(idx).extruder_id();
        /*
        Keep the first occurrence order. If a layer has groups A, B, A, the DP
        only needs "A, B", while apply_tool_order_candidate() later moves both A
        groups together without changing their internal relative order.
        */
        if (std::find(out.begin(), out.end(), extruder_id) == out.end())
            out.push_back(extruder_id);
    }
    return out;
}

std::vector<ToolOrderCandidate> tool_order_candidates_for_extruders(const std::vector<uint16_t> &extruders)
{
    std::vector<ToolOrderCandidate> candidates;
    if (extruders.empty())
        return candidates;
    if (extruders.size() == 1) {
        // A one-tool layer starts and ends with the same extruder.
        candidates.push_back(ToolOrderCandidate{extruders.front(), extruders.front(), true});
        return candidates;
    }

    /*
    With several tools, every distinct first/last pair is possible. Last is
    iterated from the current end toward the beginning so equal-cost solutions
    tend to preserve the existing layer order.
    */
    for (uint16_t first_extruder : extruders)
        for (std::vector<uint16_t>::const_reverse_iterator it = extruders.rbegin(); it != extruders.rend(); ++it)
            if (*it != first_extruder)
                candidates.push_back(ToolOrderCandidate{first_extruder, *it, true});
    return candidates;
}

bool tool_order_score_less(const ToolOrderScore &lhs, const ToolOrderScore &rhs)
{
    if (lhs.tool_changes != rhs.tool_changes)
        return lhs.tool_changes < rhs.tool_changes;
    return lhs.initial_penalty < rhs.initial_penalty;
}

std::vector<ToolOrderCandidate> select_tool_order_candidates(const PrintingGroup &printing_group,
                                                             const uint16_t first_extruder)
{
    std::vector<ToolOrderCandidate> selected(printing_group.layer_group_count());
    std::vector<uint32_t> active_layers;
    std::vector<std::vector<uint16_t>> active_extruders;
    std::vector<std::vector<ToolOrderCandidate>> active_candidates;

    for (uint32_t layer_idx = 0; layer_idx < printing_group.layer_group_count(); ++layer_idx) {
        const PrintingLayerGroup layer_group = printing_group.layer_group(layer_idx);
        std::vector<uint16_t> extruders = unique_layer_extruders(layer_group);
        if (extruders.empty())
            continue;

        /*
        Empty layers stay in selected as invalid candidates. They do not take
        part in the DP, otherwise they would create fake transitions between two
        printable layers.
        */
        active_layers.push_back(layer_idx);
        active_candidates.push_back(tool_order_candidates_for_extruders(extruders));
        active_extruders.push_back(std::move(extruders));
    }

    if (active_layers.empty())
        return selected;

    std::vector<std::vector<ToolOrderScore>> dp(active_layers.size());
    std::vector<std::vector<size_t>> previous(active_layers.size());

    for (size_t layer_idx = 0; layer_idx < active_layers.size(); ++layer_idx) {
        dp[layer_idx].resize(active_candidates[layer_idx].size());
        previous[layer_idx].assign(active_candidates[layer_idx].size(), size_t(-1));

        if (layer_idx == 0) {
            const uint16_t preferred_first =
                first_extruder == uint16_t(-1) ?
                    *(std::min_element(active_extruders[layer_idx].begin(), active_extruders[layer_idx].end())) :
                    first_extruder;

            /*
            The first printable layer has no previous tool-change cost. Give a
            small penalty to candidates that do not start with the preferred
            extruder so ties are deterministic.
            */
            for (size_t candidate_idx = 0; candidate_idx < active_candidates[layer_idx].size(); ++candidate_idx) {
                ToolOrderScore score;
                score.tool_changes = 0;
                score.initial_penalty =
                    active_candidates[layer_idx][candidate_idx].first_extruder == preferred_first ? 0u : 1u;
                dp[layer_idx][candidate_idx] = score;
            }
            continue;
        }

        for (size_t candidate_idx = 0; candidate_idx < active_candidates[layer_idx].size(); ++candidate_idx) {
            for (size_t previous_idx = 0; previous_idx < active_candidates[layer_idx - 1].size(); ++previous_idx) {
                ToolOrderScore score = dp[layer_idx - 1][previous_idx];
                if (active_candidates[layer_idx - 1][previous_idx].last_extruder !=
                    active_candidates[layer_idx][candidate_idx].first_extruder)
                    ++score.tool_changes;

                /*
                Store the best predecessor for this candidate. The predecessor
                table is the breadcrumb trail used below to reconstruct one
                globally best sequence from the last printable layer backward.
                */
                if (previous[layer_idx][candidate_idx] == size_t(-1) ||
                    tool_order_score_less(score, dp[layer_idx][candidate_idx])) {
                    dp[layer_idx][candidate_idx] = score;
                    previous[layer_idx][candidate_idx] = previous_idx;
                }
            }
        }
    }

    size_t best_idx = 0;
    const size_t last_layer = active_layers.size() - 1;
    for (size_t candidate_idx = 1; candidate_idx < dp[last_layer].size(); ++candidate_idx)
        if (tool_order_score_less(dp[last_layer][candidate_idx], dp[last_layer][best_idx]))
            best_idx = candidate_idx;

    /*
    Walk the predecessor table backward. active_layers maps compressed DP rows
    back to the real PrintingLayerGroup indices, leaving empty layers untouched.
    */
    for (size_t layer_offset = active_layers.size(); layer_offset > 0; --layer_offset) {
        const size_t layer_idx = layer_offset - 1;
        selected[active_layers[layer_idx]] = active_candidates[layer_idx][best_idx];
        best_idx = layer_idx == 0 ? size_t(-1) : previous[layer_idx][best_idx];
    }

    return selected;
}

void apply_tool_order_candidate(const PrintingLayerGroup &layer_group, const ToolOrderCandidate &candidate)
{
    if (!candidate.valid || layer_group.tool_group_count() < 2)
        return;

    uint32_t dst_idx = 0;
    for (uint32_t category = 0; category < 3; ++category) {
        /*
        Move one category at a time:
          0 = chosen first extruder,
          1 = all middle extruders,
          2 = chosen last extruder.
        The scan restarts at dst_idx after each move because the ABI move
        operation shifts the elements between source and destination.
        */
        uint32_t scan_idx = dst_idx;
        while (scan_idx < layer_group.tool_group_count()) {
            const uint16_t extruder_id = layer_group.tool_group(scan_idx).extruder_id();
            const uint32_t group_category =
                extruder_id == candidate.first_extruder ? 0u :
                extruder_id == candidate.last_extruder ? 2u :
                1u;
            if (group_category == category) {
                layer_group.move_tool_group(scan_idx, dst_idx);
                ++dst_idx;
                scan_idx = dst_idx;
            } else {
                ++scan_idx;
            }
        }
    }
}

class DefaultToolGroupOrdering : public PluginBase
{
public:
    static DefaultToolGroupOrdering &instance(orchestrator_handle *orch)
    {
        static DefaultToolGroupOrdering s_instance(orch);
        return s_instance;
    }

    explicit DefaultToolGroupOrdering(orchestrator_handle *orch) : PluginBase(orch) {}

private:
    const char *id_impl() const noexcept override { return "ordering.tool_groups.default"; }
    const char *name_impl() const noexcept override { return "Default tool ordering"; }
    const char *description_impl() const noexcept override
    {
        return "Orders tool sections in each printing group to reduce tool changes between layers.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_group_tools; }
    const char *exclusive_group_label_impl() const noexcept override { return "Tool ordering"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_ORDERING orders extruder/tool sections.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_ORDERING; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 100; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_extrusion_ordering *ctx = plugin_ctx_as_extrusion_ordering(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->plan != nullptr);
        if (ctx == nullptr || ctx->plan == nullptr)
            return;

        PrintingPlan plan(ctx->plan);
        /*
        Tool ordering works independently per PrintingGroup. Complete-object
        plans therefore keep object batches separate, while by-layer plans sort
        the single global group.
        */
        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx)
            order_tool_groups_in_plugin(plan.group(group_idx));
    }
};

} // namespace

void register_default_tool_group_ordering_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultToolGroupOrdering::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Ordering::DefaultOrderingPlugin
