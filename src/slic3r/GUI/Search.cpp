///|/ Copyright (c) Prusa Research 2020 - 2023 Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Search.hpp"

#include <cstddef>
#include <regex>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/nowide/convert.hpp>

#include "wx/dataview.h"
#include "wx/numformatter.h"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "ExtraRenderers.hpp"
#include "fts_fuzzy_match.h"

#include "imgui/imconfig.h"

using boost::optional;

namespace Slic3r {

wxDEFINE_EVENT(wxCUSTOMEVT_JUMP_TO_OPTION, wxCommandEvent);

using GUI::from_u8;
using GUI::into_u8;

namespace Search {

static char marker_by_type(Preset::Type type, PrinterTechnology pt)
{
    if (Preset::TYPE_FFF_FILAMENT == type) {
        return ImGui::FilamentIconMarker;
    } else if (Preset::TYPE_SLA_MATERIAL == type) {
        return ImGui::MaterialIconMarker;
    } else if ((Preset::TYPE_PRINTER & type) == Preset::TYPE_PRINTER) {
        return pt == ptSLA ? ImGui::PrinterSlaIconMarker : ImGui::PrinterIconMarker;
    } else if ((Preset::TYPE_PREFERENCES & type) == Preset::TYPE_PREFERENCES) {
        return ImGui::PreferencesButton;
    } else if ((Preset::TYPE_PRINT1 & type) == Preset::TYPE_PRINT1 ||
               (Preset::TYPE_FREQUENT & type) == Preset::TYPE_FREQUENT) {
        return ImGui::PrintIconMarker;
    }
    assert(false);
    return ImGui::PrintIconMarker;
}

std::string Option::opt_key_with_idx() const
{
    std::string opt_key = boost::nowide::narrow(key);
    assert(opt_key.find(';') == std::string::npos);
    if (idx >= 0) {
        opt_key += "#" + std::to_string(idx);
    }
    return opt_key;
}

std::string Option::opt_key() const
{
    std::string opt_key = boost::nowide::narrow(key);
    assert(opt_key.find(';') == std::string::npos);
    return opt_key;
}

void FoundOption::get_marked_label_and_tooltip(const char** label_, const char** tooltip_) const
{
    *label_   = marked_label.c_str();
    *tooltip_ = tooltip.c_str();
}

template<class T>
//void change_opt_key(std::string& opt_key, DynamicPrintConfig* config)
void change_opt_key(std::string& opt_key, DynamicPrintConfig* config, int& cnt)
{
    T* opt_cur = static_cast<T*>(config->option(opt_key));
    cnt = opt_cur->size();
    return;

    if (opt_cur->size() > 0)
        opt_key += "#" + std::to_string(0);
}

static Option create_option(const std::string& opt_key, const int16_t opt_idx, Preset::Type type, const GroupAndCategory& gc)
{
    wxString suffix;
    wxString suffix_local;
    if (gc.category == "Machine limits") {
        suffix = opt_idx == 1 ? L("Stealth") : L("Normal");
        suffix_local = " " + _(suffix);
        suffix = " " + suffix;
    }

    wxString category = gc.category;
    if (type == Preset::TYPE_PRINTER && category.Contains("Extruder ")) {
        category = wxString::Format("%s %d", "Extruder", opt_idx + 1);
    }

    const ConfigOptionDef& opt = gc.gui_opt;

    wxString label;
    wxString local_label;
    if (opt.full_label.empty()) {
        label = opt.label;
        local_label = _(opt.label);
    } else {
        if (opt.label.empty() || opt.label.front() == '_') {
            label = opt.full_label;
            local_label = _(opt.full_label);
        } else {
            label = opt.full_label + " (" + opt.label + ')';
            local_label = _(opt.full_label) + " (" + _(opt.label) + ')';
        }
    }

    if (!label.IsEmpty())
        return Option{ boost::nowide::widen(opt.opt_key), type, opt_idx, opt.mode,
                                    (label + suffix).ToStdWstring(), (local_label + suffix_local).ToStdWstring(),
                                    gc.group.ToStdWstring(), _(gc.group).ToStdWstring(),
                                    category.ToStdWstring(), GUI::Tab::translate_category(category, type).ToStdWstring() ,
                                    wxString(opt.tooltip).ToStdWstring(), (_(opt.tooltip)).ToStdWstring(),
                                    boost::algorithm::to_lower_copy(wxString(opt.tooltip).ToStdWstring()), boost::algorithm::to_lower_copy((_(opt.tooltip)).ToStdWstring()) };
    return Option{};

}

static std::string get_key(const std::string& opt_key, Preset::Type type)
{
    return std::to_string(int(type)) + ";" + opt_key;
}
void change_opt_keyFoP(std::string& opt_key, DynamicPrintConfig* config, int& cnt)
{
    ConfigOptionFloatsOrPercents* opt_cur = static_cast<ConfigOptionFloatsOrPercents*>(config->option(opt_key));
    cnt = opt_cur->size();
    return;

    if (opt_cur->size() > 0)
        opt_key += "#" + std::to_string(0);
}
const GroupAndCategory& OptionsSearcher::get_group_and_category(const std::string& opt_key, ConfigOptionMode tags) const
{
    static GroupAndCategory empty = GroupAndCategory{ "","",ConfigOptionDef {} };

    auto it = groups_and_categories.find(opt_key);
    if (it == groups_and_categories.end())
        return empty;
    for (const GroupAndCategory& gag : it->second) {
        if ((gag.gui_opt.mode & tags) == tags)
            return gag;
    }
    return empty;
}

void OptionsSearcher::append_options(DynamicPrintConfig* config, Preset::Type type)
{
    //const ConfigDef* defs = config->def();
    auto emplace_option = [this, type](std::string grp_key, const int16_t id)
    {

        //TODO:test that new if
        if (id >= 0)
            // ! It's very important to use "#". opt_key#n is a real option key used in GroupAndCategory
            grp_key += "#" + std::to_string(id);

        assert(groups_and_categories.find(grp_key) == groups_and_categories.end()
            || !groups_and_categories[grp_key].empty());

        for (const GroupAndCategory& gc : groups_and_categories[grp_key]) {
            if (gc.group.IsEmpty() || gc.category.IsEmpty())
                return;

            Option option = create_option(gc.gui_opt.opt_key, id, type, gc);
            if (!option.label.empty()) {
                options.push_back(std::move(option));
                sorted = false;
            }

            //wxString suffix;
            //wxString suffix_local;
            //if (gc.category == "Machine limits") {
            //    suffix = id == '1' ? L("Stealth") : L("Normal");
            //    suffix_local = " " + _(suffix);
            //    suffix = " " + suffix;
            //}
            //else if (gc.group == "Dynamic overhang speed" && id >= 0) {
            //    suffix = " " + std::to_string(id+1);
            //    suffix_local = suffix;
            //}

            //const ConfigOptionDef& opt = gc.gui_opt;
            //if (opt.opt_key == "complete_objects")
            //    std::cout << "ok";

            //std::string label = opt.full_label;
            //if (label.find(opt.label) == std::string::npos)
            //    label = opt.label;

            //if (!label.empty())
            //    options.emplace_back(Option{ boost::nowide::widen(opt.opt_key), type, opt.mode,
            //                                (wxString(label) + suffix).ToStdWstring(), (_(label) + suffix_local).ToStdWstring(),
            //                                gc.group.ToStdWstring(), _(gc.group).ToStdWstring(),
            //                                gc.category.ToStdWstring(), GUI::Tab::translate_category(gc.category, type).ToStdWstring() ,
            //                                wxString(opt.tooltip).ToStdWstring(), (_(opt.tooltip)).ToStdWstring() });
        }
    };

    for (std::string opt_key : config->keys())
    {
        const ConfigOptionDef& opt = *config->option_def(opt_key);
        //if (opt.mode != comNone && (opt.mode & current_tags) == 0)
        //    continue;

        int cnt = 0;

        if ( (type == Preset::TYPE_SLA_MATERIAL || type == Preset::TYPE_FFF_FILAMENT || type == Preset::TYPE_PRINTER || opt.is_vector_extruder) && opt_key != "bed_shape")
            switch (config->option(opt_key)->type())
            {
            case coInts:	change_opt_key<ConfigOptionInts		>(opt_key, config, cnt);	break;
            case coBools:	change_opt_key<ConfigOptionBools	>(opt_key, config, cnt);	break;
            case coFloats:	change_opt_key<ConfigOptionFloats	>(opt_key, config, cnt);	break;
            case coStrings:	change_opt_key<ConfigOptionStrings	>(opt_key, config, cnt);	break;
            case coPercents:change_opt_key<ConfigOptionPercents	>(opt_key, config, cnt);	break;
            case coPoints:	change_opt_key<ConfigOptionPoints	>(opt_key, config, cnt);	break;
            //case coFloatsOrPercents:	change_opt_key<ConfigOptionFloatsOrPercents	>(opt_key, config, cnt);	break;
            case coFloatsOrPercents:change_opt_keyFoP(opt_key, config, cnt);	break;
            case coGraphs:	change_opt_key<ConfigOptionGraphs	>(opt_key, config, cnt);	break;
            default:		break;
            }

        //wxString label = opt.full_label.empty() ? opt.label : opt.full_label;

        std::string key = get_key(opt_key, type);

        //if (label_override.find(opt.opt_key) != label_override.end()) {
        //    label = label_override[opt.opt_key][1].empty() ? label_override[opt.opt_key][0] : label_override[opt.opt_key][1];
        //}

        if (cnt == 0)
            emplace_option(key, -1);
        else
            for (int i = 0; i < cnt; ++i)
                // // ! It's very important to use "#". opt_key#n is a real option key used in GroupAndCategory
                // emplace_option(key + "#" + std::to_string(i), i);
                // ??? please prusa, make your mind...
                emplace_option(key, i);
    }
}

// Mark a string using ColorMarkerStart and ColorMarkerEnd symbols
static std::wstring mark_string(const std::wstring &str, const std::vector<uint16_t> &matches, Preset::Type type, PrinterTechnology pt)
{
	std::wstring out;
    out += marker_by_type(type, pt);
	if (matches.empty())
		out += str;
	else {
		out.reserve(str.size() * 2);
		if (matches.front() > 0)
			out += str.substr(0, matches.front());
		for (size_t i = 0;;) {
			// Find the longest string of successive indices.
			size_t j = i + 1;
            while (j < matches.size() && matches[j] == matches[j - 1] + 1)
                ++ j;
            out += ImGui::ColorMarkerStart;
            out += str.substr(matches[i], matches[j - 1] - matches[i] + 1);
            out += ImGui::ColorMarkerEnd;
            if (j == matches.size()) {
				out += str.substr(matches[j - 1] + 1);
				break;
			}
            out += str.substr(matches[j - 1] + 1, matches[j] - matches[j - 1] - 1);
            i = j;
		}
	}
	return out;
}

bool OptionsSearcher::search()
{
    return search(search_line, true);
}

static bool fuzzy_match(const std::wstring &search_pattern, const std::wstring &label, int& out_score, std::vector<uint16_t> &out_matches)
{
    uint16_t matches[fts::max_matches + 1]; // +1 for the stopper
    int score;
    if (fts::fuzzy_match(search_pattern.c_str(), label.c_str(), score, matches)) {
	    size_t cnt = 0;
	    for (; matches[cnt] != fts::stopper; ++cnt);
	    out_matches.assign(matches, matches + cnt);
		out_score = score;
		return true;
	} else
		return false;
}

static bool strong_match(const std::wregex& search_pattern, const std::wstring& label, int& out_score, std::vector<uint16_t>& out_matches) {
    std::wsmatch sm;
    out_matches.clear();
    out_score = 0;
    std::wstring str_search = label;
    size_t pos = 0;
    size_t max_iter       = 100; // prevent .* infinite combination
    while (max_iter > 0 && std::regex_search(str_search, sm, search_pattern)) {
        pos += sm.position();
        for (int64_t j = 0; j < sm.length(); ++j)
            out_matches.push_back(pos + j);
        out_score += std::max(1, int(30 - pos));
        pos += sm.length();
        str_search = sm.suffix().str();
        --max_iter;
    }
    if (out_score <= 0)
        out_score = std::numeric_limits<int>::min();
    return out_score > 0;
}

bool OptionsSearcher::search(const std::string& search,  bool force/* = false*/)
{
    if (search_line == search && !force)
        return false;

    found.clear();

    bool full_list = search.empty();
    std::wstring sep = L" : ";

    auto get_label = [this, &sep](const Option& opt, bool marked = true)
    {
        std::wstring out;
        if (marked) {
            out += marker_by_type(opt.type, printer_technology);
        }
        const std::wstring* prev = nullptr;
        for (const std::wstring* const s : {
            view_params.category ?  &opt.category_local : nullptr,
            view_params.category ?  &opt.group_local : nullptr,
                                    & opt.label_local }) {
            if (s != nullptr && (prev == nullptr || *prev != *s)) {
                if (out.size() > 2)
                    out += sep;
                out += *s;
                prev = s;
            }
        }
        return out;
    };

    auto get_label_english = [this, &sep](const Option& opt, bool marked = true)
    {
        std::wstring out;
        if (marked) {
            out += marker_by_type(opt.type, printer_technology);
        }
        const std::wstring *prev = nullptr;
        for (const std::wstring* const s : {
            view_params.category ? &opt.category : nullptr,
                view_params.category ? &opt.group : nullptr,
                & opt.label }) {
            if (s != nullptr && (prev == nullptr || *prev != *s)) {
                if (out.size() > 2)
                    out += sep;
                out += *s;
                prev = s;
            }
        }
        return out;
    };

    auto get_tooltip = [this, &sep](const Option& opt)
    {
        //add "\n" to long tooltip lines
        std::wstring tooltip;
        int length = 0;
        for (int i = 0; i < opt.tooltip_local.size(); i++) {
            if (length >= 80 && opt.tooltip_local[i] == u' ') {
                tooltip.push_back(u'\n');
            } else {
                tooltip.push_back(opt.tooltip_local[i]);
            }
            length++;
            if (tooltip.back() == u'\n') {
                length = 0;
            }
        }


        return  marker_by_type(opt.type, printer_technology) +
            opt.category_local + sep +
            opt.group_local + sep + opt.label_local +
            "\n\n" + tooltip;
    };

    std::wstring wsearch = boost::nowide::widen(search);
    boost::trim_left(wsearch);
    boost::algorithm::to_lower(wsearch);

    //precompute pattern, if possible/needed
    std::wregex pattern;
    bool        fail_pattern = false;
    try {
        if (view_params.exact)
            pattern = std::wregex(wsearch, std::regex_constants::icase);
    } catch (std::regex_error reg_err) {
        // Happens when std::wregex("]") or similar. => no result //TODO: add warning message 'wrong regexp'
        fail_pattern = true;
    }

    std::vector<uint16_t> matches, matches2;
    for (size_t i=0; i < options.size(); i++)
    {
        const Option &opt = options[i];

        if (!view_params.all_mode)
            if ((opt.tags & current_tags) != current_tags)
                continue;

        if (full_list) {
            std::string label = into_u8(get_label(opt));
            if (view_params.all_mode && (opt.tags & current_tags) == 0) {
                label += " " + into_u8(_L("tags")) + ":{";
                for (AppConfig::Tag& t : Slic3r::GUI::get_app_config()->tags()) {
                    if ((opt.tags & t.tag) == t.tag)
                        label += " " + into_u8(_(t.name));
                }
                label += "}";
            }
            found.emplace_back(FoundOption{ label, label, boost::nowide::narrow(get_tooltip(opt)), i, 0 });
            continue;
        }

        std::wstring label         = get_label(opt, false);
        std::wstring label_english = get_label_english(opt, false);
        std::wstring label_lowercase         = boost::algorithm::to_lower_copy(label);
        std::wstring label_english_lowercase = boost::algorithm::to_lower_copy(label_english);
        int score = std::numeric_limits<int>::min();
        int score2;
        matches.clear();

        
        //search for label
        if (!fail_pattern) {
            if (view_params.exact)
                strong_match(pattern, label_lowercase, score, matches);
            else
                fuzzy_match(wsearch, label_lowercase, score, matches);

            //search in english label
            if (view_params.english &&
                (view_params.exact ? strong_match(pattern, label_english_lowercase, score2, matches2) :
                                     fuzzy_match(wsearch, label_english_lowercase, score2, matches2)) &&
                score2 > score) {
                label   = std::move(label_english);
                matches = std::move(matches2);
                score   = score2;
            }

            //search in opt_key (key is always lowercase)
            if ((view_params.exact ? strong_match(pattern, opt.key, score2, matches2) :
                                     fuzzy_match(wsearch, opt.key, score2, matches2)) &&
                (view_params.exact || score2 > score)) {
                for (fts::pos_type& pos : matches2)
                    pos += label.size() + 1;
                label += L"(" + opt.key + L")";
                append(matches, matches2);
                score = std::max(score, score2);
            }
        }

        //search in tooltip
        size_t find_in_tooltip = std::wstring::npos;
        if (score <= 90) {
            //strong_match(wsearch, opt.tooltip_local, score2, matches2);  //Too slow
            std::wstring tooltip_lowercase = opt.tooltip_local;
            find_in_tooltip = opt.tooltip_local_lowercase.find(wsearch);
            if (find_in_tooltip == std::wstring::npos && view_params.english) {
                find_in_tooltip = opt.tooltip_lowercase.find(wsearch);
            }
        }
        if ((view_params.exact ? score > 10 : score > 90) /*std::numeric_limits<int>::min()*/ ||
            find_in_tooltip != std::wstring::npos) {
            if (score <= 90) {
                score = score > 0
                    ? score + int( (90.-score) * std::min(1., find_in_tooltip / 300.))
                    : int(90. * std::min(1., find_in_tooltip / 300.));
            }
		    label = mark_string(label, matches, opt.type, printer_technology);
            label += L"  [" + std::to_wstring(score) + L"]";// add score value
            if (view_params.all_mode && (opt.tags & current_tags) == 0) {
                label += L" " + _L("tags") + L":{";
                for (AppConfig::Tag& t : Slic3r::GUI::get_app_config()->tags()) {
                    if ((opt.tags & t.tag) == t.tag)
                        label +=  " " + _(t.name);
                }
                label += L"}";
            }
	        std::string label_u8 = into_u8(label);
	        std::string label_plain = label_u8;

#ifdef SUPPORTS_MARKUP
            boost::replace_all(label_plain, std::string(1, char(ImGui::ColorMarkerStart)), "<b>");
            boost::replace_all(label_plain, std::string(1, char(ImGui::ColorMarkerEnd)),   "</b>");
#else
            boost::erase_all(label_plain, std::string(1, char(ImGui::ColorMarkerStart)));
            boost::erase_all(label_plain, std::string(1, char(ImGui::ColorMarkerEnd)));
#endif
	        found.emplace_back(FoundOption{ label_plain, label_u8, boost::nowide::narrow(get_tooltip(opt)), i, score });
        }
    }

    if (!full_list)
        sort_found();
 
    if (search_line != search)
        search_line = search;

    return true;
}

OptionsSearcher::OptionsSearcher()
{
    view_params.category = Slic3r::GUI::get_app_config()->get("search_category") == "1";
    view_params.all_mode = Slic3r::GUI::get_app_config()->get("search_all_mode") == "1";
    view_params.english = Slic3r::GUI::get_app_config()->get("search_english") == "1";
    view_params.exact = Slic3r::GUI::get_app_config()->get("search_exact") == "1";
}

OptionsSearcher::~OptionsSearcher()
{
}

void OptionsSearcher::check_and_update(PrinterTechnology pt_in, ConfigOptionMode tags_in, std::vector<InputInfo> input_values)
{
    if (printer_technology == pt_in && current_tags == tags_in)
        return;

    options.clear();
    sorted = false;

    printer_technology = pt_in;
    current_tags = tags_in;

    for (auto i : input_values)
        if(i.config != nullptr)
            append_options(i.config, i.type);

    for (Option &opt : script_options) {
        if (Preset::get_tech(opt.type))
            options.insert(options.end(), opt);
    }
    

    options.insert(options.end(), preferences_options.begin(), preferences_options.end());

    sort_options();

    search(search_line, true);
}

void OptionsSearcher::append_script_option(const ConfigOptionDef &opt,
                                           Preset::Type       preset_type,
                                           int16_t            idx)
{
    wxString label = opt.full_label;
    if (label.IsEmpty())
        label = opt.label;
    if (label.IsEmpty())
        return;
    wxString tooltip = opt.tooltip;
    wxString tooltip_lc = tooltip;
    tooltip_lc.LowerCase();

    std::string             key = get_key(opt.opt_key, preset_type);
    const GroupAndCategory &gc  = get_group_and_category(key, opt.mode);
    if (gc.group.IsEmpty() && gc.category.IsEmpty())
        return; // have to do ConfigOptionGroup::register_to_search

    script_options.emplace_back(Search::Option{
        boost::nowide::widen(opt.opt_key),
        preset_type,
        idx,
        opt.mode,
        label.ToStdWstring(),
        _(label).ToStdWstring(),
        gc.group.ToStdWstring(),
        _(gc.group).ToStdWstring(),
        gc.category.ToStdWstring(),
        _(gc.category).ToStdWstring(),
        tooltip.ToStdWstring(),
        _(tooltip).ToStdWstring(),
        tooltip_lc.ToStdWstring(),
        _(tooltip_lc).ToStdWstring(),
    });
}
void OptionsSearcher::append_preferences_option(const GUI::Line& opt_line)
{
    Preset::Type type = Preset::TYPE_PREFERENCES;
    wxString label = opt_line.label;
    if (label.IsEmpty())
        return;

    std::string key = get_key(opt_line.get_options().front().opt_id, type);
    assert(groups_and_categories.find(key) != groups_and_categories.end());
    assert(!groups_and_categories[key].empty());
    // it's for TYPE_PREFERENCES, so no mode ?
    const GroupAndCategory& gc = groups_and_categories[key].front(); 
    if (gc.group.IsEmpty() || gc.category.IsEmpty())
        return;        
        
    preferences_options.emplace_back(Search::Option{ boost::nowide::widen(opt_line.get_options().front().opt_id), type, 
                                -1, ConfigOptionMode::comSimpleAE,
                                label.ToStdWstring(), _(label).ToStdWstring(),
                                gc.group.ToStdWstring(), _(gc.group).ToStdWstring(),
                                gc.category.ToStdWstring(), _(gc.category).ToStdWstring() });
}

void OptionsSearcher::append_preferences_options(const std::vector<GUI::Line>& opt_lines)
{
    //Preset::Type type = Preset::TYPE_PREFERENCES;
    for (const GUI::Line& line : opt_lines) {
        if (line.is_separator())
            continue;
        append_preferences_option(line);
        //wxString label = line.label;
        //if (label.IsEmpty())
        //    continue;

        //std::string key = get_key(line.get_options().front().opt_id, type);        
        //const GroupAndCategory& gc = groups_and_categories[key];
        //if (gc.group.IsEmpty() || gc.category.IsEmpty())
        //    continue;        
        //
        //preferences_options.emplace_back(Search::Option{ boost::nowide::widen(key), type,
        //                            -1, ConfigOptionMode::comSimpleAE,
        //                            label.ToStdWstring(), _(label).ToStdWstring(),
        //                            gc.group.ToStdWstring(), _(gc.group).ToStdWstring(),
        //                            gc.category.ToStdWstring(), _(gc.category).ToStdWstring() });
    }
}

const Option& OptionsSearcher::get_option(size_t pos_in_filter) const
{
    assert(pos_in_filter != size_t(-1) && found[pos_in_filter].option_idx != size_t(-1));
    return options[found[pos_in_filter].option_idx];
}

const Option& OptionsSearcher::get_option(const std::string& opt_key, Preset::Type type) const
{
    int16_t idx = -1;
    size_t pos_hash = opt_key.find('#');
    if (pos_hash == std::string::npos) {
        auto it = std::lower_bound(options.begin(), options.end(), Option({ boost::nowide::widen(opt_key), type, idx }));
        assert(it != options.end());
        return options[it - options.begin()];
    } else {
        std::string raw_opt_key = opt_key.substr(0, pos_hash);
        std::string opt_idx = opt_key.substr(pos_hash + 1);
        idx = atoi(opt_idx.c_str());
        auto it = std::lower_bound(options.begin(), options.end(), Option({ boost::nowide::widen(raw_opt_key), type, idx }));
        assert(it != options.end());
        return options[it - options.begin()];
    }

}

Option OptionsSearcher::get_option_names(const std::string& opt_key, Preset::Type type) const
{
    int16_t idx = -1;
    size_t pos_hash = opt_key.find('#');
    std::vector<Search::Option>::const_iterator it;
    if (pos_hash == std::string::npos) {
        it = std::lower_bound(options.begin(), options.end(), Option({ boost::nowide::widen(opt_key), type, idx }));
    } else {
        std::string raw_opt_key = opt_key.substr(0, pos_hash);
        std::string opt_idx = opt_key.substr(pos_hash + 1);
        idx = atoi(opt_idx.c_str());
        it = std::lower_bound(options.begin(), options.end(), Option({ boost::nowide::widen(raw_opt_key), type, idx }));
    }
    if (it != options.end() && it->opt_key_with_idx() == opt_key)
        return *it;
    std::string key = get_key(opt_key, type);
    if (it != options.end() && groups_and_categories.find(key) == groups_and_categories.end()) {
        //TODO check why needed
        size_t pos = key.find('#');
        if (pos == std::string::npos)
            return *it;

        std::string zero_opt_key = key.substr(0, pos + 1) + "0";

        if(groups_and_categories.find(zero_opt_key) == groups_and_categories.end())
            return *it;

        return create_option(opt_key, idx, type, get_group_and_category(zero_opt_key, ConfigOptionMode::comNone));
    }

    const GroupAndCategory& gc = get_group_and_category(key, ConfigOptionMode::comNone);
    if (gc.group.IsEmpty() || gc.category.IsEmpty())
        return *it;

    return create_option(opt_key, idx, type, gc);
}

void OptionsSearcher::show_dialog()
{
    if (!search_dialog) {
        search_dialog = new SearchDialog(this);

        auto parent = search_dialog->GetParent();
        wxPoint pos = parent->ClientToScreen(wxPoint(0, 0));
        pos.x += em_unit(parent) * 40;
        pos.y += em_unit(parent) * 4;

        search_dialog->SetPosition(pos);
    }

    search_dialog->Popup();
}

void OptionsSearcher::dlg_sys_color_changed()
{
    if (search_dialog)
        search_dialog->on_sys_color_changed();
}

void OptionsSearcher::dlg_msw_rescale()
{
    if (search_dialog)
        search_dialog->msw_rescale();
}

void OptionsSearcher::add_key(const std::string& opt_key, Preset::Type type, const wxString& group, const wxString& category, const ConfigOptionDef& gui_opt, bool reset)
{
    std::string key = get_key(opt_key, type);
    auto it = groups_and_categories.find(key);
    if (it == groups_and_categories.end()) {
        groups_and_categories[key] = { GroupAndCategory{group, category, gui_opt} };
    } else {
        //remove all entry from old presets
        if (reset)
            it->second.clear();
        //add new preset (multiple for tags)
        it->second.push_back(GroupAndCategory{ group, category, gui_opt});
    }
}


//------------------------------------------
//          SearchDialog
//------------------------------------------

static const std::map<const char, int> icon_idxs = {
    {ImGui::PrintIconMarker     , 0},
    {ImGui::PrinterIconMarker   , 1},
    {ImGui::PrinterSlaIconMarker, 2},
    {ImGui::FilamentIconMarker  , 3},
    {ImGui::MaterialIconMarker  , 4},
    {ImGui::PreferencesButton   , 5},
};

SearchDialog::SearchDialog(OptionsSearcher* searcher)
    : GUI::DPIDialog(GUI::wxGetApp().tab_panel(), wxID_ANY, _L("Search"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "search"),
    searcher(searcher)
{
    SetFont(GUI::wxGetApp().normal_font());
#if _WIN32
    GUI::wxGetApp().UpdateDarkUI(this);
#elif __WXGTK__
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    default_string = _L("Enter a search term");
    int border = 10;
    int em = em_unit();

    search_line = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    GUI::wxGetApp().UpdateDarkUI(search_line);

    search_list = new wxDataViewCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(em * 70, em * 30), wxDV_NO_HEADER | wxDV_SINGLE
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    );
    GUI::wxGetApp().UpdateDarkUI(search_list);
    search_list_model = new SearchListModel(this);
    search_list->AssociateModel(search_list_model);

#ifdef __WXMSW__
    search_list->AppendColumn(new wxDataViewColumn("", new BitmapTextRenderer(true, wxDATAVIEW_CELL_INERT), SearchListModel::colIconMarkedText, wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT));
    search_list->GetColumn(SearchListModel::colIconMarkedText)->SetWidth(48  * em_unit());
#else
    search_list->AppendBitmapColumn("", SearchListModel::colIcon);

    wxDataViewTextRenderer* const markupRenderer = new wxDataViewTextRenderer();

#ifdef SUPPORTS_MARKUP
    markupRenderer->EnableMarkup();
#endif

    search_list->AppendColumn(new wxDataViewColumn("", markupRenderer, SearchListModel::colMarkedText, wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT));

    search_list->GetColumn(SearchListModel::colIcon      )->SetWidth(3  * em_unit());
    search_list->GetColumn(SearchListModel::colMarkedText)->SetWidth(40 * em_unit());
#endif

    wxBoxSizer* check_sizer = new wxBoxSizer(wxHORIZONTAL);

    check_category  = new ::CheckBox(this, _L("Category"));
    if (GUI::wxGetApp().is_localized())
        check_english   = new ::CheckBox(this, _L("Search in English"));
    check_exact = new wxCheckBox(this, wxID_ANY, _L("Exact pattern"));
    check_all_mode = new wxCheckBox(this, wxID_ANY, _L("All tags"));

    wxStdDialogButtonSizer* cancel_btn = this->CreateStdDialogButtonSizer(wxCANCEL);
    GUI::wxGetApp().UpdateDarkUI(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)));

