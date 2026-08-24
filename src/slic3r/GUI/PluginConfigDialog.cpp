///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Plugin activation is intentionally separate from package installation and from
the per-print selection of one implementation inside an exclusive group. This
dialog builds a stable catalog when it opens, then projects that catalog through
navigation, search and filters. Toggle state therefore survives every rebuild
of the visible list and is written to activated.ini only when the user saves.
*/

#include "PluginConfigDialog.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/dcclient.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Plugins/PluginActivationConfig.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Utils.hpp"

#include "format.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/Process.hpp"

namespace Slic3r::GUI {

namespace {

enum class PluginPhase {
    Slicing,
    Perimeters,
    SurfacesInfill,
    SupportAdhesion,
    Output
};

enum class PluginNavigationKind {
    All,
    Phase,
    Step,
    Extensions,
    Problems
};

struct NavigationScope {
    PluginNavigationKind kind { PluginNavigationKind::All };
    PluginPhase phase { PluginPhase::Slicing };
    slicing_step_t step { STEP_NONE };
};

struct PluginSettingEntry {
    std::string key;
    raw_config_option_type type { RAW_CO_NONE };
};

class PluginSettingKeyText final : public wxStaticText
{
public:
    PluginSettingKeyText(wxWindow *parent, const wxString &label);
    ~PluginSettingKeyText() override = default;

protected:
    wxSize DoGetBestClientSize() const override;
};

class PluginSettingsPanel final : public wxPanel
{
public:
    explicit PluginSettingsPanel(wxWindow *parent);
    ~PluginSettingsPanel() override = default;

    void set_settings(const std::vector<PluginSettingEntry> &settings,
                      const wxString &empty_message);

protected:
    wxSize DoGetBestClientSize() const override;

private:
    wxBoxSizer *m_rows_sizer { nullptr };
    int m_rows_height { 0 };
};

class PluginDetailsPanel final : public wxScrolledWindow
{
public:
    PluginDetailsPanel(wxWindow *parent, const wxSize &initial_size, int vertical_scroll_rate);
    ~PluginDetailsPanel() override = default;

    void fit_contents();

private:
    void on_size(wxSizeEvent &event);
    void fit_after_size();

    bool m_fitting_contents { false };
    bool m_fit_scheduled { false };
};

struct PluginCatalogEntry {
    std::string id;
    std::string package_id;
    std::string exclusive_group;
    wxString name;
    wxString description;
    wxString step_label;
    wxString exclusive_group_label;
    wxString exclusive_group_tooltip;
    wxString diagnostic;
    std::vector<std::string> dependencies;
    std::vector<PluginSettingEntry> defined_settings;
    std::vector<PluginSettingEntry> other_used_settings;
    slicing_step_t step { STEP_NONE };
    int priority { 0 };
    size_t registration_index { 0 };
    bool loaded { false };
    bool external { false };
    bool active { false };
    bool modifiable { false };
    bool preserve_unavailable_activation { false };
};

class NavigationItemData final : public wxTreeItemData
{
public:
    explicit NavigationItemData(NavigationScope scope) : m_scope(scope) {}
    const NavigationScope &scope() const { return m_scope; }

private:
    NavigationScope m_scope;
};

struct NavigationRecord {
    wxTreeItemId item;
    NavigationScope scope;
    wxString label;
};

struct PluginListNode {
    enum class Kind {
        Container,
        Plugin
    };

    Kind kind { Kind::Container };
    PluginListNode *parent { nullptr };
    PluginCatalogEntry *entry { nullptr };
    wxString label;
    wxString status;
    wxString detail;
    std::vector<std::unique_ptr<PluginListNode>> children;
};

class PrimarySecondaryTextRenderer final : public wxDataViewCustomRenderer
{
public:
    PrimarySecondaryTextRenderer();
    ~PrimarySecondaryTextRenderer() override = default;

    bool SetValue(const wxVariant &value) override;
    bool GetValue(wxVariant &value) const override;
    bool Render(wxRect cell, wxDC *dc, int state) override;
    wxSize GetSize() const override;

private:
    wxString m_value;
    wxString m_primary;
    wxString m_secondary;
};

class PluginListModel final : public wxDataViewModel
{
public:
    enum Column {
        Active,
        Name,
        Status,
        Count
    };

    PluginListModel() = default;
    ~PluginListModel() override = default;

    unsigned int GetColumnCount() const override;
    wxString GetColumnType(unsigned int column) const override;
    void GetValue(wxVariant &value, const wxDataViewItem &item, unsigned int column) const override;
    bool SetValue(const wxVariant &value, const wxDataViewItem &item, unsigned int column) override;
    wxDataViewItem GetParent(const wxDataViewItem &item) const override;
    unsigned int GetChildren(const wxDataViewItem &parent, wxDataViewItemArray &children) const override;
    bool IsContainer(const wxDataViewItem &item) const override;
    bool HasContainerColumns(const wxDataViewItem &) const override;
    bool IsEnabled(const wxDataViewItem &item, unsigned int column) const override;
    bool GetAttr(const wxDataViewItem &item, unsigned int column, wxDataViewItemAttr &attr) const override;

    void clear();
    void notify_rebuilt();
    PluginListNode *add_container(PluginListNode *parent,
                                  const wxString &label,
                                  const wxString &status,
                                  const wxString &detail);
    PluginListNode *add_plugin(PluginListNode *parent, PluginCatalogEntry &entry);
    PluginListNode *node(const wxDataViewItem &item) const;
    wxDataViewItem item_for_entry(const PluginCatalogEntry *entry) const;
    const std::vector<wxDataViewItem> &container_items() const;

private:
    std::vector<std::unique_ptr<PluginListNode>> m_roots;
    std::map<const PluginCatalogEntry *, PluginListNode *> m_entry_nodes;
    std::vector<wxDataViewItem> m_container_items;
};

struct DetailWidgets {
    wxStaticText *title { nullptr };
    wxStaticText *description { nullptr };
    wxStaticText *status { nullptr };
    wxStaticText *id { nullptr };
    wxStaticText *package { nullptr };
    wxStaticText *step { nullptr };
    wxStaticText *priority { nullptr };
    wxStaticText *group { nullptr };
    wxStaticText *dependencies { nullptr };
    PluginSettingsPanel *defined_settings { nullptr };
    PluginSettingsPanel *other_used_settings { nullptr };
    wxStaticText *diagnostic_label { nullptr };
    wxStaticText *diagnostic { nullptr };
};

struct GroupBucket {
    std::string id;
    wxString label;
    wxString tooltip;
    std::vector<PluginCatalogEntry *> entries;
    int minimum_priority { std::numeric_limits<int>::max() };
    size_t registration_index { std::numeric_limits<size_t>::max() };
};

struct StepBucket {
    slicing_step_t step { STEP_NONE };
    bool problems { false };
    wxString label;
    std::vector<PluginCatalogEntry *> entries;
};

PluginActivationConfig read_plugin_configuration();
wxString unavailable_plugin_tooltip(const std::string &plugin_id,
                                    const PluginActivationConfig &config);
wxString plugin_package_load_diagnostic(const PluginPackageLoadReport &report);
wxString step_name(slicing_step_t step);
wxString phase_name(PluginPhase phase);
std::optional<PluginPhase> phase_for_step(slicing_step_t step);
std::optional<size_t> pipeline_step_index(slicing_step_t step);
bool is_pipeline_step(slicing_step_t step);
bool is_extension_step(slicing_step_t step);
bool navigation_matches(const PluginCatalogEntry &entry, const NavigationScope &scope);
wxString plugin_status(const PluginCatalogEntry &entry);
wxString join_dependencies(const std::vector<std::string> &dependencies);
wxString config_option_type_name(raw_config_option_type type);
wxString searchable_plugin_text(const PluginCatalogEntry &entry);
bool plugin_less(const PluginCatalogEntry *left, const PluginCatalogEntry *right);
bool step_bucket_less(const StepBucket &left, const StepBucket &right);
void set_detail_value(wxStaticText *widget, const wxString &value);
wxString primary_secondary_text(const wxString &primary, const wxString &secondary);

} // namespace

class PluginConfigDialogState
{
public:
    PluginActivationConfig activation_config;
    std::set<std::string> original_active_plugin_ids;
    std::vector<PluginCatalogEntry> entries;
    std::vector<NavigationRecord> navigation_records;

