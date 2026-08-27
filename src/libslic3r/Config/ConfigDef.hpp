///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Filip Sykala @Jony01, David Kocík @kocikdav, Tomáš Mészáros @tamasmeszaros, Vojtěch Král @vojtechkral
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Greg Thornton @xdissent
///|/ Copyright (c) 2014 Kamil Kwolek
///|/
///|/ ported from lib/Slic3r/Config.pm:
///|/ Copyright (c) Prusa Research 2016 - 2022 Vojtěch Bubník @bubnikv
///|/ Copyright (c) 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) Slic3r 2011 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/ Copyright (c) 2012 Mark Hindess
///|/ Copyright (c) 2012 Josh McCullough
///|/ Copyright (c) 2011 - 2012 Michael Moon
///|/ Copyright (c) 2012 Simon George
///|/ Copyright (c) 2012 Johannes Reinhardt
///|/ Copyright (c) 2011 Clarence Risher
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ConfigDef_hpp_
#define slic3r_ConfigDef_hpp_

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/property_tree/ptree_fwd.hpp>

#include "libslic3r/Api/plugin/c/slic3r_slicing_step.h"
#include "ConfigOption.hpp"
#include "libslic3r/clonable_ptr.hpp"

namespace Slic3r {

enum ForwardCompatibilitySubstitutionRule {
    // Disable susbtitution, throw exception if an option value is not recognized.
    Disable,
    // Enable substitution of an unknown option value with default. Log the substitution.
    Enable,
    // Enable substitution of an unknown option value with default. Don't log the substitution.
    EnableSilent,
    // Enable substitution of an unknown option value with default. Log substitutions in user profiles, don't log
    // substitutions in system profiles.
    EnableSystemSilent,
    // Enable silent substitution of an unknown option value with default when loading user profiles. Throw on an
    // unknown option value in a system profile.
    EnableSilentDisableSystem,
};

enum class ConfigOptionContainerType : uint8_t {
    None = 0,
    Project,
    Plater,
    Object,
    Layer,
    Region,
};

class ConfigOptionDef;

// For forward definition of ConfigOption in ConfigOptionUniquePtr, we have to define a custom deleter.
struct ConfigOptionDeleter
{
    void operator()(ConfigOption *p);
};
using ConfigOptionUniquePtr = std::unique_ptr<ConfigOption, ConfigOptionDeleter>;

// When parsing a configuration value, if the old_value is not understood by this PrusaSlicer version,
// it is being substituted with some default value that this PrusaSlicer could work with.
// This structure serves to inform the user about the substitutions having been done during file import.
struct ConfigSubstitution
{
    const ConfigOptionDef *opt_def{nullptr};
    std::string old_name; // for when opt_def is nullptr (option not defined in this version)
    std::string old_value;
    ConfigOptionUniquePtr new_value;
    ConfigSubstitution() = default;
    ConfigSubstitution(const ConfigOptionDef *def, std::string old, ConfigOptionUniquePtr &&new_v);
    ConfigSubstitution(std::string bad_key, std::string value)
        : opt_def(nullptr), old_name(bad_key), old_value(value), new_value() {}
};

using ConfigSubstitutions = std::vector<ConfigSubstitution>;

// Filled in by ConfigBase::set_deserialize_raw(), which based on "rule" either bails out
// or performs substitutions when encountering an unknown configuration value.
struct ConfigSubstitutionContext
{
    ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule rl) : rule(rl) {}

    ForwardCompatibilitySubstitutionRule rule;

    bool empty() const throw() { return m_substitutions.empty(); }
    const ConfigSubstitutions &get() const { return m_substitutions; }
    ConfigSubstitutions data() && { return std::move(m_substitutions); }
    void add(ConfigSubstitution &&substitution) { m_substitutions.push_back(std::move(substitution)); }
    void emplace(std::string &&key, std::string &&value) {
        m_substitutions.emplace_back(std::move(key), std::move(value));
    }
    void emplace(const ConfigOptionDef *def, std::string &&old_value, ConfigOptionUniquePtr &&new_v) {
        m_substitutions.emplace_back(def, std::move(old_value), std::move(new_v));
    }
    void clear() { m_substitutions.clear(); }
    void sort_and_remove_duplicates();
    std::optional<ConfigSubstitution> find(const std::string &old_name);
    bool erase(std::string old_name);

private:
    ConfigSubstitutions m_substitutions;
};

// Definition of values / labels for a combo box.
// Mostly used for closed enums (when type == coEnum), but may be used for 
// open enums with ints resp. floats, if gui_type is set to GUIType::i_enum_open" resp. GUIType::f_enum_open.
class ConfigOptionEnumDef {
public:
    bool                            has_values() const { return ! m_values.empty(); }
    bool                            has_labels() const { return ! m_labels.empty(); }
    const std::vector<std::string>& values() const { return m_values; }
    // idx is a value of an index in the combobox in the gui, not an enum value. Use enum_to_index before.
    const std::string&              value(int idx) const { return m_values[idx]; }
    // Used for open enums (gui_type is set to GUIType::i_enum_open" resp. GUIType::f_enum_open).
    // If values not defined, use labels.
    const std::vector<std::string>& enums() const { 
        assert(this->is_valid_open_enum());
        return this->has_values() ? m_values : m_labels;
    }
    // Used for closed enums. If labels are not defined, use values instead.
    const std::vector<std::string>& labels() const { return this->has_labels() ? m_labels : m_values; }
    const std::string&              label(int idx) const { return this->labels()[idx]; }