    check_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Use for search") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);
    check_sizer->Add(check_category, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);
    if (check_english)
        check_sizer->Add(check_english, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);
    check_sizer->Add(check_exact, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);;
    check_sizer->Add(check_all_mode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, border);
    check_sizer->AddStretchSpacer(border);
    check_sizer->Add(cancel_btn,     0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    topSizer->Add(search_line, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(search_list, 1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(check_sizer, 0, wxEXPAND | wxALL, border);

    search_line->Bind(wxEVT_TEXT,    &SearchDialog::OnInputText, this);
    search_line->Bind(wxEVT_LEFT_UP, &SearchDialog::OnLeftUpInTextCtrl, this);
    // process wxEVT_KEY_DOWN to navigate inside search_list, if ArrowUp/Down was pressed
    search_line->Bind(wxEVT_KEY_DOWN,&SearchDialog::OnKeyDown, this);

    search_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, &SearchDialog::OnSelect,    this);
    search_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,    &SearchDialog::OnActivate,  this);
#ifdef __WXMSW__
    search_list->GetMainWindow()->Bind(wxEVT_MOTION,    &SearchDialog::OnMotion,    this);
    search_list->GetMainWindow()->Bind(wxEVT_LEFT_DOWN, &SearchDialog::OnLeftDown, this);
#endif //__WXMSW__

    // Under OSX mouse and key states didn't fill after wxEVT_DATAVIEW_SELECTION_CHANGED call
    // As a result, we can't to identify what kind of actions was done
    // So, under OSX is used OnKeyDown function to navigate inside the list
#ifdef __APPLE__
    search_list->Bind(wxEVT_KEY_DOWN, &SearchDialog::OnKeyDown, this);
#endif

    check_category->Bind(wxEVT_CHECKBOX, &SearchDialog::OnCheck, this);
    if (check_english)
        check_english->Bind(wxEVT_CHECKBOX, &SearchDialog::OnCheck, this);
    check_exact->Bind(wxEVT_CHECKBOX, &SearchDialog::OnCheck, this);
    check_all_mode->Bind(wxEVT_CHECKBOX, &SearchDialog::OnCheck, this);

//    Bind(wxEVT_MOTION, &SearchDialog::OnMotion, this);
    Bind(wxEVT_LEFT_DOWN, &SearchDialog::OnLeftDown, this);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);
}