    wxSearchCtrl *search { nullptr };
    wxChoice *state_filter { nullptr };
    wxChoice *origin_filter { nullptr };
    wxTreeCtrl *navigation { nullptr };
    wxDataViewCtrl *plugin_list { nullptr };
    PluginListModel *plugin_list_model { nullptr };
    PluginDetailsPanel *details_panel { nullptr };
    DetailWidgets details;
};

namespace {

PluginActivationConfig read_plugin_configuration()
{
    PluginActivationConfig config;
    if (!has_data_dir())
        return config;

    std::string error_message;
    const boost::filesystem::path config_path = plugin_activation_config_path(boost::filesystem::path(data_dir()));
    if (!read_plugin_activation_config(config_path, config, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return {};
    }
    return config;
}

// Preserve every loader detail in the activation dialog. A package may fail
// for several plugin ids, so reducing the report to its first issue would hide
// the information needed to repair a partially loaded package.
wxString plugin_package_load_diagnostic(const PluginPackageLoadReport &report)
{
    wxString diagnostic = format_wxstr(_L("Package: %1%\nPath: %2%"),
                                       from_u8(report.package_id), from_u8(report.package_path));
    for (const PluginPackageLoadIssue &issue : report.issues) {
        diagnostic += "\n\n";
        if (!issue.plugin_id.empty())
            diagnostic += format_wxstr(_L("Plugin: %1%\n"), from_u8(issue.plugin_id));
        diagnostic += from_u8(issue.detail);
        if (issue.plugin_abi != 0 || issue.host_abi != 0)
            diagnostic += format_wxstr(_L("\nPlugin API: %1%; host API: %2%."),
                                       issue.plugin_abi, issue.host_abi);
        if (issue.system_error != 0)
            diagnostic += format_wxstr(_L("\nSystem error code: %1%."), issue.system_error);
    }
    return diagnostic;
}

// Explain why an activated id has no registered Plugin instance. Package
// installation and loading are separate from id activation, so the message
// distinguishes a package that is absent, broken, or incomplete.
wxString unavailable_plugin_tooltip(const std::string &plugin_id,
                                    const PluginActivationConfig &config)
{
    const std::map<std::string, std::string>::const_iterator provider =
        config.plugin_packages.find(plugin_id);
    if (provider == config.plugin_packages.end()) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled in the configuration file, but it was not loaded and its package is unknown. "
               "Saving this dialog removes the orphaned id from the active plugin list."),
            from_u8(plugin_id));
    }

    const std::string &package_id = provider->second;
    const PluginPackageLoadReport *report =
        Orchestrator::instance().plugin_package_load_report(package_id);
    if (report != nullptr && !report->issues.empty()) {
        return plugin_package_load_diagnostic(*report) + "\n\n" +
               _L("The activation request is kept when this dialog is saved. Use Plugin updates to repair "
                  "the package or choose another version.");
    }
    if (report != nullptr && report->state == PluginPackageLoadState::Loaded) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled, but the loaded package '%2%' did not register this id. "
               "Disable the id or install a package version that provides it. "
               "Saving this dialog keeps the activation request."),
            from_u8(plugin_id), from_u8(package_id));
    }
    if (config.installed.find(package_id) == config.installed.end()) {
        return format_wxstr(
            _L("Plugin '%1%' is enabled, but package '%2%' is not installed. "
               "Install the package from Plugin updates or disable this id. "
               "Saving this dialog keeps the activation request."),
            from_u8(plugin_id), from_u8(package_id));
    }
    return format_wxstr(
        _L("Plugin '%1%' is enabled and package '%2%' is selected as installed, but it was not loaded. "
           "Open Plugin updates to repair the package. Saving this dialog keeps the activation request."),
        from_u8(plugin_id), from_u8(package_id));
}

wxString step_name(slicing_step_t step)
{
    switch (step) {
    case STEP_LAYER_HEIGHT:              return _L("Layer height");
    case STEP_SLICING:                   return _L("Slicing");
    case STEP_POST_SLICING:              return _L("Post-slicing");
    case STEP_ALERT_SUPPORTS_NEEDED:     return _L("Support alert");
    case STEP_PRE_PERIMETER:             return _L("Perimeter preparation");
    case STEP_PERIMETER:                 return _L("Perimeter generation");
    case STEP_POST_PERIMETER:            return _L("Perimeter post-processing");
    case STEP_SURFACE_GENERATION:        return _L("Surface generation");
    case STEP_SKIRT_BRIM:                return _L("Skirt and brim");
    case STEP_PRE_INFILL:                return _L("Infill preparation");
    case STEP_INFILL_GROUP:              return _L("Infill grouping");
    case STEP_INFILL:                    return _L("Infill generation");
    case STEP_POST_INFILL:               return _L("Infill post-processing");
    case STEP_SUPPORT_DEMAND:            return _L("Support demand");
    case STEP_SUPPORT:                   return _L("Support generation");
    case STEP_PRE_GCODE:                 return _L("G-code preparation");
    case STEP_CHECK_CONFLICT:            return _L("Conflict detection");
    case STEP_ORDERING:                  return _L("Extrusion ordering");
    case STEP_WIPETOWER:                 return _L("Wipe tower");
    case STEP_SUPPORT_SPOT:              return _L("Support spot detection");
    case STEP_LAYER_EXTRUSION_EDIT:      return _L("Layer extrusion editing");
    case STEP_LAYER_STICHING:            return _L("Layer stitching");
    case STEP_EXTRUSION_EDIT:            return _L("Extrusion editing");
    case STEP_EXTRUSION_SIMPLIFICATION:  return _L("Extrusion simplification");
    case STEP_GCODE:                     return _L("G-code generation");
    case GCODE_FIRMWARE:                 return _L("G-code firmware");
    case INFILL_PATTERN:                 return _L("Infill patterns");
    case INFILL_SURFACE_RECIPE_MODIFIER: return _L("Infill surface recipes");
    case BRIDGE_DETECTOR:                return _L("Bridge detectors");
    case PERIMETER_GENERATION_MODULE:    return _L("Perimeter generation modules");
    case STEP_NONE:                      return _L("No pipeline step");
    case STEP_ANY:                       return _L("Multiple pipeline steps");
    default:                             return wxString::Format("STEP_%u", unsigned(step));
    }
}

wxString phase_name(PluginPhase phase)
{
    switch (phase) {
    case PluginPhase::Slicing:          return _L("Slicing");
    case PluginPhase::Perimeters:       return _L("Perimeters");
    case PluginPhase::SurfacesInfill:   return _L("Surfaces & infill");
    case PluginPhase::SupportAdhesion:  return _L("Support & adhesion");
    case PluginPhase::Output:           return _L("Output");
    }
    return wxEmptyString;
}

std::optional<PluginPhase> phase_for_step(slicing_step_t step)
{
    switch (step) {
    case STEP_LAYER_HEIGHT:
    case STEP_SLICING:
    case STEP_POST_SLICING:
        return PluginPhase::Slicing;
    case STEP_PRE_PERIMETER:
    case STEP_PERIMETER:
    case STEP_POST_PERIMETER:
        return PluginPhase::Perimeters;
    case STEP_SURFACE_GENERATION:
    case STEP_PRE_INFILL:
    case STEP_INFILL_GROUP:
    case STEP_INFILL:
    case STEP_POST_INFILL:
        return PluginPhase::SurfacesInfill;
    case STEP_SUPPORT_DEMAND:
    case STEP_SUPPORT:
    case STEP_SKIRT_BRIM:
        return PluginPhase::SupportAdhesion;
    case STEP_PRE_GCODE:
    case STEP_ORDERING:
    case STEP_WIPETOWER:
    case STEP_SUPPORT_SPOT:
    case STEP_LAYER_EXTRUSION_EDIT:
    case STEP_LAYER_STICHING:
    case STEP_EXTRUSION_EDIT:
    case STEP_EXTRUSION_SIMPLIFICATION:
    case STEP_GCODE:
        return PluginPhase::Output;
    default:
        return std::nullopt;
    }
}

