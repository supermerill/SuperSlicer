///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_cpp_denseinfill_hpp_
#define plugins_cpp_denseinfill_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace DenseInfillPlugin {

/*
Small property carried by dense-infill surfaces.

The surface-generation plugin writes this payload on the surface pieces that
must be filled with denser sparse infill. The infill recipe modifier reads it
to change the generated recipe, and the post-infill plugin reads the same
surface id later to order the generated extrusion subtrees.
*/
struct SurfaceDenseInfillHint
{
    static plugin_property_type property_type;

    uint16_t max_solid_layers_on_top = 0;
    uint16_t priority = 0;
};

class DenseInfillSurfaceMarker : public PluginBase
{
public:
    static DenseInfillSurfaceMarker &instance(orchestrator_handle *orch);

private:
    DenseInfillSurfaceMarker(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

class DenseInfillRecipeModifier : public PluginBase
{
public:
    static DenseInfillRecipeModifier &instance(orchestrator_handle *orch);

private:
    DenseInfillRecipeModifier(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

class DenseInfillPostInfillOrder : public PluginBase
{
public:
    static DenseInfillPostInfillOrder &instance(orchestrator_handle *orch);

private:
    DenseInfillPostInfillOrder(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

void register_dense_infill_plugins(orchestrator_handle *orch);

}} // namespace slic3r_api::DenseInfillPlugin

#endif // plugins_cpp_denseinfill_hpp_