SearchDialog::~SearchDialog()
{
    if (search_list_model)
        search_list_model->DecRef();
}

void SearchDialog::Popup(wxPoint position /*= wxDefaultPosition*/)
{
    const std::string& line = searcher->search_string();
    search_line->SetValue(line.empty() ? default_string : from_u8(line));
    search_line->SetFocus();
    search_line->SelectAll();

    update_list();

    const OptionViewParameters& params = searcher->view_params;
    check_category->SetValue(params.category);
    if (check_english)
        check_english->SetValue(params.english);
    check_exact->SetValue(params.exact);;
    check_all_mode->SetValue(params.all_mode);

    if (position != wxDefaultPosition)
        this->SetPosition(position);
    this->ShowModal();
}

void SearchDialog::ProcessSelection(wxDataViewItem selection)
{
    if (!selection.IsOk())
        return;
    this->EndModal(wxID_CLOSE);

    // If call GUI::wxGetApp().sidebar.jump_to_option() directly from here,
    // then mainframe will not have focus and found option will not be "active" (have cursor) as a result
    // SearchDialog have to be closed and have to lose a focus
    // and only after that jump_to_option() function can be called
    // So, post event to plater: 
    wxCommandEvent event(wxCUSTOMEVT_JUMP_TO_OPTION);
    event.SetInt(search_list_model->GetRow(selection));
    wxPostEvent(GUI::wxGetApp().plater(), event);
}