std::optional<size_t> pipeline_step_index(slicing_step_t step)
{
    const std::vector<slicing_step_t> &order = Steps::execution_order();
    const std::vector<slicing_step_t>::const_iterator found = std::find(order.begin(), order.end(), step);
    if (found == order.end())
        return std::nullopt;
    return size_t(std::distance(order.begin(), found));
}

bool is_pipeline_step(slicing_step_t step)
{
    return pipeline_step_index(step).has_value();
}

bool is_extension_step(slicing_step_t step)
{
    return !is_pipeline_step(step) || !phase_for_step(step).has_value();
}

bool navigation_matches(const PluginCatalogEntry &entry, const NavigationScope &scope)
{
    switch (scope.kind) {
    case PluginNavigationKind::All:
        return true;
    case PluginNavigationKind::Phase: {
        const std::optional<PluginPhase> phase = entry.loaded ? phase_for_step(entry.step) : std::nullopt;
        return phase.has_value() && *phase == scope.phase;
    }
    case PluginNavigationKind::Step:
        return entry.loaded && entry.step == scope.step;
    case PluginNavigationKind::Extensions:
        return entry.loaded && is_extension_step(entry.step);
    case PluginNavigationKind::Problems:
        return !entry.loaded;
    }
    return false;
}

wxString plugin_status(const PluginCatalogEntry &entry)
{
    if (!entry.loaded)
        return entry.preserve_unavailable_activation ? _L("Not loaded - kept") : _L("Not loaded - orphaned");
    return entry.active ? _L("Active") : _L("Inactive");
}

wxString join_dependencies(const std::vector<std::string> &dependencies)
{
    if (dependencies.empty())
        return _L("None");

    wxString result;
    for (const std::string &dependency : dependencies) {
        if (!result.empty())
            result += ", ";
        result += from_u8(dependency);
    }
    return result;
}

wxString config_option_type_name(raw_config_option_type type)
{
    switch (type) {
    case RAW_CO_BOOL:                    return _L("Boolean");
    case RAW_CO_INT:                     return _L("Integer");
    case RAW_CO_FLOAT:                   return _L("Float");
    case RAW_CO_PERCENT:                 return _L("Percent");
    case RAW_CO_FLOAT_OR_PERCENT:        return _L("Float or percent");
    case RAW_CO_STRING:                  return _L("String");
    case RAW_CO_POINT:                   return _L("Point");
    case RAW_CO_ENUM:                    return _L("Enum");
    case RAW_CO_GRAPH:                   return _L("Graph");
    case RAW_CO_VECTOR_BOOL:             return _L("Boolean list");
    case RAW_CO_VECTOR_INT:              return _L("Integer list");
    case RAW_CO_VECTOR_FLOAT:            return _L("Float list");
    case RAW_CO_VECTOR_PERCENT:          return _L("Percent list");
    case RAW_CO_VECTOR_FLOAT_OR_PERCENT: return _L("Float or percent list");
    case RAW_CO_VECTOR_STRING:           return _L("String list");
    case RAW_CO_VECTOR_POINT:            return _L("Point list");
    case RAW_CO_VECTOR_ENUM:             return _L("Enum list");
    case RAW_CO_VECTOR_GRAPH:            return _L("Graph list");
    case RAW_CO_NONE:                    return _L("Unknown");
    }
    return _L("Unknown");
}

wxString searchable_plugin_text(const PluginCatalogEntry &entry)
{
    wxString searchable = entry.name + " " + from_u8(entry.id) + " " + entry.description + " " +
        entry.step_label + " " + entry.exclusive_group_label + " " + from_u8(entry.exclusive_group) + " " +
        from_u8(entry.package_id);
    for (const PluginSettingEntry &setting : entry.defined_settings)
        searchable += " " + from_u8(setting.key);
    for (const PluginSettingEntry &setting : entry.other_used_settings)
        searchable += " " + from_u8(setting.key);
    return searchable.Lower();
}

bool plugin_less(const PluginCatalogEntry *left, const PluginCatalogEntry *right)
{
    if (left->priority != right->priority)
        return left->priority < right->priority;
    return left->registration_index < right->registration_index;
}

bool step_bucket_less(const StepBucket &left, const StepBucket &right)
{
    if (left.problems != right.problems)
        return !left.problems;

    const std::optional<size_t> left_index = pipeline_step_index(left.step);
    const std::optional<size_t> right_index = pipeline_step_index(right.step);
    if (left_index.has_value() != right_index.has_value())
        return left_index.has_value();
    if (left_index.has_value() && right_index.has_value())
        return *left_index < *right_index;
    return left.step < right.step;
}

void set_detail_value(wxStaticText *widget, const wxString &value)
{
    if (widget == nullptr)
        return;
    widget->SetLabel(value);
    widget->SetToolTip(value);
}

PluginSettingKeyText::PluginSettingKeyText(wxWindow *parent, const wxString &label)
    : wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize,
                   wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE)
{
}

wxSize PluginSettingKeyText::DoGetBestClientSize() const
{
    wxSize best_size = wxStaticText::DoGetBestClientSize();
    best_size.SetWidth(0);
    return best_size;
}

PluginSettingsPanel::PluginSettingsPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
    , m_rows_sizer(new wxBoxSizer(wxVERTICAL))
{
    SetSizer(m_rows_sizer);
}

wxSize PluginSettingsPanel::DoGetBestClientSize() const
{
    return wxSize(0, m_rows_height);
}

void PluginSettingsPanel::set_settings(const std::vector<PluginSettingEntry> &settings,
                                       const wxString &empty_message)
{
    // Rows are rebuilt because settings are metadata of the selected plugin.
    // Their minimum width stays at zero so long keys cannot widen the details
    // pane or create a horizontal scrollbar on its scrolling parent.
    m_rows_sizer->Clear(true);
    const size_t row_count = settings.empty() ? 1 : settings.size();
    for (size_t row_idx = 0; row_idx < row_count; ++row_idx) {
        const wxString key = settings.empty() ? empty_message : from_u8(settings[row_idx].key);
        const wxString type = settings.empty() ? wxString() : config_option_type_name(settings[row_idx].type);

        wxPanel *row = new wxPanel(this);
        wxBoxSizer *row_sizer = new wxBoxSizer(wxHORIZONTAL);
        PluginSettingKeyText *key_text = new PluginSettingKeyText(row, key);
        key_text->SetToolTip(key);
        row_sizer->Add(key_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));

        // The type keeps its natural compact width while the key receives all
        // remaining space. Right padding keeps text away from the panel border.
        wxStaticText *type_text = new wxStaticText(
            row, wxID_ANY, type, wxDefaultPosition, wxDefaultSize,
            wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
        row_sizer->Add(type_text, 0,
                       wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM,
                       FromDIP(6));
        row->SetSizer(row_sizer);
        row->SetMinSize(wxSize(0, std::max(FromDIP(24), key_text->GetBestSize().GetHeight() + FromDIP(8))));
        m_rows_sizer->Add(row, 0, wxEXPAND);

        // Separators preserve the scan-friendly table structure without
        // introducing a child control that owns its own scrolling viewport.
        if (row_idx + 1 < row_count)
            m_rows_sizer->Add(new wxStaticLine(this), 0, wxEXPAND);
    }

    // Only the vertical minimum is fixed. The containing sizer remains free to
    // assign the complete width of the details pane to every row.
    m_rows_height = m_rows_sizer->GetMinSize().GetHeight() + FromDIP(2);
    SetMinSize(wxSize(0, m_rows_height));
    InvalidateBestSize();
    Layout();
}

PluginDetailsPanel::PluginDetailsPanel(wxWindow *parent, const wxSize &initial_size,
                                       int vertical_scroll_rate)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, initial_size,
                       wxVSCROLL | wxBORDER_SIMPLE)
{
    SetScrollRate(0, vertical_scroll_rate);
    Bind(wxEVT_SIZE, &PluginDetailsPanel::on_size, this);
}

