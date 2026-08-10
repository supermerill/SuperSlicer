///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2019 - 2023 Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Pavel Mikuš @Godrak, Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ConfigManipulation.hpp"

#include <map>
#include <string>
#include <utility>

#include <wx/msgdlg.h>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/FFFPrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/SupportTreeStrategies.hpp"

#include "format.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

namespace Slic3r {
namespace GUI {

namespace {

using PluginGuiRuleTarget = std::pair<std::string, int>;

// Plugin GUI rules are evaluated from two places:
// 1. A global pass: apply_plugin_gui_rules(config)
//    This pass handles scalar targets, whole-field vector targets and
//    explicitly indexed targets.
// 2. A per-extruder pass: apply_plugin_gui_rules(config, extruder_idx)
//    This pass only handles targets that explicitly ask for
//    RAW_GUI_RULE_INDEX_CURRENT.
//
// This split avoids applying scalar rules once per extruder. Otherwise a scalar
// target controlled by an extruder-sized condition would effectively use the
// last extruder value instead of the intended "any extruder enables it" logic.
static const ConfigOptionVectorBase* extruder_sized_vector(const ConfigOption *option)
{
    if (option == nullptr || !option->is_vector())
        return nullptr;

    const ConfigOptionVectorBase *vector_option = dynamic_cast<const ConfigOptionVectorBase*>(option);
    return vector_option != nullptr && vector_option->is_extruder_size() ? vector_option : nullptr;
}

static bool gui_rule_condition_value_at_index(const ConfigOption &condition_option, const Orchestrator::PluginGuiRule &rule, int32_t condition_index)
{
    // Scalar get_bool() ignores the index. Vector get_bool() needs a real item
    // index, so "-1 / unspecified" falls back to 0 only for this low-level read.
    // Higher-level code decides when using index 0 is acceptable.
    const size_t value_index = condition_index < 0 ? 0 : size_t(condition_index);

    switch (rule.condition) {
    case RAW_GUI_RULE_CONDITION_BOOL_TRUE:
        return condition_option.get_bool(value_index);
    case RAW_GUI_RULE_CONDITION_BOOL_FALSE:
        return !condition_option.get_bool(value_index);
    case RAW_GUI_RULE_CONDITION_OPTION_ENABLED:
        return condition_option.is_enabled(condition_index);
    case RAW_GUI_RULE_CONDITION_OPTION_DISABLED:
        return !condition_option.is_enabled(condition_index);
    case RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO:
        return condition_option.get_bool(value_index);
    case RAW_GUI_RULE_CONDITION_INT_EQUALS:
        return condition_option.get_int(value_index) == rule.condition_int_value;
    case RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS:
        return condition_option.get_int(value_index) != rule.condition_int_value;
    case RAW_GUI_RULE_CONDITION_NONE:
    default:
        return false;
    }
}

static bool gui_rule_condition_value(const ConfigOption &condition_option, const Orchestrator::PluginGuiRule &rule, int current_index)
{
    // Rule-provided index is always explicit and therefore wins.
    if (rule.condition_index >= 0)
        return gui_rule_condition_value_at_index(condition_option, rule, rule.condition_index);

    if (rule.condition_index == RAW_GUI_RULE_INDEX_CURRENT) {
        if (current_index < 0 || extruder_sized_vector(&condition_option) == nullptr)
            return false;
        return gui_rule_condition_value_at_index(condition_option, rule, current_index);
    }

    // If the caller is currently processing extruder i, and the condition is an
    // extruder-sized vector, evaluate the same i-th item.
    if (current_index >= 0 && extruder_sized_vector(&condition_option) != nullptr)
        return gui_rule_condition_value_at_index(condition_option, rule, current_index);

    // No explicit index and no usable current index: for a vector condition,
    // enable the target if at least one vector item satisfies the rule. This is
    // safer than silently reading item 0 and disabling controls that should stay
    // available for another extruder.
    if (condition_option.is_vector()) {
        for (size_t condition_index = 0; condition_index < condition_option.size(); ++condition_index)
            if (gui_rule_condition_value_at_index(condition_option, rule, int32_t(condition_index)))
                return true;
        return false;
    }

    // Scalar condition, no index needed.
    return gui_rule_condition_value_at_index(condition_option, rule, -1);
}

static bool gui_rule_target_index_for_context(const ConfigOption &target_option, const Orchestrator::PluginGuiRule &rule, int current_index, int &target_index)
{
    // Rule-provided target index is explicit and is handled once by the global
    // pass. Running it again in the per-extruder pass would only duplicate the
    // same toggle call.
    target_index = rule.target_index;
    if (target_index >= 0)
        return current_index < 0;

    // RAW_GUI_RULE_INDEX_CURRENT is the only case that belongs to the indexed
    // pass. It means "toggle the item currently processed by the extruder loop".
    if (rule.target_index == RAW_GUI_RULE_INDEX_CURRENT) {
        if (current_index < 0 || extruder_sized_vector(&target_option) == nullptr)
            return false;

        target_index = current_index;
        return true;
    }

    // RAW_GUI_RULE_INDEX_ALL / default: apply the whole GUI field once during
    // the global pass. This is the form used for "enable this setting if at
    // least one extruder satisfies the condition".
    if (current_index >= 0)
        return false;

    target_index = RAW_GUI_RULE_INDEX_ALL;
    return true;
}

struct PluginGuiRuleTargetState
{
    bool has_all_rules = false;
    bool all_rules_enabled = true;
    bool has_any_rules = false;
    bool any_rule_enabled = false;

    void add(raw_gui_rule_action action, bool condition_enabled)
    {
        if (action == RAW_GUI_RULE_ACTION_ENABLE) {
            has_all_rules = true;
            all_rules_enabled = all_rules_enabled && condition_enabled;
        } else if (action == RAW_GUI_RULE_ACTION_ENABLE_ANY) {
            has_any_rules = true;
            any_rule_enabled = any_rule_enabled || condition_enabled;
        }
    }