    // Look up a closed enum value of this combo box based on an index of the combo box value / label.
    // Such a mapping should always succeed.
    int index_to_enum(int index) const;

    // Look up an index of value / label of this combo box based on enum value. 
    // Such a mapping may fail, thus an optional is returned.
    std::optional<int> enum_to_index(int enum_val) const;

    // Look up an index of value / label of this combo box based on value string.
    // strval = value(idx) <=> idx = value_to_index(strval)
    std::optional<int> value_to_index(const std::string &value) const;

    // Look up an index of label of this combo box. Used for open enums.
    std::optional<int> label_to_index(const std::string &value) const;

    std::optional<std::reference_wrapper<const std::string>> enum_to_value(int enum_val) const;

    std::optional<std::reference_wrapper<const std::string>> enum_to_label(int enum_val) const;
    
    //should be only used for debugging, but the script executor uses it to know how to read/write into the enum_def
    bool is_valid_closed_enum() const;
#ifndef NDEBUG
    bool is_valid_open_enum() const;
#endif // NDEBUG

    void                    clear();

    ConfigOptionEnumDef *clone() const;

private:
    friend ConfigDef;
    friend ConfigOptionDef;

    // Only allow ConfigOptionEnumDef() to be created from ConfigOptionDef.
    ConfigOptionEnumDef() = default;
    ConfigOptionEnumDef(const ConfigOptionEnumDef &other);

    void set_values(const std::vector<std::string> &v);
    void set_values(const std::initializer_list<std::string_view> il);
    void set_values(const std::initializer_list<std::pair<std::string_view, std::string_view>> il);
    void set_values(const std::vector<std::pair<std::string, std::string>> il);
    void set_labels(const std::initializer_list<std::string_view> il);
    void finalize_closed_enum();

    std::vector<std::string>        m_values;
    std::vector<std::string>        m_labels;
    // If true, then enum_values are sorted and they contain all the values, thus the UI element ordinary
    // to enum value could be converted directly.
    bool                            m_values_ordinary { false };

    template<typename EnumType>
    void set_enum_map()
    {
        m_enum_names    = &ConfigOptionEnum<EnumType>::get_enum_names();
        m_enum_keys_map = &ConfigOptionEnum<EnumType>::get_enum_values();
    }

    // For enums (when type == coEnum). Maps enums to enum names.
    // These are stored in a global static const storage, defined in PrintConfig
    // for scripted widget, there is no hardcoded storage, so m_enum_names is m_values and m_enum_keys_map is m_enum_keys_map_storage_for_script
    // Initialized by ConfigOptionEnum<xxx>::get_enum_names()
    const t_config_enum_names*  m_enum_names{ nullptr };
    // For enums (when type == coEnum). Maps enum_values to enums.
    // Initialized by ConfigOptionEnum<xxx>::get_enum_values()
    const t_config_enum_values* m_enum_keys_map{ nullptr };
    std::shared_ptr<t_config_enum_values> m_enum_keys_map_storage_for_script{ nullptr };
};

// Definition of a configuration value for the purpose of GUI presentation, editing, value mapping and config file handling.
class ConfigOptionDef
{
public:
    enum class GUIType {
        // closed enum
        undefined,
        // Open enums, integer value could be one of the enumerated values or something else.
        i_enum_open,
        // Open enums, float value could be one of the enumerated values or something else.
        f_enum_open,
        // Open enums, string value could be one of the enumerated values or something else.
        select_open,
        // Color picker, string value.
        color,
        // Currently unused.
        slider,
        // Static text
        legend,
        // Vector value, but edited as a single string.
        // one_string,// it's now the default for vector without any idx. If you want to edit the first value, set the idx to 0
        // Close parameter, string value could be one of the list values.
        select_close,
    };
    static bool is_gui_type_enum_open(const GUIType gui_type) 
        { return gui_type == ConfigOptionDef::GUIType::i_enum_open || gui_type == ConfigOptionDef::GUIType::f_enum_open || gui_type == ConfigOptionDef::GUIType::select_open; }

	// Identifier of this option. It is stored here so that it is accessible through the by_serialization_key_ordinal map.
	t_config_option_key 				opt_key;
    // What type? bool, int, string etc.
    ConfigOptionType                    type            = coNone;
	// If a type can be disabled (beforeit was nullable be that was a whole circus to make it works and it's dumb concept for "is enbaled": a flag is better)
	bool								can_be_disabled = false;
    // if a setting can be disabled, and is enabled, it won't be serialized, or kept.
	bool								is_optional = false;
    // Default value of this option. The default value object is owned by ConfigDef, it is released in its destructor.
    Slic3r::clonable_ptr<const ConfigOption> default_value;
    void 								set_default_value(ConfigOption* ptr);
    void 								set_default_value(ConfigOptionVectorBase* ptr);
    template<typename T> const T* 		get_default_value() const { return static_cast<const T*>(this->default_value.get()); }

    // Create an empty option to be used as a base for deserialization of DynamicConfig.
    ConfigOption*						create_empty_option() const;
    // Create a default option to be inserted into a DynamicConfig.
    ConfigOption*						create_default_option() const;

    bool                                is_scalar()     const { return (int(this->type) & int(coVectorType)) == 0; }