void PluginDetailsPanel::fit_contents()
{
    if (m_fitting_contents)
        return;

    // FitInside computes the complete content height. Its natural width may be
    // larger than the viewport because of long metadata, but this panel owns
    // vertical scrolling only, so constrain the virtual width afterwards.
    m_fitting_contents = true;
    Layout();
    FitInside();
    wxSize virtual_size = GetVirtualSize();
    const int scrollbar_width = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
    const int viewport_width = std::max(0, GetSize().GetWidth() - scrollbar_width - FromDIP(2));
    if (viewport_width > 0 && virtual_size.GetWidth() != viewport_width) {
        virtual_size.SetWidth(viewport_width);
        SetVirtualSize(virtual_size);
        Layout();
    }
    m_fitting_contents = false;
}

void PluginDetailsPanel::on_size(wxSizeEvent &event)
{
    event.Skip();
    if (!m_fit_scheduled) {
        m_fit_scheduled = true;
        CallAfter(&PluginDetailsPanel::fit_after_size);
    }
}

void PluginDetailsPanel::fit_after_size()
{
    m_fit_scheduled = false;
    fit_contents();
}

wxString primary_secondary_text(const wxString &primary, const wxString &secondary)
{
    return primary + wxString(wxChar(0x1f)) + secondary;
}

PrimarySecondaryTextRenderer::PrimarySecondaryTextRenderer()
    : wxDataViewCustomRenderer("string", wxDATAVIEW_CELL_INERT, wxDVR_DEFAULT_ALIGNMENT)
{
}

bool PrimarySecondaryTextRenderer::SetValue(const wxVariant &value)
{
    m_value = value.GetString();
    const int separator = m_value.Find(wxChar(0x1f));
    if (separator == wxNOT_FOUND) {
        m_primary = m_value;
        m_secondary.clear();
    } else {
        m_primary = m_value.Left(size_t(separator));
        m_secondary = m_value.Mid(size_t(separator + 1));
    }
    return true;
}

bool PrimarySecondaryTextRenderer::GetValue(wxVariant &value) const
{
    value = m_value;
    return true;
}

bool PrimarySecondaryTextRenderer::Render(wxRect cell, wxDC *dc, int state)
{
    const int gap = GetView()->FromDIP(12);
    const int right_padding = GetView()->FromDIP(6);
    const wxSize secondary_size = dc->GetTextExtent(m_secondary);
    const int secondary_width = m_secondary.empty() ? 0 : secondary_size.GetWidth();
    wxRect primary_cell = cell;
    primary_cell.SetWidth(std::max(0, cell.GetWidth() - secondary_width - gap - right_padding));

#ifdef _WIN32
    // Keep the established dark-mode behavior used by the other custom data
    // view renderers in this application.
    const int render_state = state & wxDATAVIEW_CELL_SELECTED ? 0 : state;
#else
    const int render_state = state;
#endif
    RenderText(m_primary, 0, primary_cell, dc, render_state);
    if (!m_secondary.empty()) {
        wxRect secondary_cell(
            cell.GetRight() - secondary_width - right_padding + 1,
            cell.GetTop(), secondary_width, cell.GetHeight());
        RenderText(m_secondary, 0, secondary_cell, dc, render_state);
    }
    return true;
}

wxSize PrimarySecondaryTextRenderer::GetSize() const
{
    wxDataViewCtrl *view = GetView();
    if (view == nullptr)
        return wxSize(80, 20);

    wxClientDC dc(view);
    if (GetAttr().HasFont())
        dc.SetFont(GetAttr().GetEffectiveFont(view->GetFont()));
    else
        dc.SetFont(view->GetFont());

    const wxSize primary_size = dc.GetTextExtent(m_primary);
    const wxSize secondary_size = dc.GetTextExtent(m_secondary);

    // A negative width asks wxDataViewCustomRenderer to pass the complete cell
    // rectangle to Render(), where the secondary text can remain right-aligned.
    return wxSize(-1, std::max(primary_size.GetHeight(), secondary_size.GetHeight()));
}

unsigned int PluginListModel::GetColumnCount() const
{
    return Column::Count;
}

wxString PluginListModel::GetColumnType(unsigned int column) const
{
    return column == Column::Active ? "bool" : "string";
}

void PluginListModel::GetValue(wxVariant &value, const wxDataViewItem &item, unsigned int column) const
{
    PluginListNode *list_node = node(item);
    assert(list_node != nullptr);
    if (list_node == nullptr)
        return;

    if (column == Column::Active)
        value = list_node->entry != nullptr && list_node->entry->active;
    else if (column == Column::Name)
        value = primary_secondary_text(
            list_node->entry != nullptr ? list_node->entry->name : list_node->label,
            list_node->entry != nullptr ? plugin_status(*list_node->entry) : list_node->status);
    else if (column == Column::Status)
        value = list_node->entry != nullptr ? plugin_status(*list_node->entry) : list_node->status;
}

bool PluginListModel::SetValue(const wxVariant &value, const wxDataViewItem &item, unsigned int column)
{
    PluginListNode *list_node = node(item);
    if (column != Column::Active || list_node == nullptr || list_node->entry == nullptr ||
        !list_node->entry->modifiable)
        return false;

    list_node->entry->active = value.GetBool();
    return true;
}

wxDataViewItem PluginListModel::GetParent(const wxDataViewItem &item) const
{
    PluginListNode *list_node = node(item);
    return list_node != nullptr && list_node->parent != nullptr ?
        wxDataViewItem(list_node->parent) : wxDataViewItem();
}

unsigned int PluginListModel::GetChildren(const wxDataViewItem &parent, wxDataViewItemArray &children) const
{
    if (!parent.IsOk()) {
        for (const std::unique_ptr<PluginListNode> &root : m_roots)
            children.push_back(wxDataViewItem(root.get()));
        return unsigned(m_roots.size());
    }

    PluginListNode *list_node = node(parent);
    if (list_node == nullptr)
        return 0;
    for (const std::unique_ptr<PluginListNode> &child : list_node->children)
        children.push_back(wxDataViewItem(child.get()));
    return unsigned(list_node->children.size());
}

bool PluginListModel::IsContainer(const wxDataViewItem &item) const
{
    return !item.IsOk() || (node(item) != nullptr && node(item)->kind == PluginListNode::Kind::Container);
}

bool PluginListModel::HasContainerColumns(const wxDataViewItem &) const
{
    return true;
}

bool PluginListModel::IsEnabled(const wxDataViewItem &item, unsigned int column) const
{
    PluginListNode *list_node = node(item);
    if (list_node == nullptr)
        return false;
    if (column != Column::Active)
        return true;
    return list_node->entry != nullptr && list_node->entry->modifiable;
}

bool PluginListModel::GetAttr(const wxDataViewItem &item,
                              unsigned int,
                              wxDataViewItemAttr &attr) const
{
    PluginListNode *list_node = node(item);
    if (list_node != nullptr && list_node->kind == PluginListNode::Kind::Container) {
        attr.SetBold(true);
        return true;
    }
    return false;
}

void PluginListModel::clear()
{
    m_roots.clear();
    m_entry_nodes.clear();
    m_container_items.clear();
}

void PluginListModel::notify_rebuilt()
{
    // One reset after the complete hierarchy has been assembled is both less
    // noisy and safer than publishing child nodes before their parents exist.
    Cleared();
}

PluginListNode *PluginListModel::add_container(PluginListNode *parent,
                                               const wxString &label,
                                               const wxString &status,
                                               const wxString &detail)
{
    std::unique_ptr<PluginListNode> created = std::make_unique<PluginListNode>();
    created->kind = PluginListNode::Kind::Container;
    created->parent = parent;
    created->label = label;
    created->status = status;
    created->detail = detail;
    PluginListNode *result = created.get();
    if (parent == nullptr)
        m_roots.push_back(std::move(created));
    else
        parent->children.push_back(std::move(created));
    m_container_items.emplace_back(result);
    return result;
}

PluginListNode *PluginListModel::add_plugin(PluginListNode *parent, PluginCatalogEntry &entry)
{
    assert(parent != nullptr);
    std::unique_ptr<PluginListNode> created = std::make_unique<PluginListNode>();
    created->kind = PluginListNode::Kind::Plugin;
    created->parent = parent;
    created->entry = &entry;
    PluginListNode *result = created.get();
    parent->children.push_back(std::move(created));
    m_entry_nodes[&entry] = result;
    return result;
}