void SearchDialog::OnInputText(wxCommandEvent&)
{
    wxString input_string = search_line->GetValue();
    if (input_string == default_string)
        input_string.Clear();

    searcher->search(into_u8(input_string));

    update_list();
}

void SearchDialog::OnLeftUpInTextCtrl(wxEvent& event)
{
    if (search_line->GetValue() == default_string)
        search_line->SetValue("");

    event.Skip();
}

void SearchDialog::OnKeyDown(wxKeyEvent& event)
{
    int key = event.GetKeyCode();

    // change selected item in the list
    if (key == WXK_UP || key == WXK_DOWN)
    {
        // So, for the next correct navigation, set focus on the search_list
        search_list->SetFocus();

        auto item = search_list->GetSelection();

        if (item.IsOk()) {
            unsigned selection = search_list_model->GetRow(item);

            if (key == WXK_UP && selection > 0)
                selection--;
            if (key == WXK_DOWN && selection < unsigned(search_list_model->GetCount() - 1))
                selection++;

            prevent_list_events = true;
            search_list->Select(search_list_model->GetItem(selection));
            prevent_list_events = false;
        }
    }
    // process "Enter" pressed
    else if (key == WXK_NUMPAD_ENTER || key == WXK_RETURN)
        ProcessSelection(search_list->GetSelection());
    else
        event.Skip(); // !Needed to have EVT_CHAR generated as well
}

