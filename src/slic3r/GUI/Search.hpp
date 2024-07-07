#ifndef slic3r_SearchComboBox_hpp_
#define slic3r_SearchComboBox_hpp_

#include <vector>
#include <map>

#include <boost/nowide/convert.hpp>

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/listctrl.h>

#include <wx/combo.h>

#include <wx/checkbox.h>
#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Preset.hpp"


namespace Slic3r {

wxDECLARE_EVENT(wxCUSTOMEVT_JUMP_TO_OPTION, wxCommandEvent);

namespace Search{

class SearchDialog;

struct InputInfo
{
    DynamicPrintConfig* config  {nullptr};
    Preset::Type        type    {Preset::TYPE_INVALID};
};

struct GroupAndCategory {
    wxString        group;
    wxString        category;
    ConfigOptionDef gui_opt;
};

struct Option {

//private:
    //Option() {}
//public:
//    bool operator<(const Option& other) const { return other.label > this->label; }
    bool operator<(const Option& other) const {
        if (this->type == other.type)
            if (this->key == other.key)
                return this->idx < other.idx;
            else
               return this->key < other.key;
        else
            return  this->type < other.type;
    }

    // Fuzzy matching works at a character level. Thus matching with wide characters is a safer bet than with short characters,
    // though for some languages (Chinese?) it may not work correctly.
    std::wstring    key;
    Preset::Type    type {Preset::TYPE_INVALID};
    int16_t        idx;
    ConfigOptionMode tags;
    std::wstring    label;
    std::wstring    label_local;
    std::wstring    group;
    std::wstring    group_local;
    std::wstring    category;
    std::wstring    category_local;
    std::wstring    tooltip;
    std::wstring    tooltip_local;
    std::wstring    tooltip_lowercase;
    std::wstring    tooltip_local_lowercase;
    std::string     opt_key_with_idx() const;
};

struct FoundOption {
    // UTF8 encoding, to be consumed by ImGUI by reference.
    std::string     label;
    std::string     marked_label;
    std::string     tooltip;
    size_t          option_idx {0};
    int             outScore {0};

    // Returning pointers to contents of std::string members, to be used by ImGUI for rendering.
    void get_marked_label_and_tooltip(const char** label, const char** tooltip) const;
};

struct OptionViewParameters
{
    bool category   {true};
    bool english    {false};
    bool exact      {false};
    bool all_mode   {true};

    int  hovered_id {0};
};


class OptionsSearcher
{
    std::string                             search_line;
    std::map<std::string, std::vector<GroupAndCategory>> groups_and_categories;
    PrinterTechnology                       printer_technology {ptAny};
    ConfigOptionMode                        current_tags {comNone};

    std::vector<Option>                     options{};
    bool sorted = false;
    std::vector<Option>                     script_options{};
    std::vector<FoundOption>                found {};
    std::map<ConfigOptionMode, wxString>    tag_label_cache;

    void append_options(DynamicPrintConfig* config, Preset::Type type);

    void sort_options() {
        std::sort(options.begin(), options.end());
        sorted = true;
    }
    void sort_found() {
        std::sort(found.begin(), found.end(), [](const FoundOption& f1, const FoundOption& f2) {
            return f1.outScore > f2.outScore || (f1.outScore == f2.outScore && f1.label < f2.label); });
    };

    size_t options_size() const { return options.size(); }
    size_t found_size()   const { return found.size(); }

public:
    OptionViewParameters                    view_params;

    SearchDialog*                           search_dialog { nullptr };

    OptionsSearcher();
    ~OptionsSearcher();

    void check_and_update(  PrinterTechnology pt_in,
                            ConfigOptionMode tags_in,
                            std::vector<InputInfo> input_values);
    void append_script_option(const ConfigOptionDef &opt, Preset::Type preset_type, int16_t idx);
    bool search();
    bool search(const std::string& search, bool force = false);

    void add_key(const std::string& opt_key, Preset::Type type, const wxString& group, const wxString& category, const ConfigOptionDef& gui_opt, bool reset = false);

    size_t size() const         { return found_size(); }

    const FoundOption& operator[](const size_t pos) const noexcept { return found[pos]; }
    const Option& get_option(size_t pos_in_filter) const;
    const Option& get_option(const std::string& opt_key, Preset::Type type) const;
    Option get_option_names(const std::string& opt_key, Preset::Type type) const;

    const std::vector<FoundOption>& found_options() { return found; }
    const GroupAndCategory&         get_group_and_category (const std::string& opt_key, ConfigOptionMode tags) const;
    std::string& search_string() { return search_line; }

    bool is_sorted() { return sorted; }

    void show_dialog();
    void dlg_sys_color_changed();
    void dlg_msw_rescale();

};


//------------------------------------------
//          SearchDialog
//------------------------------------------
class SearchListModel;
class SearchDialog : public GUI::DPIDialog
{
    wxString search_str;
    wxString default_string;

    bool     prevent_list_events {false};

    wxTextCtrl*         search_line         { nullptr };
    wxDataViewCtrl*     search_list         { nullptr };
    SearchListModel*    search_list_model   { nullptr };
    wxCheckBox*         check_category      { nullptr };
    wxCheckBox*         check_english       { nullptr };
    wxCheckBox*         check_exact         { nullptr };
    wxCheckBox*         check_all_mode      { nullptr };

    OptionsSearcher*    searcher            { nullptr };

    void OnInputText(wxCommandEvent& event);
    void OnLeftUpInTextCtrl(wxEvent& event);
    void OnKeyDown(wxKeyEvent& event);

    void OnActivate(wxDataViewEvent& event);
    void OnSelect(wxDataViewEvent& event);

    void OnCheck(wxCommandEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnLeftDown(wxMouseEvent& event);

    void update_list();

public:
    SearchDialog(OptionsSearcher* searcher);
    ~SearchDialog() {}

    void Popup(wxPoint position = wxDefaultPosition);
    void ProcessSelection(wxDataViewItem selection);

    void msw_rescale();
    void on_sys_color_changed() override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override { msw_rescale(); }
};


// ----------------------------------------------------------------------------
// SearchListModel
// ----------------------------------------------------------------------------

class SearchListModel : public wxDataViewVirtualListModel
{
    std::vector<std::pair<wxString, int>>   m_values;
    ScalableBitmap                          m_icon[5];

public:
    enum {
        colIcon,
        colMarkedText,
        colMax
    };

    SearchListModel(wxWindow* parent);

    // helper methods to change the model

    void Clear();
    void Prepend(const std::string& text);
    void msw_rescale();

    // implementation of base class virtuals to define model

    unsigned int GetColumnCount() const override { return colMax; }
    wxString GetColumnType(unsigned int col) const override;
    void GetValueByRow(wxVariant& variant, unsigned int row, unsigned int col) const override;
    bool GetAttrByRow(unsigned int row, unsigned int col, wxDataViewItemAttr& attr) const override { return true; }
    bool SetValueByRow(const wxVariant& variant, unsigned int row, unsigned int col) override { return false; }
};




} // Search namespace
}

#endif //slic3r_SearchComboBox_hpp_