PluginListNode *PluginListModel::node(const wxDataViewItem &item) const
{
    return item.IsOk() ? static_cast<PluginListNode *>(item.GetID()) : nullptr;
}

wxDataViewItem PluginListModel::item_for_entry(const PluginCatalogEntry *entry) const
{
    const std::map<const PluginCatalogEntry *, PluginListNode *>::const_iterator found = m_entry_nodes.find(entry);
    return found == m_entry_nodes.end() ? wxDataViewItem() : wxDataViewItem(found->second);
}

const std::vector<wxDataViewItem> &PluginListModel::container_items() const
{
    return m_container_items;
}

} // namespace

PluginConfigDialog::PluginConfigDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Plugin configuration"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "plugin_config")
    , m_state(std::make_unique<PluginConfigDialogState>())
{
    build_catalog();
    build();
}

PluginConfigDialog::~PluginConfigDialog()
{
    // A native tree control may report a final selection change while its
    // window is being destroyed. The handler captures this dialog and must not
    // run after the dialog state starts being released.
    if (m_state->navigation != nullptr)
        m_state->navigation->SetEvtHandlerEnabled(false);

    // wx destroys child controls after C++ members. Remove every node that
    // points into the catalog while the catalog is still alive.
    if (m_state->plugin_list_model != nullptr) {
        m_state->plugin_list_model->clear();
        m_state->plugin_list_model->notify_rebuilt();
    }
}

void PluginConfigDialog::build_catalog()
{
    m_state->activation_config = read_plugin_configuration();
    for (const std::pair<const std::string, bool> &activation : m_state->activation_config.activated)
        if (activation.second)
            m_state->original_active_plugin_ids.insert(activation.first);

    std::vector<Plugin *> plugins = Orchestrator::instance().registered_plugins();
    m_state->entries.reserve(plugins.size() + m_state->original_active_plugin_ids.size());
    std::set<std::string> loaded_plugin_ids;

    // Loaded plugins contribute their complete metadata. The catalog keeps the
    // registration index as the final stable ordering key when priorities tie.
    for (size_t plugin_idx = 0; plugin_idx < plugins.size(); ++plugin_idx) {
        Plugin *plugin = plugins[plugin_idx];
        if (plugin == nullptr)
            continue;

        PluginCatalogEntry entry;
        entry.id = plugin->get_id();
        entry.name = I18N::translate_in_domain(plugin->get_name(), plugin->get_translation_domain());
        entry.description = I18N::translate_in_domain(plugin->get_description(), plugin->get_translation_domain());
        entry.step = plugin->get_step();
        entry.step_label = step_name(entry.step);
        entry.priority = plugin->get_priority();
        entry.registration_index = plugin_idx;
        entry.exclusive_group = plugin->get_exclusive_group();
        entry.exclusive_group_label =
            I18N::translate_in_domain(plugin->get_exclusive_group_label(), plugin->get_translation_domain());
        entry.exclusive_group_tooltip =
            I18N::translate_in_domain(plugin->get_exclusive_group_tooltip(), plugin->get_translation_domain());
        entry.dependencies = plugin->get_dependencies();
        std::set<std::string> defined_setting_keys;
        for (const Plugin::DefinedConfigKey &defined_key : plugin->get_defined_config_keys()) {
            if (defined_setting_keys.insert(defined_key.key).second)
                entry.defined_settings.push_back({defined_key.key, defined_key.type});
        }
        for (const Plugin::UsedConfigKey &used_key : plugin->get_used_config_keys()) {
            if (defined_setting_keys.find(used_key.key) == defined_setting_keys.end())
                entry.other_used_settings.push_back({used_key.key, used_key.type});
        }
        entry.loaded = true;
        entry.modifiable = true;
        entry.active = Orchestrator::instance().is_plugin_active(plugin) ||
                       m_state->original_active_plugin_ids.find(entry.id) !=
                           m_state->original_active_plugin_ids.end();
        if (!plugin->get_package_root().empty()) {
            entry.package_id = boost::filesystem::path(plugin->get_package_root()).filename().string();
            entry.external = true;
        } else {
            const std::map<std::string, std::string>::const_iterator provider =
                m_state->activation_config.plugin_packages.find(entry.id);
            if (provider != m_state->activation_config.plugin_packages.end()) {
                entry.package_id = provider->second;
                entry.external = true;
            }
        }

        loaded_plugin_ids.insert(entry.id);
        m_state->entries.push_back(std::move(entry));
    }

    // Configured ids with no Plugin object remain visible in Problems. A known
    // package association protects the requested activation while the package
    // is repaired; an unknown orphan keeps the existing removal-on-save rule.
    for (const std::string &plugin_id : m_state->original_active_plugin_ids) {
        if (loaded_plugin_ids.find(plugin_id) != loaded_plugin_ids.end())
            continue;

        PluginCatalogEntry entry;
        entry.id = plugin_id;
        entry.name = format_wxstr(_L("%1% (not loaded)"), from_u8(plugin_id));
        entry.description = _L("This configured plugin id is not currently loaded.");
        entry.diagnostic = unavailable_plugin_tooltip(plugin_id, m_state->activation_config);
        entry.step_label = _L("Unknown");
        entry.registration_index = m_state->entries.size();
        entry.loaded = false;
        entry.modifiable = false;
        const std::map<std::string, std::string>::const_iterator provider =
            m_state->activation_config.plugin_packages.find(plugin_id);
        entry.preserve_unavailable_activation = provider != m_state->activation_config.plugin_packages.end();
        entry.active = entry.preserve_unavailable_activation;
        if (entry.preserve_unavailable_activation) {
            entry.package_id = provider->second;
            entry.external = true;
        }
        m_state->entries.push_back(std::move(entry));
    }
}

void PluginConfigDialog::build_navigation()
{
    wxTreeCtrl *tree = m_state->navigation;
    tree->DeleteAllItems();
    m_state->navigation_records.clear();
    const wxTreeItemId root = tree->AddRoot("Plugins");

    const wxTreeItemId all_item = tree->AppendItem(
        root, _L("All plugins"), -1, -1, new NavigationItemData({PluginNavigationKind::All}));
    m_state->navigation_records.push_back({all_item, {PluginNavigationKind::All}, _L("All plugins")});

    const std::array<PluginPhase, 5> phases {{
        PluginPhase::Slicing,
        PluginPhase::Perimeters,
        PluginPhase::SurfacesInfill,
        PluginPhase::SupportAdhesion,
        PluginPhase::Output
    }};
    for (PluginPhase phase : phases) {
        const NavigationScope phase_scope { PluginNavigationKind::Phase, phase, STEP_NONE };
        const wxTreeItemId phase_item = tree->AppendItem(
            root, phase_name(phase), -1, -1, new NavigationItemData(phase_scope));
        m_state->navigation_records.push_back({phase_item, phase_scope, phase_name(phase)});

        // Pipeline order, not enum numeric order, controls the navigation.
        // STEP_SKIRT_BRIM is one example whose execution position differs.
        for (slicing_step_t step : Steps::execution_order()) {
            const std::optional<PluginPhase> step_phase = phase_for_step(step);
            if (!step_phase.has_value() || *step_phase != phase)
                continue;
            const NavigationScope step_scope { PluginNavigationKind::Step, phase, step };
            const wxTreeItemId step_item = tree->AppendItem(
                phase_item, step_name(step), -1, -1, new NavigationItemData(step_scope));
            m_state->navigation_records.push_back({step_item, step_scope, step_name(step)});
        }
    }

    const NavigationScope extensions_scope { PluginNavigationKind::Extensions };
    const wxTreeItemId extensions_item = tree->AppendItem(
        root, _L("Extension points"), -1, -1, new NavigationItemData(extensions_scope));
    m_state->navigation_records.push_back(
        {extensions_item, extensions_scope, _L("Extension points")});

    std::set<slicing_step_t> extension_steps;
    for (const PluginCatalogEntry &entry : m_state->entries)
        if (entry.loaded && is_extension_step(entry.step))
            extension_steps.insert(entry.step);
    for (slicing_step_t step : extension_steps) {
        const NavigationScope step_scope { PluginNavigationKind::Step, PluginPhase::Slicing, step };
        const wxTreeItemId step_item = tree->AppendItem(
            extensions_item, step_name(step), -1, -1, new NavigationItemData(step_scope));
        m_state->navigation_records.push_back({step_item, step_scope, step_name(step)});
    }

    const NavigationScope problems_scope { PluginNavigationKind::Problems };
    const wxTreeItemId problems_item = tree->AppendItem(
        root, _L("Problems"), -1, -1, new NavigationItemData(problems_scope));
    m_state->navigation_records.push_back({problems_item, problems_scope, _L("Problems")});

    tree->ExpandAll();
    tree->SelectItem(all_item);
    refresh_navigation_counts();
}