    bool enabled() const
    {
        return (!has_all_rules || all_rules_enabled) && (!has_any_rules || any_rule_enabled);
    }
};

} // namespace

void ConfigManipulation::apply_plugin_gui_rules(DynamicPrintConfig *config, int current_index)
{
    if (config == nullptr)
        return;

    // Rules with the same target key and resolved target index are combined by
    // action:
    // - ENABLE rules are ANDed.
    // - ENABLE_ANY rules are ORed.
    // If a target uses both groups, both the AND group and the OR group have to
    // pass. This keeps the original "multiple ENABLE rules mean stricter"
    // behavior while allowing plugin-owned legacy options with OR conditions.
    std::map<PluginGuiRuleTarget, PluginGuiRuleTargetState> enabled_by_target;

    // current_index == -1 means "global pass".
    // current_index >= 0 means "per-extruder pass for that vector item".
    for (const Orchestrator::PluginGuiRule &rule : Orchestrator::instance().gui_rules()) {
        // This function currently only knows how to apply enable/disable rules.
        // Other actions may be added to the C API later, but they must not be
        // interpreted as enable rules by accident.
        if (rule.action != RAW_GUI_RULE_ACTION_ENABLE && rule.action != RAW_GUI_RULE_ACTION_ENABLE_ANY)
            continue;

        // A plugin may register a rule before the corresponding option exists
        // in this config kind, or the same rule list may be evaluated for FFF,
        // SLA and printer configs. Missing keys simply mean "not applicable to
        // this config".
        if (!config->has(rule.target_key) || !config->has(rule.condition_key))
            continue;

        const ConfigOption *target_option = config->optptr(rule.target_key);
        const ConfigOption *condition_option = config->optptr(rule.condition_key);

        // has() and optptr() should agree, but keep this defensive check because
        // plugin-provided keys cross the C API boundary and we do not want GUI
        // refresh code to crash on malformed input.
        if (target_option == nullptr || condition_option == nullptr)
            continue;

        int target_index = -1;
        // This decides whether the rule belongs to this pass and, if so, which
        // target index should be toggled.
        //
        // Examples:
        // - scalar target, global pass: applied with target_index == -1.
        // - scalar target, per-extruder pass: skipped here, applied later by the
        //   global pass.
        // - target_index == RAW_GUI_RULE_INDEX_ALL on an extruder-sized target:
        //   applied once to the whole field during the global pass.
        // - target_index == RAW_GUI_RULE_INDEX_CURRENT on an extruder-sized
        //   target: applied item by item during the per-extruder pass.
        if (!gui_rule_target_index_for_context(*target_option, rule, current_index, target_index))
            continue;

        // The condition may use the same current_index, an explicit rule index,
        // or "any vector item" depending on what is available.
        const bool condition_enabled = gui_rule_condition_value(*condition_option, rule, current_index);
        const PluginGuiRuleTarget target(rule.target_key, target_index);
        enabled_by_target[target].add(rule.action, condition_enabled);
    }

    for (const std::pair<const PluginGuiRuleTarget, PluginGuiRuleTargetState> &target_state : enabled_by_target) {
        try {
            this->toggle_field(target_state.first.first, target_state.second.enabled(), target_state.first.second);
        } catch (...) {
            assert(false);
        }
    }
}

void ConfigManipulation::apply(DynamicPrintConfig* config, DynamicPrintConfig* new_config)
{
    bool modified = false;
    for (auto opt_key : config->diff(*new_config)) {
        config->set_key_value(opt_key, new_config->option(opt_key)->clone());
        modified = true;
    }

    if (modified && load_config != nullptr)
        load_config();
}

void ConfigManipulation::toggle_field(const std::string& opt_key, const bool toggle, int opt_index/* = -1*/)
{
    if (local_config) {
        if (local_config->option(opt_key) == nullptr)
            return;
    }
    cb_toggle_field(opt_key, toggle, opt_index);
}

// !! if using cb_value_change(X) or something like that, you need a special code in Field.cpp (search for 'update_print_fff_config')
void ConfigManipulation::update_print_fff_config(DynamicPrintConfig* config, const bool is_global_config)
{
    // #ys_FIXME_to_delete
    //! Temporary workaround for the correct updates of the TextCtrl (like "layer_height"):
    // KillFocus() for the wxSpinCtrl use CallAfter function. So,
    // to except the duplicate call of the update() after dialog->ShowModal(),
    // let check if this process is already started.
    if (is_msg_dlg_already_exist)
        return;

    // layer_height shouldn't be equal to zero
    if (config->opt_float("layer_height") < EPSILON)
    {
        const wxString msg_text = _(L("Layer height is not valid.\n\nThe layer height will be reset to 0.01."));
        MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("Layer height")), wxICON_WARNING | wxOK);
        DynamicPrintConfig new_conf = *config;
        is_msg_dlg_already_exist = true;
        dialog.ShowModal();
        new_conf.set_key_value("layer_height", new ConfigOptionFloat(0.01));
        apply(config, &new_conf);
        is_msg_dlg_already_exist = false;
    }