    template<class Archive> ConfigOption* load_option_from_archive(Archive &archive) const {
        ConfigOption* option;
		switch (this->type) {
        case coFloat:           { auto opt = new ConfigOptionFloat();           archive(*opt);
            assert(this->can_be_disabled == opt->can_be_disabled());
            if (this->can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coFloats:          { auto opt = new ConfigOptionFloats();          archive(*opt);
            assert(this->can_be_disabled == opt->can_be_disabled());
            assert(this->is_vector_extruder == opt->is_extruder_size());
            opt->set_is_extruder_size(this->is_vector_extruder); if (this->can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coInt:             { auto opt = new ConfigOptionInt();             archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coInts:            { auto opt = new ConfigOptionInts();            archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coString:          { auto opt = new ConfigOptionString();          archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coStrings:         { auto opt = new ConfigOptionStrings();         archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coPercent:         { auto opt = new ConfigOptionPercent();         archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coPercents:        { auto opt = new ConfigOptionPercents();        archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coFloatOrPercent:  { auto opt = new ConfigOptionFloatOrPercent();  archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coFloatsOrPercents:{ auto opt = new ConfigOptionFloatsOrPercents();archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coPoint:           { auto opt = new ConfigOptionPoint();           archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coPoints:          { auto opt = new ConfigOptionPoints();          archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coPoint3:          { auto opt = new ConfigOptionPoint3();          archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coGraph:           { auto opt = new ConfigOptionGraph();           archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coGraphs:          { auto opt = new ConfigOptionGraphs();          archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coBool:            { auto opt = new ConfigOptionBool();            archive(*opt); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
        case coBools:           { auto opt = new ConfigOptionBools();           archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); if (can_be_disabled) opt->set_can_be_disabled(); return opt; }
		case coEnum:            { auto opt = new ConfigOptionEnumGeneric(this->enum_def->m_enum_keys_map); archive(*opt); return opt; }
		default:                throw ConfigurationError(std::string("ConfigOptionDef::load_option_from_archive(): Unknown option type for option ") + this->opt_key);
		}
	}

    template<class Archive> ConfigOption* save_option_to_archive(Archive &archive, const ConfigOption *opt) const {
		switch (this->type) {
		case coFloat:           archive(*static_cast<const ConfigOptionFloat*>(opt));  			break;
		case coFloats:          archive(*static_cast<const ConfigOptionFloats*>(opt)); 			break;
		case coInt:             archive(*static_cast<const ConfigOptionInt*>(opt)); 	 		break;
		case coInts:            archive(*static_cast<const ConfigOptionInts*>(opt)); 	 		break;
		case coString:          archive(*static_cast<const ConfigOptionString*>(opt)); 			break;
		case coStrings:         archive(*static_cast<const ConfigOptionStrings*>(opt)); 		break;
		case coPercent:         archive(*static_cast<const ConfigOptionPercent*>(opt)); 		break;
		case coPercents:        archive(*static_cast<const ConfigOptionPercents*>(opt)); 		break;
		case coFloatOrPercent:  archive(*static_cast<const ConfigOptionFloatOrPercent*>(opt));	break;
		case coFloatsOrPercents:archive(*static_cast<const ConfigOptionFloatsOrPercents*>(opt));break;
		case coPoint:           archive(*static_cast<const ConfigOptionPoint*>(opt)); 			break;
		case coPoints:          archive(*static_cast<const ConfigOptionPoints*>(opt)); 			break;
		case coPoint3:          archive(*static_cast<const ConfigOptionPoint3*>(opt)); 			break;
		case coGraph:           archive(*static_cast<const ConfigOptionGraph*>(opt)); 			break;
		case coGraphs:          archive(*static_cast<const ConfigOptionGraphs*>(opt)); 			break;
		case coBool:            archive(*static_cast<const ConfigOptionBool*>(opt)); 			break;
		case coBools:           archive(*static_cast<const ConfigOptionBools*>(opt)); 			break;
		case coEnum:            archive(*static_cast<const ConfigOptionEnumGeneric*>(opt)); 	break;
		default:                throw ConfigurationError(std::string("ConfigOptionDef::save_option_to_archive(): Unknown option type for option ") + this->opt_key);
		}
		// Make the compiler happy, shut up the warnings.
		return nullptr;
	}

    // Usually empty. 
    // Special values - "i_enum_open", "f_enum_open" to provide combo box for int or float selection,
    // "select_open" - to open a selection dialog (currently only a serial port selection).
    GUIType                             gui_type { GUIType::undefined };
    bool                                is_gui_type_enum_open() const { return is_gui_type_enum_open(this->gui_type); }
    // Usually empty. Otherwise "serialized" or "show_value"
    // The flags may be combined.
    // "serialized" - vector valued option is entered in a single edit field. Values are separated by a semicolon.
    // "show_value" - even if enum_values / enum_labels are set, still display the value, not the enum label.
    std::string                         gui_flags;
    // Label of the GUI input field.
    // In case the GUI input fields are grouped in some views, the label defines a short label of a grouped value,
    // while full_label contains a label of a stand-alone field.
    // The full label is shown, when adding an override parameter for an object or a modified object.
    std::string                         label;
    std::string                         full_label;
    // Gettext catalog of all user-facing text owned by this definition. An
    // empty value keeps the historic application catalog lookup.
    std::string                         translation_domain;
    std::string                         get_full_label() const { return !full_label.empty() ? full_label : label; }
    // With which printer technology is this configuration valid?
    PrinterTechnology                   printer_technology = ptUnknown;
    // Where plugin-created static options shall store their actual value.
    ConfigOptionContainerType           container_type = ConfigOptionContainerType::None;
    // Preset/UI bucket for plugin-created options. Stored as an ABI-neutral integer.
    uint32_t                            option_preset_type = 0;
    // Earliest slicing step invalidated by this option when it changes.
    // STEP_ANY means unknown; invalidate the full slicing state.
    slicing_step_t                      invalidates_step = STEP_ANY;
    // Category of a configuration field, from the GUI perspective.
    OptionCategory                      category        = OptionCategory::none;
    // A tooltip text shown in the GUI.
    std::string                         tooltip;
    // Text right from the input field, usually a unit of measurement.
    std::string                         sidetext;
    // Format of this parameter on a command line.
    std::string                         cli;
    // Set for type == coFloatOrPercent.
    // It provides a link to a configuration value, of which this option provides a ratio.
    // For example, 
    // For example external_perimeter_speed may be defined as a fraction of perimeter_speed.
    t_config_option_key                 ratio_over;
    // True for multiline strings.
    bool                                multiline       = false;
    // For text input: If true, the GUI text box spans the complete page width.
    bool                                full_width      = false;
    // For text input: If true, the GUI formats text as code (fixed-width)
    bool                                is_code         = false;
    // For array setting: If true, It has the same size as the number of extruders.
    bool                                is_vector_extruder = false;
    // Not editable. Currently only used for the display of the number of threads.
    bool                                readonly        = false;
    // Can be phony. if not present at laoding, mark it as phony. Also adapt the gui to look for phony status.
    bool                                can_phony       = false;
    // Height of a multiline GUI text box.
    int                                 height          = -1;
    // Optional width of an input field.
    int                                 width           = -1;
    // Optional label width of the label (if in a line).
    int                                 label_width     = -1;
    // Optional label alignement to the left instead of the right
    bool                                aligned_label_left = false;
    // Optional label width of the sidetext (if in a line).
    int                                 sidetext_width  = -1;
    // <min, max> limit of a numeric input.
    // If not set, the <min, max> is set to <INT_MIN, INT_MAX>
    // By setting min=0, only nonnegative input is allowed.
    double                              min             = -FLT_MAX;
    double                              max             =  FLT_MAX;
    // To check if it's not a typo and a % is missing. Ask for confirmation if the value is higher than that.
    // if negative, if it's lower than the opposite.
    // if percentage, multiply by the nozzle_diameter.
    FloatOrPercent                      max_literal     = FloatOrPercent{ 0., false };
    // max precision after the dot, only for display
    int                                 precision       = 6;
    // flags for which it can appear (64b flags)
    ConfigOptionMode                    mode            = comNone;
    // Legacy names for this configuration option.
    // Used when parsing legacy configuration file.
    std::vector<t_config_option_key>    aliases;
    // Sometimes a single value may well define multiple values in a "beginner" mode.
    // Currently used for aliasing "solid_layers" to "top_solid_layers", "bottom_solid_layers".
    std::vector<t_config_option_key>    shortcut;
    
    // Initialized by ConfigOptionEnum<xxx>::get_enum_values()
    std::shared_ptr<GraphSettings>      graph_settings;

    // for scripted gui widgets
    // true if it's not a real option but a simplified/composite one that use angelscript for interaction.
    bool                                is_script = false;
    boost::any                          default_script_value;
    // list of opt_key#idx strings that changes our computed value
    std::vector<std::string>            depends_on; // from Option

    // Definition of values / labels for a combo box.
    Slic3r::clonable_ptr<ConfigOptionEnumDef> enum_def;

protected:
    // Don't let these methods avaialable: the gui type isn't checked!

    void set_enum_values(const std::vector<std::string> il);

    void set_enum_values(const std::initializer_list<std::string_view> il);

    void set_enum_values(const std::initializer_list<std::pair<std::string_view, std::string_view>> il);

    void set_enum_values(const std::vector<std::pair<std::string, std::string>> il);

    template<typename Values, typename Labels>
    void set_enum_values(Values &&values, Labels &&labels) {
        this->enum_def_new();
        enum_def->set_values(std::move(values));
        enum_def->set_labels(std::move(labels));
    }

public:
    void set_enum_values(GUIType gui_type, const std::initializer_list<std::string_view> il);

    void set_enum_as_closed_for_scripted_enum(const std::vector<std::pair<std::string, std::string>> il);

    void set_enum_values(GUIType gui_type, const std::initializer_list<std::pair<std::string_view, std::string_view>> il);

    void set_enum_values(GUIType gui_type, const std::vector<std::pair<std::string, std::string>> il);

    void set_enum_values(GUIType gui_type, const std::vector<std::string> il);

    void set_enum_labels(GUIType gui_type, const std::initializer_list<std::string_view> il);

    template<typename EnumType>
    void set_enum(std::initializer_list<std::string_view> il) {
        this->set_enum_values(il);
        enum_def->set_enum_map<EnumType>();
    }

    template<typename EnumType>
    void set_enum(std::initializer_list<std::pair<std::string_view, std::string_view>> il) {
        this->set_enum_values(il);
        enum_def->set_enum_map<EnumType>();
    }

    template<typename EnumType>
    void set_enum(const std::vector<std::pair<std::string, std::string>> &il) {
        this->set_enum_values(il);
        enum_def->set_enum_map<EnumType>();
    }

    template<typename EnumType, typename Values, typename Labels>
    void set_enum(Values &&values, Labels &&labels) {
        this->set_enum_values(std::move(values), std::move(labels));
        enum_def->set_enum_map<EnumType>();
    }

    template<typename EnumType, typename Values>
    void set_enum(Values &&values, const std::initializer_list<std::string_view> labels) {
        this->set_enum_values(std::move(values), labels);
        enum_def->set_enum_map<EnumType>();
    }

    bool has_enum_value(const std::string &value) const;

    // 0 is an invalid key.
    size_t 								serialization_key_ordinal = 0;

    // Returns the alternative CLI arguments for the given option.
    // If there are no cli arguments defined, use the key and replace underscores with dashes.
    std::vector<std::string> cli_args(const std::string &key) const;

    // Assign this key to cli to disable CLI for this option.
    static const constexpr char *nocli =  "~~~noCLI";

    static std::map<std::string, ConfigOptionMode> names_2_tag_mode;
    //static void init_mode();

private:
    void enum_def_new();
};

bool operator<(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw();
bool operator==(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw();

// Map from a config option name to its definition.
// The definition does not carry an actual value of the config option, only its constant default value.
// t_config_option_key is std::string
typedef std::map<t_config_option_key, ConfigOptionDef> t_optiondef_map;

// Definition of configuration values for the purpose of GUI presentation, editing, value mapping and config file handling.
// The configuration definition is static: It does not carry the actual configuration values,
// but it carries the defaults of the configuration values.
class ConfigDef
{
public:
    t_optiondef_map         					options;
    std::map<size_t, const ConfigOptionDef*>	by_serialization_key_ordinal;

    bool                    has(const t_config_option_key &opt_key) const { return this->options.count(opt_key) > 0; }
    const ConfigOptionDef*  get(const t_config_option_key &opt_key) const {
        t_optiondef_map::iterator it = const_cast<ConfigDef*>(this)->options.find(opt_key);
        return (it == this->options.end()) ? nullptr : &it->second;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(options.size());
        for(auto const& kvp : options)
            out.push_back(kvp.first);
        return out;
    }
    bool                    empty() const { return options.empty(); }

    // Iterate through all of the CLI options and write them to a stream.
    std::ostream&           print_cli_help(
        std::ostream& out, bool show_defaults, 
        std::function<bool(const ConfigOptionDef &)> filter = [](const ConfigOptionDef &){ return true; }) const;

    bool is_finalized() const { return m_is_finalized; }

    // public for orchestrator
    ConfigOptionDef*        add(const t_config_option_key &opt_key, ConfigOptionType type, PrinterTechnology pt = ptUnknown);
    // Finalize open / close enums, validate everything.
    void                    finalize();
protected:
    bool                    m_is_finalized = false;
};

// A pure interface to resolving ConfigOptions.
// This pure interface is useful as a base of ConfigBase, also it may be overriden to combine 
// various config sources.
class ConfigOptionResolver
{
public:
    ConfigOptionResolver() {}
    virtual ~ConfigOptionResolver() {}

    // Find a ConfigOption instance for a given name.
    virtual const ConfigOption* optptr(const t_config_option_key &opt_key) const = 0;

    bool 						has(const t_config_option_key &opt_key) const { return this->optptr(opt_key) != nullptr; }
    
    const ConfigOption* 		option(const t_config_option_key &opt_key) const { return this->optptr(opt_key); }

    template<typename TYPE>
    const TYPE* 				option(const t_config_option_key& opt_key) const
    {
        const ConfigOption *opt = this->optptr(opt_key);
        const TYPE *opt_type = (opt == nullptr || opt->type() != TYPE::static_type()) ?
            nullptr :
            static_cast<const TYPE *>(opt);
        return opt_type;
    }

    const ConfigOption* 		option_throw(const t_config_option_key& opt_key) const
    {
        const ConfigOption* opt = this->optptr(opt_key);
        if (opt == nullptr)
            throw UnknownOptionException(opt_key);
        return opt;
    }

    template<typename TYPE>
    const TYPE* 				option_throw(const t_config_option_key& opt_key) const
    {
        const ConfigOption* opt = this->option_throw(opt_key);
        if (opt->type() != TYPE::static_type())
            throw BadOptionTypeException("Conversion to a wrong type");
        return static_cast<TYPE*>(opt);
    }
};



// An abstract configuration store.
class ConfigBase : public ConfigOptionResolver
{
public:
    // Definition of configuration values for the purpose of GUI presentation, editing, value mapping and config file handling.
    // The configuration definition is static: It does not carry the actual configuration values,
    // but it carries the defaults of the configuration values.
    
    ConfigBase() = default;
#ifndef _DEBUG
    ~ConfigBase() override = default;
#endif
    // to get to the config more generic than this one, if available
    const ConfigBase* parent = nullptr;

    // Virtual overridables:
public:
    // Static configuration definition. Any value stored into this ConfigBase shall have its definition here.
    // will search in parent definition if not found here.
    virtual const ConfigDef*        def() const = 0;
    // Find ando/or create a ConfigOption instance for a given name.
    using ConfigOptionResolver::optptr;    // won't search in parent definition, as you can't change a parent value
    virtual ConfigOption*           optptr(const t_config_option_key &opt_key, bool create = false) = 0;
    // Collect names of all configuration values maintained by this configuration store.
    virtual t_config_option_keys    keys() const = 0;

protected:
    //// Verify whether the opt_key has not been obsoleted or renamed.
    //// Both opt_key and value may be modified by handle_legacy().
    //// If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    //// handle_legacy() is called internally by set_deserialize().
#ifdef _DEBUGINFO
    virtual void                    handle_legacy(t_config_option_key &/*opt_key*/, std::string &/*value*/) const {}
#endif
    // Verify whether the opt_key has to be converted or isn't present in prusaslicer
    // Both opt_key and value may be modified by to_prusa().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by to_prusa().
    virtual void                    to_prusa(t_config_option_key&/*opt_key*/, std::string&/*value*/) const {}
    // Called after a config is loaded as a whole.
    // Perform composite conversions, for example merging multiple keys into one key.
    // For conversion of single options, the handle_legacy() method above is called.
    virtual void                    handle_legacy_composite(std::map<t_config_option_key, std::string> &opt_deleted) {}

public:
	using ConfigOptionResolver::option;
	using ConfigOptionResolver::option_throw;

    // Non-virtual methods:
    ConfigOption* option(const t_config_option_key &opt_key, bool create = false)
        { return this->optptr(opt_key, create); }
    
    template<typename TYPE>
    TYPE* option(const t_config_option_key &opt_key, bool create = false)
    { 
        ConfigOption *opt = this->optptr(opt_key, create);
        TYPE* opt_type = (opt == nullptr || opt->type() != TYPE::static_type()) ? nullptr : static_cast<TYPE*>(opt);
        return opt_type;
    }

    ConfigOption* option_throw(const t_config_option_key &opt_key, bool create = false)
    { 
        ConfigOption *opt = this->optptr(opt_key, create);
        if (opt == nullptr)
            throw UnknownOptionException(opt_key);
        return opt;
    }
    
    template<typename TYPE>
    TYPE* option_throw(const t_config_option_key &opt_key, bool create = false)
    { 
        ConfigOption *opt = this->option_throw(opt_key, create);
        if (opt->type() != TYPE::static_type())
            throw BadOptionTypeException("Conversion to a wrong type");
        return static_cast<TYPE*>(opt);
    }

    template<class T> T*       opt(const t_config_option_key &opt_key, bool create = false)
        { return dynamic_cast<T*>(this->optptr(opt_key, create)); }
    template<class T> const T* opt(const t_config_option_key &opt_key) const
        { return dynamic_cast<const T*>(this->optptr(opt_key)); }

    // Get definition for a particular option.
    // Returns null if such an option definition does not exist.
    const ConfigOptionDef*           option_def(const t_config_option_key &opt_key) const
        { return this->def()->get(opt_key); }

    // Apply all keys of other ConfigBase defined by this->def() to this ConfigBase.
    // An UnknownOptionException is thrown in case some option keys of other are not defined by this->def(),
    // or this ConfigBase is of a StaticConfig type and it does not support some of the keys, and ignore_nonexistent is not set.
    void apply(const ConfigBase &other, bool ignore_nonexistent = false) { this->apply_only(other, other.keys(), ignore_nonexistent); }
    // Apply explicitely enumerated keys of other ConfigBase defined by this->def() to this ConfigBase.
    // An UnknownOptionException is thrown in case some option keys are not defined by this->def(),
    // or this ConfigBase is of a StaticConfig type and it does not support some of the keys, and ignore_nonexistent is not set.
    void apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false);
    void apply_only(const ConfigBase &other, const std::set<t_config_option_key> &keys, bool ignore_nonexistent = false);
    // Are the two configs equal? Ignoring options not present in both configs.
    bool equals(const ConfigBase &other) const;
    // Returns options differing in the two configs, ignoring options not present in both configs.
    t_config_option_keys diff(const ConfigBase &other, bool even_phony = true) const;
    // Returns options being equal in the two configs, ignoring options not present in both configs.
    t_config_option_keys equal(const ConfigBase &other) const;
    std::string opt_serialize(const t_config_option_key &opt_key) const;

    // Set a value. Convert numeric types using a C style implicit conversion / promotion model.
    // Throw if option is not avaiable and create is not enabled,
    // or if the conversion is not possible.
    // Conversion to string is always possible.
    void set(const std::string &opt_key, bool  				value, bool create = false)
    	{ this->option_throw<ConfigOptionBool>(opt_key, create)->value = value; }
    void set(const std::string &opt_key, int32_t				value, bool create = false);
    void set(const std::string &opt_key, double				value, bool create = false);
    void set(const std::string &opt_key, const char		   *value, bool create = false)
    	{ this->option_throw<ConfigOptionString>(opt_key, create)->value = value; }
    void set(const std::string &opt_key, const std::string &value, bool create = false)
    	{ this->option_throw<ConfigOptionString>(opt_key, create)->value = value; }

    // Set a configuration value from a string, it will NOT call an overridable handle_legacy(), so please call it before.
    // to resolve renamed and removed configuration keys.
    bool set_deserialize_nothrow(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions, bool append = false);
	// May throw BadOptionTypeException() if the operation fails.
    void set_deserialize(const t_config_option_key &opt_key, const std::string &str, ConfigSubstitutionContext& config_substitutions, bool append = false);
    void set_deserialize(const t_config_option_key &opt_key, const std::string &str);
    void set_deserialize_strict(const t_config_option_key &opt_key, const std::string &str, bool append = false)
        { ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable }; this->set_deserialize(opt_key, str, ctxt, append); }
    struct SetDeserializeItem {
        SetDeserializeItem(const char *opt_key, const char *opt_value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const std::string &opt_value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const std::string_view opt_value, bool append = false);
        SetDeserializeItem(const char *opt_key, const bool value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const bool value, bool append = false);
        SetDeserializeItem(const char *opt_key, const int32_t value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const int32_t value, bool append = false);
        SetDeserializeItem(const char *opt_key, const std::initializer_list<int32_t> values, bool append = false);
        SetDeserializeItem(const std::string &opt_key,
                           const std::initializer_list<int32_t> values,
                           bool append = false);
        SetDeserializeItem(const char *opt_key, const float value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const float value, bool append = false);
        SetDeserializeItem(const char *opt_key, const double value, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const double value, bool append = false);
        SetDeserializeItem(const char *opt_key, const std::initializer_list<float> values, bool append = false);
        SetDeserializeItem(const std::string &opt_key, const std::initializer_list<float> values, bool append = false);
        SetDeserializeItem(const char *opt_key, const std::initializer_list<double> values, bool append = false);
        SetDeserializeItem(const std::string &opt_key,
                           const std::initializer_list<double> values,
                           bool append = false);

        std::string opt_key; std::string opt_value; bool append = false;

    private:
        static std::string format(std::initializer_list<int32_t> values);
        static std::string format(std::initializer_list<float> values);
        static std::string format(std::initializer_list<double> values);
    };
	// May throw BadOptionTypeException() if the operation fails.
    void set_deserialize(std::initializer_list<SetDeserializeItem> items, ConfigSubstitutionContext& substitutions);
    void set_deserialize_strict(std::initializer_list<SetDeserializeItem> items)
        { ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable }; this->set_deserialize(items, ctxt); }

    const ConfigOptionDef* get_option_def(const t_config_option_key& opt_key) const;
    double get_computed_value(const t_config_option_key &opt_key, int extruder_id = -1) const;

    std::string&        opt_string(const t_config_option_key &opt_key, bool create = false)     { return this->option<ConfigOptionString>(opt_key, create)->value; }
    const std::string&  opt_string(const t_config_option_key &opt_key) const                    { return const_cast<ConfigBase*>(this)->opt_string(opt_key); }
    std::string&        opt_string(const t_config_option_key &opt_key, size_t idx)              { return this->option<ConfigOptionStrings>(opt_key)->get_at(idx); }
    const std::string&  opt_string(const t_config_option_key &opt_key, size_t idx) const        { return const_cast<ConfigBase*>(this)->opt_string(opt_key, idx); }

    double&             opt_float(const t_config_option_key &opt_key)                           { return this->option<ConfigOptionFloat>(opt_key)->value; }
    const double&       opt_float(const t_config_option_key &opt_key) const                     { return dynamic_cast<const ConfigOptionFloat*>(this->option(opt_key))->value; }
    double&             opt_float(const t_config_option_key &opt_key, size_t idx)               { return this->option<ConfigOptionFloats>(opt_key)->get_at(idx); }
    const double&       opt_float(const t_config_option_key &opt_key, size_t idx) const         { return dynamic_cast<const ConfigOptionFloats*>(this->option(opt_key))->get_at(idx); }

    int32_t&            opt_int(const t_config_option_key &opt_key)                             { return this->option<ConfigOptionInt>(opt_key)->value; }
    int32_t             opt_int(const t_config_option_key &opt_key) const                       { return dynamic_cast<const ConfigOptionInt*>(this->option(opt_key))->value; }
    int32_t&            opt_int(const t_config_option_key &opt_key, size_t idx)                 { return this->option<ConfigOptionInts>(opt_key)->get_at(idx); }
    int32_t             opt_int(const t_config_option_key &opt_key, size_t idx) const           { return dynamic_cast<const ConfigOptionInts*>(this->option(opt_key))->get_at(idx); }
    
    // no dynamic_cast
    bool      get_bool(const t_config_option_key &opt_key, size_t idx = 0) const                 {return this->option(opt_key)->get_bool(idx);}
    int32_t   get_int(const t_config_option_key &opt_key, size_t idx = 0) const                  {return this->option(opt_key)->get_int(idx);}
    double    get_float(const t_config_option_key &opt_key, size_t idx = 0) const                {return this->option(opt_key)->get_float(idx);}
    bool      is_enabled(const t_config_option_key &opt_key, size_t idx = 0) const                {return this->option(opt_key)->is_enabled(idx);}

    // In ConfigManipulation::toggle_print_fff_options, it is called on option with type ConfigOptionEnumGeneric* and also ConfigOptionEnum*.
    // Thus the virtual method getInt() is used to retrieve the enum value.
    template<typename ENUM>
    ENUM                opt_enum(const t_config_option_key &opt_key) const                      { return static_cast<ENUM>(this->option(opt_key)->get_int()); }

    bool                opt_bool(const t_config_option_key &opt_key) const                      { auto opt = this->option<ConfigOptionBool>(opt_key); assert(opt); return opt->value;  }
    bool                opt_bool(const t_config_option_key &opt_key, size_t idx) const          { auto opt = this->option<ConfigOptionBools>(opt_key); assert(opt); return opt->get_at(idx) != 0; }
    //bool&               opt_bool(const t_config_option_key &opt_key)                            { return this->option<ConfigOptionBool>(opt_key)->value; }
    //uint8_t&            opt_bool(const t_config_option_key &opt_key, size_t idx)          { return this->option<ConfigOptionBools>(opt_key)->get_at(idx); }

    void setenv_() const;
    ConfigSubstitutions load(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_ini(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_ini_string(const std::string &data, ForwardCompatibilitySubstitutionRule compatibility_rule);
    // Loading a "will be one day a legacy format" of configuration stored into 3MF or AMF.
    // Accepts the same data as load_from_ini_string(), only with each configuration line possibly prefixed with a semicolon (G-code comment).
    ConfigSubstitutions load_from_ini_string_commented(std::string &&data, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_gcode_file(const std::string &filename, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_binary_gcode_file(const std::string& filename, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load(const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule);
    void save(const std::string &file, bool to_prusa = false) const;
#ifdef _DEBUG
    std::string to_debug_string() const;
#endif

    // Disable all the optional settings.
    void disable_optionals();

    static std::map<t_config_option_key, std::string> load_gcode_string_legacy(const char* str);
    static size_t load_from_gcode_string_legacy(ConfigBase& config, const char* str, ConfigSubstitutionContext& substitutions);

#ifdef _DEBUG
    //little dirty test to be sure it exists (not needed, but it's good for testing)
    int32_t m_exists = 0x55555555;
    bool    exists() { return m_exists == 0x55555555; }
    ~ConfigBase() override { m_exists = 0; }
#endif

private:
    void apply_only(const ConfigBase &other, const t_config_option_key &key, bool ignore_nonexistent = false);
    // Set a configuration value from a string.
    bool set_deserialize_raw(const t_config_option_key& opt_key_src, const std::string& value, ConfigSubstitutionContext& substitutions, bool append);
};

// Configuration store with dynamic number of configuration values.
// In Slic3r, the dynamic config is mostly used at the user interface layer.
class DynamicConfig : public virtual ConfigBase
{
public:
    DynamicConfig() = default;
    DynamicConfig(const DynamicConfig &rhs);
    DynamicConfig(DynamicConfig &&rhs) noexcept;
    explicit DynamicConfig(const ConfigBase &rhs, const t_config_option_keys &keys);
	explicit DynamicConfig(const ConfigBase& rhs) : DynamicConfig(rhs, rhs.keys()) {}
	virtual ~DynamicConfig() override = default;

    // Copy a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig &operator=(const DynamicConfig &rhs);

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig &operator=(DynamicConfig &&rhs) noexcept;

    // Add a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig &operator+=(const DynamicConfig &rhs);

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig &operator+=(DynamicConfig &&rhs);

    bool           operator==(const DynamicConfig &rhs) const;
    bool           operator!=(const DynamicConfig &rhs) const { return ! (*this == rhs); }

    void swap(DynamicConfig &other) { std::swap(this->options, other.options); }

    void clear() { this->options.clear(); }

    bool erase(const t_config_option_key &opt_key);

    // Remove disabled optional options, it does not help to hold them.
    size_t remove_optional_disabled_options();

    // Allow DynamicConfig to be instantiated on ints own without a definition.
    // If the definition is not defined, the method requiring the definition will throw NoDefinitionException.
    const ConfigDef*        def() const override { return nullptr; }
    // Overrides ConfigResolver::optptr().
    const ConfigOption*     optptr(const t_config_option_key &opt_key) const override;
    // Overrides ConfigBase::optptr(). Find ando/or create a ConfigOption instance for a given name.
    ConfigOption*           optptr(const t_config_option_key &opt_key, bool create = false) override;
    // Overrides ConfigBase::keys(). Collect names of all configuration values maintained by this configuration store.
    t_config_option_keys    keys() const override;
    bool                    empty() const { return options.empty(); }

    // Set a value for an opt_key. Returns true if the value did not exist yet.
    // This DynamicConfig will take ownership of opt.
    // Be careful, as this method does not test the existence of opt_key in this->def().
    bool set_key_value(const std::string &opt_key, ConfigOption *opt);

    // Are the two configs equal? Ignoring options not present in both configs and phony fields.
    bool equals(const DynamicConfig &other, bool even_phony =true) const;
    // Returns options differing in the two configs, ignoring options not present in both configs and phony fields.
    t_config_option_keys diff(const DynamicConfig &other, bool even_phony=true) const;
    // Returns options being equal in the two configs, ignoring options not present in both configs.
    t_config_option_keys equal(const DynamicConfig &other) const;

    // Command line processing
    bool                read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys = nullptr);

    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cbegin() const { return options.cbegin(); }
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cend() const {
        return options.cend();
    }
    size_t size() const { return options.size(); }

private:
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>> options;

	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(options); }
};

// Configuration store with a static definition of configuration values.
// In Slic3r, the static configuration stores are during the slicing / g-code generation for efficiency reasons,
// because the configuration values could be accessed directly.
class StaticConfig : public virtual ConfigBase
{
public:
    /// Gets list of config option names for each config option of this->def, which has a static counter-part defined by the derived object
    /// and which could be resolved by this->optptr(key) call.
    t_config_option_keys keys() const;

    /// Set all statically defined config options to their defaults defined by this->def().
    /// used (only) by tests
    void set_defaults();
protected:
    StaticConfig() {}
};

} // namespace Slic3r

#endif