void PluginConfigDialog::refresh_navigation_counts()
{
    for (const NavigationRecord &record : m_state->navigation_records) {
        size_t active_count = 0;
        size_t total_count = 0;
        for (const PluginCatalogEntry &entry : m_state->entries) {
            if (!navigation_matches(entry, record.scope))
                continue;
            ++total_count;
            if (entry.active)
                ++active_count;
        }
        m_state->navigation->SetItemText(
            record.item,
            record.label + wxString::Format(" (%u/%u)", unsigned(active_count), unsigned(total_count)));
    }
}

void PluginConfigDialog::refresh_plugin_list()
{
    PluginCatalogEntry *previous_entry = nullptr;
    const wxDataViewItem previous_selection = m_state->plugin_list->GetSelection();
    PluginListNode *previous_node = m_state->plugin_list_model->node(previous_selection);
    if (previous_node != nullptr)
        previous_entry = previous_node->entry;

    NavigationScope scope;
    const wxTreeItemId selected_navigation = m_state->navigation->GetSelection();
    if (selected_navigation.IsOk()) {
        const NavigationItemData *data =
            dynamic_cast<const NavigationItemData *>(m_state->navigation->GetItemData(selected_navigation));
        if (data != nullptr)
            scope = data->scope();
    }

    const wxString search = m_state->search->GetValue().Lower().Strip(wxString::both);
    const int state_filter = m_state->state_filter->GetSelection();
    const int origin_filter = m_state->origin_filter->GetSelection();
    std::vector<PluginCatalogEntry *> visible;
    for (PluginCatalogEntry &entry : m_state->entries) {
        // A non-empty search intentionally ignores navigation, making it a
        // global catalog search. State and origin filters still apply.
        if (search.empty() && !navigation_matches(entry, scope))
            continue;
        if (!search.empty() && searchable_plugin_text(entry).Find(search) == wxNOT_FOUND)
            continue;
        if (state_filter == 1 && !entry.active)
            continue;
        if (state_filter == 2 && entry.active)
            continue;
        if (state_filter == 3 && entry.loaded)
            continue;
        if (origin_filter == 1 && (entry.external || !entry.loaded))
            continue;
        if (origin_filter == 2 && !entry.external)
            continue;
        visible.push_back(&entry);
    }

    std::stable_sort(visible.begin(), visible.end(), plugin_less);
    std::vector<StepBucket> step_buckets;
    for (PluginCatalogEntry *entry : visible) {
        std::vector<StepBucket>::iterator bucket = std::find_if(
            step_buckets.begin(), step_buckets.end(), [entry](const StepBucket &candidate) {
                return candidate.problems == !entry->loaded &&
                       (candidate.problems || candidate.step == entry->step);
            });
        if (bucket == step_buckets.end()) {
            StepBucket created;
            created.problems = !entry->loaded;
            created.step = entry->step;
            created.label = created.problems ? _L("Problems") : entry->step_label;
            step_buckets.push_back(std::move(created));
            bucket = std::prev(step_buckets.end());
        }
        bucket->entries.push_back(entry);
    }
    std::stable_sort(step_buckets.begin(), step_buckets.end(), step_bucket_less);

    m_state->plugin_list->UnselectAll();
    m_state->plugin_list_model->clear();
    const bool show_step_containers = step_buckets.size() > 1 || !search.empty() ||
                                      scope.kind == PluginNavigationKind::All ||
                                      scope.kind == PluginNavigationKind::Phase ||
                                      scope.kind == PluginNavigationKind::Extensions;

    for (StepBucket &step_bucket : step_buckets) {
        PluginListNode *step_parent = nullptr;
        if (show_step_containers) {
            size_t active_count = 0;
            for (const PluginCatalogEntry *entry : step_bucket.entries)
                if (entry->active)
                    ++active_count;
            step_parent = m_state->plugin_list_model->add_container(
                nullptr,
                step_bucket.label,
                wxString::Format("%u/%u", unsigned(active_count), unsigned(step_bucket.entries.size())),
                step_bucket.problems ? _L("Configured plugin ids that are not currently loaded.") :
                                       step_bucket.label);
        }

        std::map<std::string, size_t> group_counts;
        for (const PluginCatalogEntry &catalog_entry : m_state->entries) {
            const bool same_bucket = step_bucket.problems ? !catalog_entry.loaded :
                                                           catalog_entry.loaded && catalog_entry.step == step_bucket.step;
            if (same_bucket && !catalog_entry.exclusive_group.empty())
                ++group_counts[catalog_entry.exclusive_group];
        }

        std::vector<GroupBucket> groups;
        for (PluginCatalogEntry *entry : step_bucket.entries) {
            const bool shared_group = !entry->exclusive_group.empty() && group_counts[entry->exclusive_group] > 1;
            const std::string group_id = shared_group ? entry->exclusive_group : std::string();
            std::vector<GroupBucket>::iterator group = std::find_if(
                groups.begin(), groups.end(), [&group_id](const GroupBucket &candidate) {
                    return candidate.id == group_id;
                });
            if (group == groups.end()) {
                GroupBucket created;
                created.id = group_id;
                created.label = shared_group ?
                    (entry->exclusive_group_label.empty() ? from_u8(entry->exclusive_group) :
                                                           entry->exclusive_group_label) :
                    _L("Other plugins");
                created.tooltip = shared_group ? entry->exclusive_group_tooltip :
                    _L("Plugins that do not share an alternative-selection group in this step.");
                groups.push_back(std::move(created));
                group = std::prev(groups.end());
            }
            group->entries.push_back(entry);
            group->minimum_priority = std::min(group->minimum_priority, entry->priority);
            group->registration_index = std::min(group->registration_index, entry->registration_index);
        }
        std::stable_sort(groups.begin(), groups.end(), [](const GroupBucket &left, const GroupBucket &right) {
            if (left.minimum_priority != right.minimum_priority)
                return left.minimum_priority < right.minimum_priority;
            return left.registration_index < right.registration_index;
        });

        for (GroupBucket &group : groups) {
            std::stable_sort(group.entries.begin(), group.entries.end(), plugin_less);
            const wxString group_count = group.entries.size() == 1 ? _L("1 plugin") :
                wxString::Format(_L("%u plugins"), unsigned(group.entries.size()));
            PluginListNode *group_parent = m_state->plugin_list_model->add_container(
                step_parent,
                group.label,
                group_count,
                group.tooltip);
            for (PluginCatalogEntry *entry : group.entries)
                m_state->plugin_list_model->add_plugin(group_parent, *entry);
        }
    }

    m_state->plugin_list_model->notify_rebuilt();
    for (const wxDataViewItem &container : m_state->plugin_list_model->container_items())
        m_state->plugin_list->Expand(container);

    wxDataViewItem selection = m_state->plugin_list_model->item_for_entry(previous_entry);
    if (!selection.IsOk() && !visible.empty())
        selection = m_state->plugin_list_model->item_for_entry(visible.front());
    if (selection.IsOk()) {
        m_state->plugin_list->Select(selection);
        m_state->plugin_list->EnsureVisible(selection);
    }
    refresh_details();
}

