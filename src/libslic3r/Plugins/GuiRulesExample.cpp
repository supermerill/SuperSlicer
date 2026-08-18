///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GuiRulesExample.hpp"

#include <cstdint>
#include <iterator>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/FFFPrintConfig.hpp"

namespace slic3r_api { namespace GuiRulesExamplePlugin {

namespace {

const char *k_gui_rules_example_id = "gui_rules_example";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "plugin_gui_rule_test_bool_true_condition", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_bool_true_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_bool_false_condition", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_bool_false_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_value_nonzero_condition", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_value_nonzero_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_option_enabled_condition", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_option_enabled_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_option_disabled_condition", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_option_disabled_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_int_equals_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_int_not_equals_target", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_thin_walls", RAW_CO_BOOL, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "plugin_gui_rule_test_thin_walls_min_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "perimeter_generator", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
constexpr size_t k_defined_config_key_count = 14;

void create_rule_test_option(orchestrator_handle *orchestrator,
                             const char *key,
                             raw_config_option_type type,
                             const char *label,
                             const char *default_value,
                             bool can_be_disabled = false)
{
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = key;
    def.type = type;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = label;
    def.full_label = label;
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_NONE;
    def.tooltip = "Debug-only setting used to exercise plugin GUI activation rules.";
    def.mode = RAW_CONFIG_OPTION_MODE_SIM_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.can_be_disabled = can_be_disabled ? 1 : 0;
    def.default_serialized_value = default_value;
    orchestrator_create_option_def(orchestrator, &def);
}

void add_enable_rule(orchestrator_handle *orchestrator,
                     const char *target_key,
                     raw_gui_rule_condition condition,
                     const char *condition_key,
                     int32_t condition_int_value = 0)
{
    raw_gui_rule rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE;
    rule.condition = condition;
    rule.condition_key = condition_key;
    rule.target_key = target_key;
    rule.condition_int_value = condition_int_value;
    orchestrator_add_gui_rule(orchestrator, &rule);
}

} // namespace

GuiRulesExample &GuiRulesExample::instance(orchestrator_handle *orch)
{
    static GuiRulesExample s_instance(orch);
    return s_instance;
}

const char *GuiRulesExample::id_impl() const noexcept
{
    return k_gui_rules_example_id;
}

const char *GuiRulesExample::name_impl() const noexcept
{
    return "GUI rules example";
}

const char *GuiRulesExample::description_impl() const noexcept
{
    return "Example plugin that demonstrates plugin-defined GUI activation rules.";
}

slicing_step_t GuiRulesExample::step_impl() const noexcept
{
    return STEP_NONE;
}

const char *const *GuiRulesExample::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t GuiRulesExample::priority_impl() const noexcept
{
    return 0;
}

int32_t GuiRulesExample::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (size_t idx = 0; idx < std::size(k_used_config_keys); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(std::size(k_used_config_keys));
}

int32_t GuiRulesExample::defined_config_keys(const char **keys) const noexcept
{
    if (keys != nullptr)
        for (size_t idx = 0; idx < k_defined_config_key_count; ++idx)
            keys[idx] = k_used_config_keys[idx].key;
    return int32_t(k_defined_config_key_count);
}

const char *GuiRulesExample::print_ui_fragment() noexcept
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:insert$afterline$Overhangs cut:Plugin GUI rule bool true\n"
           "setting:plugin_gui_rule_test_bool_true_condition\n"
           "setting:plugin_gui_rule_test_bool_true_target\n"
           "end_line\n"
           "line:Plugin GUI rule bool false\n"
           "setting:plugin_gui_rule_test_bool_false_condition\n"
           "setting:plugin_gui_rule_test_bool_false_target\n"
           "end_line\n"
           "line:Plugin GUI rule value non zero\n"
           "setting:plugin_gui_rule_test_value_nonzero_condition\n"
           "setting:plugin_gui_rule_test_value_nonzero_target\n"
           "end_line\n"
           "line:Plugin GUI rule option enabled\n"
           "setting:plugin_gui_rule_test_option_enabled_condition\n"
           "setting:plugin_gui_rule_test_option_enabled_target\n"
           "end_line\n"
           "line:Plugin GUI rule option disabled\n"
           "setting:plugin_gui_rule_test_option_disabled_condition\n"
           "setting:plugin_gui_rule_test_option_disabled_target\n"
           "end_line\n"
           "line:Plugin GUI rule int equals\n"
           "setting:plugin_gui_rule_test_int_equals_target\n"
           "setting:plugin_gui_rule_test_int_not_equals_target\n"
           "end_line\n"
           "line:Plugin GUI rule thin walls copy\n"
           "setting:plugin_gui_rule_test_thin_walls\n"
           "setting:sidetext_width$5:plugin_gui_rule_test_thin_walls_min_width\n"
           "end_line\n";
}

void GuiRulesExample::inilialize_impl(storage_handle *) const
{
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_bool_true_condition", RAW_CO_BOOL, "Test bool true condition", "1");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_bool_true_target", RAW_CO_BOOL, "Enabled by bool true", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_bool_false_condition", RAW_CO_BOOL, "Test bool false condition", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_bool_false_target", RAW_CO_BOOL, "Enabled by bool false", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_value_nonzero_condition", RAW_CO_FLOAT, "Test non-zero condition", "1");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_value_nonzero_target", RAW_CO_BOOL, "Enabled by non-zero", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_option_enabled_condition", RAW_CO_FLOAT, "Test option enabled condition", "1", true);
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_option_enabled_target", RAW_CO_BOOL, "Enabled by enabled option", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_option_disabled_condition", RAW_CO_FLOAT, "Test option disabled condition", "!1", true);
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_option_disabled_target", RAW_CO_BOOL, "Enabled by disabled option", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_int_equals_target", RAW_CO_BOOL, "Enabled when perimeter generator is Classic", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_int_not_equals_target", RAW_CO_BOOL, "Enabled when perimeter generator is not Classic", "0");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_thin_walls", RAW_CO_BOOL, "Thin walls copy", "1");
    create_rule_test_option(m_orchestrator, "plugin_gui_rule_test_thin_walls_min_width", RAW_CO_FLOAT_OR_PERCENT, "Thin walls min width copy", "33%");

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_gui_rules_example_id,
                                 GuiRulesExample::print_ui_fragment(),
                                 0);

    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_bool_true_target", RAW_GUI_RULE_CONDITION_BOOL_TRUE, "plugin_gui_rule_test_bool_true_condition");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_bool_false_target", RAW_GUI_RULE_CONDITION_BOOL_FALSE, "plugin_gui_rule_test_bool_false_condition");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_value_nonzero_target", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "plugin_gui_rule_test_value_nonzero_condition");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_option_enabled_target", RAW_GUI_RULE_CONDITION_OPTION_ENABLED, "plugin_gui_rule_test_option_enabled_condition");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_option_disabled_target", RAW_GUI_RULE_CONDITION_OPTION_DISABLED, "plugin_gui_rule_test_option_disabled_condition");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_int_equals_target", RAW_GUI_RULE_CONDITION_INT_EQUALS, "perimeter_generator", static_cast<int32_t>(Slic3r::PerimeterGeneratorType::Classic));
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_int_not_equals_target", RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS, "perimeter_generator", static_cast<int32_t>(Slic3r::PerimeterGeneratorType::Classic));

    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_thin_walls", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "perimeters");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_thin_walls", RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS, "perimeter_generator", static_cast<int32_t>(Slic3r::PerimeterGeneratorType::Arachne));
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_thin_walls_min_width", RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO, "perimeters");
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_thin_walls_min_width", RAW_GUI_RULE_CONDITION_INT_NOT_EQUALS, "perimeter_generator", static_cast<int32_t>(Slic3r::PerimeterGeneratorType::Arachne));
    add_enable_rule(m_orchestrator, "plugin_gui_rule_test_thin_walls_min_width", RAW_GUI_RULE_CONDITION_BOOL_TRUE, "plugin_gui_rule_test_thin_walls");
}

void GuiRulesExample::run_impl(const plugin_run_context *) const
{
}

void register_gui_rules_example_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, GuiRulesExample::instance(orch).c_instance());
}

}} // namespace slic3r_api::GuiRulesExamplePlugin

#ifdef GUI_RULES_EXAMPLE_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::GuiRulesExamplePlugin::register_gui_rules_example_plugin(orch);
}
#endif // GUI_RULES_EXAMPLE_PLUGIN_DLL