void SearchDialog::OnActivate(wxDataViewEvent& event)
{
    ProcessSelection(event.GetItem());
}

void SearchDialog::OnSelect(wxDataViewEvent& event)
{
    // To avoid selection update from Select() under osx
    if (prevent_list_events)
        return;    

    // Under OSX mouse and key states didn't fill after wxEVT_DATAVIEW_SELECTION_CHANGED call
    // As a result, we can't to identify what kind of actions was done
    // So, under OSX is used OnKeyDown function to navigate inside the list
#ifndef __APPLE__
    // wxEVT_DATAVIEW_SELECTION_CHANGED is processed, when selection is changed after mouse click or press the Up/Down arrows
    // But this two cases should be processed in different way:
    // Up/Down arrows   -> leave it as it is (just a navigation)
    // LeftMouseClick   -> call the ProcessSelection function  
    if (wxGetMouseState().LeftIsDown())
#endif //__APPLE__
        ProcessSelection(search_list->GetSelection());
}

void SearchDialog::update_list()
{
    // Under OSX model->Clear invoke wxEVT_DATAVIEW_SELECTION_CHANGED, so
    // set prevent_list_events to true already here 
    prevent_list_events = true;
    search_list_model->Clear();

    const std::vector<FoundOption>& filters = searcher->found_options();
    for (const FoundOption& item : filters)
        search_list_model->Prepend(item.label);

    // select first item, if search_list
    if (search_list_model->GetCount() > 0)
        search_list->Select(search_list_model->GetItem(0));
    prevent_list_events = false;
}