void PluginConfigDialog::refresh_details()
{
    const wxDataViewItem selection = m_state->plugin_list->GetSelection();
    PluginListNode *node = m_state->plugin_list_model->node(selection);
    PluginCatalogEntry *entry = node != nullptr ? node->entry : nullptr;
    const wxString unavailable_settings = entry == nullptr ? _L("Select a plugin") :
        entry->loaded ? _L("None") : _L("Not available because the plugin is not loaded");
    if (entry == nullptr) {
        const wxString title = node != nullptr ? node->label : _L("Select a plugin");
        const wxString description = node != nullptr ? node->detail :
            _L("Choose a plugin in the list to inspect its role and activation metadata.");
        set_detail_value(m_state->details.title, title);
        set_detail_value(m_state->details.description, description);
        set_detail_value(m_state->details.status, wxEmptyString);
        set_detail_value(m_state->details.id, wxEmptyString);
        set_detail_value(m_state->details.package, wxEmptyString);
        set_detail_value(m_state->details.step, wxEmptyString);
        set_detail_value(m_state->details.priority, wxEmptyString);
        set_detail_value(m_state->details.group, wxEmptyString);
        set_detail_value(m_state->details.dependencies, wxEmptyString);
        m_state->details.diagnostic_label->Show(false);
        m_state->details.diagnostic->Show(false);
    } else {
        set_detail_value(m_state->details.title, entry->name);
        set_detail_value(m_state->details.description,
                         entry->description.empty() ? _L("No description is available.") : entry->description);
        set_detail_value(m_state->details.status, plugin_status(*entry));
        set_detail_value(m_state->details.id, from_u8(entry->id));
        const wxString package = !entry->loaded && entry->package_id.empty() ? _L("Unknown") :
                                 entry->external ? from_u8(entry->package_id) : _L("Built-in");
        set_detail_value(m_state->details.package, package);

        const std::optional<size_t> step_index = entry->loaded ? pipeline_step_index(entry->step) : std::nullopt;
        wxString step_value = entry->step_label;
        if (step_index.has_value())
            step_value += wxString::Format(_L(" (pipeline position %u)"), unsigned(*step_index + 1));
        else if (entry->loaded)
            step_value += _L(" (extension point)");
        set_detail_value(m_state->details.step, step_value);
        set_detail_value(m_state->details.priority,
                         entry->loaded ? wxString::Format("%d", entry->priority) : _L("Not available"));

        wxString group_value = entry->exclusive_group.empty() ? _L("None") :
            (entry->exclusive_group_label.empty() ? from_u8(entry->exclusive_group) :
                                                   entry->exclusive_group_label);
        if (!entry->exclusive_group.empty() && group_value != from_u8(entry->exclusive_group))
            group_value += " (" + from_u8(entry->exclusive_group) + ")";
        if (!entry->exclusive_group_tooltip.empty())
            group_value += "\n" + entry->exclusive_group_tooltip;
        set_detail_value(m_state->details.group, group_value);
        set_detail_value(m_state->details.dependencies, join_dependencies(entry->dependencies));

        const bool show_diagnostic = !entry->diagnostic.empty();
        m_state->details.diagnostic_label->Show(show_diagnostic);
        m_state->details.diagnostic->Show(show_diagnostic);
        if (show_diagnostic)
            set_detail_value(m_state->details.diagnostic, entry->diagnostic);
    }

    const std::vector<PluginSettingEntry> no_settings;
    m_state->details.defined_settings->set_settings(
        entry != nullptr ? entry->defined_settings : no_settings, unavailable_settings);
    m_state->details.other_used_settings->set_settings(
        entry != nullptr ? entry->other_used_settings : no_settings, unavailable_settings);

    const int wrap_width = 35 * em_unit();
    m_state->details.description->Wrap(wrap_width);
    m_state->details.group->Wrap(wrap_width);
    m_state->details.dependencies->Wrap(wrap_width);
    m_state->details.diagnostic->Wrap(wrap_width);
    m_state->details_panel->fit_contents();
}

