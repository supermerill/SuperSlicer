///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CreateToolChange.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

/*
Default tool-change event producer
==================================

The layer-extrusion-edit step executes one run per PrintingLayerGroup, possibly
in parallel. Tool selection, however, is a prefix state across the complete
plan. setup_impl() therefore performs one cheap hierarchy-only scan and stores
the tool active before each layer. run_impl() can then replay only its local
tool-groups and append events without consulting or modifying another layer.

No extrusion tree is visited here. CreateTransitionScope later observes the
same ordered tool-group sequence and marks its geometric boundaries, while
CreateRetraction uses those flags only to choose tool-change E settings.
*/

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateToolChangePlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

/* Immutable prefix facts consumed by one parallel layer run. */
struct LayerToolState
{
    int32_t layer_number = -1;
    int32_t incoming_extruder = -1;
    coord_t print_z = 0;
};

/* Return true when a setting contains at least one non-whitespace character. */
bool has_visible_text(const std::string &text);

/* Add one typed integer argument to a script's temporary Config. */
void set_int_argument(MutableConfig &config, const char *key, int32_t value);

/* Add one typed floating-point argument to a script's temporary Config. */
void set_float_argument(MutableConfig &config, const char *key, double value);

/* Append a configured script or semantic fallback to one target tool visit. */
void append_tool_change_event(storage_handle *storage,
                              const PrintingToolGroup &tool_group,
                              const std::string &script,
                              int32_t layer_number,
                              coord_t layer_z,
                              coord_t max_layer_z,
                              uint16_t previous_extruder,
                              uint16_t next_extruder);

/* Built-in producer registered in the layer-extrusion-edit plugin chain. */
class CreateToolChange final : public PluginBase
{
public:
    /* Return the process-wide built-in instance registered in one orchestrator. */
    static CreateToolChange &instance(orchestrator_handle *orchestrator);

    /* Initialize the generic plugin facade; this provider owns no property key. */
    explicit CreateToolChange(orchestrator_handle *orchestrator);

private:
    /* Plugin identity and user-facing metadata consumed by the orchestrator. */
    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    const char *exclusive_group_impl() const noexcept override;
    const char *exclusive_group_label_impl() const noexcept override;
    const char *exclusive_group_tooltip_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;

    /* Prefix-scan tool visits without traversing any extrusion tree. */
    void setup_impl(const plugin_run_context *run_ctx, uint32_t run_count) const override;

    /* Count one work unit for each valid future layer run. */
    void setup_run_impl(const plugin_run_context *run_ctx) const override;

    /* Materialize only the tool changes owned by the current layer. */
    void run_impl(const plugin_run_context *run_ctx) const override;

    /* False until setup_impl() publishes a complete layer-indexed cache. */
    mutable bool m_setup_valid = false;

    /* One immutable prefix-state entry for every final group/layer pair. */
    mutable std::vector<std::vector<LayerToolState>> m_layer_states;

    /* Highest final layer Z made available to toolchange_gcode placeholders. */
    mutable coord_t m_max_layer_z = 0;

    /* PluginStorage is shared and non-thread-safe; guard only event allocation. */
    mutable std::mutex m_storage_mutex;
};

bool has_visible_text(const std::string &text)
{
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

void set_int_argument(MutableConfig &config, const char *key, const int32_t value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_INT).set_int(value);
}

void set_float_argument(MutableConfig &config, const char *key, const double value)
{
    config.get_or_add(key, SLIC3R_CONFIG_OPTION_FLOAT).set_float(value);
}

void append_tool_change_event(storage_handle *storage,
                              const PrintingToolGroup &tool_group,
                              const std::string &script,
                              const int32_t layer_number,
                              const coord_t layer_z,
                              const coord_t max_layer_z,
                              const uint16_t previous_extruder,
                              const uint16_t next_extruder)
{
    if (storage == nullptr)
        throw std::invalid_argument("A tool-change event needs plugin storage.");

    // Build the complete temporary event before transferring it into the
    // tool-group. append_before_move() then moves both text and Config buffers
    // into the PrintingPlan-owned event tree.
    StoredExtrusionEntity event(storage);
    if (has_visible_text(script)) {
        StoredConfig arguments(storage);
        set_int_argument(arguments, "layer_num", layer_number);
        set_float_argument(arguments, "layer_z", unscaled(layer_z));
        set_float_argument(arguments, "max_layer_z", unscaled(max_layer_z));
        set_int_argument(arguments, "previous_extruder", previous_extruder);
        set_int_argument(arguments, "next_extruder", next_extruder);
        set_float_argument(arguments, "toolchange_z", unscaled(layer_z));
        event.script_gcode(
            script, GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE, arguments,
            next_extruder);
    } else {
        event.get_or_add(EPropertySpecialCommand::key).set(
            C_EXTRUSION_SPECIAL_COMMAND_TOOLCHANGE, double(next_extruder));
    }

    MutableExtrusionEntity appended =
        tool_group.events().append_before_move(event.mutable_view());
    if (!appended.valid())
        throw std::runtime_error(
            "Unable to append a tool-change event to the PrintingToolGroup.");
}

