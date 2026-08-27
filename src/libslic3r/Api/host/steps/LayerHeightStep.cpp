///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "LayerHeightStep.hpp"

#include <memory>
#include <utility>

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/internal/PrintObjectAccess.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

namespace Slic3r::ApiHost::Steps {
namespace {

std::vector<coord_t> layer_profile_from_legacy_height_profile(const PrintObject &object,
                                                              const std::vector<coordf_t> &height_profile)
{
    // ModelObject::layer_height_profile is the historical compact
    // [z, height, z, height, ...] representation. The layer-height step exposes
    // explicit [layer_top_z, layer_height] pairs instead, so convert the legacy
    // input once at the API boundary and keep the new pipeline data simple
    // afterwards.
    if (height_profile.empty())
        return {};

    std::vector<double> unscaled_profile;
    unscaled_profile.reserve(height_profile.size());
    for (coordf_t value : height_profile)
        unscaled_profile.push_back(value);

    std::vector<double> object_layers = generate_object_layers(object.slicing_parameters(), unscaled_profile);
    std::vector<coord_t> out;
    out.reserve(object_layers.size() / 2);
    for (size_t idx = 1; idx < object_layers.size(); idx += 2) {
        out.push_back(scale_i(object_layers[idx]));
        out.push_back(scale_i(object_layers[idx] - object_layers[idx - 1]));
    }
    return out;
}

void set_layer_height_profile(const object_handle *object_handler,
                              coord_t *layer_descriptors,
                              uint32_t descriptor_count)
{
    if (object_handler == nullptr || (layer_descriptors == nullptr && descriptor_count != 0))
        return;

    PrintObject *object = const_cast<PrintObject *>(reinterpret_cast<const PrintObject *>(object_handler));
    std::vector<coord_t> new_layer_profile;
    new_layer_profile.reserve(descriptor_count);
    for (uint32_t i = 0; i < descriptor_count; ++i)
        new_layer_profile.push_back(layer_descriptors[i]);
    ApiInternal::PrintObjectAccess::set_layer_profile(*object, std::move(new_layer_profile));
}

} // namespace

std::unique_ptr<LayerHeightRunContext> make_layer_height_run_context(Print &print, size_t object_idx)
{
    std::unique_ptr<LayerHeightRunContext> out = std::make_unique<LayerHeightRunContext>();

    PrintObject &print_object = print.object(object_idx);
    ModelObject &model_object = *print_object.model_object();
    const double object_print_z_max = check_z_step(model_object.max_z(), print.config().z_step.value);

    out->enforced_layer_profile = layer_profile_from_legacy_height_profile(print_object, model_object.layer_height_profile.get());

    out->layer_config_ranges.reserve(model_object.layer_config_ranges.size());
    for (const std::pair<const t_layer_height_range, ModelConfig> &range : model_object.layer_config_ranges) {
        c_layer_config_range c_range = {};
        c_range.z_min = scale_i(range.first.first);
        c_range.z_max = scale_i(range.first.second);
        c_range.config = ApiHost::to_config_handle(&range.second.get());
        out->layer_config_ranges.push_back(c_range);
    }

    out->context_step.print = reinterpret_cast<const print_handle *>(&print);
    out->context_step.object = reinterpret_cast<const object_handle *>(&print_object);
    out->context_step.enforce_layer_zs = out->enforced_layer_profile.data();
    out->context_step.enforce_layer_zs_size = uint32_t(out->enforced_layer_profile.size());
    out->context_step.layer_config_ranges = out->layer_config_ranges.data();
    out->context_step.layer_config_ranges_size = uint32_t(out->layer_config_ranges.size());
    out->context_step.set_layer_height_profile = set_layer_height_profile;
    out->context_step.max_z = scale_i(object_print_z_max);
    return out;
}

} // namespace Slic3r::ApiHost::Steps
