#include "StepGenerateGcode.hpp"

#include <cassert>
#include <cstddef>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/host/GCodeScriptProcessor.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode_firmware.h"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeFirmwareViews.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

/*
STEP_GCODE bridge
=================

The G-code step is now a plugin boundary. Earlier pipeline steps build a
PrintingPlan, then this step creates the selected firmware session and invokes
the active STEP_GCODE writer. The writer receives the source Print, mutable
PrintingPlan, borrowed firmware session, and resolved final output path.

The step itself intentionally stays small. File format decisions belong to the
selected plugin, so external writers can replace the built-in implementation
without changing the pipeline runner.
*/

namespace Slic3r::Steps::StepGenerateGcode {

namespace {

/*
Own the raw firmware session returned through the plugin ABI.

The provider allocates the session, but STEP_GCODE may fail before or during
file publication. Keeping ownership in this stack object guarantees that the
provider destructor is called once on every path.
*/
class FirmwareInstanceOwner
{
public:
    FirmwareInstanceOwner() = default;
    ~FirmwareInstanceOwner();

    FirmwareInstanceOwner(const FirmwareInstanceOwner &) = delete;
    FirmwareInstanceOwner &operator=(const FirmwareInstanceOwner &) = delete;

    raw_gcode_firmware_instance *mutable_instance();
    const raw_gcode_firmware_instance *instance() const;

private:
    raw_gcode_firmware_instance m_instance = {};
};

// Ask the selected provider for one session and validate it before the
// STEP_GCODE writer can observe the borrowed instance.
void create_firmware_session(Orchestrator &orchestrator,
                             Print &print,
                             const raw_gcode_script_processor *script_processor,
                             FirmwareInstanceOwner &owner);

FirmwareInstanceOwner::~FirmwareInstanceOwner()
{
    // A provider may publish owning fields before producing an invalid table.
    // Destroy whenever the required pointers are available so validation
    // failures do not leak that partially published session.
    if (m_instance.session != nullptr && m_instance.vtable != nullptr &&
        m_instance.vtable->struct_size >=
            offsetof(raw_gcode_firmware_vtable, destroy) + sizeof(gcode_firmware_destroy_fn) &&
        m_instance.vtable->destroy != nullptr)
        m_instance.vtable->destroy(m_instance.session);
}

raw_gcode_firmware_instance *FirmwareInstanceOwner::mutable_instance()
{
    return &m_instance;
}

const raw_gcode_firmware_instance *FirmwareInstanceOwner::instance() const
{
    return &m_instance;
}

void create_firmware_session(Orchestrator &orchestrator,
                             Print &print,
                             const raw_gcode_script_processor *script_processor,
                             FirmwareInstanceOwner &owner)
{
    Plugin *firmware_plugin = selected_or_active_plugin_for_step(
        orchestrator, GCODE_FIRMWARE, &print.full_print_config());
    if (firmware_plugin == nullptr)
        throw RuntimeError("No active G-code firmware plugin is available.");

    plugin_host_context host_context = orchestrator.prepare_plugin_host_context(
        GCODE_FIRMWARE, firmware_plugin, &print);
    plugin_run_context run_context = orchestrator.prepare_plugin_run_context(
        GCODE_FIRMWARE, firmware_plugin, &host_context);
    run_ctx_gcode_firmware payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.script_processor = script_processor;
    run_context.data = &payload;

    firmware_plugin->setup(run_context, 1);
    firmware_plugin->setup_run(run_context);
    firmware_plugin->run(run_context);

    // Transfer ownership before checking cancellation or table completeness;
    // the provider may allocate a session before reporting its error.
    *owner.mutable_instance() = payload.instance;
    if (run_context.is_cancelled != nullptr && run_context.is_cancelled(run_context.host_context))
        throw RuntimeError("G-code firmware plugin failed while creating its session.");

    // Constructing the borrowed view validates struct sizes and every required
    // callback without invoking firmware code.
    const slic3r_api::GCodeFirmwareView validated(owner.instance());
    (void)validated;
}

} // namespace

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print, const std::string &path)
{
    if (path.empty())
        throw RuntimeError("G-code generation plugin needs a non-empty output path.");

    Plugin *plugin = selected_or_active_plugin_for_step(orchestrator, STEP_GCODE, &print.full_print_config());
    if (plugin == nullptr)
        throw RuntimeError("No active G-code generation plugin is available.");

    // The host parser outlives the borrowed firmware view and is destroyed
    // only after the firmware session has released every reference to it.
    GCodeScriptProcessor scripts(print);
    FirmwareInstanceOwner firmware;
    create_firmware_session(orchestrator, print, scripts.c_processor(), firmware);

    // The G-code step consumes the PrintingPlan created by STEP_ORDERING. If a
    // caller runs this step directly in a test, mutable_printing_plan() still
    // gives the plugin a valid empty plan instead of a null handle.
    Printing::PrintingPlan &plan = print.mutable_printing_plan();
    run_ctx_generate_gcode payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.plan = reinterpret_cast<printing_plan_handle *>(&plan);
    payload.firmware = firmware.instance();
    payload.output_path = path.c_str();

    plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_GCODE, plugin, &print);
    plugin_run_context run_context = orchestrator.prepare_plugin_run_context(STEP_GCODE, plugin, &host_context);
    run_context.data = &payload;

    plugin->setup(run_context, 1);
    plugin->setup_run(run_context);
    plugin->run(run_context);

    // PluginBase reports C++ exceptions through report_error() instead of
    // letting them cross the C ABI. report_error() requests cancellation; the
    // host step turns that request back into a normal export failure.
    if (run_context.is_cancelled != nullptr && run_context.is_cancelled(run_context.host_context))
        throw RuntimeError("G-code generation plugin failed.");
}

} // namespace Slic3r::Steps::StepGenerateGcode