    if (config->option<ConfigOptionFloatOrPercent>("first_layer_height")->value < EPSILON)
    {
        const wxString msg_text = _(L("First layer height is not valid.\n\nThe first layer height will be reset to 0.01."));
        MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("First layer height")), wxICON_WARNING | wxOK);
        DynamicPrintConfig new_conf = *config;
        is_msg_dlg_already_exist = true;
        dialog.ShowModal();
        new_conf.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent(0.01, false));
        apply(config, &new_conf);
        is_msg_dlg_already_exist = false;
    }

    double fill_density = config->option<ConfigOptionPercent>("fill_density")->value;

    if (config->opt_bool("spiral_vase") && !(
        config->opt_int("top_solid_layers") == 0
        && fill_density == 0
        && config->opt_bool("support_material") == false
        && config->opt_int("support_material_enforce_layers") == 0
        && config->opt_enum<PerimeterGeneratorType>("perimeter_generator") == PerimeterGeneratorType::Classic
        // && config->opt_bool("exact_last_layer_height") == false
        && config->opt_bool("infill_dense") == false
        && config->opt_bool("extra_perimeters") == false
        && config->option("extra_perimeters_below_area")->get_float() == 0
        && config->opt_int("extra_perimeters_count") == 0
        && config->opt_bool("extra_perimeters_odd_layers") == false
        && config->opt_bool("extra_perimeters_on_overhangs") == false
        && config->opt_bool("overhangs_reverse") == false
        && config->opt_bool("gap_fill_last") == false
        && config->opt_int("solid_infill_every_layers") == 0
        && config->opt_int("solid_over_perimeters") == 0
        && config->option("seam_notch_all")->get_float() == 0
        && config->option("seam_notch_inner")->get_float() == 0
        && config->option("seam_notch_outer")->get_float() == 0
        )) {
        wxString msg_text = _(L("The Spiral Vase mode requires:\n"
            "- no top solid layers\n"
            "- 0% fill density\n"
            "- classic perimeter slicing\n"
            "- no support material\n"
            "- disabled 'no solid infill over perimeters'\n"
            // "- unchecked 'exact last layer height'\n"
            "- unchecked 'dense infill'\n"
            "- unchecked 'extra perimeters'"
            "- unchecked 'gap fill after last perimeter'"
            "- set 'solid infill every layers' to 0"
            "- disabled  'no solid fill over X perimeters'"
            "- disabled 'seam notch'"));
        if (is_global_config)
            msg_text += "\n\n" + _(L("Shall I adjust those settings in order to enable Spiral Vase?"));
        MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("Spiral Vase")),
            wxICON_WARNING | (is_global_config ? wxYES | wxNO : wxOK));
        DynamicPrintConfig new_conf = *config;
        auto answer = dialog.ShowModal();
        bool support = true;
        if (!is_global_config) {
            if (this->local_config->get().optptr("spiral_vase"))
                new_conf.set_key_value("spiral_vase", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("top_solid_layers"))
                new_conf.set_key_value("top_solid_layers", new ConfigOptionInt(0));
            else if (this->local_config->get().optptr("fill_density"))
                new_conf.set_key_value("fill_density", new ConfigOptionPercent(0));
            else if (this->local_config->get().optptr("perimeter_generator"))
                new_conf.set_key_value("perimeter_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
            else if (this->local_config->get().optptr("support_material"))
                new_conf.set_key_value("support_material", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("support_material_enforce_layers"))
                new_conf.set_key_value("support_material_enforce_layers", new ConfigOptionInt(0));
            // else if (this->local_config->get().optptr("exact_last_layer_height"))
                // new_conf.set_key_value("exact_last_layer_height", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("infill_dense"))
                new_conf.set_key_value("infill_dense", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("extra_perimeters"))
                new_conf.set_key_value("extra_perimeters", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("extra_perimeters_below_area"))
                new_conf.set_key_value("extra_perimeters_below_area", new ConfigOptionFloatOrPercent(0, false));
            else if (this->local_config->get().optptr("extra_perimeters_count"))
                new_conf.set_key_value("extra_perimeters_count", new ConfigOptionInt(0));
            else if (this->local_config->get().optptr("extra_perimeters_odd_layers"))
                new_conf.set_key_value("extra_perimeters_odd_layers", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("extra_perimeters_on_overhangs"))
                new_conf.set_key_value("extra_perimeters_on_overhangs", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("overhangs_reverse"))
                new_conf.set_key_value("overhangs_reverse", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("gap_fill_last"))
                new_conf.set_key_value("gap_fill_last", new ConfigOptionBool(false));
            else if (this->local_config->get().optptr("solid_infill_every_layers"))
                new_conf.set_key_value("solid_infill_every_layers", new ConfigOptionInt(0));
            else if (this->local_config->get().optptr("solid_over_perimeters"))
                new_conf.set_key_value("solid_over_perimeters", new ConfigOptionInt(0));
            else if (this->local_config->get().optptr("seam_notch_all"))
                new_conf.set_key_value("seam_notch_all", new ConfigOptionFloatOrPercent(0, false));
            else if (this->local_config->get().optptr("seam_notch_inner"))
                new_conf.set_key_value("seam_notch_all", new ConfigOptionFloatOrPercent(0, false));
            else if (this->local_config->get().optptr("seam_notch_outer"))
                new_conf.set_key_value("seam_notch_all", new ConfigOptionFloatOrPercent(0, false));
            this->local_config->apply_only(new_conf, this->local_config->keys(), true);
        } else if (answer == wxID_YES) {
            new_conf.set_key_value("top_solid_layers", new ConfigOptionInt(0));
            new_conf.set_key_value("fill_density", new ConfigOptionPercent(0));
            new_conf.set_key_value("perimeter_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
            new_conf.set_key_value("support_material", new ConfigOptionBool(false));
            new_conf.set_key_value("support_material_enforce_layers", new ConfigOptionInt(0));
            // new_conf.set_key_value("exact_last_layer_height", new ConfigOptionBool(false));
            new_conf.set_key_value("infill_dense", new ConfigOptionBool(false));
            new_conf.set_key_value("extra_perimeters", new ConfigOptionBool(false));
            new_conf.set_key_value("extra_perimeters_below_area", new ConfigOptionFloatOrPercent(0, false));
            new_conf.set_key_value("extra_perimeters_count", new ConfigOptionInt(0));
            new_conf.set_key_value("extra_perimeters_odd_layers", new ConfigOptionBool(false));
            new_conf.set_key_value("extra_perimeters_on_overhangs", new ConfigOptionBool(false));
            new_conf.set_key_value("overhangs_reverse", new ConfigOptionBool(false));
            new_conf.set_key_value("gap_fill_last", new ConfigOptionBool(false));
            new_conf.set_key_value("solid_infill_every_layers", new ConfigOptionInt(0));
            new_conf.set_key_value("solid_over_perimeters", new ConfigOptionInt(0));
            new_conf.set_key_value("seam_notch_all", new ConfigOptionFloatOrPercent(0, false));
            new_conf.set_key_value("seam_notch_inner", new ConfigOptionFloatOrPercent(0, false));
            new_conf.set_key_value("seam_notch_outer", new ConfigOptionFloatOrPercent(0, false));
            fill_density = 0;
            support = false;
        } else {
            new_conf.set_key_value("spiral_vase", new ConfigOptionBool(false));
        }
        apply(config, &new_conf);
        if (cb_value_change) {
            cb_value_change("fill_density", fill_density);
            if (!support)
                cb_value_change("support_material", false);
        }
    }

    if (config->opt_bool("wipe_tower") && config->opt_bool("support_material") && 
        // Organic supports are always synchronized with object layers as of now.
        config->opt_enum<SupportMaterialStyle>("support_material_style") != smsOrganic) {
        // soluble support
        if (((ConfigOptionEnumGeneric*)config->option("support_material_contact_distance_type"))->value == zdNone) {
            if (!config->opt_bool("support_material_synchronize_layers")) {
                wxString msg_text = _(L("For the Wipe Tower to work with the soluble supports, the support layers\n"
                                        "need to be synchronized with the object layers."));
                if (is_global_config)
                    msg_text += "\n\n" + _(L("Shall I synchronize support layers in order to enable the Wipe Tower?"));
                MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("Wipe Tower")),
                                       wxICON_WARNING | (is_global_config ? wxYES | wxNO : wxOK));
                DynamicPrintConfig new_conf = *config;
                auto answer = dialog.ShowModal();
                if (!is_global_config) {
                    if (this->local_config->get().optptr("wipe_tower"))
                        new_conf.set_key_value("wipe_tower", new ConfigOptionBool(false));
                    else if (this->local_config->get().optptr("support_material_synchronize_layers"))
                        new_conf.set_key_value("support_material_synchronize_layers", new ConfigOptionBool(true));
                    else if (this->local_config->get().optptr("support_material_contact_distance_type"))
                        new_conf.set_key_value("support_material_contact_distance_type", new ConfigOptionEnum<SupportZDistanceType>(zdFilament));
                    else if (this->local_config->get().optptr("support_material"))
                        new_conf.set_key_value("support_material", new ConfigOptionBool(false));
                    this->local_config->apply_only(new_conf, this->local_config->keys(), true);
                } else if (answer == wxID_YES) {
                    new_conf.set_key_value("support_material_synchronize_layers", new ConfigOptionBool(true));
                } else {
                    new_conf.set_key_value("wipe_tower", new ConfigOptionBool(false));
                }
                apply(config, &new_conf);
            }
        }
        //else {
        //    // not-soluble support branch
        //    if ((config->opt_int("support_material_extruder") != 0 || config->opt_int("support_material_interface_extruder") != 0)) {
        //        wxString msg_text = _(L("The Wipe Tower currently supports the non-soluble supports only (support-> distance -> not 'none/soluble') "
        //                                "if they are printed with the current extruder without triggering a tool change. "
        //                                "(both support_material_extruder and support_material_interface_extruder need to be set to 0)."));
        //        if (is_global_config)
        //            msg_text += "\n\n" + _(L("Shall I adjust those settings in order to enable the Wipe Tower?"));
        //        MessageDialog dialog (m_msg_dlg_parent, msg_text, _(L("Wipe Tower")),
        //                                wxICON_WARNING | (is_global_config ? wxYES | wxNO : wxOK));
        //        DynamicPrintConfig new_conf = *config;
        //        auto answer = dialog.ShowModal();
        //        if (!is_global_config) {
        //            if (this->local_config->get().optptr("wipe_tower"))
        //                new_conf.set_key_value("wipe_tower", new ConfigOptionBool(false));
        //            else if (this->local_config->get().optptr("support_material_extruder"))
        //                new_conf.set_key_value("support_material_extruder", new ConfigOptionInt(0));
        //            else if (this->local_config->get().optptr("support_material_interface_extruder"))
        //                new_conf.set_key_value("support_material_interface_extruder", new ConfigOptionInt(0));
        //            else if (this->local_config->get().optptr("support_material_contact_distance_type"))
        //                new_conf.set_key_value("support_material_contact_distance_type", new ConfigOptionEnum<SupportZDistanceType>(zdNone));
        //            else if (this->local_config->get().optptr("support_material"))
        //                new_conf.set_key_value("support_material", new ConfigOptionBool(false));
        //            this->local_config->apply_only(new_conf, this->local_config->keys(), true);
        //        } else if (answer == wxID_YES) {
        //            new_conf.set_key_value("support_material_extruder", new ConfigOptionInt(0));
        //            new_conf.set_key_value("support_material_interface_extruder", new ConfigOptionInt(0));
        //        }
        //        else
        //            new_conf.set_key_value("wipe_tower", new ConfigOptionBool(false));
        //        apply(config, &new_conf);
        //    }
        //}
    }

    if (config->opt_float("brim_width") > 0 && config->opt_float("brim_separation") >= config->opt_float("brim_width")) {
        wxString msg_text = _(L("It's not possible to use a bigger value for the brim offset than the brim width, as it won't extrude anything."
            " Brim offset have to be lower than the brim width."));
        if (is_global_config) {
            msg_text += "\n\n" + _(L("Shall I switch the brim offset to 0?"));
            wxMessageDialog dialog(nullptr, msg_text, _(L("Brim configuration")),
                wxICON_WARNING | (is_global_config ? wxYES | wxNO : wxOK));
            auto answer = dialog.ShowModal();
            if (!is_global_config || answer == wxID_YES) {
                DynamicPrintConfig new_conf = *config;
                new_conf.set_key_value("brim_separation", new ConfigOptionFloat(0));
                apply(config, &new_conf);
            }
        }
    }

    // Check "support_material" and "overhangs" relations only on global settings level
    if (is_global_config && config->opt_bool("support_material")) {
        // Ask only once.
        if (!m_support_material_overhangs_queried) {
            m_support_material_overhangs_queried = true;
            if (!config->option("overhangs")->get_bool()) {
                wxString msg_text = _(L("Supports work better, if the following feature is enabled:\n"
                    "- overhangs threshold for speed & fan\n"
                    "- overhangs threshold for flow"));
                if (is_global_config) {
                    msg_text += "\n\n" + _(L("Shall I adjust those settings for supports?"));
                    MessageDialog dialog(m_msg_dlg_parent, msg_text, _L("Support Generator"), wxICON_WARNING | wxYES | wxNO);
                        DynamicPrintConfig new_conf = *config;
                        auto answer = dialog.ShowModal();
                    if (answer == wxID_YES) {
                        // Enable "detect bridging perimeters".
                        new_conf.set_key_value("overhangs", new ConfigOptionBool(true));
                        new_conf.set_key_value("overhangs_flow_ratio", config->option("overhangs_flow_ratio")->clone()->set_enabled(true));
                    } else if (answer == wxID_NO) {
                        // Do nothing, leave supports on and "detect bridging perimeters" off.
                    } else if (answer == wxID_CANCEL) {
                        // Disable supports.
                        new_conf.set_key_value("support_material", new ConfigOptionBool(false));
                        m_support_material_overhangs_queried = false;
                    }
                    apply(config, &new_conf);
                }
            }
        }
    }
    else {
        m_support_material_overhangs_queried = false;
    }

    if (config->option<ConfigOptionPercent>("fill_density")->value == 100) {
        const int fill_pattern = config->option<ConfigOptionEnum<InfillPattern>>("fill_pattern")->value;
        bool correct_100p_fill = config->option_def("top_fill_pattern")->enum_def->enum_to_index(fill_pattern).has_value();
        if (!correct_100p_fill) {
            correct_100p_fill = config->option_def("bottom_fill_pattern")->enum_def->enum_to_index(fill_pattern).has_value();
        }
        if (!correct_100p_fill) {
            correct_100p_fill = config->option_def("solid_fill_pattern")->enum_def->enum_to_index(fill_pattern).has_value();
        }
        if (!correct_100p_fill) {
            // get fill_pattern name from enum_labels for using this one at dialog_msg
            const ConfigOptionDef *fill_pattern_def = config->option_def("fill_pattern");
            assert(fill_pattern_def != nullptr);
            if (auto label = fill_pattern_def->enum_def->enum_to_label(fill_pattern); label.has_value()) {
                wxString msg_text = GUI::format_wxstr(_L("The %1% infill pattern is not supposed to work at 100%% density."), _(*label));
                if (is_global_config) {
                    msg_text += "\n\n" + _L("Shall I switch to rectilinear fill pattern?");
                MessageDialog dialog(m_msg_dlg_parent, msg_text, _L("Infill"),
                        wxICON_WARNING | (is_global_config ? wxYES | wxNO : wxOK));
                    DynamicPrintConfig new_conf;// = *config;
                    auto answer = dialog.ShowModal();
                    std::string key_changed;
                    boost::any new_value;
                    if (!is_global_config || answer == wxID_YES) {
                        new_conf.set_key_value("fill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
                        key_changed = "fill_pattern";
                        new_value = new_conf.option("fill_pattern")->get_any();
                    } else {
                        fill_density = wxGetApp().preset_bundle->fff_prints.get_selected_preset().config.option<ConfigOptionPercent>("fill_density")->value;
                        new_conf.set_key_value("fill_density", new ConfigOptionPercent(fill_density));
                        key_changed = "fill_density";
                        new_value = new_conf.option("fill_density")->get_any();
                    }
                    apply(config, &new_conf);
                    if (cb_value_change)
                        cb_value_change(key_changed, new_value);
                }
            }
        }
    }
}