CreateToolChange &CreateToolChange::instance(orchestrator_handle *orchestrator)
{
    static CreateToolChange plugin(orchestrator);
    return plugin;
}

CreateToolChange::CreateToolChange(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator)
{
}

const char *CreateToolChange::id_impl() const noexcept
{
    return "layer_extrusion_edit.toolchange.default";
}

const char *CreateToolChange::name_impl() const noexcept
{
    return "Default tool changes";
}

const char *CreateToolChange::description_impl() const noexcept
{
    return "Places configured or semantic tool-selection events before target tool visits.";
}

const char *CreateToolChange::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.toolchange";
}

const char *CreateToolChange::exclusive_group_label_impl() const noexcept
{
    return "Tool changes";
}

const char *CreateToolChange::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how ordered tool visits materialize physical tool selection.";
}

slicing_step_t CreateToolChange::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *CreateToolChange::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t CreateToolChange::priority_impl() const noexcept
{
    return -110;
}

int32_t CreateToolChange::used_config_keys(
    raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = raw_used_config_key{
            "toolchange_gcode", RAW_CO_STRING,
            RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
    return 1;
}

const char *CreateToolChange::progress_message_format_impl() const noexcept
{
    return "Creating tool changes: %u / %u layers";
}

void CreateToolChange::setup_impl(const plugin_run_context *run_ctx,
                                  const uint32_t run_count) const
{
    m_setup_valid = false;
    m_layer_states.clear();
    m_max_layer_z = 0;

    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->plan == nullptr)
        return;

    const PrintingPlan plan(ctx->plan);
    m_layer_states.resize(plan.group_count());
    int32_t current_extruder = -1;
    int32_t layer_number = 0;
    uint32_t counted_layers = 0;

    // This prefix scan visits only the three PrintingPlan vectors. Empty tool
    // visits deliberately update current_extruder because the file writer will
    // still enter their tool-group and therefore change the selection history.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        std::vector<LayerToolState> &group_states = m_layer_states[group_idx];
        group_states.reserve(group.layer_group_count());
        for (uint32_t layer_idx = 0;
             layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            group_states.push_back(
                LayerToolState{layer_number++, current_extruder, layer.print_z()});
            m_max_layer_z = std::max(m_max_layer_z, layer.print_z());
            ++counted_layers;

            for (uint32_t tool_idx = 0;
                 tool_idx < layer.tool_group_count(); ++tool_idx) {
                const uint16_t tool = layer.tool_group(tool_idx).extruder_id();
                if (tool == UINT16_MAX)
                    throw std::runtime_error(
                        "A PrintingToolGroup has no valid extruder for tool-change planning.");
                current_extruder = int32_t(tool);
            }
        }
    }
    if (counted_layers != run_count)
        throw std::runtime_error(
            "The tool-change run count does not match the PrintingPlan.");
    m_setup_valid = true;
}

void CreateToolChange::setup_run_impl(
    const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx != nullptr && ctx->layer_group != nullptr)
        progress().add_max(1);
}

void CreateToolChange::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->layer_group == nullptr ||
        run_ctx->plugin_storage == nullptr)
        return;
    if (ctx->group_idx >= m_layer_states.size() ||
        ctx->layer_group_idx >= m_layer_states[ctx->group_idx].size())
        throw std::runtime_error(
            "A tool-change run references an unknown PrintingLayerGroup.");

    const LayerToolState &state =
        m_layer_states[ctx->group_idx][ctx->layer_group_idx];
    const PrintingLayerGroup layer(ctx->layer_group);
    const std::string script = Print(ctx->print).config().string_or_default(
        "toolchange_gcode", std::string());
    int32_t current_extruder = state.incoming_extruder;

    // Each tool-group belongs to this worker's layer. The first global tool and
    // repeated visits need no explicit event; every actual transition is added
    // before entering its target group.
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
        const uint16_t target_extruder = tool_group.extruder_id();
        if (target_extruder == UINT16_MAX)
            throw std::runtime_error(
                "A PrintingToolGroup has no valid extruder for tool-change planning.");
        if (current_extruder >= 0 &&
            current_extruder != int32_t(target_extruder)) {
            // Runs own separate tool-groups, but the host currently lends one
            // PluginStorage to all of them. Keep its short allocation and move
            // transaction serialized while all hierarchy work remains parallel.
            const std::lock_guard<std::mutex> storage_lock(m_storage_mutex);
            append_tool_change_event(
                run_ctx->plugin_storage, tool_group, script,
                state.layer_number, state.print_z, m_max_layer_z,
                uint16_t(current_extruder), target_extruder);
        }
        current_extruder = int32_t(target_extruder);
    }
    progress().increment();
}

} // namespace

void register_create_tool_change_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, CreateToolChange::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateToolChangePlugin
