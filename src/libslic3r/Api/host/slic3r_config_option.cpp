///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <cmath>
#include <cstring>
#include <string>

#include "libslic3r/Api/plugin/c/slic3r_config_option.h"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/ConfigOption.hpp"

namespace Slic3r {

static ConfigOption *to_option(config_option_handle *me)
{
    return reinterpret_cast<ConfigOption*>(me);
}

static const ConfigOption *to_option(const config_option_handle *me)
{
    return reinterpret_cast<const ConfigOption*>(me);
}

static const ConfigBase *to_config(const config_handle *me)
{
    return reinterpret_cast<const ConfigBase *>(me);
}

static ConfigOptionVectorBase *to_option_vector(config_option_vector_handle *me)
{
    return reinterpret_cast<ConfigOptionVectorBase*>(me);
}

static const ConfigOptionVectorBase *to_option_vector(const config_option_vector_handle *me)
{
    return reinterpret_cast<const ConfigOptionVectorBase*>(me);
}

static c_float_or_percent to_c_float_or_percent(const FloatOrPercent &value)
{
    c_float_or_percent out = {};
    out.value = value.value;
    out.percent = value.percent ? 1u : 0u;
    return out;
}

static FloatOrPercent to_float_or_percent(const c_float_or_percent &value)
{
    return FloatOrPercent{ value.value, value.percent != 0 };
}

static uint32_t copy_string_out(const std::string &value, char *out, uint32_t max_size)
{
    if (out != nullptr && max_size > 0) {
        const uint32_t count = (value.size() < max_size - 1) ? value.size() : (max_size - 1);
        if (count > 0)
            std::memcpy(out, value.data(), count);
        out[count] = '\0';
    }
    return value.size();
}

} // namespace Slic3r

extern "C" {

int32_t config_get_computed_value(const config_handle *me,
                                  const char *key,
                                  int32_t extruder_id,
                                  double *value_out)
{
    if (me == nullptr || key == nullptr || value_out == nullptr)
        return 0;

    const Slic3r::ConfigBase *config = Slic3r::to_config(me);
    const Slic3r::ConfigOption *option = config->option(key);
    if (option == nullptr || option->size() == 0)
        return 0;

    // Disabled optional values are unresolved and therefore use the caller's
    // fallback instead of entering the ConfigBase ratio chain.
    const int32_t enabled_idx = option->is_vector() ? extruder_id : 0;
    if (enabled_idx < 0 || uint32_t(enabled_idx) >= option->size() || !option->is_enabled(enabled_idx))
        return 0;

    try {
        const double value = config->get_computed_value(key, extruder_id);
        if (!std::isfinite(value))
            return 0;
        *value_out = value;
        return 1;
    } catch (...) {
        return 0;
    }
}

double c_float_or_percent_get_effective_value(const c_float_or_percent *me, double ratio_over)
{
    return me == nullptr ? 0.0 : (me->percent ? (ratio_over * me->value / 100.0) : me->value);
}

double c_float_or_percent_get_float(const c_float_or_percent *me)
{
    return c_float_or_percent_get_effective_value(me, 1.0);
}

int32_t c_float_or_percent_is_percent(const c_float_or_percent *me)
{
    return me != nullptr && me->percent != 0;
}

int32_t c_float_or_percent_equal(const c_float_or_percent *lhs, const c_float_or_percent *rhs)
{
    if (lhs == rhs)
        return 1;
    if (lhs == nullptr || rhs == nullptr)
        return 0;
    return lhs->value == rhs->value && lhs->percent == rhs->percent;
}

int32_t c_float_or_percent_less(const c_float_or_percent *lhs, const c_float_or_percent *rhs)
{
    if (lhs == nullptr || rhs == nullptr)
        return 0;
    return lhs->value < rhs->value || (lhs->value == rhs->value && lhs->percent < rhs->percent);
}

uint32_t graph_data_size(const graph_data_handle *me)
{
    return me == nullptr ? 0 : reinterpret_cast<const Slic3r::GraphData*>(me)->data_size();
}

const c_point *graph_data_ptr(const graph_data_handle *me)
{
    if (me == nullptr)
        return nullptr;
    const Slic3r::GraphData *graph = reinterpret_cast<const Slic3r::GraphData*>(me);
    if (graph->begin_idx >= graph->end_idx || graph->end_idx > graph->graph_points.size())
        return nullptr;
    return reinterpret_cast<const c_point*>(graph->graph_points.data() + graph->begin_idx);
}

double graph_interpolate(const graph_data_handle *me, double x)
{
    return me == nullptr ? 0.0 : reinterpret_cast<const Slic3r::GraphData*>(me)->interpolate(x);
}

double graph_inverse_interpolate(const graph_data_handle *me, double y)
{
    return me == nullptr ? 0.0 : reinterpret_cast<const Slic3r::GraphData*>(me)->inverse_interpolate(y);
}

int32_t graph_validate(const graph_data_handle *me)
{
    return me != nullptr && reinterpret_cast<const Slic3r::GraphData*>(me)->validate();
}

uint32_t graph_serialize(const graph_data_handle *me, char *out, uint32_t max_size)
{
    if (me == nullptr)
        return 0;
    return Slic3r::copy_string_out(reinterpret_cast<const Slic3r::GraphData*>(me)->serialize(), out, max_size);
}

int32_t graph_deserialize(graph_data_handle *me, const char *str)
{
    if (me == nullptr || str == nullptr)
        return 0;
    return reinterpret_cast<Slic3r::GraphData*>(me)->deserialize(std::string(str));
}

config_option_type config_option_type_get(const config_option_handle *me)
{
    return me == nullptr ? SLIC3R_CONFIG_OPTION_NONE : static_cast<config_option_type>(Slic3r::to_option(me)->type());
}

uint32_t config_option_flags_get(const config_option_handle *me)
{
    return me == nullptr ? 0u : Slic3r::to_option(me)->flags;
}

uint32_t config_option_size(const config_option_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_option(me)->size();
}

int32_t config_option_is_scalar(const config_option_handle *me)
{
    return me != nullptr && Slic3r::to_option(me)->is_scalar();
}

int32_t config_option_is_vector(const config_option_handle *me)
{
    return me != nullptr && Slic3r::to_option(me)->is_vector();
}

int32_t config_option_is_enabled(const config_option_handle *me, int32_t idx)
{
    return me != nullptr && Slic3r::to_option(me)->is_enabled(idx);
}

int32_t config_option_set_enabled(config_option_handle *me, int32_t enabled, int32_t idx)
{
    if (me == nullptr)
        return 0;
    Slic3r::to_option(me)->set_enabled(enabled != 0, idx);
    return 1;
}

int32_t config_option_can_be_disabled(const config_option_handle *me)
{
    return me != nullptr && Slic3r::to_option(me)->can_be_disabled();
}

int32_t config_option_set_can_be_disabled(config_option_handle *me, int32_t force_disabled)
{
    if (me == nullptr)
        return 0;
    Slic3r::to_option(me)->set_can_be_disabled(force_disabled != 0);
    return 1;
}

int32_t config_option_is_phony(const config_option_handle *me)
{
    return me != nullptr && Slic3r::to_option(me)->is_phony();
}

int32_t config_option_set_phony(config_option_handle *me, int32_t phony)
{
    if (me == nullptr)
        return 0;
    Slic3r::to_option(me)->set_phony(phony != 0);
    return 1;
}

int32_t config_option_equals(const config_option_handle *me, const config_option_handle *other)
{
    if (me == other)
        return 1;
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_option(me) == *Slic3r::to_option(other);
}

int32_t config_option_less(const config_option_handle *me, const config_option_handle *other)
{
    if (me == nullptr || other == nullptr)
        return 0;
    return *Slic3r::to_option(me) < *Slic3r::to_option(other);
}

void config_option_copy(config_option_handle *dst, const config_option_handle *src)
{
    if (dst != nullptr && src != nullptr)
        Slic3r::to_option(dst)->set(*Slic3r::to_option(src));
}

uint32_t config_option_serialize(const config_option_handle *me, char *out, uint32_t max_size)
{
    if (me == nullptr)
        return 0;
    return Slic3r::copy_string_out(Slic3r::to_option(me)->serialize(), out, max_size);
}

int32_t config_option_deserialize(config_option_handle *me, const char *str, int32_t append)
{
    if (me == nullptr || str == nullptr)
        return 0;
    return Slic3r::to_option(me)->deserialize(std::string(str), append != 0);
}

int32_t config_option_get_int(const config_option_handle *me, uint32_t idx)
{
    return me == nullptr ? 0 : Slic3r::to_option(me)->get_int(idx);
}

double config_option_get_float(const config_option_handle *me, uint32_t idx)
{
    return me == nullptr ? 0.0 : Slic3r::to_option(me)->get_float(idx);
}

c_float_or_percent config_option_get_float_or_percent(const config_option_handle *me, uint32_t idx)
{
    c_float_or_percent out = {};
    if (me != nullptr) {
        const Slic3r::ConfigOption *opt = Slic3r::to_option(me);
        const bool percent = opt->is_percent(idx);
        // ConfigOption::get_float() returns the normalized float value
        // (75% -> 0.75). The ABI FloatOrPercent stores the raw percent value
        // (75), because c_float_or_percent_get_effective_value() applies the
        // /100 conversion itself.
        const double value = percent ? opt->get_effective_value(100.0, idx) : opt->get_float(idx);
        out = Slic3r::to_c_float_or_percent(Slic3r::FloatOrPercent{ value, percent });
    }
    return out;
}

int32_t config_option_get_bool(const config_option_handle *me, uint32_t idx)
{
    return me != nullptr && Slic3r::to_option(me)->get_bool(idx);
}

uint32_t config_option_get_string(const config_option_handle *me, uint32_t idx, char *out, uint32_t max_size)
{
    if (me == nullptr)
        return 0;

    const Slic3r::ConfigOption *opt = Slic3r::to_option(me);
    std::string value;
    if (const Slic3r::ConfigOptionString *string = dynamic_cast<const Slic3r::ConfigOptionString *>(opt)) {
        value = string->value;
    } else if (const Slic3r::ConfigOptionStrings *strings = dynamic_cast<const Slic3r::ConfigOptionStrings *>(opt)) {
        const std::vector<std::string> &values = strings->get_values();
        if (idx >= values.size())
            return 0;
        value = values[idx];
    } else {
        return 0;
    }
    return Slic3r::copy_string_out(value, out, max_size);
}

const graph_data_handle *config_option_get_graph(const config_option_handle *me, uint32_t idx)
{
    if (me == nullptr)
        return nullptr;

    const Slic3r::ConfigOption *opt = Slic3r::to_option(me);
    if (const Slic3r::ConfigOptionGraph *graph = dynamic_cast<const Slic3r::ConfigOptionGraph *>(opt))
        return reinterpret_cast<const graph_data_handle *>(&graph->value);

    if (const Slic3r::ConfigOptionGraphs *graphs = dynamic_cast<const Slic3r::ConfigOptionGraphs *>(opt)) {
        const std::vector<Slic3r::GraphData> &values = graphs->get_values();
        if (idx >= values.size())
            return nullptr;
        return reinterpret_cast<const graph_data_handle *>(&values[idx]);
    }

    return nullptr;
}

void config_option_set_int(config_option_handle *me, int32_t value, uint32_t idx)
{
    if (me == nullptr)
        return;
    Slic3r::to_option(me)->set_int(value, idx);
}

void config_option_set_float(config_option_handle *me, double value, uint32_t idx)
{
    if (me == nullptr)
        return;
    Slic3r::to_option(me)->set_float(value, idx);
}

void config_option_set_float_or_percent(config_option_handle *me, c_float_or_percent value, uint32_t idx)
{
    if (me == nullptr)
        return;
    if(value.percent) {
        Slic3r::to_option(me)->set_percent(value.value, idx);
    } else {
        Slic3r::to_option(me)->set_float(value.value, idx);
    }
}

void config_option_set_bool(config_option_handle *me, int32_t value, uint32_t idx)
{
    if (me == nullptr)
        return;

    // Bool options store either a native bool scalar or byte vector. Assign
    // those concrete payloads because their classes intentionally do not
    // implement ConfigOption's numeric conversion setter.
    Slic3r::ConfigOption *option = Slic3r::to_option(me);
    if (Slic3r::ConfigOptionBool *scalar = dynamic_cast<Slic3r::ConfigOptionBool *>(option)) {
        scalar->value = value != 0;
        return;
    }
    if (Slic3r::ConfigOptionBools *values = dynamic_cast<Slic3r::ConfigOptionBools *>(option)) {
        if (idx < values->size())
            values->set_at(idx, static_cast<unsigned char>(value != 0));
    }
}

void config_option_set_string(config_option_handle *me, const char *value, uint32_t idx)
{
    if (me == nullptr || value == nullptr)
        return;

    Slic3r::ConfigOption *option = Slic3r::to_option(me);
    if (Slic3r::ConfigOptionString *scalar = dynamic_cast<Slic3r::ConfigOptionString *>(option)) {
        if (idx == 0)
            scalar->value = value;
        return;
    }
    if (Slic3r::ConfigOptionStrings *values = dynamic_cast<Slic3r::ConfigOptionStrings *>(option)) {
        if (idx < values->size())
            values->get_at(idx) = value;
    }
}

config_option_vector_handle *config_option_vector_cast_mutable(config_option_handle *me)
{
    if (me == nullptr || !Slic3r::to_option(me)->is_vector())
        return nullptr;
    return reinterpret_cast<config_option_vector_handle*>(me);
}

const config_option_vector_handle *config_option_vector_cast(const config_option_handle *me)
{
    if (me == nullptr || !Slic3r::to_option(me)->is_vector())
        return nullptr;
    return reinterpret_cast<const config_option_vector_handle*>(me);
}

uint32_t config_option_vector_size(const config_option_vector_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_option_vector(me)->size();
}

int32_t config_option_vector_empty(const config_option_vector_handle *me)
{
    return me != nullptr && Slic3r::to_option_vector(me)->empty();
}

uint32_t config_option_vector_serialize_at(const config_option_vector_handle *me, int32_t idx, char *out, uint32_t max_size)
{
    if (me == nullptr)
        return 0;
    return Slic3r::copy_string_out(Slic3r::to_option_vector(me)->serialize_at(idx), out, max_size);
}

void config_option_vector_resize(
    config_option_vector_handle *me, uint32_t new_size, const config_option_handle *default_value)
{
    if (me != nullptr)
        Slic3r::to_option_vector(me)->resize(new_size, Slic3r::to_option(default_value));
}

void config_option_vector_clear(config_option_vector_handle *me)
{
    if (me != nullptr)
        Slic3r::to_option_vector(me)->clear();
}

int32_t config_option_vector_is_extruder_size(const config_option_vector_handle *me)
{
    return me != nullptr && Slic3r::to_option_vector(me)->is_extruder_size();
}

void config_option_vector_set_is_extruder_size(config_option_vector_handle *me, int32_t is_extruder_size)
{
    if (me != nullptr)
        Slic3r::to_option_vector(me)->set_is_extruder_size(is_extruder_size != 0);
}

} // extern "C"