void SearchDialog::OnCheck(wxCommandEvent& event)
{
    OptionViewParameters& params = searcher->view_params;
    if (check_english) {
        params.english = check_english->GetValue();
        Slic3r::GUI::get_app_config()->set("search_english", params.english ? "1" : "0");
    }
    params.category = check_category->GetValue();
    Slic3r::GUI::get_app_config()->set("search_category", params.category ? "1" : "0");
    params.exact = check_exact->GetValue();
    Slic3r::GUI::get_app_config()->set("search_exact", params.exact ? "1" : "0");
    params.all_mode = check_all_mode->GetValue();
    Slic3r::GUI::get_app_config()->set("search_all_mode", params.all_mode ? "1" : "0");

    searcher->search();
    update_list();
}

void SearchDialog::OnMotion(wxMouseEvent& event)
{
    wxDataViewItem    item;
    wxDataViewColumn* col;
    wxWindow* win = this;
#ifdef __WXMSW__
    win = search_list;
#endif
    search_list->HitTest(wxGetMousePosition() - win->GetScreenPosition(), item, col);
    search_list->Select(item);

    event.Skip();
}

void SearchDialog::OnLeftDown(wxMouseEvent& event)
{
    ProcessSelection(search_list->GetSelection());
}

void SearchDialog::msw_rescale()
{
    const int& em = em_unit();
#ifdef __WXMSW__
    search_list->GetColumn(SearchListModel::colIconMarkedText)->SetWidth(48  * em);
#else
    search_list->GetColumn(SearchListModel::colIcon      )->SetWidth(3  * em);
    search_list->GetColumn(SearchListModel::colMarkedText)->SetWidth(45 * em);
#endif
    const wxSize& size = wxSize(40 * em, 30 * em);
    SetMinSize(size);

    Fit();
    Refresh();
}