void ConfigManipulation::toggle_print_fff_options(DynamicPrintConfig* config)
{
    bool have_perimeters = config->opt_int("perimeters") > 0;
    for (auto el : {
        "extra_perimeters", "extra_perimeters_below_area", "extra_perimeters_count", "extra_perimeters_odd_layers", "extra_perimeters_on_overhangs",
        "external_perimeters_first", "external_perimeter_extrusion_width", "external_perimeter_extrusion_spacing","external_perimeter_extrusion_change_odd_layers",
        "overhangs",
        "seam_position","staggered_inner_seams",
        "perimeter_speed", "perimeter_reverse", "perimeter_generator",
        "external_perimeter_speed", "small_perimeter_speed",
        "overhangs_dynamic_flow", "overhangs_dynamic_speed",
        "small_perimeter_min_length", " small_perimeter_max_length", "spiral_vase",
        "seam_notch_all", "seam_notch_inner", "seam_notch_outer"})
        toggle_field(el, have_perimeters);

    bool has_spiral_vase = have_perimeters && config->opt_bool("spiral_vase");
    
    toggle_field("perimeter_generator", have_perimeters);
    bool have_arachne = have_perimeters && config->opt_enum<PerimeterGeneratorType>("perimeter_generator") == PerimeterGeneratorType::Arachne;
    bool have_perimeter_hole = !have_arachne && (config->opt_int("perimeters") == config->opt_int("perimeters_hole") || !config->is_enabled("perimeters_hole"));
    for (auto el : {"wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle",
                    "wall_distribution_count", "min_feature_size", "min_bead_width"}) {
        toggle_field(el, have_arachne);
    }
    toggle_field("perimeters_hole", !have_arachne);
    toggle_field("overhangs_extrusion_spacing", !have_arachne);

    for (auto el : {"perimeter_loop", "thin_perimeters", "perimeter_round_corners"})
        toggle_field(el, have_perimeters && !have_arachne);

    bool have_perimeter_loop = config->opt_bool("perimeter_loop") && !have_arachne;

    bool has_external_peri_not_loop = config->opt_bool("external_perimeters_first") && !have_perimeter_loop;
    toggle_field("seam_slope_type", has_external_peri_not_loop);
    for (auto el : { "seam_slope_min_height", "seam_slope_max_length"})
        toggle_field(el, has_external_peri_not_loop && config->option<ConfigOptionEnum<SeamScarfType>>("seam_slope_type")->value != SeamScarfType::None);
    toggle_field("external_perimeters_first_force", has_external_peri_not_loop && !have_arachne );
    bool is_ext_forced = config->opt_bool("external_perimeters_first_force");
    for (auto el : { "external_perimeters_nothole", "external_perimeters_hole"})
        toggle_field(el, has_external_peri_not_loop && !have_arachne && !is_ext_forced);

    toggle_field("perimeter_bonding", config->opt_bool("external_perimeters_first") && !have_arachne && config->option("perimeter_overlap")->get_float() == 100.f && config->option("external_perimeter_overlap")->get_float() == 100.f);

    
    toggle_field("no_perimeter_unsupported_algo", have_perimeters);
    toggle_field("only_one_perimeter_top", have_perimeters);
    toggle_field("only_one_perimeter_first_layer", config->opt_int("perimeters") > 1);
    bool have_overhangs = have_perimeters &&config->opt_bool("overhangs");
    bool can_have_overhangs_reverse =  !have_arachne && have_overhangs && !config->opt_bool("perimeter_reverse");
    toggle_field("overhangs_reverse", can_have_overhangs_reverse);
    toggle_field("overhangs_reverse_threshold", can_have_overhangs_reverse && config->opt_bool("overhangs_reverse"));
    toggle_field("overhangs_speed_enforce", have_overhangs && !have_perimeter_loop && have_overhangs);
    for (auto el : { "overhangs_speed", "overhangs_width_speed", "overhangs_dynamic_speed", "overhangs_flow_ratio" })
        toggle_field(el, have_overhangs);
    bool have_overhangs_flow = have_overhangs && config->option("overhangs_flow_ratio")->is_enabled();
    for (auto el : { "overhangs_width", "overhangs_dynamic_flow" })
        toggle_field(el, have_overhangs_flow);


    toggle_field("min_width_top_surface", have_perimeters && config->opt_bool("only_one_perimeter_top"));
    toggle_field("thin_perimeters_all", have_perimeters && config->option("thin_perimeters")->get_float() != 0 && !have_arachne);
    bool have_thin_wall = !have_arachne && have_perimeters;
    toggle_field("thin_walls", have_thin_wall);
    for (auto el : { "thin_walls_min_width", "thin_walls_overlap", "thin_walls_merge" })
        toggle_field(el, have_thin_wall && config->opt_bool("thin_walls"));

    for (auto el : { "seam_angle_cost", "seam_travel_cost", "seam_visibility" })
        toggle_field(el, have_perimeters && config->option<ConfigOptionEnum<SeamPosition>>("seam_position")->value == SeamPosition::spCost);

    toggle_field("perimeter_loop_seam", have_perimeter_loop);

    bool have_notch = have_perimeters && (config->option("seam_notch_all")->get_float() != 0 ||
                                          config->option("seam_notch_inner")->get_float() != 0 ||
                                          config->option("seam_notch_outer")->get_float() != 0);
    toggle_field("seam_notch_angle", have_notch);

    bool have_gap_fill = !have_arachne;
    toggle_field("gap_fill_enabled", have_gap_fill);
    have_gap_fill = have_gap_fill && config->opt_bool("gap_fill_enabled");
    for (auto el : { "gap_fill_last"})
        toggle_field(el, have_gap_fill);
    for (auto el : { "gap_fill_no_overhang" })
        toggle_field(el, have_gap_fill);
    if (!have_gap_fill) {
        have_gap_fill = config->opt_bool("infill_filled_bottom") || config->opt_bool("infill_filled_solid") || config->opt_bool("infill_filled_top");
    }
    for (auto el : { "gap_fill_extension", "gap_fill_max_width", "gap_fill_min_area", "gap_fill_min_length", "gap_fill_min_width" })
        toggle_field(el, have_gap_fill);
    // gap fill  can appear in infill
    //toggle_field("gap_fill_speed", have_perimeters && config->opt_bool("gap_fill_enabled"));

    for (auto el : { "fuzzy_skin_thickness", "fuzzy_skin_point_dist" })
        toggle_field(el, config->option<ConfigOptionEnum<FuzzySkinType>>("fuzzy_skin")->value != FuzzySkinType::None);

    bool have_infill = config->option<ConfigOptionPercent>("fill_density")->value > 0;
    // infill_extruder uses the same logic as in Print::extruders()
    for (auto el : { "fill_aligned_z", "fill_pattern", "infill_connection", "infill_every_layers", "infill_only_where_needed",
                    "solid_infill_every_layers", "solid_infill_below_area", "solid_infill_below_layer_area", "solid_infill_below_width",
                    "infill_extruder", "infill_anchor_max" })
        toggle_field(el, have_infill);
    // Only allow configuration of open anchors if the anchoring is enabled.
    bool has_infill_anchors = have_infill && config->option<ConfigOptionEnum<InfillConnection>>("infill_connection")->value != InfillConnection::icNotConnected;
    toggle_field("infill_anchor_max", has_infill_anchors);
    has_infill_anchors = has_infill_anchors && config->option<ConfigOptionFloatOrPercent>("infill_anchor_max")->value > 0;
    toggle_field("infill_anchor", has_infill_anchors);

    bool can_have_infill_dense = config->option<ConfigOptionPercent>("fill_density")->value < 50;
    for (auto el : { "infill_dense" })
        toggle_field(el, can_have_infill_dense);

    bool have_infill_dense = config->opt_bool("infill_dense") && can_have_infill_dense;
    for (auto el : { "infill_dense_algo" })
        toggle_field(el, have_infill_dense);
    if(have_infill)
        for (auto el : { "infill_every_layers", "infill_only_where_needed" })
            toggle_field(el, !have_infill_dense);

    bool has_top_solid_infill 	 = config->opt_int("top_solid_layers") > 0 || has_spiral_vase || config->opt_int("solid_infill_every_layers") == 1;
    bool has_bottom_solid_infill = config->opt_int("bottom_solid_layers") > 0 || config->opt_int("solid_infill_every_layers") == 1;
    bool has_solid_infill 		 = has_top_solid_infill || has_bottom_solid_infill || (have_infill && (config->opt_int("solid_infill_every_layers") > 0 || config->opt_float("solid_infill_below_area") > 0));
    // solid_infill_extruder uses the same logic as in Print::extruders()
    for (auto el : { "top_fill_pattern", "bottom_fill_pattern", "solid_fill_pattern", "enforce_full_fill_volume", "external_infill_margin", "bridged_infill_margin",
        "solid_infill_extruder", "solid_infill_extrusion_width", "solid_nfill_extrusion_change_odd_layers", "solid_infill_extrusion_spacing", "solid_infill_speed" })
        toggle_field(el, has_solid_infill);

    toggle_field("infill_first", (has_solid_infill || have_infill));

    for (auto el : {"fill_angle_cross","fill_angle_follow_model","fill_angle_increment", "fill_angle_template", "bridge_angle", "infill_extrusion_width",
                    "infill_extrusion_spacing", "infill_extrusion_change_odd_layers", "infill_speed" })
        toggle_field(el, have_infill || has_solid_infill);
        
    toggle_field("fill_angle", (have_infill || has_solid_infill) && ((ConfigOptionVectorBase*)config->option("fill_angle_template"))->size() == 0);

    const bool has_ensure_vertical_shell_thickness = config->opt_enum<EnsureVerticalShellThickness>("ensure_vertical_shell_thickness") != EnsureVerticalShellThickness::Disabled;
    toggle_field("top_solid_min_thickness", ! has_spiral_vase && has_top_solid_infill && has_ensure_vertical_shell_thickness);
    toggle_field("bottom_solid_min_thickness", ! has_spiral_vase && has_bottom_solid_infill && has_ensure_vertical_shell_thickness);

    //speed
    for (auto el : { "small_perimeter_min_length", "small_perimeter_max_length" })
        toggle_field(el, config->option("small_perimeter_speed")->get_float() > 0);

    bool has_ironing_pattern = config->opt_enum<InfillPattern>("top_fill_pattern") == InfillPattern::ipSmooth
        || config->opt_enum<InfillPattern>("bottom_fill_pattern") == InfillPattern::ipSmooth
        || config->opt_enum<InfillPattern>("solid_fill_pattern") == InfillPattern::ipSmooth;
    for (auto el : {"fill_smooth_width, fill_smooth_distribution" })
        toggle_field(el, has_ironing_pattern);

    for (auto el : { "ironing", "top_fill_pattern", "infill_connection_top",  "top_infill_extrusion_width",  "top_infill_extrusion_spacing", "top_solid_infill_speed" })
        toggle_field(el, has_top_solid_infill || (has_spiral_vase && has_bottom_solid_infill));

    for (auto el : { "bottom_fill_pattern", "infill_connection_bottom" })
        toggle_field(el, has_bottom_solid_infill);

    for (auto el : { "solid_fill_pattern", "infill_connection_solid", "bridge_fill_pattern", "infill_connection_bridge" })
        toggle_field(el, has_solid_infill); // should be top_solid_layers") > 1 || bottom_solid_layers") > 1

    bool have_skirt = config->opt_int("skirts") > 0;
    toggle_field("skirt_height", have_skirt && config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
    toggle_field("skirt_width", have_skirt);
    for (auto el : { "skirt_brim", "skirt_distance", "skirt_distance_from_brim", "draft_shield", "min_skirt_length" })
        toggle_field(el, have_skirt);

    bool have_brim = config->opt_float("brim_width") > 0 || config->opt_float("brim_width_interior") > 0;
    toggle_field("brim_separation", have_brim);
    // perimeter_extruder uses the same logic as in Print::extruders()
    toggle_field("perimeter_extruder", have_perimeters || have_brim);

    bool have_raft = config->opt_int("raft_layers") > 0;
    bool have_support_material = config->opt_bool("support_material") || have_raft;
    bool have_support_material_auto = have_support_material && config->opt_bool("support_material_auto");
    bool have_support_interface = have_support_material && (config->opt_int("support_material_interface_layers") > 0 || config->opt_int("support_material_bottom_interface_layers") > 0);
    bool have_support_soluble = have_support_material && ((ConfigOptionEnumGeneric*)config->option("support_material_contact_distance_type"))->value == zdNone;
    auto support_material_style = config->opt_enum<SupportMaterialStyle>("support_material_style");
    for (auto el : { "support_material_style", "support_material_pattern", "support_material_with_sheath",
                    "support_material_spacing", "support_material_angle", "support_material_angle_height", 
                    "support_material_bottom_interface_layers", "support_material_interface_layers",
                    "support_material_extrusion_width",
                    "support_material_contact_distance_type",
                    "support_material_xy_spacing",
                    "support_material_layer_height"})
        toggle_field(el, have_support_material);
    toggle_field("support_material_threshold", have_support_material_auto);
    toggle_field("support_material_bottom_contact_distance", have_support_material && ! have_support_soluble);
    toggle_field("support_material_closing_radius", have_support_material && support_material_style == smsSnug);

    const bool has_organic_supports = support_material_style == smsOrganic && 
                                     (config->opt_bool("support_material") || 
                                      config->opt_int("support_material_enforce_layers") > 0);
    for (const std::string& key : { "support_tree_angle", "support_tree_angle_slow", "support_tree_branch_diameter",
                                    "support_tree_branch_diameter_angle", "support_tree_branch_diameter_double_wall", 
                                    "support_tree_tip_diameter", "support_tree_branch_distance", "support_tree_top_rate" })
        toggle_field(key, has_organic_supports);

    for (auto el : { "support_material_contact_distance", "support_material_bottom_contact_distance" })
        toggle_field(el, have_support_material && !have_support_soluble);

    for (auto el : { "support_material_bottom_interface_pattern", "support_material_top_interface_pattern", "support_material_interface_spacing", "support_material_interface_extruder",
                    "support_material_interface_speed", "support_material_interface_contact_loops", "support_material_interface_layer_height"
                    "support_material_interface_angle", "support_material_interface_angle_increment", "support_material_bottom_interface_expansion"})
        toggle_field(el, have_support_material && have_support_interface);
    toggle_field("support_material_synchronize_layers", have_support_soluble);

    // organic suport don't use soem fields, force disable them.
    if (has_organic_supports) {
        for (const std::string &key :
             {"support_material_interface_layer_height", "support_material_bottom_interface_pattern",
              "support_material_interface_contact_loops", "support_material_with_sheath", "support_material_pattern",
              "support_material_spacing", "support_material_angle", "support_material_angle_height", "support_material_layer_height",
              "support_material_bottom_interface_pattern"})
            toggle_field(key, false);
    }

    toggle_field("perimeter_extrusion_width", have_perimeters || have_brim);
    toggle_field("perimeter_extrusion_spacing", have_perimeters || have_brim);
    toggle_field("perimeter_extrusion_change_odd_layers", have_perimeters || have_brim);
    toggle_field("skirt_extrusion_width", have_skirt);
    toggle_field("support_material_extruder", have_support_material || have_skirt);
    toggle_field("support_material_speed", have_support_material || have_skirt || have_brim);
    toggle_field("brim_speed", have_brim || have_skirt);

    bool have_raft_soluble = have_support_material && ((ConfigOptionEnumGeneric*)config->option("raft_contact_distance_type"))->value == zdNone;
    toggle_field("raft_contact_distance", have_raft && !have_raft_soluble);
    for (auto el : { "raft_expansion", "first_layer_acceleration_over_raft", "first_layer_speed_over_raft",
        "raft_layer_height", "raft_interface_layer_height"})
        toggle_field(el, have_raft);

    //for default_extrusion_width/spacing, you need to ahve at least an extrusion_width with 0
    auto opt_first_layer_width = config->option("first_layer_extrusion_width");
    auto opt_first_layer_infill_width = config->option("first_layer_infill_extrusion_width");
    assert(opt_first_layer_width);
    assert(opt_first_layer_infill_width);
    bool have_default_width = 
        (opt_first_layer_width->is_enabled() && opt_first_layer_width->get_float() == 0) ||
        (opt_first_layer_infill_width->is_enabled() && opt_first_layer_infill_width->get_float() == 0) ||
        (config->option("perimeter_extrusion_width")->get_float() == 0 && (have_perimeters || have_brim)) ||
        (config->option("external_perimeter_extrusion_width")->get_float() == 0 && have_perimeters) ||
        (config->option("infill_extrusion_width")->get_float() == 0 && (have_infill || has_solid_infill)) ||
        (config->option("solid_infill_extrusion_width")->get_float() == 0 && has_solid_infill) ||
        (config->option("top_infill_extrusion_width")->get_float() == 0 && has_top_solid_infill) ||
        (config->option("support_material_extrusion_width")->get_float() == 0 && have_support_material) ||
        (config->option("skirt_extrusion_width")->get_float() == 0 && have_skirt);
    toggle_field("extrusion_width", have_default_width);
    toggle_field("extrusion_spacing", have_default_width);
    
    toggle_field("first_layer_extrusion_spacing", opt_first_layer_width->is_enabled());
    toggle_field("first_layer_infill_extrusion_spacing", opt_first_layer_infill_width->is_enabled());
    

    bool has_PP_ironing = has_top_solid_infill && config->opt_bool("ironing");
    for (auto el : { "ironing_type", "ironing_flowrate", "ironing_spacing", "ironing_angle" })
        toggle_field(el, has_PP_ironing);

    bool has_ironing = has_PP_ironing || has_ironing_pattern;
    for (auto el : { "ironing_speed" })
        toggle_field(el, has_ironing);
    

    bool have_sequential_printing = config->opt_bool("complete_objects") || config->opt_float("parallel_objects_step") > 0;
    for (auto el : { /*"extruder_clearance_radius", "extruder_clearance_height",*/ "complete_objects_one_skirt",
        "complete_objects_sort"})
        toggle_field(el, have_sequential_printing);
    bool temp_complete_step = !config->opt_bool("complete_objects");
    toggle_field("parallel_objects_step", temp_complete_step);
    temp_complete_step = temp_complete_step && config->opt_float("parallel_objects_step") > 0;
    toggle_field("parallel_objects_step_max_z", temp_complete_step);
    toggle_field("parallel_islands", temp_complete_step);
    temp_complete_step = temp_complete_step && config->opt_bool("parallel_islands");

    bool have_ooze_prevention = config->opt_bool("ooze_prevention");
    toggle_field("standby_temperature_delta", have_ooze_prevention);

    bool have_wipe_tower = config->opt_float("parallel_objects_step") <= 0 || config->opt_bool("parallel_islands");
    toggle_field("wipe_tower", have_wipe_tower);
    have_wipe_tower = have_wipe_tower && config->opt_bool("wipe_tower");
    for (auto el : { "wipe_tower_x", "wipe_tower_y", "wipe_tower_width", "wipe_tower_rotation_angle", "wipe_tower_brim_width",
                     "wipe_tower_cone_angle", "wipe_tower_extra_spacing",
                     "wipe_tower_bridging", "wipe_tower_brim", "wipe_tower_no_sparse_layers", "single_extruder_multi_material_priming",
                     "wipe_tower_speed", "wipe_tower_wipe_starting_speed",
                     "wipe_tower_extrusion_width", "wipe_tower_rest_in_middle" })
        toggle_field(el, have_wipe_tower);

    bool have_non_zero_mmu_segmented_region_max_width = config->opt_float("mmu_segmented_region_max_width") > 0.;
    toggle_field("mmu_segmented_region_interlocking_depth", have_non_zero_mmu_segmented_region_max_width);

    toggle_field("avoid_crossing_curled_overhangs", !config->opt_bool("avoid_crossing_perimeters"));
    toggle_field("avoid_crossing_perimeters", !config->opt_bool("avoid_crossing_curled_overhangs"));

    bool have_avoid_crossing_perimeters = config->opt_bool("avoid_crossing_perimeters");
    toggle_field("avoid_crossing_perimeters_max_detour", have_avoid_crossing_perimeters);
    toggle_field("avoid_crossing_not_first_layer", have_avoid_crossing_perimeters);
    toggle_field("avoid_crossing_top", have_avoid_crossing_perimeters);
    toggle_field("avoid_travel_island", have_avoid_crossing_perimeters);
    toggle_field("avoid_travel_island_weight", have_avoid_crossing_perimeters && config->opt_bool("avoid_travel_island"));
    
    toggle_field("enforce_retract_first_layer", config->opt_bool("only_retract_when_crossing_perimeters"));


    for (auto el : { "fill_smooth_width", "fill_smooth_distribution" })
        toggle_field(el, (has_top_solid_infill && config->option<ConfigOptionEnum<InfillPattern>>("top_fill_pattern")->value == InfillPattern::ipSmooth)
            || (has_bottom_solid_infill && config->option<ConfigOptionEnum<InfillPattern>>("bottom_fill_pattern")->value == InfillPattern::ipSmooth)
            || (has_solid_infill && config->option<ConfigOptionEnum<InfillPattern>>("solid_fill_pattern")->value == InfillPattern::ipSmooth)
            || (have_support_material && config->option<ConfigOptionEnum<InfillPattern>>("support_material_top_interface_pattern")->value == InfillPattern::ipSmooth)
            || (have_support_material && config->option<ConfigOptionEnum<InfillPattern>>("support_material_bottom_interface_pattern")->value == InfillPattern::ipSmooth));

    //TODO: can the milling_diameter or the milling_cutter be check to enable/disable this?
    for (auto el : { "milling_after_z", "milling_extra_size", "milling_speed" })
        toggle_field(el, config->opt_bool("milling_post_process"));

    bool have_default_acceleration = config->option<ConfigOptionFloatOrPercent>("default_acceleration")->value > 0;
    for (auto el : { "perimeter_acceleration", "external_perimeter_acceleration", "thin_walls_acceleration" })
        toggle_field(el, have_default_acceleration && have_perimeters);
    toggle_field("infill_acceleration", have_default_acceleration && have_infill);
    toggle_field("solid_infill_acceleration", have_default_acceleration && has_solid_infill);
    toggle_field("top_solid_infill_acceleration", have_default_acceleration && has_top_solid_infill);
    toggle_field("ironing_acceleration", have_default_acceleration && has_ironing);
    toggle_field("support_material_acceleration", have_default_acceleration && (have_support_material || have_brim || have_skirt));
    toggle_field("support_material_interface_acceleration", have_default_acceleration && have_support_material && have_support_interface);
    toggle_field("brim_acceleration", have_default_acceleration && (have_brim || have_skirt));
    for (auto el : { "bridge_acceleration", "internal_bridge_acceleration", "overhangs_acceleration", "gap_fill_acceleration", "travel_acceleration", "travel_deceleration_use_target", "first_layer_acceleration" })
        toggle_field(el, have_default_acceleration);

    // for default speed, it needs at least a dependent field with a %
    toggle_field("default_speed", config->option<ConfigOptionFloatOrPercent>("perimeter_speed")->percent || 
        config->option<ConfigOptionFloatOrPercent>("solid_infill_speed")->percent || 
        config->option<ConfigOptionFloatOrPercent>("bridge_speed")->percent || 
        config->option<ConfigOptionFloatOrPercent>("support_material_speed")->percent);
    toggle_field("max_print_speed", config->opt_float("max_volumetric_speed") != 0);
    toggle_field("autospeed_min_thin_flow", config->opt_float("max_volumetric_speed") != 0);

    this->apply_plugin_gui_rules(config);
}


void ConfigManipulation::update_printer_fff_config(DynamicPrintConfig *config,
                                                   const bool          is_global_config)
{
    const std::vector<double> &nozzle_sizes = config->option<ConfigOptionFloats>("nozzle_diameter")->get_values();
    double min_step_size = config->option("z_step")->get_float();
    //for each extruder
    for (size_t extruder_idx = 0; extruder_idx < nozzle_sizes.size(); ++extruder_idx) {
        double min_lh = config->get_computed_value("min_layer_height", extruder_idx);
        double max_lh = config->option("max_layer_height")->is_enabled() ? config->get_computed_value("max_layer_height", extruder_idx) : nozzle_sizes[extruder_idx] * 0.75f;
        if (config->option("max_layer_height")->is_enabled() && (max_lh < min_step_size || max_lh < EPSILON)) {
            const wxString msg_text = _(
                L("Maximum layer height is not valid, it can't be lower than minimum z step, and not 0.\n\nThe maximum layer height will be deactivated (set to 75% of the nozzle diameter)."));
            MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("Maximum layer height")), wxICON_WARNING | wxOK);
            DynamicPrintConfig new_conf = *config;
            is_msg_dlg_already_exist    = true;
            dialog.ShowModal();
            new_conf.option<ConfigOptionFloatsOrPercents>("max_layer_height")->set_at(FloatOrPercent{75., true}, extruder_idx);
            new_conf.option<ConfigOptionFloatsOrPercents>("max_layer_height")->set_enabled(false, extruder_idx);
            apply(config, &new_conf);
            is_msg_dlg_already_exist = false;
            max_lh = config->get_computed_value("max_layer_height", extruder_idx);
        }
        // now max_lh > nozzle_size is allowed, but a warning is sent when changed
        if (min_lh >= max_lh) {
            const wxString msg_text = _(
                L("Minimum layer height is not valid, it can't be higher or equal to the maximum layer height.\n\nThe minimum layer height will be set to 0."));
            MessageDialog dialog(m_msg_dlg_parent, msg_text, _(L("Minimum layer height")), wxICON_WARNING | wxOK);
            DynamicPrintConfig new_conf = *config;
            is_msg_dlg_already_exist    = true;
            dialog.ShowModal();
            new_conf.option<ConfigOptionFloatsOrPercents>("min_layer_height")->set_at(FloatOrPercent{0.0, false}, extruder_idx);
            apply(config, &new_conf);
            min_lh = config->get_computed_value("min_layer_height", extruder_idx);
            is_msg_dlg_already_exist = false;
        }
        
        bool have_retract_length = config->opt_float("retract_length", extruder_idx) > 0;
        bool use_firmware_retraction = config->opt_bool("use_firmware_retraction");
        bool wipe = config->get_bool("wipe", extruder_idx) && have_retract_length;
        if (use_firmware_retraction && wipe) {
            DynamicPrintConfig new_conf = *config;
            //wxMessageDialog dialog(parent(),
            MessageDialog dialog(m_msg_dlg_parent,
                _(L("The Wipe option is not available when using the Firmware Retraction mode.\n"
                "\nShall I disable it in order to enable Firmware Retraction?")),
                _(L("Firmware Retraction")), wxICON_WARNING | wxYES | wxNO);

            if (dialog.ShowModal() == wxID_YES) {
                new_conf.option<ConfigOptionBools>("wipe")->set_at(uint8_t(false), extruder_idx);
            } else {
                new_conf.option<ConfigOptionBool>("use_firmware_retraction")->value = false;
            }
            apply(config, &new_conf);
        }
    }
}
void ConfigManipulation::toggle_printer_fff_options(DynamicPrintConfig *config, DynamicPrintConfig &full_config)
{

    size_t extruder_count = config->option("nozzle_diameter")->size();
    toggle_field("toolchange_gcode", extruder_count > 1);
    toggle_field("single_extruder_multi_material", extruder_count > 1);

    //thumbnails
    bool custom_color = config->opt_bool("thumbnails_custom_color");
    toggle_field("thumbnails_color", custom_color);
    const ConfigOptionEnum<GCodeThumbnailsFormat>* thumbnails_format = config->option<ConfigOptionEnum<GCodeThumbnailsFormat>>("thumbnails_format");
    
    if (thumbnails_format) {
        toggle_field("thumbnails_end_file", thumbnails_format->value != (GCodeThumbnailsFormat::BIQU));
        toggle_field("thumbnails_tag_format", thumbnails_format->value != (GCodeThumbnailsFormat::BIQU));
    }

    bool have_arc_fitting = config->option("arc_fitting")->get_int() != int(ArcFittingType::Disabled);
    toggle_field("arc_fitting_ignore_holes", have_arc_fitting);
    toggle_field("arc_fitting_resolution", have_arc_fitting);
    toggle_field("arc_fitting_tolerance", have_arc_fitting);

    //firmware
    bool have_remaining_times = config->opt_bool("remaining_times");
    toggle_field("remaining_times_type", have_remaining_times);

    bool gcode_min_length = config->get_float("gcode_min_length") > 0 && config->is_enabled("gcode_min_length");
    bool max_gcode_per_second = config->get_float("max_gcode_per_second") > 0 && config->is_enabled("max_gcode_per_second");
    toggle_field("gcode_min_resolution", gcode_min_length || max_gcode_per_second);
    toggle_field("gcode_command_buffer", max_gcode_per_second);

    auto flavor = config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    bool is_marlin_flavor = flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware;
    // Disable silent mode for non-marlin firmwares.
    toggle_field("silent_mode", is_marlin_flavor);

    for (size_t i = 0; i < extruder_count; ++i) {
        
        bool have_retract_length = config->opt_float("retract_length", i) > 0;
        
        const bool ramping_lift = config->get_bool("travel_ramping_lift", i);
        //const bool lifts_z = (ramping_lift && config->get_float("travel_max_lift", i) > 0)
        //                  || (! ramping_lift && config->get_float("retract_lift", i) > 0);

        //toggle_field("travel_max_lift", ramping_lift, i);
        toggle_field("travel_slope", ramping_lift, i);
        // toggle_field("retract_lift", ! ramping_lift, i);

        // when using firmware retraction, firmware decides retraction length
        bool use_firmware_retraction = config->opt_bool("use_firmware_retraction");
        toggle_field("retract_length", !use_firmware_retraction, i);

        // retraction only if have retraction length or we're using firmware retraction
        bool retraction = (have_retract_length || use_firmware_retraction);
        
        // user can customize travel length if we have retraction length or we"re using
        // firmware retraction
        toggle_field("retract_before_travel", have_retract_length || use_firmware_retraction, i);
        
        // user can customize other retraction options if retraction is enabled
        //std::vector<std::string> vec = {"retract_layer_change" }; // "retract_lift" "retract_before_travel"
        // now possible outside retraction
        // for (auto el : vec) {
            // toggle_field(el, retraction, i);
        // }
                bool has_lift = /*retraction &&  now possible outside retraction */ config->get_float("retract_lift", i) > 0;
        // retract lift above / below only applies if using retract lift
        // vec.resize(0);
        std::vector<std::string> vec = { "retract_lift_above", "retract_lift_below", "retract_lift_top", "retract_lift_first_layer", "retract_lift_before_travel"};
        for (auto el : vec) {
            toggle_field(el, has_lift, i);
        }

        // some options only apply when not using firmware retraction
        vec.resize(0);
        vec = { "retract_speed", "deretract_speed", "retract_before_wipe",
            "retract_restart_extra", "wipe", "wipe_speed" , "wipe_only_crossing",
            "wipe_return"};
        for (auto el : vec) {
            toggle_field(el, retraction && !use_firmware_retraction, i);
        }

        bool wipe = config->opt_bool("wipe", i) && have_retract_length;
        vec.resize(0);
        vec = { "retract_before_wipe", "wipe_only_crossing", "wipe_return", "wipe_speed" };
        for (auto el : vec) {
            toggle_field(el, wipe, i);
        }

        // wipe_only_crossing can only work if avoid_crossing_perimeters
        if (!full_config.option("avoid_crossing_perimeters")->get_bool()) {
            toggle_field("wipe_only_crossing", false, i);
        }
        
        toggle_field("travel_lift_before_obstacle", ramping_lift, i);

        toggle_field("retract_length_toolchange", extruder_count > 1, i);

        bool toolchange_retraction = config->opt_float("retract_length_toolchange", i) > 0;
        toggle_field("retract_restart_extra_toolchange", extruder_count > 1 && toolchange_retraction, i);
        toggle_field("retract_restart_toolchange_on_perimeter", extruder_count > 1 && toolchange_retraction, i);
        toggle_field("retract_restart_wipe_toolchange", extruder_count > 1 && toolchange_retraction, i);

        this->apply_plugin_gui_rules(config, int(i));
    }

    if (config->opt_bool("single_extruder_multi_material") && extruder_count > 1) {
        bool have_advanced_wipe_volume = config->opt_bool("wipe_advanced");
        for (auto el : { "wipe_advanced_nozzle_melted_volume", "wipe_advanced_multiplier", "wipe_advanced_algo" }) {
            toggle_field(el, have_advanced_wipe_volume);
        }
    }

    this->apply_plugin_gui_rules(config);
}

