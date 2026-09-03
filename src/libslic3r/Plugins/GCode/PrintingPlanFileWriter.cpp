///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintingPlanFileWriter.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/FilesystemTransaction.hpp"
#include "libslic3r/libslic3r.h"

/*
PrintingPlan file serializer
============================

This STEP_GCODE plugin owns the output artifact for the selected firmware
implementation. It walks the already ordered PrintingPlan and writes the
chunks returned by the firmware session to one atomically published file. The
plugin does not interpret extrusion geometry or assemble individual machine
commands.

The normal execution flow is:

    setup_run_impl()
    `-- count all extrusion roots for progress reporting

    run_impl()
    `-- write_plan_file()
        |-- validate the G-code context and output path
        |-- open a sibling temporary file
        |-- serialize the nested plan boundaries in order:
        |   |-- print begin/events/end
        |   |-- group begin/events/end
        |   |-- layer begin/events/end
        |   |-- tool-group begin/events/end
        |   `-- extrusion and event chunks
        |-- flush and close the temporary stream
        |-- replace the final output through FilesystemTransaction
        `-- remove the temporary file if serialization fails

Every firmware result is written before the next firmware callback is called,
because the returned string may be backed by state owned by the session. Empty
groups and layers are still forwarded so firmware implementations can observe
their boundaries. The firmware view owns G-code generation and state changes;
this plugin only provides the traversal, stream checks, and publication
guarantee. A cleanup warning after a successful commit is reported separately
from an output-generation failure.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace PrintingPlanFileWriterPlugin {
namespace {

const char *const k_no_dependencies[] = { nullptr };

// Count the ordered work units once so progress stays stable while chunks are
// produced sequentially.
uint32_t count_plan_extrusions(const PrintingPlan &plan);

// Append one borrowed firmware result before another firmware callback can
// invalidate its storage.
void write_firmware_chunk(boost::nowide::ofstream &stream, std::string_view chunk);

// Traverse every plan boundary in strict nesting order and forward it to the
// stateful firmware session.
void serialize_plan(boost::nowide::ofstream &stream,
                    const Print &print,
                    const PrintingPlan &plan,
                    const GCodeFirmwareView &firmware,
                    PluginProgress &progress);

// Replace the final output only after the staging stream is fully closed.
void publish_output_file(const boost::filesystem::path &temporary_path,
                         const boost::filesystem::path &final_path);

// Own the complete staging, serialization, stream validation, and publication
// lifecycle for one STEP_GCODE run.
void write_plan_file(const run_ctx_generate_gcode &context, PluginProgress &progress);

uint32_t count_plan_extrusions(const PrintingPlan &plan)
{
    uint32_t count = 0;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx)
                count += layer.tool_group(tool_idx).extrusion_count();
        }
    }
    return count;
}

void write_firmware_chunk(boost::nowide::ofstream &stream, std::string_view chunk)
{
    if (chunk.empty())
        return;

    stream.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!stream)
        throw Slic3r::RuntimeError("The temporary G-code stream rejected firmware output.");
}

void serialize_plan(boost::nowide::ofstream &stream,
                    const Print &print,
                    const PrintingPlan &plan,
                    const GCodeFirmwareView &firmware,
                    PluginProgress &progress)
{
    write_firmware_chunk(stream, firmware.begin_print(print));
    const PrintingScopeEvents plan_events = plan.events();
    if (plan_events.has_before())
        write_firmware_chunk(stream, firmware.write_event(plan_events.before()));

    // Every begin call is matched before the enclosing scope is left. Empty
    // groups and layers are still observable because a firmware may need their
    // boundaries even when they contain no extrusion.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        write_firmware_chunk(stream, firmware.begin_group(group));
        const PrintingScopeEvents group_events = group.events();
        if (group_events.has_before())
            write_firmware_chunk(stream, firmware.write_event(group_events.before()));

        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            write_firmware_chunk(stream, firmware.begin_layer(layer));
            const PrintingScopeEvents layer_events = layer.events();
            if (layer_events.has_before())
                write_firmware_chunk(stream, firmware.write_event(layer_events.before()));

            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                write_firmware_chunk(stream, firmware.begin_tool_group(tool_group));
                const PrintingScopeEvents tool_events = tool_group.events();
                if (tool_events.has_before())
                    write_firmware_chunk(stream, firmware.write_event(tool_events.before()));

                for (uint32_t extrusion_idx = 0;
                     extrusion_idx < tool_group.extrusion_count();
                     ++extrusion_idx) {
                    const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
                    write_firmware_chunk(stream, firmware.write_extrusion(extrusion));
                    progress.increment();
                }

                if (tool_events.has_after())
                    write_firmware_chunk(stream, firmware.write_event(tool_events.after()));
                write_firmware_chunk(stream, firmware.end_tool_group());
            }

            if (layer_events.has_after())
                write_firmware_chunk(stream, firmware.write_event(layer_events.after()));
            write_firmware_chunk(stream, firmware.end_layer());
        }

        if (group_events.has_after())
            write_firmware_chunk(stream, firmware.write_event(group_events.after()));
        write_firmware_chunk(stream, firmware.end_group());
    }

    if (plan_events.has_after())
        write_firmware_chunk(stream, firmware.write_event(plan_events.after()));
    write_firmware_chunk(stream, firmware.end_print());
}

void publish_output_file(const boost::filesystem::path &temporary_path,
                         const boost::filesystem::path &final_path)
{
    Slic3r::FilesystemTransaction transaction;
    transaction.add_replacement(temporary_path, final_path);
    const Slic3r::FilesystemTransactionResult result = transaction.commit();

    // Cleanup warnings concern hidden transaction artifacts after the visible
    // output was committed. Report them without turning a valid file into a
    // failed export.
    for (const Slic3r::FilesystemTransactionFailure &warning : result.cleanup_warnings)
        BOOST_LOG_TRIVIAL(warning) << "G-code output cleanup warning: "
                                   << Slic3r::format_filesystem_transaction_failure(warning);

    if (result.status != Slic3r::FilesystemTransactionStatus::Committed)
        throw Slic3r::RuntimeError(
            "Could not publish the G-code output file: " +
            Slic3r::format_filesystem_transaction_error(result));
}

void write_plan_file(const run_ctx_generate_gcode &context, PluginProgress &progress)
{
    if (context.print == nullptr || context.plan == nullptr || context.firmware == nullptr ||
        context.output_path == nullptr || context.output_path[0] == '\0')
        throw Slic3r::RuntimeError("PrintingPlan file writer received an incomplete G-code context.");

    const boost::filesystem::path final_path(context.output_path);
    const boost::filesystem::path temporary_path = final_path.parent_path() /
        (final_path.filename().string() + ".tmp");
    boost::system::error_code cleanup_error;
    boost::filesystem::remove_all(temporary_path, cleanup_error);

    boost::nowide::ofstream stream(
        temporary_path.string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream)
        throw Slic3r::RuntimeError(
            "Cannot open temporary G-code output file '" + temporary_path.string() + "'.");

    try {
        const Print print(context.print);
        const PrintingPlan plan(context.plan);
        const GCodeFirmwareView firmware(context.firmware);
        serialize_plan(stream, print, plan, firmware, progress);

        // Individual writes may succeed while the operating system reports a
        // delayed error during flush or close. Validate both before publishing
        // the staging file.
        stream.flush();
        if (!stream)
            throw Slic3r::RuntimeError("Failed while flushing the temporary G-code output file.");
        stream.close();
        if (!stream)
            throw Slic3r::RuntimeError("Failed while closing the temporary G-code output file.");

        publish_output_file(temporary_path, final_path);
    } catch (...) {
        try {
            stream.close();
        } catch (...) {
            // Cleanup is best effort; preserve the original serialization error.
        }
        boost::filesystem::remove_all(temporary_path, cleanup_error);
        throw;
    }
}

class PrintingPlanFileWriter final : public PluginBase
{
public:
    static PrintingPlanFileWriter &instance(orchestrator_handle *orchestrator)
    {
        static PrintingPlanFileWriter plugin(orchestrator);
        return plugin;
    }

    explicit PrintingPlanFileWriter(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "gcode.printing_plan_file_writer"; }
    const char *name_impl() const noexcept override { return "PrintingPlan file writer"; }
    const char *description_impl() const noexcept override
    {
        return "Writes ordered firmware chunks to one atomically published output file.";
    }
    const char *exclusive_group_impl() const noexcept override { return "step_gcode_plugin"; }
    const char *exclusive_group_label_impl() const noexcept override { return "G-code plugin"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how STEP_GCODE creates the output artifact.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_GCODE; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 0; }
    const char *progress_message_format_impl() const noexcept override
    {
        return "Writing G-code: %u / %u extrusions";
    }

    void setup_run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_generate_gcode *context = plugin_ctx_as_generate_gcode(run_ctx);
        if (context != nullptr && context->plan != nullptr)
            progress().add_max(count_plan_extrusions(PrintingPlan(context->plan)));
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_generate_gcode *context = plugin_ctx_as_generate_gcode(run_ctx);
        if (context == nullptr)
            throw Slic3r::RuntimeError("PrintingPlan file writer received the wrong step payload.");
        write_plan_file(*context, progress());
    }
};

} // namespace

void register_printing_plan_file_writer_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, PrintingPlanFileWriter::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::PrintingPlanFileWriterPlugin
