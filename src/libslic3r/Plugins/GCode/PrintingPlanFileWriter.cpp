///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintingPlanFileWriter.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <boost/format.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

/*
Prototype G-code writer
=======================

This plugin is the first STEP_GCODE implementation that consumes a
PrintingPlan. It does not try to generate real motion commands yet. Its job is
to prove the pipeline boundary: STEP_ORDERING creates an ordered plan, the
G-code step receives that plan plus the final output path, and the selected
writer owns the file-writing transaction.

The file is written through a temporary sibling path and renamed at the end.
That keeps readers from seeing a partial output if formatting throws halfway
through. Formatting inside one PrintingToolGroup is parallel because each
PrintingExtrusion is independent at this prototype stage; the strings are then
written serially so the file remains deterministic and follows the plan order.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace PrintingPlanFileWriterPlugin {
namespace {

const char *const k_no_dependencies[] = { nullptr };

/*
State shared by the worker that formats one PrintingExtrusion line.

The parallel loop gives every worker a unique extrusion index. The worker writes
only to chunks[index], so no mutex is needed and the final serial write can keep
the exact PrintingPlan order.
*/
struct ToolGroupWriteData
{
    const printing_tool_group_handle *tool_group = nullptr;
    std::vector<std::string> *chunks = nullptr;
    PluginProgress *progress = nullptr;
    uint32_t group_idx = 0;
    uint32_t layer_idx = 0;
    uint32_t tool_idx = 0;
};

// Count the extrusion lines that will be formatted, so progress has a real total.
uint32_t count_plan_extrusions(const PrintingPlan &plan);

// Write the complete PrintingPlan to output_path through output_path + ".tmp".
void write_plan_file(const PrintingPlan &plan, const char *output_path, PluginProgress &progress);

// Format all extrusions of one tool group in parallel, then append them in order.
void write_tool_group(boost::nowide::ofstream &file,
                      const PrintingToolGroup &tool_group,
                      uint32_t group_idx,
                      uint32_t layer_idx,
                      uint32_t tool_idx,
                      PluginProgress &progress);

// Worker used by slic3r_parallel_for() to fill one string slot.
void write_extrusion_chunk(uint32_t extrusion_idx, void *user_data);

uint32_t count_plan_extrusions(const PrintingPlan &plan)
{
    uint32_t count = 0;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx)
                count += layer_group.tool_group(tool_idx).extrusion_count();
        }
    }
    return count;
}

void write_plan_file(const PrintingPlan &plan, const char *output_path, PluginProgress &progress)
{
    if (output_path == nullptr || output_path[0] == '\0')
        throw Slic3r::RuntimeError("PrintingPlan file writer needs a non-empty output path.");

    const std::string final_path(output_path);
    const std::string temporary_path = final_path + ".tmp";
    boost::nowide::remove(temporary_path.c_str());

    boost::nowide::ofstream file(temporary_path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file)
        throw Slic3r::RuntimeError("Cannot open temporary PrintingPlan output file '" + temporary_path + "'.");

    try {
        for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
            const PrintingGroup group = plan.group(group_idx);
            for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
                const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
                for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx)
                    write_tool_group(file, layer_group.tool_group(tool_idx), group_idx, layer_idx, tool_idx, progress);
            }
        }

        file.close();
        if (!file)
            throw Slic3r::RuntimeError("Failed while writing temporary PrintingPlan output file '" +
                                       temporary_path + "'.");

        if (Slic3r::rename_file(temporary_path, final_path)) {
            boost::nowide::remove(temporary_path.c_str());
            throw Slic3r::RuntimeError("Failed to rename PrintingPlan output file from '" + temporary_path +
                                       "' to '" + final_path + "'.");
        }
    } catch (...) {
        try {
            file.close();
        } catch (...) {
            // Ignore errors during cleanup.
        }
        try {
            boost::nowide::remove(temporary_path.c_str());
        } catch (...) {
            // Ignore errors during cleanup.
        }
        throw;
    }
}

void write_tool_group(boost::nowide::ofstream &file,
                      const PrintingToolGroup &tool_group,
                      const uint32_t group_idx,
                      const uint32_t layer_idx,
                      const uint32_t tool_idx,
                      PluginProgress &progress)
{
    const uint32_t extrusion_count = tool_group.extrusion_count();
    std::vector<std::string> chunks(extrusion_count);
    ToolGroupWriteData data;
    data.tool_group = tool_group.handle();
    data.chunks = &chunks;
    data.progress = &progress;
    data.group_idx = group_idx;
    data.layer_idx = layer_idx;
    data.tool_idx = tool_idx;

    slic3r_parallel_for(0, extrusion_count, &data, &write_extrusion_chunk);

    // Workers finish in arbitrary order. The file must still mirror the
    // PrintingPlan order, so only this serial section touches the stream.
    for (const std::string &chunk : chunks)
        file << chunk;
}

void write_extrusion_chunk(const uint32_t extrusion_idx, void *user_data)
{
    ToolGroupWriteData *data = static_cast<ToolGroupWriteData *>(user_data);
    assert(data != nullptr);
    assert(data->chunks != nullptr);
    assert(extrusion_idx < data->chunks->size());

    const PrintingToolGroup tool_group(data->tool_group);
    const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
    (void)extrusion;

    (*data->chunks)[extrusion_idx] =
        (boost::format("group %1% layer %2% tool %3% extrusion num%4%\n")
            % data->group_idx
            % data->layer_idx
            % data->tool_idx
            % extrusion_idx).str();

    // Formatting one extrusion line is the unit of work exposed by this
    // prototype writer. The helper is thread-safe, so workers can report
    // directly without serializing on the file-writing loop.
    if (data->progress != nullptr)
        data->progress->increment();
}

class PrintingPlanFileWriter : public PluginBase
{
public:
    static PrintingPlanFileWriter &instance(orchestrator_handle *orch)
    {
        static PrintingPlanFileWriter s_instance(orch);
        return s_instance;
    }

    explicit PrintingPlanFileWriter(orchestrator_handle *orch) : PluginBase(orch) {}

private:
    const char *id_impl() const noexcept override { return "gcode.printing_plan_file_writer"; }
    const char *name_impl() const noexcept override { return "PrintingPlan file writer"; }
    const char *description_impl() const noexcept override
    {
        return "Prototype writer that dumps the ordered PrintingPlan to the selected output file.";
    }
    const char *exclusive_group_impl() const noexcept override { return "step_gcode_plugin"; }
    const char *exclusive_group_label_impl() const noexcept override { return "G-code plugin"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_GCODE creates the output file.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_GCODE; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 0; }
    const char *progress_message_format_impl() const noexcept override { return "Writing G-code: %u / %u extrusions"; }

    void setup_run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_generate_gcode *ctx = plugin_ctx_as_generate_gcode(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->plan != nullptr);
        if (ctx == nullptr || ctx->plan == nullptr)
            return;

        // The writer reports one unit per formatted PrintingExtrusion. Counting
        // in setup_run_impl() lets the host display a stable percentage while
        // run_impl() writes the file.
        progress().add_max(count_plan_extrusions(PrintingPlan(ctx->plan)));
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_generate_gcode *ctx = plugin_ctx_as_generate_gcode(run_ctx);
        assert(ctx != nullptr);
        assert(ctx->plan != nullptr);
        if (ctx == nullptr || ctx->plan == nullptr)
            return;

        write_plan_file(PrintingPlan(ctx->plan), ctx->output_path, progress());
    }
};

} // namespace

void register_printing_plan_file_writer_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, PrintingPlanFileWriter::instance(orch).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::PrintingPlanFileWriterPlugin