void ConfigManipulation::toggle_print_sla_options(DynamicPrintConfig* config)
{
    bool supports_en = config->opt_bool("supports_enable");
    sla::SupportTreeType treetype = config->opt_enum<sla::SupportTreeType>("support_tree_type");
    bool is_default_tree = treetype == sla::SupportTreeType::Default;
    bool is_branching_tree = treetype == sla::SupportTreeType::Branching;

    toggle_field("support_tree_type", supports_en);

    toggle_field("support_head_front_diameter", supports_en && is_default_tree);
    toggle_field("support_head_penetration", supports_en && is_default_tree);
    toggle_field("support_head_width", supports_en && is_default_tree);
    toggle_field("support_pillar_diameter", supports_en && is_default_tree);
    toggle_field("support_small_pillar_diameter_percent", supports_en && is_default_tree);
    toggle_field("support_max_bridges_on_pillar", supports_en && is_default_tree);
    toggle_field("support_pillar_connection_mode", supports_en && is_default_tree);
    toggle_field("support_buildplate_only", supports_en && is_default_tree);
    toggle_field("support_base_diameter", supports_en && is_default_tree);
    toggle_field("support_base_height", supports_en && is_default_tree);
    toggle_field("support_base_safety_distance", supports_en && is_default_tree);
    toggle_field("support_critical_angle", supports_en && is_default_tree);
    toggle_field("support_max_bridge_length", supports_en && is_default_tree);
    toggle_field("support_enforcers_only", supports_en);
    toggle_field("support_max_pillar_link_distance", supports_en && is_default_tree);
    toggle_field("support_pillar_widening_factor", false);
    toggle_field("support_max_weight_on_model", false);

    toggle_field("branchingsupport_head_front_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_head_penetration", supports_en && is_branching_tree);
    toggle_field("branchingsupport_head_width", supports_en && is_branching_tree);
    toggle_field("branchingsupport_pillar_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_small_pillar_diameter_percent", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_bridges_on_pillar", false);
    toggle_field("branchingsupport_pillar_connection_mode", false);
    toggle_field("branchingsupport_buildplate_only", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_height", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_safety_distance", supports_en && is_branching_tree);
    toggle_field("branchingsupport_critical_angle", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_bridge_length", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_pillar_link_distance", false);
    toggle_field("branchingsupport_pillar_widening_factor", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_weight_on_model", supports_en && is_branching_tree);

    toggle_field("support_points_density_relative", supports_en);
    toggle_field("support_points_minimal_distance", supports_en);

    bool pad_en = config->opt_bool("pad_enable");

    toggle_field("pad_wall_thickness", pad_en);
    toggle_field("pad_wall_height", pad_en);
    toggle_field("pad_brim_size", pad_en);
    toggle_field("pad_max_merge_distance", pad_en);
 // toggle_field("pad_edge_radius", supports_en);
    toggle_field("pad_wall_slope", pad_en);
    toggle_field("pad_around_object", pad_en);
    toggle_field("pad_around_object_everywhere", pad_en);

    bool zero_elev = config->opt_bool("pad_around_object") && pad_en;

    toggle_field("support_object_elevation", supports_en && is_default_tree && !zero_elev);
    toggle_field("branchingsupport_object_elevation", supports_en && is_branching_tree && !zero_elev);
    toggle_field("pad_object_gap", zero_elev);
    toggle_field("pad_around_object_everywhere", zero_elev);
    toggle_field("pad_object_connector_stride", zero_elev);
    toggle_field("pad_object_connector_width", zero_elev);
    toggle_field("pad_object_connector_penetration", zero_elev);

    this->apply_plugin_gui_rules(config);
}


} // GUI
} // Slic3r