void PluginConfigDialog::build()
{
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *description = new wxStaticText(
        this, wxID_ANY,
        _L("Choose which loaded plugins will be active after the next restart. Runtime selectors decide which "
           "active alternative is used for an individual print."));
    description->Wrap(95 * em_unit());
    main_sizer->Add(description, 0, wxEXPAND | wxALL, 10);

    // Search is global; the two choices refine both search results and the
    // current navigation scope without changing pending activation values.
    wxBoxSizer *filters = new wxBoxSizer(wxHORIZONTAL);
    m_state->search = new wxSearchCtrl(this, wxID_ANY);
    m_state->search->SetDescriptiveText(_L("Search plugins"));
    m_state->search->ShowCancelButton(true);
    filters->Add(m_state->search, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    filters->Add(new wxStaticText(this, wxID_ANY, _L("State")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_state->state_filter = new wxChoice(this, wxID_ANY);
    m_state->state_filter->Append(_L("All"));
    m_state->state_filter->Append(_L("Active"));
    m_state->state_filter->Append(_L("Inactive"));
    m_state->state_filter->Append(_L("Problems"));
    m_state->state_filter->SetSelection(0);
    filters->Add(m_state->state_filter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    filters->Add(new wxStaticText(this, wxID_ANY, _L("Origin")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_state->origin_filter = new wxChoice(this, wxID_ANY);
    m_state->origin_filter->Append(_L("All"));
    m_state->origin_filter->Append(_L("Built-in"));
    m_state->origin_filter->Append(_L("External"));
    m_state->origin_filter->SetSelection(0);
    filters->Add(m_state->origin_filter, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(filters, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    wxBoxSizer *content = new wxBoxSizer(wxHORIZONTAL);
    m_state->navigation = new wxTreeCtrl(
        this, wxID_ANY, wxDefaultPosition, wxSize(25 * em_unit(), 32 * em_unit()),
        wxTR_HIDE_ROOT | wxTR_HAS_BUTTONS | wxTR_SINGLE | wxBORDER_SIMPLE);
    // Navigation labels may scroll horizontally, but the filter pane itself
    // remains stable while the plugin list and details consume extra width.
    m_state->navigation->SetMinSize(wxSize(25 * em_unit(), -1));
    content->Add(m_state->navigation, 0, wxEXPAND | wxRIGHT, 8);

    m_state->plugin_list = new wxDataViewCtrl(
        this, wxID_ANY, wxDefaultPosition, wxSize(52 * em_unit(), 32 * em_unit()),
        wxDV_SINGLE | wxDV_ROW_LINES | wxDV_VERT_RULES | wxBORDER_SIMPLE);
    m_state->plugin_list_model = new PluginListModel();
    m_state->plugin_list->AssociateModel(m_state->plugin_list_model);
    m_state->plugin_list_model->DecRef();
    wxDataViewColumn *active_column = m_state->plugin_list->AppendToggleColumn(
        _L("Active"), PluginListModel::Active, wxDATAVIEW_CELL_ACTIVATABLE, 7 * em_unit());
    wxDataViewColumn *name_column = new wxDataViewColumn(
        _L("Plugin"), new PrimarySecondaryTextRenderer(), PluginListModel::Name,
        43 * em_unit(), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_state->plugin_list->AppendColumn(name_column);
    m_state->plugin_list->SetExpanderColumn(name_column);
    (void) active_column;
    wxGetApp().UpdateDVCDarkUI(m_state->plugin_list);
    content->Add(m_state->plugin_list, 2, wxEXPAND | wxRIGHT, 8);

    m_state->details_panel = new PluginDetailsPanel(
        this, wxSize(38 * em_unit(), 32 * em_unit()), em_unit());
    wxBoxSizer *details_sizer = new wxBoxSizer(wxVERTICAL);
    m_state->details.title = new wxStaticText(m_state->details_panel, wxID_ANY, _L("Select a plugin"));
    wxFont title_font = m_state->details.title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_state->details.title->SetFont(title_font);
    details_sizer->Add(m_state->details.title, 0, wxEXPAND | wxALL, 10);
    m_state->details.description = new wxStaticText(
        m_state->details_panel, wxID_ANY,
        _L("Choose a plugin in the list to inspect its role and activation metadata."));
    details_sizer->Add(m_state->details.description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    details_sizer->Add(new wxStaticLine(m_state->details_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    wxFlexGridSizer *details_grid = new wxFlexGridSizer(2, 6, 10);
    details_grid->AddGrowableCol(1, 1);
    const std::array<wxString, 7> detail_labels {{
        _L("State"), _L("ID"), _L("Package"), _L("Step"), _L("Priority"), _L("Exclusive group"), _L("Dependencies")
    }};
    std::array<wxStaticText **, 7> detail_values {{
        &m_state->details.status,
        &m_state->details.id,
        &m_state->details.package,
        &m_state->details.step,
        &m_state->details.priority,
        &m_state->details.group,
        &m_state->details.dependencies
    }};
    for (size_t detail_idx = 0; detail_idx < detail_labels.size(); ++detail_idx) {
        wxStaticText *label = new wxStaticText(m_state->details_panel, wxID_ANY, detail_labels[detail_idx]);
        wxFont label_font = label->GetFont();
        label_font.SetWeight(wxFONTWEIGHT_BOLD);
        label->SetFont(label_font);
        details_grid->Add(label, 0, wxALIGN_TOP);
        *detail_values[detail_idx] = new wxStaticText(m_state->details_panel, wxID_ANY, wxEmptyString);
        details_grid->Add(*detail_values[detail_idx], 1, wxEXPAND);
    }
    details_sizer->Add(details_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // Settings remain readable for plugins that are not active because their
    // registration metadata is collected before initialize() is called.
    details_sizer->Add(new wxStaticLine(m_state->details_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    wxStaticText *defined_settings_label =
        new wxStaticText(m_state->details_panel, wxID_ANY, _L("Defined settings"));
    wxFont settings_label_font = defined_settings_label->GetFont();
    settings_label_font.SetWeight(wxFONTWEIGHT_BOLD);
    defined_settings_label->SetFont(settings_label_font);
    details_sizer->Add(defined_settings_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    m_state->details.defined_settings = new PluginSettingsPanel(m_state->details_panel);
    details_sizer->Add(m_state->details.defined_settings, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    wxStaticText *used_settings_label =
        new wxStaticText(m_state->details_panel, wxID_ANY, _L("Other settings used"));
    used_settings_label->SetFont(settings_label_font);
    details_sizer->Add(used_settings_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    m_state->details.other_used_settings = new PluginSettingsPanel(m_state->details_panel);
    details_sizer->Add(m_state->details.other_used_settings, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    m_state->details.diagnostic_label = new wxStaticText(m_state->details_panel, wxID_ANY, _L("Diagnostic"));
    wxFont diagnostic_font = m_state->details.diagnostic_label->GetFont();
    diagnostic_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_state->details.diagnostic_label->SetFont(diagnostic_font);
    m_state->details.diagnostic = new wxStaticText(m_state->details_panel, wxID_ANY, wxEmptyString);
    details_sizer->Add(m_state->details.diagnostic_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    details_sizer->Add(m_state->details.diagnostic, 0, wxEXPAND | wxALL, 10);
    m_state->details.diagnostic_label->Show(false);
    m_state->details.diagnostic->Show(false);
    details_sizer->AddStretchSpacer();
    m_state->details_panel->SetSizer(details_sizer);
    content->Add(m_state->details_panel, 1, wxEXPAND);
    main_sizer->Add(content, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
    wxButton *save = new wxButton(this, wxID_OK, _L("Save and restart"));
    wxButton *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    buttons->AddStretchSpacer();
    buttons->Add(save, 0, wxRIGHT, 6);
    buttons->Add(cancel, 0);
    main_sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

    build_navigation();
    refresh_plugin_list();

    // Every projection event rebuilds from the catalog. Toggle events also
    // update tree counters before applying filters that may hide the row.
    m_state->search->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { refresh_plugin_list(); });
    m_state->state_filter->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { refresh_plugin_list(); });
    m_state->origin_filter->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { refresh_plugin_list(); });
    m_state->navigation->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent &) { refresh_plugin_list(); });
    m_state->plugin_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                               [this](wxDataViewEvent &) { refresh_details(); });
    m_state->plugin_list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent &) {
        refresh_navigation_counts();
        refresh_plugin_list();
    });
    save->Bind(wxEVT_BUTTON, &PluginConfigDialog::save_and_restart, this);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    SetSizer(main_sizer);
    SetMinSize(wxSize(106 * em_unit(), 36 * em_unit()));
    SetSize(wxSize(126 * em_unit(), 50 * em_unit()));
    wxGetApp().UpdateDarkUI(this);
    Layout();
    CentreOnParent();
}

bool PluginConfigDialog::write_active_plugins(std::string &error_message) const
{
    if (!has_data_dir()) {
        error_message = "The configuration directory is not initialized.";
        return false;
    }

    const boost::filesystem::path config_path = plugin_activation_config_path(boost::filesystem::path(data_dir()));
    try {
        std::set<std::string> active_ids = m_state->original_active_plugin_ids;
        for (const PluginCatalogEntry &entry : m_state->entries) {
            if (entry.preserve_unavailable_activation)
                continue;
            active_ids.erase(entry.id);
            if (entry.active)
                active_ids.insert(entry.id);
        }

        std::vector<std::string> selected_plugin_ids;
        for (const std::string &plugin_id : active_ids)
            if (Orchestrator::instance().get_plugin(plugin_id) != nullptr)
                selected_plugin_ids.push_back(plugin_id);
        if (!Orchestrator::instance().validate_plugin_activation(selected_plugin_ids, error_message))
            return false;

        PluginActivationConfig config;
        if (!read_plugin_activation_config(config_path, config, error_message))
            return false;

        // Re-reading immediately before publication preserves package changes
        // made by another dialog while this activation catalog was open.
        for (const PluginCatalogEntry &entry : m_state->entries) {
            if (!entry.preserve_unavailable_activation) {
                config.activated[entry.id] = entry.active;
                if (!entry.active)
                    config.plugin_packages.erase(entry.id);
            }
            if (entry.active && entry.loaded && entry.external && !entry.package_id.empty())
                config.plugin_packages[entry.id] = entry.package_id;
        }
        if (!write_plugin_activation_config(config_path, config, error_message))
            return false;
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }

    return true;
}

bool PluginConfigDialog::prepare_restart() const
{
    if (wxGetApp().plater() == nullptr)
        return true;

    const int saved_project = wxGetApp().plater()->save_project_if_dirty(
        format_wxstr(_L("Closing %1%. Current project is modified."), SLIC3R_APP_NAME));
    if (saved_project == wxID_CANCEL)
        return false;

    if (saved_project == wxID_NO && wxGetApp().plater()->is_presets_dirty())
        return wxGetApp().check_and_save_current_preset_changes(
            format_wxstr(_L("Closing %1%"), SLIC3R_APP_NAME),
            format_wxstr(_L("Closing %1% while some presets are modified."), SLIC3R_APP_NAME));

    return true;
}

void PluginConfigDialog::save_and_restart(wxCommandEvent &)
{
    if (!prepare_restart())
        return;

    std::string error_message;
    if (!write_active_plugins(error_message)) {
        show_error(this, error_message);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Plugin activation configuration saved to '"
                            << plugin_activation_config_path(boost::filesystem::path(data_dir())).string()
                            << "'. Restarting.";
    EndModal(wxID_OK);
    start_new_slicer(nullptr, false);
    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->Close(true);
}

void PluginConfigDialog::on_dpi_changed(const wxRect &)
{
    SetFont(wxGetApp().normal_font());
    if (m_state->navigation != nullptr)
        m_state->navigation->SetMinSize(wxSize(25 * em_unit(), -1));
    if (m_state->plugin_list != nullptr && m_state->plugin_list->GetColumnCount() >= 2) {
        m_state->plugin_list->GetColumn(PluginListModel::Active)->SetWidth(7 * em_unit());
        m_state->plugin_list->GetColumn(PluginListModel::Name)->SetWidth(43 * em_unit());
    }
    msw_buttons_rescale(this, em_unit(), { wxID_OK, wxID_CANCEL });
    refresh_details();
    Layout();
    Refresh();
}

} // namespace Slic3r::GUI