void SearchDialog::on_sys_color_changed()
{
#ifdef _WIN32
    GUI::wxGetApp().UpdateAllStaticTextDarkUI(this);
    GUI::wxGetApp().UpdateDarkUI(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)), true);
    for (wxWindow* win : std::vector<wxWindow*> {search_line, search_list, check_category, check_english})
        if (win) GUI::wxGetApp().UpdateDarkUI(win);
#endif

    // msw_rescale updates just icons, so use it
    search_list_model->sys_color_changed();

    Refresh();
}

// ----------------------------------------------------------------------------
// SearchListModel
// ----------------------------------------------------------------------------

SearchListModel::SearchListModel(wxWindow* parent) : wxDataViewVirtualListModel(0)
{
    int icon_id = 0;
    for (const std::string icon : { "cog", "printer", "sla_printer", "spool", "resin", "notification_preferences" })
        m_icon[icon_id++] = ScalableBitmap(parent, icon);    
}

void SearchListModel::Clear()
{
    m_values.clear();
    Reset(0);
}

void SearchListModel::Prepend(const std::string& label)
{
    const char icon_c = label.at(0);
    wxString   str    = from_u8(label).Remove(0, 1);

    int        icon_idx = 0; 
    if(auto it = icon_idxs.find(icon_c); it != icon_idxs.end())
        icon_idx = it->second;
    m_values.emplace_back(str, icon_idx);

    RowPrepended();
}

void SearchListModel::sys_color_changed()
{
    for (ScalableBitmap& bmp : m_icon)
        bmp.sys_color_changed();
}

wxString SearchListModel::GetColumnType(unsigned int col) const 
{
#ifdef __WXMSW__
    if (col == colIconMarkedText)
        return "DataViewBitmapText";
#else
    if (col == colIcon)
        return "wxBitmap";
#endif
    return "string";
}

void SearchListModel::GetValueByRow(wxVariant& variant,
    unsigned int row, unsigned int col) const
{
    switch (col)
    {
#ifdef __WXMSW__
    case colIconMarkedText: {
        const ScalableBitmap& icon = m_icon[m_values[row].second];
        variant << DataViewBitmapText(m_values[row].first, icon.bmp().GetBitmapFor(icon.parent()));
        break;
    }
#else
    case colIcon: 
        variant << m_icon[m_values[row].second].bmp().GetBitmapFor(m_icon[m_values[row].second].parent());
        break;
    case colMarkedText:
        variant = m_values[row].first;
        break;
#endif
    case colMax:
        wxFAIL_MSG("invalid column");
    default:
        break;
    }
}


}

}    // namespace Slic3r::GUI
