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
///|/
#ifndef slic3r_Config_hpp_
#define slic3r_Config_hpp_

#include <assert.h>
#include <map>
#include <climits>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>
#include <float.h>
#include "libslic3r.h"
#include "clonable_ptr.hpp"
#include "Exception.hpp"
#include "Point.hpp"

#include <boost/any.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format/format_fwd.hpp>
#include <boost/functional/hash.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include <cereal/access.hpp>
#include <cereal/types/base_class.hpp>

namespace Slic3r {
    struct FloatOrPercent
    {
        double  value;
        bool    percent;

    private:
        friend class cereal::access;
        template<class Archive> void serialize(Archive& ar) { ar(this->value); ar(this->percent); }
    };

    inline bool operator==(const FloatOrPercent& l, const FloatOrPercent& r) noexcept { return l.value == r.value && l.percent == r.percent; }
    inline bool operator!=(const FloatOrPercent& l, const FloatOrPercent& r) noexcept { return !(l == r); }
    inline bool operator< (const FloatOrPercent& l, const FloatOrPercent& r) noexcept { return l.value < r.value || (l.value == r.value && int(l.percent) < int(r.percent)); }
    inline bool operator> (const FloatOrPercent& l, const FloatOrPercent& r) throw() { return l.value > r.value || (l.value == r.value && int(l.percent) > int(r.percent)); }

    struct GraphData
    {
    public:
        enum GraphType : uint8_t {
            SQUARE,
            LINEAR,
            SPLINE,
            COUNT
        };
    
        GraphData() {}
        GraphData(Pointfs graph_data) : graph_points(graph_data) {
            begin_idx = 0;
            end_idx = graph_data.size();
        }
        GraphData(size_t start_idx, size_t stop_idx, Pointfs graph_data)
            : graph_points(graph_data), begin_idx(start_idx), end_idx(stop_idx) {}
        GraphData(size_t start_idx, size_t stop_idx, GraphType graph_type, Pointfs graph_data)
            : graph_points(graph_data), begin_idx(start_idx), end_idx(stop_idx), type(graph_type) {}
    
        bool operator==(const GraphData &rhs) const { return this->data_size() == rhs.data_size() && this->data() == rhs.data() && this->type == rhs.type; }
        bool operator!=(const GraphData &rhs) const { return this->data_size() != rhs.data_size() || this->data() != rhs.data() || this->type != rhs.type; }
        bool operator<(const GraphData &rhs) const;
        bool operator>(const GraphData &rhs) const;
    
        // data is the useable part of the graph
        Pointfs data() const;
        size_t data_size() const;

        double interpolate(double x_value) const;

        //return false if data are not good
        bool validate() const;
        
    //protected:
        Pointfs graph_points = {};
        size_t begin_idx = 0; //included
        size_t end_idx = 0; //excluded
        GraphType type = GraphType::LINEAR;
    
        std::string serialize() const;
        bool deserialize(const std::string &str);

    private:
        friend class cereal::access;
        template<class Archive> void serialize(Archive &ar)
        {
            ar(this->begin_idx);
            ar(this->end_idx);
            ar(this->type);
            // does this works?
            ar(this->graph_points);
        }
    };

    struct GraphSettings
    {
        std::string title;
        std::string description;
        std::string y_label;
        std::string x_label;
        std::string null_label;
        double min_x, max_x, step_x;
        double min_y, max_y, step_y;
        std::string label_min_x;
        std::string label_max_x;
        std::string label_min_y;
        std::string label_max_y;
        std::vector<GraphData::GraphType> allowed_types;
        GraphData reset_vals;
    };
}

namespace std {
    template<> struct hash<Slic3r::FloatOrPercent> {
        std::size_t operator()(const Slic3r::FloatOrPercent& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.value);
            return v.percent ? seed ^ 0x9e3779b9 : seed;
        }
    };
    
    template<> struct hash<Slic3r::GraphData> {
        std::size_t operator()(const Slic3r::GraphData& v) const noexcept {
            std::size_t seed = 0;
            boost::hash_combine(seed, std::hash<double>{}(v.begin_idx));
            boost::hash_combine(seed, std::hash<double>{}(v.end_idx));
            boost::hash_combine(seed, std::hash<double>{}(v.type));
            for (const auto &pt : v.graph_points) {
                boost::hash_combine(seed, std::hash<double>{}(pt.x()));
                boost::hash_combine(seed, std::hash<double>{}(pt.y()));
            }
            return seed;
        }
    };

    template<> struct hash<Slic3r::Vec2d> {
        std::size_t operator()(const Slic3r::Vec2d& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.x());
            boost::hash_combine(seed, std::hash<double>{}(v.y()));
            return seed;
        }
    };

    template<> struct hash<Slic3r::Vec3d> {
        std::size_t operator()(const Slic3r::Vec3d& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.x());
            boost::hash_combine(seed, std::hash<double>{}(v.y()));
            boost::hash_combine(seed, std::hash<double>{}(v.z()));
            return seed;
        }
    };
}

namespace Slic3r {

// Name of the configuration option.
typedef std::string                 t_config_option_key;
typedef std::vector<std::string>    t_config_option_keys;

extern std::string  escape_string_cstyle(const std::string &str);
extern std::string  escape_strings_cstyle(const std::vector<std::string> &strs);
extern std::string  escape_strings_cstyle(const std::vector<std::string> &strs, const std::vector<bool> &enables);
extern bool         unescape_string_cstyle(const std::string &str, std::string &out);
extern bool         unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values);
extern bool         unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values, std::vector<bool> &out_enables);

extern std::string  escape_ampersand(const std::string& str);

constexpr char NIL_STR_VALUE[] = "nil";

enum class OptionCategory : int
{
    none,

    perimeter,
    slicing,
    infill,
    ironing,
    fuzzy_skin,
    skirtBrim,
    support,
    speed,
    width,
    extruders,
    output,
    notes,
    dependencies,

    filament,
    cooling,
    advanced,
    filoverride,
    customgcode,
    
    general,
    limits,
    mmsetup,
    firmware,

    pad,
    padSupp,
    wipe,

    hollowing,

    milling_extruders,
    milling,
};
std::string toString(OptionCategory opt);

namespace ConfigHelpers {
	inline bool looks_like_enum_value(std::string value)
	{
		boost::trim(value);
		if (value.empty() || value.size() > 64 || ! isalpha(value.front()))
			return false;
		for (const char c : value)
			if (! (isalnum(c) || c == '_' || c == '-'))
				return false;
		return true;
	}

    inline bool enum_looks_like_bool_value(std::string value) {
        boost::trim(value);
        return boost::iequals(value, "enabled") || boost::iequals(value, "disabled") || boost::iequals(value, "on") || boost::iequals(value, "off");
    }

    inline bool enum_looks_like_true_value(std::string value) {
        boost::trim(value);
        return boost::iequals(value, "enabled") || boost::iequals(value, "on");
    }

	enum class DeserializationSubstitution {
		Disabled,
		DefaultsToFalse,
		DefaultsToTrue
	};

    enum class DeserializationResult {
    	Loaded,
    	Substituted,
    	Failed,
    };
};

// Base for all exceptions thrown by the configuration layer.
class ConfigurationError : public Slic3r::RuntimeError {
public:
    using RuntimeError::RuntimeError;
};

// Specialization of std::exception to indicate that an unknown config option has been encountered.
class UnknownOptionException : public ConfigurationError {
public:
    UnknownOptionException() :
        ConfigurationError("Unknown option exception") {}
    UnknownOptionException(const std::string &opt_key) :
        ConfigurationError(std::string("Unknown option exception: ") + opt_key) {}
};

// Indicate that the ConfigBase derived class does not provide config definition (the method def() returns null).
class NoDefinitionException : public ConfigurationError
{
public:
    NoDefinitionException() :
        ConfigurationError("No definition exception") {}
    NoDefinitionException(const std::string &opt_key) :
        ConfigurationError(std::string("No definition exception: ") + opt_key) {}
};
// a bit more specific than a runtime_error
class ConfigurationException : public std::runtime_error
{
public:
    ConfigurationException() :
        std::runtime_error("Configuration exception") {}
    ConfigurationException(const std::string &opt_key) :
        std::runtime_error(std::string("Configuration exception: ") + opt_key) {}
};

// Indicate that an unsupported accessor was called on a config option.
class BadOptionTypeException : public ConfigurationError
{
public:
	BadOptionTypeException() : ConfigurationError("Bad option type exception") {}
	BadOptionTypeException(const std::string &message) : ConfigurationError(message) {}
    BadOptionTypeException(const char* message) : ConfigurationError(message) {}
};

// Indicate that an option has been deserialized from an invalid value.
class BadOptionValueException : public ConfigurationError
{
public:
    BadOptionValueException() : ConfigurationError("Bad option value exception") {}
    BadOptionValueException(const std::string &message) : ConfigurationError(message) {}
    BadOptionValueException(const char* message) : ConfigurationError(message) {}
};

// Type of a configuration value.
enum ConfigOptionType : uint16_t{
    coVectorType    = 0x4000, // 16384
    coNone          = 0,
    // single float
    coFloat         = 1,
    // vector of floats
    coFloats        = coFloat + coVectorType,
    // single int
    coInt           = 2,
    // vector of ints
    coInts          = coInt + coVectorType,
    // single string
    coString        = 3,
    // vector of strings
    coStrings       = coString + coVectorType,
    // percent value. Currently only used for infill & flow ratio.
    coPercent       = 4,
    // percents value. Currently used for retract before wipe only.
    coPercents      = coPercent + coVectorType,
    // a fraction or an absolute value
    coFloatOrPercent = 5,
    // vector of the above
    coFloatsOrPercents = coFloatOrPercent + coVectorType,
    // single 2d point (Point2f). Currently not used.
    coPoint         = 6,
    // vector of 2d points (Point2f). Currently used for the definition of the print bed and for the extruder offsets.
    coPoints        = coPoint + coVectorType,
    coPoint3        = 7,
//    coPoint3s       = coPoint3 + coVectorType,
    // single boolean value
    coBool          = 8,
    // vector of boolean values
    coBools         = coBool + coVectorType,
    // a generic enum
    coEnum          = 9,
    // a graph of double->double
    coGraph         = 10,
    // a vector of graph of double->double
    coGraphs        = coGraph + coVectorType,
};

enum ConfigOptionMode : uint64_t {
    comNone = 0,
    comSimple = 1,
    comAdvanced = 1 << 1,
    comExpert = 1 << 2,
    comAdvancedE = comAdvanced | comExpert,
    comSimpleAE = comSimple | comAdvanced | comExpert,
    comPrusa = 1 << 3,
    comSuSi = 1 << 4,
    comHidden = 1 << 5,
    
};
//note: you have to add ConfigOptionMode into the ConfigOptionDef::names_2_tag_mode (in the .cpp)
inline ConfigOptionMode operator|(ConfigOptionMode a, ConfigOptionMode b) {
    return static_cast<ConfigOptionMode>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
inline ConfigOptionMode operator&(ConfigOptionMode a, ConfigOptionMode b) {
    return static_cast<ConfigOptionMode>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
inline ConfigOptionMode operator^(ConfigOptionMode a, ConfigOptionMode b) {
    return static_cast<ConfigOptionMode>(static_cast<uint64_t>(a) ^ static_cast<uint64_t>(b));
}
inline ConfigOptionMode operator|=(ConfigOptionMode& a, ConfigOptionMode b) {
    a = a | b; return a;
}
inline ConfigOptionMode operator&=(ConfigOptionMode& a, ConfigOptionMode b) {
    a = a & b; return a;
}

enum PrinterTechnology : uint8_t
{
    // Fused Filament Fabrication
    ptFFF = 1 << 0,
    // Stereolitography
    ptSLA = 1 << 1,
    // Selective Laser-Sintering
    ptSLS = 1 << 2,
    // CNC
    ptMill = 1 << 3,
    // Laser engraving
    ptLaser = 1 << 4,
    // Any technology, useful for parameters compatible with both ptFFF and ptSLA
    ptAny = ptFFF | ptSLA | ptSLS | ptMill | ptLaser,
    // Unknown, useful for command line processing
    ptUnknown = 1 << 7
};
inline PrinterTechnology operator|(PrinterTechnology a, PrinterTechnology b) {
    return static_cast<PrinterTechnology>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline PrinterTechnology operator&(PrinterTechnology a, PrinterTechnology b) {
    return static_cast<PrinterTechnology>(static_cast<uint8_t>(a)& static_cast<uint8_t>(b));
}
inline PrinterTechnology operator|=(PrinterTechnology& a, PrinterTechnology b) {
    a = a | b; return a;
}
inline PrinterTechnology operator&=(PrinterTechnology& a, PrinterTechnology b) {
    a = a & b; return a;
}

// defined here isntead of PrintConfig to be more visible.
enum OutputFormat : uint16_t {
    ofUnknown = 0,
    ofGCode   = 1,
    ofSL1,
    ofSL1_SVG,
    ofMaskedCWS,
    ofAnycubicMono,
    ofAnycubicMonoX,
    ofAnycubicMonoSE,
};

enum class BridgeType : uint8_t {
    btNone,
    btFromNozzle,
    btFromHeight,
    btFromFlow,
};

enum ForwardCompatibilitySubstitutionRule
{
    // Disable susbtitution, throw exception if an option value is not recognized.
    Disable,
    // Enable substitution of an unknown option value with default. Log the substitution.
    Enable,
    // Enable substitution of an unknown option value with default. Don't log the substitution.
    EnableSilent,
    // Enable substitution of an unknown option value with default. Log substitutions in user profiles, don't log substitutions in system profiles.
    EnableSystemSilent,
    // Enable silent substitution of an unknown option value with default when loading user profiles. Throw on an unknown option value in a system profile.
    EnableSilentDisableSystem,
};

class  ConfigDef;
class  ConfigOption;
class  ConfigOptionDef;
// For forward definition of ConfigOption in ConfigOptionUniquePtr, we have to define a custom deleter.
struct ConfigOptionDeleter { void operator()(ConfigOption* p); };
using  ConfigOptionUniquePtr = std::unique_ptr<ConfigOption, ConfigOptionDeleter>;

// When parsing a configuration value, if the old_value is not understood by this PrusaSlicer version,
// it is being substituted with some default value that this PrusaSlicer could work with.
// This structure serves to inform the user about the substitutions having been done during file import.
struct ConfigSubstitution {
    const ConfigOptionDef   *opt_def { nullptr };
    std::string              old_name; // for when opt_def is nullptr (option not defined in this version)
    std::string              old_value;
    ConfigOptionUniquePtr    new_value;
    ConfigSubstitution() = default;
    ConfigSubstitution(const ConfigOptionDef* def, std::string old, ConfigOptionUniquePtr&& new_v) : opt_def(def), old_name(), old_value(old), new_value(std::move(new_v)) {}
    ConfigSubstitution(std::string bad_key, std::string value) : opt_def(nullptr), old_name(bad_key), old_value(value), new_value() {}
};

using  ConfigSubstitutions = std::vector<ConfigSubstitution>;

// Filled in by ConfigBase::set_deserialize_raw(), which based on "rule" either bails out
// or performs substitutions when encountering an unknown configuration value.
struct ConfigSubstitutionContext
{
    ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule rl) : rule(rl) {}

    ForwardCompatibilitySubstitutionRule 	rule;
    
    bool empty() const throw() { return m_substitutions.empty(); }
    const ConfigSubstitutions &get() const { return m_substitutions; }
    ConfigSubstitutions data() && { return std::move(m_substitutions); }
    void add(ConfigSubstitution&& substitution) { m_substitutions.push_back(std::move(substitution)); }
    void emplace(std::string &&key, std::string &&value) { m_substitutions.emplace_back(std::move(key), std::move(value)); }
    void emplace(const ConfigOptionDef* def, std::string &&old_value, ConfigOptionUniquePtr&& new_v) { m_substitutions.emplace_back(def, std::move(old_value), std::move(new_v)); }
    void clear() { m_substitutions.clear(); }
    void sort_and_remove_duplicates() { sort_remove_duplicates(m_substitutions); }

private:
    ConfigSubstitutions					    m_substitutions;
};

// A generic value of a configuration option.
class ConfigOption {
public:
    // Flags ta save some states into the option.
    // note: uint32_t because macos crash if it's a bool. and it doesn't change the size of the object because of alignment.
    // FCO_PHONY: if true, this option doesn't need to be saved (or with empty string), it's a computed value from an other ConfigOption.
    // FCO_EXTRUDER_ARRAY: set if the ConfigDef has is_extruder_size(). Only apply to ConfigVectorBase and childs
    // FCO_PLACEHOLDER_TEMP: for PlaceholderParser, to be able to recognise temporary fake ConfigOption (for default_XXX() macro)
    // FCO_ENABLED: to see if this option is activated or disabled ( same as 0 or -1 value in the old way) (for single-value options only)
    uint32_t flags;
    enum FlagsConfigOption : uint32_t {
        FCO_PHONY = 1,
        FCO_EXTRUDER_ARRAY = 1 << 1,
        FCO_PLACEHOLDER_TEMP = 1 << 2,
        FCO_ENABLED = 1 << 3,
    };

    ConfigOption() : flags(uint32_t(FCO_ENABLED)) { assert(this->flags != 0); }

    virtual ~ConfigOption() {}

    virtual ConfigOptionType    type() const = 0;
    virtual std::string         serialize() const = 0;
    virtual bool                deserialize(const std::string &str, bool append = false) = 0;
    virtual ConfigOption*       clone() const = 0;
    // Set a value from a ConfigOption. The two options should be compatible.
    virtual void                set(const ConfigOption *option) = 0;
    // Getters, idx is ignored if it's a scalar value.
    virtual int32_t             get_int(size_t idx = 0)        const { throw BadOptionTypeException("Calling ConfigOption::get_int on a non-int ConfigOption"); }
    virtual double              get_float(size_t idx = 0)      const { throw BadOptionTypeException("Calling ConfigOption::get_float on a non-float ConfigOption"); }
    virtual bool                is_percent(size_t idx = 0)     const { return false;  }
    virtual bool                get_bool(size_t idx = 0)       const { throw BadOptionTypeException("Calling ConfigOption::get_bool on a non-boolean ConfigOption");  }
    virtual void                set_enum_int(int32_t /* val */) { throw BadOptionTypeException("Calling ConfigOption::set_enum_int on a non-enum ConfigOption"); }
    // If scalar, idx is ignore, else: if idx < 0 return the vector; if idx >=0 then return the value at theis index; if idx >= size(), then return the first value or a default one.
    virtual boost::any          get_any(int32_t idx = -1)      const { throw BadOptionTypeException("Calling ConfigOption::get_any on a raw ConfigOption"); }
    virtual void                set_any(boost::any, int32_t idx = -1) { throw BadOptionTypeException("Calling ConfigOption::set_any on a raw ConfigOption"); }
    virtual bool                is_enabled(size_t idx = -1)    const { return (flags & FCO_ENABLED) != 0; }
    virtual ConfigOption *set_enabled(bool enabled, size_t idx = -1)
    {
        if (enabled)
            this->flags |= FCO_ENABLED;
        else
            this->flags &= uint32_t(0xFF ^ FCO_ENABLED);
        return this;
    }
    virtual bool                operator==(const ConfigOption &rhs) const = 0;
    bool                        operator!=(const ConfigOption &rhs) const { return ! (*this == rhs); }
    virtual size_t              hash()          const throw() = 0;
    bool                        is_scalar()     const { return (int(this->type()) & int(coVectorType)) == 0; }
    bool                        is_vector()     const { return ! this->is_scalar(); }
    // Size of the vector it contains, or 1 if it's a scalar value.
    virtual size_t              size()          const = 0;
    // If this option is nullable, then it may have its value or values set to nil.
    virtual bool 				nullable()		const { return false; }
    // A scalar is nil, or all values of a vector are nil if idx < 0.
    virtual bool                is_nil(int32_t idx = -1) const { return false; }
    bool                        is_phony()      const { return (flags & FCO_PHONY) != 0; }
    ConfigOption*               set_phony(bool phony) { if (phony) this->flags |= FCO_PHONY; else this->flags &= uint8_t(0xFF ^ FCO_PHONY); return this; }
    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal.
    virtual bool                overriden_by(const ConfigOption *rhs) const {
    	assert(! this->nullable() && ! rhs->nullable());
        return *this != *rhs && this->is_enabled() == rhs->is_enabled();
    }
    // Apply an override option, possibly a nullable one.
    virtual bool                apply_override(const ConfigOption *rhs) {
    	if (*this == *rhs) 
    		return false; 
    	*this = *rhs; 
    	return true;
    }
private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive& ar) { ar(this->flags); }
};

typedef ConfigOption*       ConfigOptionPtr;
typedef const ConfigOption* ConfigOptionConstPtr;

// Nill value will be defined in specializations
template<class T, class En = void> struct NilValueTempl
{
    using NilType = T;
    static_assert(always_false<T>::value, "Type has no well defined nil value");
};

template<class T> struct NilValueTempl<T, std::enable_if_t<std::is_integral_v<T>, void>> {
    using NilType = T;
    static constexpr auto value = std::numeric_limits<T>::max();
};

template<> struct NilValueTempl<bool> : public NilValueTempl<int>{};

// For enums the nil is the max value of the underlying type.
template<class T>
struct NilValueTempl<T, std::enable_if_t<std::is_enum_v<T>, void>>
{
    using NilType = T;
    static constexpr auto value = static_cast<T>(std::numeric_limits<std::underlying_type_t<T>>::max());
};

// For Graph the nil is an empty array (there are no nil usage anyway)
template<class T>
struct NilValueTempl<T, std::enable_if_t<std::is_same<GraphData, T>::value, void>>
{
    using NilType = GraphData::GraphType;
    static constexpr auto value = GraphData::GraphType::COUNT;
};

template<class T> struct NilValueTempl<T, std::enable_if_t<std::is_floating_point_v<T>, void>> {
    using NilType = T;
    static constexpr auto value = std::numeric_limits<T>::quiet_NaN();
};

template<>
struct NilValueTempl<FloatOrPercent> : public NilValueTempl<double> {};

template<> struct NilValueTempl<std::string> {
    using NilType = const char *;

    static constexpr const char* value = "";
};

template<int N, class T> struct NilValueTempl<Vec<N, T>> {
    using NilType = Vec<N, T>;
    // No constexpr for Vec<N, T>
    static inline const Vec<N, T> value = Vec<N, T>::Ones() * NilValueTempl<remove_cvref_t<T>>::value;
};

template<class T> using NilType = typename NilValueTempl<remove_cvref_t<T>>::NilType;

// Define shortcut as a function instead of a static const var so that it can be constexpr
// even if the NilValueTempl::value is not constexpr.
template<class T> static constexpr NilType<T> NilValue() noexcept { return NilValueTempl<remove_cvref_t<T>>::value; }

// Value of a single valued option (bool, int, float, string, point, enum)
template <class T, bool NULLABLE = false>
class ConfigOptionSingle : public ConfigOption {
public:
    T value;
    explicit ConfigOptionSingle(T value) : value(std::move(value)) {}
    operator T() const { return this->value; }
    boost::any get_any(int32_t idx = -1) const override { return boost::any(value); }
    void       set_any(boost::any anyval, int32_t idx = -1) override { value = boost::any_cast<T>(anyval); }
    size_t     size() const override { return 1; }
    
    void set(const ConfigOption *rhs) override
    {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionSingle: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionSingle*>(rhs));
        this->value = static_cast<const ConfigOptionSingle*>(rhs)->value;
        this->flags = rhs->flags;
    }

    bool operator==(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type()) {
            throw ConfigurationError("ConfigOptionSingle: Comparing incompatible types");
        }

        assert(dynamic_cast<const ConfigOptionSingle<T>*>(&rhs));

        return (this->value == static_cast<const ConfigOptionSingle<T>*>(&rhs)->value 
                || (this->is_nil() && rhs.is_nil()))
            && this->is_enabled() == rhs.is_enabled()
            && this->is_phony() == rhs.is_phony();
        // should compare all flags?
    }
    bool operator==(const T &rhs) const throw() { return this->value == rhs; }
    bool operator!=(const T &rhs) const throw() { return this->value != rhs; }
    bool operator< (const T &rhs) const throw() { return this->value < rhs; }

    size_t hash() const throw() override {
        std::hash<T> hasher;
        size_t seed = 0;
        boost::hash_combine(seed, this->is_enabled());
        boost::hash_combine(seed, hasher(this->value));
        return seed;
    }

    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal.
    bool overriden_by(const ConfigOption *rhs) const override {
        if (this->nullable())
            throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.overriden_by() applied to different types.");
        auto rhs_co = static_cast<const ConfigOptionSingle*>(rhs);
        if (! rhs->nullable())
            // Overridding a non-nullable object with another non-nullable object.
            return this->value != rhs_co->value;

        return !rhs_co->is_nil() && rhs_co->value != this->value;
    }
    // Apply an override option, possibly a nullable one.
    bool apply_override(const ConfigOption *rhs) override {
        if (this->nullable())
            throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.apply_override() applied to different types.");
        auto rhs_co = static_cast<const ConfigOptionSingle*>(rhs);
        if (! rhs->nullable()) {
            // Overridding a non-nullable object with another non-nullable object.
            if (this->value != rhs_co->value) {
                this->value = rhs_co->value;
                return true;
            }
            return false;
        }

        if (!rhs_co->is_nil() && rhs_co->value != this->value) {
            this->value = rhs_co->value;
            return true;
        }

        return false;
    }

    bool nullable() const override { return NULLABLE; }

    static constexpr NilType<T> nil_value() { return NilValue<T>(); }

    // A scalar is nil, or all values of a vector are nil.
    bool is_nil(int32_t idx = -1) const override
    {
        bool ret = false;

        if constexpr (NULLABLE)
            ret = this->value == nil_value();

        return ret;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive & ar) { ar(this->flags); ar(this->value); }
};

template<class T>
using ConfigOptionSingleNullable = ConfigOptionSingle<T, true>;

// Value of a vector valued option (bools, ints, floats, strings, points)
class ConfigOptionVectorBase : public ConfigOption {
public:
    virtual std::string         serialize() const override = 0;
    virtual bool                deserialize(const std::string &str, bool append = false) override = 0;
    // Currently used only to initialize the PlaceholderParser.
    virtual std::vector<std::string> vserialize() const = 0;
    // Set from a vector of ConfigOptions. 
    // If the rhs ConfigOption is scalar, then its value is used,
    // otherwise for each of rhs, the first value of a vector is used.
    // This function is useful to collect values for multiple extrder / filament settings.
    virtual void set(const std::vector<const ConfigOption*> &rhs) = 0;
    // Set a single vector item from either a scalar option or the first value of a vector option.vector of ConfigOptions. 
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    virtual void set_at(const ConfigOption *rhs, size_t i, size_t j) = 0;
    // Resize the vector of values, copy the newly added values from opt_default if provided.
    virtual void resize(size_t n, const ConfigOption *opt_default = nullptr) = 0;
    // Clear the values vector.
    virtual void clear() = 0;
    // get the stored default value for filling empty vector.
    // If you use it, double check if you shouldn't instead use the ConfigOptionDef.defaultvalue, which is the default value of a setting.
    // currently, it's used to try to have a meaningful value for a Field if the default value is Nil (and to avoid cloning the option, clear it, asking for an item)
    virtual boost::any get_default_value() const = 0;

    // Is this vector empty?
    virtual bool   empty() const = 0;
    // Is the value nil? That should only be possible if this->nullable().
    bool   is_nil(int32_t idx) const override = 0;
    // Get if the size of this vector is/should be the same as nozzle_diameter
    bool is_extruder_size() const { return (flags & FCO_EXTRUDER_ARRAY) != 0; }
    ConfigOptionVectorBase* set_is_extruder_size(bool is_extruder_size = true) {
        if (is_extruder_size) this->flags |= FCO_EXTRUDER_ARRAY; else this->flags &= uint8_t(0xFF ^ FCO_EXTRUDER_ARRAY);
        assert(this->flags != 0);
        return this;
    }

    
    bool is_enabled(size_t idx = 0) const override {
        assert (m_enabled.size() == size());
        return idx >= 0 && idx < m_enabled.size() ? m_enabled[idx] : ConfigOption::is_enabled();
    }
    
    bool has_same_enabled(const ConfigOptionVectorBase &rhs) const
    {
        return this->m_enabled == rhs.m_enabled;
    }
    ConfigOption *set_enabled(bool enabled, size_t idx = 0) override
    {
        assert (m_enabled.size() == size());
        // reset evrything, use the default.
        if (idx < 0) {
            for(size_t i=0; i<this->m_enabled.size(); ++i)
                this->m_enabled[i] = enabled;
            ConfigOption::set_enabled(enabled);
            return this;
        }
        // can't enable something that doesn't exist
        if(idx >= size())
            return this;
        // set our value
        m_enabled[idx] = enabled;
        return this;
    }

    // We just overloaded and hid two base class virtual methods.
    // Let's show it was intentional (warnings).
    using ConfigOption::set;
    using ConfigOption::is_nil;


protected:
    // Is the size of the vector, so every bit is set.
    // If at least one is true, then the default (from configoption flag) is also true, else false.
    std::vector<bool> m_enabled;
    // Used to verify type compatibility when assigning to / from a scalar ConfigOption.
    ConfigOptionType scalar_type() const { return static_cast<ConfigOptionType>(this->type() - coVectorType); }

    void set_default_enabled()
    {
        if (!m_enabled.empty()) {
            bool has_enabled = false;
            for (size_t i = 0; !has_enabled && i < m_enabled.size(); ++i) { has_enabled = m_enabled[i]; }
            ConfigOption::set_enabled(has_enabled);
        }
    }
    
	friend class cereal::access;
	template<class Archive> void serialize(Archive & ar) { ar(this->flags); ar(this->m_enabled); }
};

// Value of a vector valued option (bools, ints, floats, strings, points), template
template <class T>
class ConfigOptionVector : public ConfigOptionVectorBase
{
private:
    void set_default_from_values() {
        assert(!m_values.empty());
        if (!m_values.empty())
            default_value = m_values.front();
    }

protected:
    // this default is used to fill this vector when resized. It's not the default of a setting, for it please use the
    // ConfigOptionDef. It's not even serialized or put in the undo/redo.
    T default_value;
    std::vector<T> m_values;
public:

    ConfigOptionVector() {}
    explicit ConfigOptionVector(T default_val) : default_value(default_val) { assert (m_enabled.size() == size()); }
    explicit ConfigOptionVector(size_t n, const T &value) : m_values(n, value), default_value(value) { this->m_enabled.resize(m_values.size(), ConfigOption::is_enabled()); assert (m_enabled.size() == size()); }
    explicit ConfigOptionVector(std::initializer_list<T> il) : m_values(std::move(il)) { set_default_from_values(); this->m_enabled.resize(m_values.size(), ConfigOption::is_enabled()); assert (m_enabled.size() == size()); }
    explicit ConfigOptionVector(const std::vector<T> &values) : m_values(values) { set_default_from_values(); this->m_enabled.resize(m_values.size(), ConfigOption::is_enabled()); assert (m_enabled.size() == size()); }
    explicit ConfigOptionVector(std::vector<T> &&values) : m_values(std::move(values)) { set_default_from_values(); this->m_enabled.resize(m_values.size(), ConfigOption::is_enabled()); assert (m_enabled.size() == size()); }

    const std::vector<T> &get_values() const { return m_values; }

    void set(const ConfigOption *rhs) override
    {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionVector<T>*>(rhs));
        this->m_values = static_cast<const ConfigOptionVector<T>*>(rhs)->m_values;
        this->m_enabled = static_cast<const ConfigOptionVector<T>*>(rhs)->m_enabled;
        this->flags = rhs->flags;
        assert (m_enabled.size() == this->m_values.size());
    }

    // Set from a vector of ConfigOptions. 
    // If the rhs ConfigOption is scalar, then its value is used,
    // otherwise for each of rhs, the first value of a vector is used.
    // This function is useful to collect values for multiple extrder / filament settings.
    void set(const std::vector<const ConfigOption*> &rhs) override
    {
        this->m_values.clear();
        this->m_values.reserve(rhs.size());
        this->m_enabled.clear();
        this->m_enabled.reserve(rhs.size());
        for (const ConfigOption *opt : rhs) {
            if (opt->type() == this->type()) {
                assert(dynamic_cast<const ConfigOptionVector<T>*>(opt) != nullptr);
                auto other = static_cast<const ConfigOptionVector<T>*>(opt);
                if (other->m_values.empty())
                    throw ConfigurationError("ConfigOptionVector::set(): Assigning from an empty vector");
                this->m_values.emplace_back(other->get_at(0));
                this->m_enabled.push_back(other->is_enabled(0));
                if (other->is_enabled(0))
                    ConfigOption::set_enabled(true);
            } else if (opt->type() == this->scalar_type()) {
                this->m_values.emplace_back(static_cast<const ConfigOptionSingle<T> *>(opt)->value);
                this->m_enabled.push_back(opt->is_enabled());
                if (opt->is_enabled())
                    ConfigOption::set_enabled(true);
            }
            else
                throw ConfigurationError("ConfigOptionVector::set():: Assigning an incompatible type");
        }
        assert (m_enabled.size() == size());
    }

    // Set from a vector of values, all enabled (if default is enabled)
    void set(const std::vector<T> &rhs)
    {
        this->m_values.clear();
        this->m_values.reserve(rhs.size());
        this->m_enabled.clear();
        this->m_enabled.reserve(rhs.size());
        for (const T &val : rhs) {
            this->m_values.push_back(val);
            this->m_enabled.push_back(ConfigOption::is_enabled());
        }
        assert (m_enabled.size() == size());
    }

    // Set a single vector item from either a scalar option or the first value of a vector option.vector of ConfigOptions. 
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    void set_at(const ConfigOption *rhs, size_t i, size_t j) override
    {
        // Fill with default value up to the needed position
        if (this->m_values.size() <= i) {
            // Resize this vector, fill in the new vector fields with the copy of the first field.
            this->m_values.resize(i + 1, this->default_value);
            this->m_enabled.resize(i + 1, ConfigOption::is_enabled());
        }
        if (rhs->type() == this->type()) {
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);
            if (other->empty())
                throw ConfigurationError("ConfigOptionVector::set_at(): Assigning from an empty vector");
            this->m_values[i] = other->get_at(j);
            this->m_enabled[i] = other->is_enabled(j);
            ConfigOption::set_enabled(other->is_enabled(-1));
        } else if (rhs->type() == this->scalar_type()) {
            auto other = static_cast<const ConfigOptionSingle<T>*>(rhs);
            this->m_values[i] = other->value;
            this->m_enabled[i] = other->is_enabled();
            set_default_enabled();
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_at(): Assigning an incompatible type");
        assert (m_enabled.size() == size());
    }
    void set_at(T val, size_t i)
    {
        // Fill with default value up to the needed position
        if (this->m_values.size() <= i) {
            // Resize this vector, fill in the new vector fields with the copy of the first field.
            this->m_values.resize(i + 1, this->default_value);
            this->m_enabled.resize(i + 1, ConfigOption::is_enabled());
        }
        this->m_values[i] = val;
        assert (m_enabled.size() == size());
    }

    const T& get_at(size_t i) const
    {
        //assert(! this->m_values.empty());
        assert (m_enabled.size() == size());
        return (i < this->m_values.size()) ? this->m_values[i] :
                                             (this->m_values.empty() ? default_value : this->m_values.front());
    }

    T& get_at(size_t i) { return const_cast<T&>(std::as_const(*this).get_at(i)); }
    boost::any get_any(int32_t idx = -1) const override { return idx < 0 ? boost::any(this->m_values) : boost::any(get_at(idx)); }
    void       set_any(boost::any anyval, int32_t idx = -1) override
    { 
       if (idx < 0) {
            this->m_values = boost::any_cast<std::vector<T>>(anyval);
            this->m_enabled.resize(this->m_values.size(), ConfigOption::is_enabled());
       } else {
           set_at(boost::any_cast<T>(anyval), idx);
       }
        assert (m_enabled.size() == size());
    }

    // Resize this vector by duplicating the /*last*/first or default value.
    // If the current vector is empty, the default value is used instead.
    void resize(size_t n, const ConfigOption *opt_default = nullptr) override
    {
        assert(opt_default == nullptr || opt_default->is_vector());
        assert(n >= 0);
//        assert(opt_default == nullptr || dynamic_cast<ConfigOptionVector<T>>(opt_default));
       // assert(! this->m_values.empty() || opt_default != nullptr);
        if (n == 0) {
            this->m_values.clear();
            this->m_enabled.clear();
        } else if (n < this->m_values.size()) {
            assert (this->m_enabled.size() == this->m_values.size());
            this->m_values.erase(this->m_values.begin() + n, this->m_values.end());
            this->m_enabled.erase(this->m_enabled.begin() + n, this->m_enabled.end());
        } else if (n > this->m_values.size()) {
            if (this->m_values.empty()) {
                if (opt_default == nullptr)
                    this->m_values.resize(n, this->default_value);
                else if (opt_default->type() != this->type())
                    throw ConfigurationError("ConfigOptionVector::resize(): Extending with an incompatible type.");
                else if(auto other = static_cast<const ConfigOptionVector<T>*>(opt_default); other->m_values.empty())
                    this->m_values.resize(n, other->default_value);
                else
                    this->m_values.resize(n, other->get_at(0));
            } else {
                // Resize by duplicating the first value.
                this->m_values.resize(n, this->get_at(0));
            }
            this->m_enabled.resize(n, ConfigOption::is_enabled());
        }
        assert(m_enabled.size() == size());
    }

    // Resize this vector by duplicating the given value
    void resize(size_t n, const T &resize_value)
    {
        assert(n >= 0);
        if (n == 0) {
            this->m_values.clear();
            this->m_enabled.clear();
        } else if (n < this->m_values.size()) {
            assert(this->m_enabled.size() == this->m_values.size());
            this->m_values.erase(this->m_values.begin() + n, this->m_values.end());
            this->m_enabled.erase(this->m_enabled.begin() + n, this->m_enabled.end());
        } else if (n > this->m_values.size()) {
            this->m_values.resize(n, resize_value);
            this->m_enabled.resize(n, ConfigOption::is_enabled());
        }
        assert(m_enabled.size() == size());
    }

    // Clear the values vector.
    void   clear() override { this->m_values.clear(); this->m_enabled.clear(); }
    size_t size()  const override { return this->m_values.size(); }
    bool   empty() const override { return this->m_values.empty(); }
    // get the stored default value for filling empty vector.
    // If you use it, double check if you shouldn't instead use the ConfigOptionDef.defaultvalue, which is the default value of a setting.
    // currently, it's used to try to have a meaningful value for a Field if the default value is Nil
    boost::any get_default_value() const override { return boost::any(default_value); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionVector: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<T>*>(&rhs));
        return this->has_same_enabled(*static_cast<const ConfigOptionVector<T> *>(&rhs)) &&
               this->m_values == static_cast<const ConfigOptionVector<T> *>(&rhs)->m_values;
    }

    bool operator==(const std::vector<T> &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->m_values == rhs; }
    bool operator!=(const std::vector<T> &rhs) const throw() { return this->is_enabled() != rhs.is_enabled() || this->m_values != rhs; }

    size_t hash() const throw() override {
        std::hash<T> hasher;
        std::hash<bool> hasher_b;
        size_t seed = 0;
        for (const auto &v : this->m_values)
            boost::hash_combine(seed, hasher(v));
        for (bool b : this->m_enabled)
            boost::hash_combine(seed, hasher_b(b));
        return seed;
    }

    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal
    bool overriden_by(const ConfigOption *rhs) const override {
        if (this->nullable())
        	throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.overriden_by() applied to different types.");
    	auto rhs_vec = static_cast<const ConfigOptionVector<T>*>(rhs);
    	if (! rhs->nullable())
    		// Overridding a non-nullable object with another non-nullable object.
    		return this->m_values != rhs_vec->m_values && this->m_enabled != rhs_vec->m_enabled;
    	size_t i = 0;
    	size_t cnt = std::min(this->size(), rhs_vec->size());
    	for (; i < cnt; ++ i)
            if (! rhs_vec->is_nil(i) &&
                (this->m_values[i] != rhs_vec->m_values[i] || this->is_enabled(i) != rhs_vec->is_enabled(i)))
    			return true;
    	for (; i < rhs_vec->size(); ++ i)
    		if (! rhs_vec->is_nil(i))
    			return true;
    	return false;
    }

    // Apply an override option, possibly a nullable one.
    bool apply_override(const ConfigOption *rhs) override {
        if (this->nullable())
        	throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
			throw ConfigurationError("ConfigOptionVector.apply_override() applied to different types.");
		auto rhs_vec = static_cast<const ConfigOptionVector<T>*>(rhs);
		if (! rhs->nullable()) {
    		// Overridding a non-nullable object with another non-nullable object.
    		if (this->m_values != rhs_vec->m_values || this->m_enabled != rhs_vec->m_enabled) {
    			this->m_values = rhs_vec->m_values;
    			this->m_enabled = rhs_vec->m_enabled;
    			return true;
    		}
    		return false;
    	}
    	size_t i = 0;
    	size_t cnt = std::min(this->size(), rhs_vec->size());
    	bool   modified = false;
    	for (; i < cnt; ++ i)
    		if (! rhs_vec->is_nil(i) && this->m_values[i] != rhs_vec->m_values[i]) {
    			this->m_values[i] = rhs_vec->m_values[i];
    			this->m_enabled[i] = rhs_vec->m_enabled[i];
    			modified = true;
    		}
    	for (; i < rhs_vec->size(); ++ i)
    		if (! rhs_vec->is_nil(i)) {
    			this->m_values.resize(i + 1, this->default_value);
    			this->m_values[i] = rhs_vec->m_values[i];
    			this->m_enabled[i] = rhs_vec->m_enabled[i];
    			modified = true;
    		}
        return modified;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive & ar) { ar(this->m_values); ar(cereal::base_class<ConfigOptionVectorBase>(this)); }
};

template<bool NULLABLE = false>
class ConfigOptionFloatTempl : public ConfigOptionSingle<double, NULLABLE>
{
public:
    ConfigOptionFloatTempl() : ConfigOptionSingle<double, NULLABLE>(0) {}
    explicit ConfigOptionFloatTempl(double _value) : ConfigOptionSingle<double, NULLABLE>(_value) {}

    static ConfigOptionType static_type() { return coFloat; }
    ConfigOptionType        type()      const override { return static_type(); }
    double                  get_float(size_t idx = 0) const override { return this->value; }
    ConfigOption*           clone()     const override { return new ConfigOptionFloatTempl(*this); }
    bool                    operator==(const ConfigOptionFloatTempl &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionFloatTempl &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }
    
    std::string serialize() const override
    {
        std::ostringstream ss;
        double v = this->value;
        if (!this->is_enabled())
            ss << "!";
        ss << this->value;
        if (is_nil())
            if (NULLABLE)
                ss << "nil";
            else
                throw ConfigurationError("Serializing NaN");
        else if (std::isfinite(v)) {
            ss << v;

        } else if(std::isnan(v))
            throw ConfigurationError("Serializing invalid number");

        return ss.str();
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        std::istringstream iss(this->is_enabled() ? str : str.substr(1));

        if (str == "nil") {
            if (NULLABLE)
                this->value = this->nil_value();
            else
                throw ConfigurationError("Deserializing nil into a non-nullable object");
        } else {
            iss >> this->value;
        }

        return !iss.fail();
    }

    ConfigOptionFloatTempl& operator=(const ConfigOption *opt)
    {   
        this->set(opt);
        return *this;
    }

    bool is_nil(int32_t idx = -1) const override
    {
        return std::isnan(this->value) || this->value == ConfigOptionSingle<double, NULLABLE>::nil_value();
    }

    static inline bool is_any_nil(const boost::any &to_check) {
        double dlb_val = boost::any_cast<double>(to_check);
        return std::isnan(dlb_val) || dlb_val == ConfigOptionSingle<double, NULLABLE>::nil_value();
    }

    // don't use it to compare, use is_nil() to check.
    static inline boost::any create_any_nil() { return boost::any(ConfigOptionSingle<double, NULLABLE>::nil_value()); }

private:
	friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<double, NULLABLE>>(this)); }
};

using ConfigOptionFloat = ConfigOptionFloatTempl<false>;
using ConfigOptionFloatNullable  = ConfigOptionFloatTempl<true>;

template<bool NULLABLE>
class ConfigOptionFloatsTempl : public ConfigOptionVector<double>
{
public:
    ConfigOptionFloatsTempl() : ConfigOptionVector<double>() {}
    explicit ConfigOptionFloatsTempl(double default_value) : ConfigOptionVector<double>(default_value) {}
    explicit ConfigOptionFloatsTempl(size_t n, double value) : ConfigOptionVector<double>(n, value) {}
    explicit ConfigOptionFloatsTempl(std::initializer_list<double> il) : ConfigOptionVector<double>(std::move(il)) {}
    explicit ConfigOptionFloatsTempl(const std::vector<double> &vec) : ConfigOptionVector<double>(vec) {}
    explicit ConfigOptionFloatsTempl(std::vector<double> &&vec) : ConfigOptionVector<double>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coFloats; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionFloatsTempl(*this); }
    bool                    operator==(const ConfigOptionFloatsTempl &rhs) const throw()
    {
        return this->m_enabled == rhs.m_enabled && vectors_equal(this->m_values, rhs.m_values);
    }
    bool operator<(const ConfigOptionFloatsTempl &rhs) const throw()
    {
        return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && vectors_lower(this->m_values, rhs.m_values));
    }
    bool 					operator==(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatsTempl: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<double>*>(&rhs));
        return this->has_same_enabled(*static_cast<const ConfigOptionVector<double> *>(&rhs)) &&
               vectors_equal(this->m_values, static_cast<const ConfigOptionVector<double> *>(&rhs)->get_values());
    }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // A scalar is nil, or all values of a vector are nil.
    bool                    is_nil(int32_t idx = -1) const override
    {
        if (idx < 0) {
            for (double v : this->m_values)
                if (!std::isnan(v) && v != ConfigOptionFloatNullable::nil_value())
                    return false;
            return true;
        } else {
            return idx < int32_t(this->size()) ? (std::isnan(this->m_values[idx])    || ConfigOptionFloatNullable::nil_value() == this->m_values[idx]) :
                   this->empty()               ? (std::isnan(this->default_value)    || ConfigOptionFloatNullable::nil_value() == this->default_value) :
                                                 (std::isnan(this->m_values.front()) || ConfigOptionFloatNullable::nil_value() == this->m_values.front());
        }
    }
    double                  get_float(size_t idx = 0) const override { return get_at(idx); }

    std::string serialize() const override
    {
        assert(this->m_values.size() == this->m_enabled.size());
        std::ostringstream ss;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            if (idx > 0)
                ss << ",";
            this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
        }
        return ss.str();
    }
    
    std::vector<std::string> vserialize() const override
    {
        assert(this->m_values.size() == this->m_enabled.size());
        std::vector<std::string> vv;
        vv.reserve(this->m_values.size());
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            std::ostringstream ss;
            this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
        	boost::trim(item_str);
            bool enabled = true;
            if (!item_str.empty() && item_str.front() == '!') {
                enabled = false;
                item_str = item_str.substr(1);
                boost::trim(item_str);
            }
        	if (item_str == NIL_STR_VALUE) {
        		if (NULLABLE)
                    this->m_values.push_back(ConfigOptionFloatNullable::nil_value());
        		else
        			throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else {
	            std::istringstream iss(item_str);
	            double value;
	            iss >> value;
                this->m_values.push_back(value);
            }
            this->m_enabled.push_back(enabled);
        }
        set_default_enabled();
        assert(this->m_values.size() == this->m_enabled.size());
        return true;
    }

    ConfigOptionFloatsTempl& operator=(const ConfigOption *opt)
    {   
        this->set(opt);
        assert(this->m_values.size() == this->m_enabled.size());
        return *this;
    }

protected:
    // Special "nil" value to be stored into the vector if this->supports_nil().
    //please use is_nil & create_nil, to better support nan
    //static double 			NIL_VALUE() { return std::numeric_limits<double>::quiet_NaN(); }
    void serialize_single_value(std::ostringstream &ss, const double v, const bool enabled) const {
        if (!enabled)
            ss << "!";
        if (std::isnan(v) || v == ConfigOptionFloatNullable::nil_value())
            if (NULLABLE)
                ss << NIL_STR_VALUE;
            else
                throw ConfigurationError("Serializing NaN");
        else if (std::isfinite(v)) {
            ss << v;
        } else
            throw ConfigurationError("Serializing invalid number");
	}
    static bool vectors_equal(const std::vector<double> &v1, const std::vector<double> &v2) {
    	if (NULLABLE) {
    		if (v1.size() != v2.size())
    			return false;
    		for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end(); ++ it1, ++ it2)
                if (!(((std::isnan(*it1) || *it1 == ConfigOptionFloatNullable::nil_value()) && (std::isnan(*it2) || *it2 == ConfigOptionFloatNullable::nil_value())) ||
                      *it1 == *it2))
	    			return false;
    		return true;
    	} else
    		// Not supporting nullable values, the default vector compare is cheaper.
    		return v1 == v2;
    }
    static bool vectors_lower(const std::vector<double> &v1, const std::vector<double> &v2) {
        if (NULLABLE) {
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end() && it2 != v2.end(); ++ it1, ++ it2) {
                auto null1 = int(std::isnan(*it1) || *it1 == ConfigOptionFloatNullable::nil_value());
                auto null2 = int(std::isnan(*it2) || *it2 == ConfigOptionFloatNullable::nil_value());
                return (null1 < null2) || (null1 == null2 && *it1 < *it2);
            }
            return v1.size() < v2.size();
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 < v2;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<double>>(this)); }
};

using ConfigOptionFloats 		 = ConfigOptionFloatsTempl<false>;
using ConfigOptionFloatsNullable = ConfigOptionFloatsTempl<true>;

template<bool NULLABLE = false>
class ConfigOptionIntTempl : public ConfigOptionSingle<int32_t, NULLABLE>
{
public:
    ConfigOptionIntTempl() : ConfigOptionSingle<int32_t, NULLABLE>(0) {}
    explicit ConfigOptionIntTempl(int32_t value) : ConfigOptionSingle<int32_t, NULLABLE>(value) {}
    explicit ConfigOptionIntTempl(double _value) : ConfigOptionSingle<int32_t, NULLABLE>(int32_t(floor(_value + 0.5))) {}
    
    static ConfigOptionType static_type() { return coInt; }
    ConfigOptionType        type()   const override { return static_type(); }
    int32_t                 get_int(size_t idx = 0) const override { return this->value; }
    double                  get_float(size_t idx = 0) const override { return this->value; }
    ConfigOption*           clone()  const override { return new ConfigOptionIntTempl(*this); }
    bool                    operator==(const ConfigOptionIntTempl &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator<(const ConfigOptionIntTempl &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }
    
    std::string serialize() const override 
    {
        std::ostringstream ss;
        if (!this->is_enabled())
            ss << "!";
        if (this->value == this->nil_value()) {
            if (NULLABLE)
                ss << "nil";
            else
                throw ConfigurationError("Serializing NaN");
        } else
            ss << this->value;

        return ss.str();
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        std::istringstream iss(this->is_enabled() ? str : str.substr(1));

        if (str == "nil") {
            if (NULLABLE)
                this->value = this->nil_value();
            else
                throw ConfigurationError("Deserializing nil into a non-nullable object");
        } else {
            iss >> this->value;
        }

        return !iss.fail();
    }

    ConfigOptionIntTempl& operator=(const ConfigOption *opt)
    {   
        this->set(opt);
        return *this;
    }
    

    static inline bool is_any_nil(const boost::any &to_check) {
        return boost::any_cast<int32_t>(to_check) == ConfigOptionSingle<int32_t, NULLABLE>::nil_value();
    }

    // don't use it to compare, use is_nil() to check.
    static inline boost::any create_any_nil() { return boost::any(ConfigOptionSingle<int32_t, NULLABLE>::nil_value()); }

private:
	friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<int32_t, NULLABLE>>(this)); }
};

using ConfigOptionInt = ConfigOptionIntTempl<false>;
using ConfigOptionIntNullable = ConfigOptionIntTempl<true>;

template<bool NULLABLE>
class ConfigOptionIntsTempl : public ConfigOptionVector<int32_t>
{
public:
    ConfigOptionIntsTempl() : ConfigOptionVector<int32_t>() {}
    explicit ConfigOptionIntsTempl(int32_t default_value) : ConfigOptionVector<int32_t>(default_value) {}
    explicit ConfigOptionIntsTempl(size_t n, int32_t value) : ConfigOptionVector<int32_t>(n, value) {}
    explicit ConfigOptionIntsTempl(std::initializer_list<int32_t> &&il) : ConfigOptionVector<int32_t>(std::move(il)) {}
    //explicit ConfigOptionIntsTempl(const std::vector<int> &v) : ConfigOptionVector<int>(v) {}
    //explicit ConfigOptionIntsTempl(std::vector<int> &&v) : ConfigOptionVector<int>(std::move(v)) {}

    static ConfigOptionType static_type() { return coInts; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionIntsTempl(*this); }
    ConfigOptionIntsTempl&  operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionIntsTempl &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionIntsTempl &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    //static int32_t          NIL_VALUE() { return std::numeric_limits<int32_t>::max(); }
    // A scalar is nil, or all values of a vector are nil.
    bool is_nil(int32_t idx = -1) const override
    {
        if (idx < 0) {
            for (int32_t v : this->m_values)
                if (v != ConfigOptionIntNullable::nil_value())
                    return false;
            return true;
        } else {
            return idx < int32_t(this->size()) ? ConfigOptionIntNullable::nil_value() == this->m_values[idx] :
                   this->empty()               ? ConfigOptionIntNullable::nil_value() == this->default_value :
                                                 ConfigOptionIntNullable::nil_value() == this->get_at(0);
        }
    }
    int32_t                 get_int(size_t idx = 0) const override { return get_at(idx); }
    double                  get_float(size_t idx = 0) const override { return get_at(idx); }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            if (idx > 0)
                ss << ",";
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
        }
        return ss.str();
    }
    
    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->m_values.size());
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            std::ostringstream ss;
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
            vv.push_back(ss.str());
        }
        return vv;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
            bool enabled = true;
            boost::trim(item_str);
            if (!item_str.empty() && item_str.front() == '!') {
                enabled  = false;
                item_str = item_str.substr(1);
                boost::trim(item_str);
            }
        	if (item_str == NIL_STR_VALUE) {
        		if (NULLABLE)
                    this->m_values.push_back(ConfigOptionIntNullable::nil_value());
        		else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else {
	            std::istringstream iss(item_str);
	            int32_t value;
	            iss >> value;
                this->m_values.push_back(value);
            }
            this->m_enabled.push_back(enabled);
        }
        set_default_enabled();
        assert(this->m_values.size() == this->m_enabled.size());
        return true;
    }

private:
    void serialize_single_value(std::ostringstream &ss, const int32_t v, bool enabled) const {
        if (!enabled)
            ss << "!";
        if (v == ConfigOptionIntNullable::nil_value()) {
            if (NULLABLE)
               ss << NIL_STR_VALUE;
            else
                throw ConfigurationError("Serializing NaN int");
        } else
            ss << v;
	}

	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<int32_t>>(this)); }
};

using ConfigOptionInts   	   = ConfigOptionIntsTempl<false>;
using ConfigOptionIntsNullable = ConfigOptionIntsTempl<true>;

class ConfigOptionString : public ConfigOptionSingle<std::string>
{
public:
    ConfigOptionString() : ConfigOptionSingle<std::string>(std::string{}) {}
    explicit ConfigOptionString(std::string value) : ConfigOptionSingle<std::string>(std::move(value)) {}

    static ConfigOptionType static_type() { return coString; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionString(*this); }
    ConfigOptionString&     operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionString &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionString &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }
    bool 					empty() const { return this->value.empty(); }

    std::string serialize() const override
    { 
        if (!this->is_enabled())
            return std::string("!") + escape_string_cstyle(this->value);
        return escape_string_cstyle(this->value); 
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        return unescape_string_cstyle(this->is_enabled() ? str : str.substr(1), this->value);
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<std::string>>(this)); }
};

// semicolon-separated strings
class ConfigOptionStrings : public ConfigOptionVector<std::string>
{
public:
    ConfigOptionStrings() : ConfigOptionVector<std::string>() {}
    explicit ConfigOptionStrings(std::string default_value) : ConfigOptionVector<std::string>(default_value) {}
    explicit ConfigOptionStrings(size_t n, const std::string &value) : ConfigOptionVector<std::string>(n, value) {}
    explicit ConfigOptionStrings(std::initializer_list<std::string> il) : ConfigOptionVector<std::string>(std::move(il)) {}
    explicit ConfigOptionStrings(const std::vector<std::string> &values) : ConfigOptionVector<std::string>(values) {}
    explicit ConfigOptionStrings(std::vector<std::string> &&values) : ConfigOptionVector<std::string>(std::move(values)) {}

    static ConfigOptionType static_type() { return coStrings; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionStrings(*this); }
    ConfigOptionStrings&    operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionStrings &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionStrings &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }
    bool                    is_nil(int32_t idx = 0) const override { return false; }

    std::string serialize() const override
    {
        if (this->m_enabled.empty() && !this->m_values.empty()) {
            std::vector<bool> filled;
            filled.resize(this->m_values.size(), ConfigOption::is_enabled(0));
            return escape_strings_cstyle(this->m_values, filled);
        }
        return escape_strings_cstyle(this->m_values, this->m_enabled);
    }
    
    std::vector<std::string> vserialize() const override
    {
        return this->m_values;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        assert(this->m_enabled.size() == this->m_values.size());
        bool success =  unescape_strings_cstyle(str, this->m_values, this->m_enabled);
        if (success) {
            set_default_enabled();
        }
        assert(this->m_values.size() == this->m_enabled.size());
        return success;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<std::string>>(this)); }
};

class ConfigOptionPercent : public ConfigOptionFloat
{
public:
    ConfigOptionPercent() : ConfigOptionFloat(0) {}
    explicit ConfigOptionPercent(double _value) : ConfigOptionFloat(_value) {}
    
    static ConfigOptionType static_type() { return coPercent; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPercent(*this); }
    ConfigOptionPercent&    operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPercent &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPercent &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }
    
    double                  get_abs_value(double ratio_over) const { return ratio_over * this->value / 100.; }
    bool                    is_percent(size_t idx = 0) const override { return true; }
    
    std::string serialize() const override 
    {
        std::ostringstream ss;
        if (!this->is_enabled())
            ss << "!";
        ss << this->value;
        std::string s(ss.str());
        s += "%";
        return s;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        // don't try to parse the trailing % since it's optional
        std::istringstream iss(this->is_enabled() ? str : str.substr(1));
        iss >> this->value;
        return !iss.fail();
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloat>(this)); }
};

template<bool NULLABLE>
class ConfigOptionPercentsTempl : public ConfigOptionFloatsTempl<NULLABLE>
{
public:
    ConfigOptionPercentsTempl() : ConfigOptionFloatsTempl<NULLABLE>() {}
    explicit ConfigOptionPercentsTempl(double default_value) : ConfigOptionFloatsTempl<NULLABLE>(default_value) {}
    explicit ConfigOptionPercentsTempl(size_t n, double value) : ConfigOptionFloatsTempl<NULLABLE>(n, value) {}
    explicit ConfigOptionPercentsTempl(std::initializer_list<double> il) : ConfigOptionFloatsTempl<NULLABLE>(std::move(il)) {}
    explicit ConfigOptionPercentsTempl(const std::vector<double>& vec) : ConfigOptionFloatsTempl<NULLABLE>(vec) {}
    explicit ConfigOptionPercentsTempl(std::vector<double>&& vec) : ConfigOptionFloatsTempl<NULLABLE>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionPercentsTempl(*this); }
    ConfigOptionPercentsTempl& operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool operator==(const ConfigOptionPercentsTempl &rhs) const throw()
    {
        return this->m_enabled == rhs.m_enabled && ConfigOptionFloatsTempl<NULLABLE>::vectors_equal(this->m_values, rhs.m_values);
    }
    bool operator<(const ConfigOptionPercentsTempl &rhs) const throw()
    {
        return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && ConfigOptionFloatsTempl<NULLABLE>::vectors_lower(this->m_values, rhs.m_values));
    }
    double                  get_abs_value(size_t i, double ratio_over) const { return this->is_nil(i) ? 0 : ratio_over * this->get_at(i) / 100; }
    bool                    is_percent(size_t idx = 0) const override { return true; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            if (idx > 0)
                ss << ",";
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
            if (! (std::isnan(this->m_values[idx]) || this->m_values[idx] == ConfigOptionFloatNullable::nil_value()))
                ss << "%";
        }
        std::string str = ss.str();
        return str;
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->m_values.size());
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            std::ostringstream ss;
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
            if (! (std::isnan(this->m_values[idx]) || this->m_values[idx] == ConfigOptionFloatNullable::nil_value()))
                ss << "%";
            vv.push_back(ss.str());
        }
        return vv;
    }

    // The float's deserialize function shall ignore the trailing optional %.
    // bool deserialize(const std::string &str, bool append = false) override;

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloatsTempl<NULLABLE>>(this)); }
};

using ConfigOptionPercents 	   		= ConfigOptionPercentsTempl<false>;
using ConfigOptionPercentsNullable 	= ConfigOptionPercentsTempl<true>;

class ConfigOptionFloatOrPercent : public ConfigOptionPercent
{
public:
    bool percent;
    ConfigOptionFloatOrPercent() : ConfigOptionPercent(0), percent(false) {}
    explicit ConfigOptionFloatOrPercent(double _value, bool _percent) : ConfigOptionPercent(_value), percent(_percent) {}

    static ConfigOptionType     static_type() { return coFloatOrPercent; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption*               clone() const override { return new ConfigOptionFloatOrPercent(*this); }
    ConfigOptionFloatOrPercent& operator=(const ConfigOption* opt) { this->set(opt); return *this; }
    bool                        operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent*>(&rhs));
        return *this == *static_cast<const ConfigOptionFloatOrPercent*>(&rhs);
    }
    bool                        operator==(const ConfigOptionFloatOrPercent &rhs) const throw()
        { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value && this->percent == rhs.percent; }
    size_t                      hash() const throw() override 
        { size_t seed = std::hash<double>{}(this->value); return this->percent ? seed ^ 0x9e3779b9 : seed; }
    bool                        operator< (const ConfigOptionFloatOrPercent &rhs) const throw() {
        bool this_enabled = this->is_enabled();
        bool rhs_enabled = this->is_enabled();
        return std::tie(this_enabled, this->value, this->percent) < std::tie(rhs_enabled, rhs.value, rhs.percent);
    }

    double                      get_abs_value(double ratio_over) const 
        { return this->percent ? (ratio_over * this->value / 100) : this->value; }
    double                      get_float(size_t idx = 0) const override { return get_abs_value(1.); }
    bool                        is_percent(size_t idx = 0) const override { return this->percent;  }
    // special case for get/set any: use a FloatOrPercent like for FloatsOrPercents, to have the is_percent
    boost::any get_any(int32_t idx = 0) const override { return boost::any(FloatOrPercent{value, percent}); }
    void       set_any(boost::any anyval, int32_t idx = -1) override
    {
        auto fl_or_per = boost::any_cast<FloatOrPercent>(anyval);
        this->value    = fl_or_per.value;
        this->percent  = fl_or_per.percent;
    }

    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent*>(rhs));
        *this = *static_cast<const ConfigOptionFloatOrPercent*>(rhs);
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        if (!this->is_enabled())
            ss << "!";
        ss << this->value;
        std::string s(ss.str());
        if (this->percent) s += "%";
        return s;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        this->percent = str.find_first_of("%") != std::string::npos;
        std::istringstream iss(this->is_enabled() ? str : str.substr(1));
        iss >> this->value;
        return !iss.fail();
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionPercent>(this), percent); }
};

template<bool NULLABLE>
class ConfigOptionFloatsOrPercentsTempl : public ConfigOptionVector<FloatOrPercent>
{
public:
    ConfigOptionFloatsOrPercentsTempl() : ConfigOptionVector<FloatOrPercent>() {}
    explicit ConfigOptionFloatsOrPercentsTempl(FloatOrPercent default_value) : ConfigOptionVector<FloatOrPercent>(default_value) {}
    explicit ConfigOptionFloatsOrPercentsTempl(size_t n, FloatOrPercent value) : ConfigOptionVector<FloatOrPercent>(n, value) {}
    explicit ConfigOptionFloatsOrPercentsTempl(std::initializer_list<FloatOrPercent> il) : ConfigOptionVector<FloatOrPercent>(std::move(il)) {}
    explicit ConfigOptionFloatsOrPercentsTempl(const std::vector<FloatOrPercent> &vec) : ConfigOptionVector<FloatOrPercent>(vec) {}
    explicit ConfigOptionFloatsOrPercentsTempl(std::vector<FloatOrPercent> &&vec) : ConfigOptionVector<FloatOrPercent>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coFloatsOrPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionFloatsOrPercentsTempl(*this); }
    bool                    operator==(const ConfigOptionFloatsOrPercentsTempl &rhs) const throw()
    {
        return this->m_enabled == rhs.m_enabled && vectors_equal(this->m_values, rhs.m_values);
    }
    bool                    operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatsOrPercentsTempl: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<FloatOrPercent> *>(&rhs));
        return this->has_same_enabled(*static_cast<const ConfigOptionVector<FloatOrPercent> *>(&rhs)) &&
               vectors_equal(this->m_values, static_cast<const ConfigOptionVector<FloatOrPercent> *>(&rhs)->get_values());
    }
    bool                    operator<(const ConfigOptionFloatsOrPercentsTempl &rhs) const throw()
    {
        return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && vectors_lower(this->m_values, rhs.m_values));
    }

    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool                    nullable() const override { return NULLABLE; }
    // A scalar is nil, or all values of a vector are nil.
    bool                    is_nil(int32_t idx = -1) const override
    {
        if (idx < 0) {
            for (const FloatOrPercent &v : this->m_values)
                if (!(std::isnan(v.value) || v.value == NIL_VALUE().value || v.value > std::numeric_limits<float>::max()))
                    return false;
            return true;
        } else {
            return idx < int32_t(this->size()) ? (std::isnan(this->m_values[idx].value) || NIL_VALUE() == this->m_values[idx]) :
                   this->empty()               ? (std::isnan(this->default_value.value) || NIL_VALUE() == this->default_value) :
                                                 (std::isnan(this->m_values.front().value) || NIL_VALUE() == this->m_values.front());
        }
    }
    double                  get_abs_value(size_t i, double ratio_over) const {
        if (this->is_nil(i)) return 0;
        const FloatOrPercent& data = this->get_at(i);
        if (data.percent) return ratio_over * data.value / 100;
        return data.value;
    }
    double                  get_float(size_t idx = 0) const override { return get_abs_value(idx, 1.); }
    bool                    is_percent(size_t idx = 0) const override
    {
        if (this->is_nil(idx))
            return false;
        return this->get_at(idx).percent;
    }

    static inline bool is_any_nil(const boost::any &to_check) {
        bool ok = std::isnan(boost::any_cast<FloatOrPercent>(to_check).value) || boost::any_cast<FloatOrPercent>(to_check).value == NIL_VALUE().value
            || boost::any_cast<FloatOrPercent>(to_check).value > std::numeric_limits<float>::max();
        return ok;
    }
    // don't use it to compare, use is_nil() to check.
    static inline boost::any create_any_nil() { return boost::any(NIL_VALUE()); }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            if (idx > 0)
                ss << ",";
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
        }
        return ss.str();
    }
    
    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->m_values.size());
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            std::ostringstream ss;
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
            boost::trim(item_str);
            bool enabled = true;
            if (!item_str.empty() && item_str.front() == '!') {
                enabled = false;
                item_str = item_str.substr(1);
                boost::trim(item_str);
            }
            if (item_str == NIL_STR_VALUE) {
                if (NULLABLE)
                    this->m_values.push_back(NIL_VALUE());
                else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
            } else {
                bool percent = item_str.find_first_of("%") != std::string::npos;
                std::istringstream iss(item_str);
                double value;
                iss >> value;
                this->m_values.push_back({ value, percent });
            }
            this->m_enabled.push_back(enabled);
        }
        set_default_enabled();
        assert(this->m_values.size() == this->m_enabled.size());
        return true;
    }

    ConfigOptionFloatsOrPercentsTempl& operator=(const ConfigOption *opt)
    {   
        this->set(opt);
        return *this;
    }

protected:
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static FloatOrPercent   NIL_VALUE() { return FloatOrPercent{ std::numeric_limits<double>::max(), false }; }

    void serialize_single_value(std::ostringstream &ss, const FloatOrPercent &v, bool enabled) const {
        if (!enabled)
            ss << "!";
        if (std::isfinite(v.value)) {
            ss << v.value;
            if (v.percent)
                ss << "%";
        } else if (std::isnan(v.value) || v.value == NIL_VALUE().value || v.value > std::numeric_limits<float>::max()) {
            if (NULLABLE)
                ss << NIL_STR_VALUE;
            else
                throw ConfigurationError("Serializing NaN");
        } else
            throw ConfigurationError("Serializing invalid number");
    }
    static bool vectors_equal(const std::vector<FloatOrPercent> &v1, const std::vector<FloatOrPercent> &v2) {
        if (NULLABLE) {
            if (v1.size() != v2.size())
                return false;
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end(); ++ it1, ++ it2)
                if (!(((std::isnan(it1->value) || it1->value == NIL_VALUE().value || it1->value > std::numeric_limits<float>::max()) &&
                       (std::isnan(it2->value) || it2->value == NIL_VALUE().value || it1->value > std::numeric_limits<float>::max())) ||
                      *it1 == *it2))
                    return false;
            return true;
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 == v2;
    }
    static bool vectors_lower(const std::vector<FloatOrPercent> &v1, const std::vector<FloatOrPercent> &v2) {
        if (NULLABLE) {
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end() && it2 != v2.end(); ++ it1, ++ it2) {
                auto null1 = int(std::isnan(it1->value) || it1->value == NIL_VALUE().value || it1->value > std::numeric_limits<float>::max());
                auto null2 = int(std::isnan(it2->value) || it2->value == NIL_VALUE().value || it1->value > std::numeric_limits<float>::max());
                return (null1 < null2) || (null1 == null2 && *it1 < *it2);
            }
            return v1.size() < v2.size();
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 < v2;
    }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<FloatOrPercent>>(this)); }
};

using ConfigOptionFloatsOrPercents          = ConfigOptionFloatsOrPercentsTempl<false>;
using ConfigOptionFloatsOrPercentsNullable  = ConfigOptionFloatsOrPercentsTempl<true>;

class ConfigOptionPoint : public ConfigOptionSingle<Vec2d>
{
public:
    ConfigOptionPoint() : ConfigOptionSingle<Vec2d>(Vec2d(0,0)) {}
    explicit ConfigOptionPoint(const Vec2d &value) : ConfigOptionSingle<Vec2d>(value) {}
    
    static ConfigOptionType static_type() { return coPoint; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoint(*this); }
    ConfigOptionPoint&      operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoint &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }

    std::string serialize() const override
    {
        std::ostringstream ss;
        if (!this->is_enabled())
            ss << "!";
        ss << this->value(0);
        ss << "x";
        ss << this->value(1);
        return ss.str();
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }

        Vec2d point(Vec2d::Zero());
        std::istringstream iss(this->is_enabled() ? str : str.substr(1));
        std::string coord_str;
        char sep = 'x';
        // compatibility withy old ',' separator
        if (str.find(sep) == std::string::npos)
            sep = ',';
        if (std::getline(iss, coord_str, sep)) {
            std::istringstream(coord_str) >> point.x();
            if (std::getline(iss, coord_str, sep)) {
                std::istringstream(coord_str) >> point.y();
            } else
                return false;
        } else
            return false;
        this->value=point;
        return true;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<Vec2d>>(this)); }
};

class ConfigOptionPoints : public ConfigOptionVector<Vec2d>
{
public:
    ConfigOptionPoints() : ConfigOptionVector<Vec2d>() {}
    explicit ConfigOptionPoints(Vec2d default_value) : ConfigOptionVector<Vec2d>(default_value) {}
    explicit ConfigOptionPoints(size_t n, const Vec2d &value) : ConfigOptionVector<Vec2d>(n, value) {}
    explicit ConfigOptionPoints(std::initializer_list<Vec2d> il) : ConfigOptionVector<Vec2d>(std::move(il)) {}
    explicit ConfigOptionPoints(const std::vector<Vec2d> &values) : ConfigOptionVector<Vec2d>(values) {}

    static ConfigOptionType static_type() { return coPoints; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionPoints(*this); }
    ConfigOptionPoints&     operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoints &rhs) const throw()
    {
        return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values;
    }
    bool                    operator< (const ConfigOptionPoints &rhs) const throw() 
    { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled &&
               std::lexicographical_compare(this->m_values.begin(), this->m_values.end(), rhs.m_values.begin(),
                                            rhs.m_values.end(), [](const auto &l, const auto &r) { return l < r; }));
    }
    bool                    is_nil(int32_t idx = 0) const override { return false; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0 ; idx < this->m_values.size(); ++idx) {
            if (idx != 0) ss << ",";
            if (!m_enabled[idx])
                ss << "!";
            ss << this->m_values[idx].x();
            ss << "x";
            ss << this->m_values[idx].y();
        }
        return ss.str();
    }
    
    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        for (Pointfs::const_iterator it = this->m_values.begin(); it != this->m_values.end(); ++it) {
            std::ostringstream ss;
            ss << *it;
            vv.push_back(ss.str());
        }
        return vv;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string point_str;
        while (std::getline(is, point_str, ',')) {
        	boost::trim(point_str);
            bool enabled = true;
            if (!point_str.empty() && point_str.front() == '!') {
                enabled = false;
                point_str = point_str.substr(1);
            }
            Vec2d point(Vec2d::Zero());
            std::istringstream iss(point_str);
            std::string coord_str;
            if (std::getline(iss, coord_str, 'x')) {
                std::istringstream(coord_str) >> point(0);
                if (std::getline(iss, coord_str, 'x')) {
                    std::istringstream(coord_str) >> point(1);
                }
            }
            this->m_values.push_back(point);
            this->m_enabled.push_back(enabled);
        }
        set_default_enabled();
        assert(this->m_values.size() == this->m_enabled.size());
        return true;
    }

private:
	friend class cereal::access;
	template<class Archive> void save(Archive& archive) const {
        archive(flags);
		size_t cnt = this->m_values.size();
		archive(cnt);
		archive.saveBinary((const char*)this->m_values.data(), sizeof(Vec2d) * cnt);
	}
	template<class Archive> void load(Archive& archive) {
        archive(flags);
		size_t cnt;
		archive(cnt);
		this->m_values.assign(cnt, Vec2d());
		archive.loadBinary((char*)this->m_values.data(), sizeof(Vec2d) * cnt);
	}
};

class ConfigOptionPoint3 : public ConfigOptionSingle<Vec3d>
{
public:
    ConfigOptionPoint3() : ConfigOptionSingle<Vec3d>(Vec3d(0,0,0)) {}
    explicit ConfigOptionPoint3(const Vec3d &value) : ConfigOptionSingle<Vec3d>(value) {}
    
    static ConfigOptionType static_type() { return coPoint3; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoint3(*this); }
    ConfigOptionPoint3&     operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoint3 &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint3 &rhs) const throw() 
    {
        bool this_enabled = this->is_enabled();
        bool rhs_enabled  = this->is_enabled();
        return std::tie(this_enabled, this->value.x(), this->value.y()) < std::tie(rhs_enabled, rhs.value.x(), rhs.value.y());
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        if (!this->is_enabled())
            ss << "!";
        ss << this->value(0);
        ss << ",";
        ss << this->value(1);
        ss << ",";
        ss << this->value(2);
        return ss.str();
    }
    
    bool deserialize(const std::string &str_raw, bool append = false) override
    {
        UNUSED(append);
        if (!str_raw.empty() && str_raw.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        
        Vec2d point(Vec2d::Zero());
        std::string str = (this->is_enabled() ? str_raw : str_raw.substr(1));
        char dummy;
        return sscanf(str.data(), " %lf , %lf , %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) == 3 ||
               sscanf(str.data(), " %lf x %lf x %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) == 3;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<Vec3d>>(this)); }
};

class ConfigOptionGraph : public ConfigOptionSingle<GraphData>
{
public:
    ConfigOptionGraph() : ConfigOptionSingle<GraphData>(GraphData()) {}
    explicit ConfigOptionGraph(const GraphData &value) : ConfigOptionSingle<GraphData>(value) {}
    
    static ConfigOptionType static_type() { return coGraph; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionGraph(*this); }
    ConfigOptionGraph&      operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionGraph &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionGraph &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }
    
    std::string serialize() const override
    {
        std::ostringstream ss;
        if(!this->is_enabled())
            ss << "!";
        ss << this->value.serialize();
        return ss.str();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        GraphData data;
        bool enabled = true;
        if (!str.empty() && str.front() == '!') {
            enabled = false;
        }
        bool ok = data.deserialize(enabled ? str : str.substr(1));
        if (!ok)
            return false;
        this->set_enabled(enabled);
        this->value = data;
        return true;
    }
    

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<GraphData>>(this)); }
};


class ConfigOptionGraphs : public ConfigOptionVector<GraphData>
{
public:
    ConfigOptionGraphs() : ConfigOptionVector<GraphData>() {}
    explicit ConfigOptionGraphs(const GraphData &value) : ConfigOptionVector<GraphData>(value) {}
    explicit ConfigOptionGraphs(size_t n, const GraphData& value) : ConfigOptionVector<GraphData>(n, value) {}
    explicit ConfigOptionGraphs(std::initializer_list<GraphData> il) : ConfigOptionVector<GraphData>(std::move(il)) {}
    explicit ConfigOptionGraphs(const std::vector<GraphData> &values) : ConfigOptionVector<GraphData>(values) {}
    
    static ConfigOptionType static_type() { return coGraphs; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionGraphs(*this); }
    ConfigOptionGraphs&    operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionGraphs &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool operator<(const ConfigOptionGraphs &rhs) const throw()
    {
        return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && 
            std::lexicographical_compare(this->m_values.begin(), this->m_values.end(), rhs.m_values.begin(),
                                            rhs.m_values.end(), [](const auto &l, const auto &r) { return l < r; }));
    }
    bool                    is_nil(int32_t idx = 0) const override { return false; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0; idx < size(); ++idx) {
            const GraphData &graph = this->m_values[idx];
            if (idx != 0) ss << ",";
            if (!this->is_enabled(idx)) ss << "!";
            ss << graph.serialize();
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        for (size_t idx = 0; idx < size(); ++idx) {
            const GraphData &graph = this->m_values[idx];
            std::ostringstream ss;
            if (!this->is_enabled(idx)) ss << "!";
            ss << graph.serialize();
            vv.push_back(ss.str());
        }
        return vv;
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string graph_str;
        char sep = ',';
        if (str.find(';') != std::string::npos)
            sep = ';';
        while (std::getline(is, graph_str, sep)) {
            boost::trim(graph_str);
            bool enabled = true;
            if (!graph_str.empty() && graph_str.front() == '!') {
                enabled = false;
                graph_str = graph_str.substr(1);
                boost::trim(graph_str);
            }
            GraphData graph;
            bool ok = graph.deserialize(graph_str);
            if (ok) {
                this->m_values.push_back(std::move(graph));
                this->m_enabled.push_back(enabled);
            }
        }
        set_default_enabled();
        return true;
    }

private:
    // use the string representation for cereal archive, as it's convenient.
    // TODO: try to save/load the vector of pair of double and the two bits.
    friend class cereal::access;
    template<class Archive> void save(Archive& archive) const {
        archive(flags);
        std::string serialized = this->serialize();
        size_t cnt = serialized.size();
        archive(cnt);
        archive.saveBinary((const char*)serialized.data(), sizeof(char) * cnt);
    }
    template<class Archive> void load(Archive& archive) {
        archive(flags);
        size_t cnt;
        archive(cnt);
        std::string serialized;
        serialized.assign(cnt, char());
        archive.loadBinary((char*)serialized.data(), sizeof(char) * cnt);
        deserialize(serialized, false);
    }
};

class ConfigOptionBool : public ConfigOptionSingle<bool>
{
public:
    ConfigOptionBool() : ConfigOptionSingle<bool>(false) {}
    explicit ConfigOptionBool(bool _value) : ConfigOptionSingle<bool>(_value) {}
    
    static ConfigOptionType static_type() { return coBool; }
    ConfigOptionType        type()      const override { return static_type(); }
    bool                    get_bool(size_t idx = 0) const override { return this->value; }
    int32_t                 get_int(size_t idx = 0) const override { return this->value ? 1 : 0; }
    double                  get_float(size_t idx = 0) const override { return this->value ? 1. : 0.; }
    ConfigOption*           clone()     const override { return new ConfigOptionBool(*this); }
    ConfigOptionBool&       operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionBool &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() &&this->value == rhs.value; }
    bool                    operator< (const ConfigOptionBool &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && int(this->value) < int(rhs.value)); }

    std::string serialize() const override
    {
        return std::string(this->is_enabled() ? "" : "!") + std::string(this->value ? "1" : "0");
    }
    
    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (str.empty())
            return false;
        if (str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        if (str.back() == '1') {
            this->value = true;
            return true;
        }
        if (str.back() == '0') {
            this->value = false;
            return true;
        }
        return false;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<bool>>(this)); }
};

template<bool NULLABLE>
class ConfigOptionBoolsTempl : public ConfigOptionVector<unsigned char>
{
public:
    ConfigOptionBoolsTempl() : ConfigOptionVector<unsigned char>() {}
    explicit ConfigOptionBoolsTempl(bool default_value) : ConfigOptionVector<unsigned char>(default_value) {}
    explicit ConfigOptionBoolsTempl(size_t n, bool value) : ConfigOptionVector<unsigned char>(n, (unsigned char)value) {}
    explicit ConfigOptionBoolsTempl(std::initializer_list<bool> il)
    {
        this->m_values.reserve(il.size());
        for (bool b : il) this->m_values.emplace_back((unsigned char) b);
        this->m_enabled.resize(this->m_values.size(), ConfigOption::is_enabled());
        assert(m_enabled.size() == size());
    }
    explicit ConfigOptionBoolsTempl(std::initializer_list<unsigned char> il)
    {
        this->m_values.reserve(il.size());
        for (unsigned char b : il) this->m_values.emplace_back(b);
        this->m_enabled.resize(this->m_values.size(), ConfigOption::is_enabled());
        assert(m_enabled.size() == size());
    }
	explicit ConfigOptionBoolsTempl(const std::vector<unsigned char>& vec) : ConfigOptionVector<unsigned char>(vec) {}
	explicit ConfigOptionBoolsTempl(std::vector<unsigned char>&& vec) : ConfigOptionVector<unsigned char>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coBools; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionBoolsTempl(*this); }
    ConfigOptionBoolsTempl& operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionBoolsTempl &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionBoolsTempl &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values <  rhs.m_values); }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static unsigned char    NIL_VALUE() { return std::numeric_limits<unsigned char>::max(); }
    // A scalar is nil, or all values of a vector are nil.
    bool is_nil(int32_t idx = -1) const override
    {
        if (idx < 0) {
            for (uint8_t v : this->m_values)
                if (v != NIL_VALUE())
                    return false;
            return true;
        } else {
            return idx < int32_t(this->size()) ? NIL_VALUE() == this->m_values[idx] :
                   this->empty()               ? NIL_VALUE() == this->default_value :
                                                 NIL_VALUE() == this->m_values.front();
    }
    }
    bool                    get_bool(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0 ? 1 : 0; }
    double                  get_float(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0 ? 1. : 0.; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            if (idx > 0)
                ss << ",";
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
        }
        return ss.str();
    }
    
    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        for (size_t idx = 0;idx < this->m_values.size(); ++idx) {
            std::ostringstream ss;
            this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
            vv.push_back(ss.str());
        }
        return vv;
    }

    ConfigHelpers::DeserializationResult deserialize_with_substitutions(const std::string &str, bool append, ConfigHelpers::DeserializationSubstitution substitution)
    {
        if (!append) {
            this->m_values.clear();
            this->m_enabled.clear();
        }
        std::istringstream is(str);
        std::string item_str;
        bool substituted = false;
        while (std::getline(is, item_str, ',')) {
        	boost::trim(item_str);
            bool enabled = true;
            if (!item_str.empty() && item_str.front() == '!') {
                enabled = false;
                item_str = item_str.substr(1);
                boost::trim(item_str);
            }
        	unsigned char new_value = 0;
        	if (item_str == NIL_STR_VALUE) {
        		if (NULLABLE)
                    new_value = NIL_VALUE();
        		else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else if (item_str == "1") {
        		new_value = true;
        	} else if (item_str == "0") {
        		new_value = false;
        	} else if (substitution != ConfigHelpers::DeserializationSubstitution::Disabled && ConfigHelpers::looks_like_enum_value(item_str)) {
        		new_value = ConfigHelpers::enum_looks_like_true_value(item_str) || substitution == ConfigHelpers::DeserializationSubstitution::DefaultsToTrue;
        		substituted = true;
        	} else
        		return ConfigHelpers::DeserializationResult::Failed;
            this->m_values.push_back(new_value);
            this->m_enabled.push_back(enabled);
        }
        set_default_enabled();
        assert(this->m_values.size() == this->m_enabled.size());
        return substituted ? ConfigHelpers::DeserializationResult::Substituted : ConfigHelpers::DeserializationResult::Loaded;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
    	return this->deserialize_with_substitutions(str, append, ConfigHelpers::DeserializationSubstitution::Disabled) == ConfigHelpers::DeserializationResult::Loaded;
    }

protected:
    void serialize_single_value(std::ostringstream &ss, const unsigned char v, bool enabled) const {
        if (!enabled)
            ss << "!";
        if (v == NIL_VALUE()) {
            if (NULLABLE)
                ss << NIL_STR_VALUE;
            else
                throw ConfigurationError("Serializing NaN");
        } else
            ss << (v ? "1" : "0");
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<unsigned char>>(this)); }
};

using ConfigOptionBools    	    = ConfigOptionBoolsTempl<false>;
using ConfigOptionBoolsNullable = ConfigOptionBoolsTempl<true>;

// Map from an enum integer value to an enum name.
typedef std::vector<std::string>  t_config_enum_names;
// Map from an enum name to an enum integer value.
typedef std::map<std::string,int32_t> t_config_enum_values;

template <class T>
class ConfigOptionEnum : public ConfigOptionSingle<T>
{
public:
    // by default, use the first value (0) of the T enum type
    ConfigOptionEnum() : ConfigOptionSingle<T>(static_cast<T>(0)) {}
    explicit ConfigOptionEnum(T _value) : ConfigOptionSingle<T>(_value) {}
    
    static ConfigOptionType static_type() { return coEnum; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionEnum<T>(*this); }
    ConfigOptionEnum<T>&    operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionEnum<T> &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionEnum<T> &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && int(this->value) < int(rhs.value)); }
    int32_t                 get_int(size_t idx = 0) const override { return int32_t(this->value); }
    void                    set_enum_int(int32_t val) override { this->value = T(val); }
    // special case for get/set any: use a int like for ConfigOptionEnumGeneric, to simplify
    boost::any get_any(int32_t idx = -1) const override { return boost::any(get_int()); }
    void       set_any(boost::any anyval, int32_t idx = -1) override { set_enum_int(boost::any_cast<int32_t>(anyval)); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() == rhs.is_enabled() && this->value == (T)rhs.get_int();
    }

    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = (T)rhs->get_int();
        this->flags = rhs->flags;
    }

    std::string serialize() const override
    {
        const t_config_enum_names& names = ConfigOptionEnum<T>::get_enum_names();
        assert(static_cast<int>(this->value) < int(names.size()));
        return std::string(this->is_enabled() ? "" : "!") + names[static_cast<int>(this->value)];
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        if (!str.empty() && str.front() == '!') {
            this->set_enabled(false);
        } else {
            this->set_enabled(true);
        }
        return from_string(this->is_enabled() ? str : str.substr(1), this->value);
    }

    static bool has(T value) 
    {
        for (const std::pair<std::string, int32_t> &kvp : ConfigOptionEnum<T>::get_enum_values())
            if (kvp.second == value)
                return true;
        return false;
    }

    // Map from an enum integer value to name.
    static const t_config_enum_names& get_enum_names();
    // Map from an enum name to an enum integer value.
    static const t_config_enum_values& get_enum_values();

    static bool from_string(const std::string &str, T &value)
    {
        const t_config_enum_values &enum_keys_map = ConfigOptionEnum<T>::get_enum_values();
        auto it = enum_keys_map.find(str);
        if (it == enum_keys_map.end())
            return false;
        value = static_cast<T>(it->second);
        return true;
    }
};

// Generic enum configuration value.
// We use this one in DynamicConfig objects when creating a config value object for ConfigOptionType == coEnum.
// In the StaticConfig, it is better to use the specialized ConfigOptionEnum<T> containers.
class ConfigOptionEnumGeneric : public ConfigOptionInt
{
public:
    ConfigOptionEnumGeneric(const t_config_enum_values* keys_map = nullptr) : keys_map(keys_map) {}
    explicit ConfigOptionEnumGeneric(const t_config_enum_values* keys_map, int32_t value) : ConfigOptionInt(value), keys_map(keys_map) {}

    const t_config_enum_values* keys_map;
    
    static ConfigOptionType     static_type() { return coEnum; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption*               clone() const override { return new ConfigOptionEnumGeneric(*this); }
    ConfigOptionEnumGeneric&    operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                        operator==(const ConfigOptionEnumGeneric &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                        operator< (const ConfigOptionEnumGeneric &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() == rhs.is_enabled() && this->value == rhs.get_int();
    }

    void set_enum_int(int32_t val) override { this->value = val; }
    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = rhs->get_int();
        this->flags = rhs->flags;
    }

    std::string serialize() const override
    {
        std::string prefix;
        if (!this->is_enabled())
            prefix = "!";
        for (const auto &kvp : *this->keys_map)
            if (kvp.second == this->value) 
                return prefix + kvp.first;
        return prefix;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        auto it = this->keys_map->find(str);
        if (it == this->keys_map->end())
            return false;
        this->value = it->second;
        return true;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive& ar) { ar(cereal::base_class<ConfigOptionInt>(this)); }
};

// Definition of values / labels for a combo box.
// Mostly used for closed enums (when type == coEnum), but may be used for 
// open enums with ints resp. floats, if gui_type is set to GUIType::i_enum_open" resp. GUIType::f_enum_open.
class ConfigOptionEnumDef {
public:
    bool                            has_values() const { return ! m_values.empty(); }
    bool                            has_labels() const { return ! m_labels.empty(); }
    const std::vector<std::string>& values() const { return m_values; }
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

    ConfigOptionEnumDef*    clone() const { return new ConfigOptionEnumDef{ *this }; }

private:
    friend ConfigDef;
    friend ConfigOptionDef;

    // Only allow ConfigOptionEnumDef() to be created from ConfigOptionDef.
    ConfigOptionEnumDef() = default;
    ConfigOptionEnumDef(const ConfigOptionEnumDef &other) // default copy, but with a check for m_enum_names when it reference 'itself'
        : m_values(other.m_values)
        , m_labels(other.m_labels)
        , m_values_ordinary(other.m_values_ordinary)
        , m_enum_names(other.m_enum_names)
        , m_enum_keys_map(other.m_enum_keys_map)
    {
        if (other.m_enum_names == &other.m_values) {
            this->m_enum_names = &this->m_values;
        }
    }

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
	// If a type is nullable, then it accepts a "nil" value (scalar) or "nil" values (vector).
	bool								nullable		= false;
    // Default value of this option. The default value object is owned by ConfigDef, it is released in its destructor.
    Slic3r::clonable_ptr<const ConfigOption> default_value;
    void 								set_default_value(const ConfigOption* ptr) {
        assert(!ptr->is_vector());
        this->default_value = Slic3r::clonable_ptr<const ConfigOption>(ptr);
    }
    void 								set_default_value(ConfigOptionVectorBase* ptr) {
        ptr->set_is_extruder_size(this->is_vector_extruder);
        this->default_value = Slic3r::clonable_ptr<const ConfigOption>(ptr);
    }
    template<typename T> const T* 		get_default_value() const { return static_cast<const T*>(this->default_value.get()); }

    // Create an empty option to be used as a base for deserialization of DynamicConfig.
    ConfigOption*						create_empty_option() const;
    // Create a default option to be inserted into a DynamicConfig.
    ConfigOption*						create_default_option() const;

    bool                                is_scalar()     const { return (int(this->type) & int(coVectorType)) == 0; }

    template<class Archive> ConfigOption* load_option_from_archive(Archive &archive) const {
        if (this->nullable) {
            switch (this->type) {
            case coFloat:           { auto opt = new ConfigOptionFloatNullable();   archive(*opt); return opt; }
            case coInt:             { auto opt = new ConfigOptionIntNullable();	    archive(*opt); return opt; }
            case coFloats:          { auto opt = new ConfigOptionFloatsNullable();  archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coInts:            { auto opt = new ConfigOptionIntsNullable();    archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coPercents:        { auto opt = new ConfigOptionPercentsNullable();archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coFloatsOrPercents:{ auto opt = new ConfigOptionFloatsOrPercentsNullable();archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coBools:           { auto opt = new ConfigOptionBoolsNullable();   archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
		    default:                throw ConfigurationError(std::string("ConfigOptionDef::load_option_from_archive(): Unknown nullable option type for option ") + this->opt_key);
		    }
    	} else {
		    switch (this->type) {
            case coFloat:           { auto opt = new ConfigOptionFloat();           archive(*opt); return opt; }
            case coFloats:          { auto opt = new ConfigOptionFloats();          archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coInt:             { auto opt = new ConfigOptionInt();             archive(*opt); return opt; }
            case coInts:            { auto opt = new ConfigOptionInts();            archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coString:          { auto opt = new ConfigOptionString();          archive(*opt); return opt; }
            case coStrings:         { auto opt = new ConfigOptionStrings();         archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coPercent:         { auto opt = new ConfigOptionPercent();         archive(*opt); return opt; }
            case coPercents:        { auto opt = new ConfigOptionPercents();        archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coFloatOrPercent:  { auto opt = new ConfigOptionFloatOrPercent();  archive(*opt); return opt; }
            case coFloatsOrPercents:{ auto opt = new ConfigOptionFloatsOrPercents();archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coPoint:           { auto opt = new ConfigOptionPoint();           archive(*opt); return opt; }
            case coPoints:          { auto opt = new ConfigOptionPoints();          archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coPoint3:          { auto opt = new ConfigOptionPoint3();          archive(*opt); return opt; }
            case coGraph:           { auto opt = new ConfigOptionGraph();           archive(*opt); return opt; }
            case coGraphs:          { auto opt = new ConfigOptionGraphs();          archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
            case coBool:            { auto opt = new ConfigOptionBool();            archive(*opt); return opt; }
            case coBools:           { auto opt = new ConfigOptionBools();           archive(*opt); opt->set_is_extruder_size(this->is_vector_extruder); return opt; }
		    case coEnum:            { auto opt = new ConfigOptionEnumGeneric(this->enum_def->m_enum_keys_map); archive(*opt); return opt; }
		    default:                throw ConfigurationError(std::string("ConfigOptionDef::load_option_from_archive(): Unknown option type for option ") + this->opt_key);
		    }
		}
	}

    template<class Archive> ConfigOption* save_option_to_archive(Archive &archive, const ConfigOption *opt) const {
    	if (this->nullable) {
		    switch (this->type) {
            case coFloat:           archive(*static_cast<const ConfigOptionFloatNullable*>(opt));  break;
            case coInt:             archive(*static_cast<const ConfigOptionIntNullable*>(opt));  break;
		    case coFloats:          archive(*static_cast<const ConfigOptionFloatsNullable*>(opt));  break;
		    case coInts:            archive(*static_cast<const ConfigOptionIntsNullable*>(opt));    break;
		    case coPercents:        archive(*static_cast<const ConfigOptionPercentsNullable*>(opt));break;
		    case coFloatsOrPercents:archive(*static_cast<const ConfigOptionFloatsOrPercentsNullable*>(opt));break;
		    case coBools:           archive(*static_cast<const ConfigOptionBoolsNullable*>(opt)); 	break;
		    default:                throw ConfigurationError(std::string("ConfigOptionDef::save_option_to_archive(): Unknown nullable option type for option ") + this->opt_key);
		    }
		} else {
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
    std::string                         get_full_label() const { return !full_label.empty() ? full_label : label; }
    // With which printer technology is this configuration valid?
    PrinterTechnology                   printer_technology = ptUnknown;
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
    // Can be enabled/disabled by a check box.
    bool                                can_enable      = false;
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
    void    enum_def_new() {
        if (enum_def)
            enum_def->clear();
        else
            enum_def = Slic3r::clonable_ptr<ConfigOptionEnumDef>(new ConfigOptionEnumDef{});
    }
};

inline bool operator<(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw() {
    return lhs.opt_def->opt_key < rhs.opt_def->opt_key ||
           (lhs.opt_def->opt_key == rhs.opt_def->opt_key && lhs.old_value < rhs.old_value);
}
inline bool operator==(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw() {
    return lhs.opt_def == rhs.opt_def && lhs.old_value == rhs.old_value;
}

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

protected:
    ConfigOptionDef*        add(const t_config_option_key &opt_key, ConfigOptionType type);
    ConfigOptionDef*        add_nullable(const t_config_option_key &opt_key, ConfigOptionType type);
    // Finalize open / close enums, validate everything.
    void                    finalize();
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
        const ConfigOption* opt = this->optptr(opt_key);
        return (opt == nullptr || opt->type() != TYPE::static_type()) ? nullptr : static_cast<const TYPE*>(opt);
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
    // Verify whether the opt_key has not been obsoleted or renamed.
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    // handle_legacy() is called internally by set_deserialize().
    virtual void                    handle_legacy(t_config_option_key &/*opt_key*/, std::string &/*value*/) const {}
    // Verify whether the opt_key has to be converted or isn't present in prusaslicer
    // Both opt_key and value may be modified by to_prusa().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by to_prusa().
    virtual void                    to_prusa(t_config_option_key&/*opt_key*/, std::string&/*value*/) const {}
    // Called after a config is loaded as a whole.
    // Perform composite conversions, for example merging multiple keys into one key.
    // For conversion of single options, the handle_legacy() method above is called.
    virtual void                    handle_legacy_composite() {}

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
        return (opt == nullptr || opt->type() != TYPE::static_type()) ? nullptr : static_cast<TYPE*>(opt);
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

    // Set a configuration value from a string, it will call an overridable handle_legacy() 
    // to resolve renamed and removed configuration keys.
    bool set_deserialize_nothrow(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions, bool append = false);
	// May throw BadOptionTypeException() if the operation fails.
    void set_deserialize(const t_config_option_key &opt_key, const std::string &str, ConfigSubstitutionContext& config_substitutions, bool append = false);
    void set_deserialize(const t_config_option_key &opt_key, const std::string &str){ //for tests
        ConfigSubstitutionContext no_context(ForwardCompatibilitySubstitutionRule::Disable);
        set_deserialize(opt_key, str, no_context);
    }
    void set_deserialize_strict(const t_config_option_key &opt_key, const std::string &str, bool append = false)
        { ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable }; this->set_deserialize(opt_key, str, ctxt, append); }
    struct SetDeserializeItem {
    	SetDeserializeItem(const char *opt_key, const char *opt_value, bool append = false) : opt_key(opt_key), opt_value(opt_value), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const std::string &opt_value, bool append = false) : opt_key(opt_key), opt_value(opt_value), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const std::string_view opt_value, bool append = false) : opt_key(opt_key), opt_value(opt_value), append(append) {}
    	SetDeserializeItem(const char *opt_key, const bool value, bool append = false) : opt_key(opt_key), opt_value(value ? "1" : "0"), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const bool value, bool append = false) : opt_key(opt_key), opt_value(value ? "1" : "0"), append(append) {}
    	SetDeserializeItem(const char *opt_key, const int32_t value, bool append = false) : opt_key(opt_key), opt_value(std::to_string(value)), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const int32_t value, bool append = false) : opt_key(opt_key), opt_value(std::to_string(value)), append(append) {}
        SetDeserializeItem(const char *opt_key, const std::initializer_list<int32_t> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const std::initializer_list<int32_t> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}
        SetDeserializeItem(const char *opt_key, const float value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const float value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const char *opt_key, const double value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const double value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const char *opt_key, const std::initializer_list<float> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const std::initializer_list<float> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}
        SetDeserializeItem(const char *opt_key, const std::initializer_list<double> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const std::initializer_list<double> values, bool append = false) : opt_key(opt_key), opt_value(format(values)), append(append) {}

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
    double get_abs_value(const t_config_option_key &opt_key, double ratio_over) const; //TODO: 2.7: use extruder_id, reform the gat_abs_value to have common signature.

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

    // In ConfigManipulation::toggle_print_fff_options, it is called on option with type ConfigOptionEnumGeneric* and also ConfigOptionEnum*.
    // Thus the virtual method getInt() is used to retrieve the enum value.
    template<typename ENUM>
    ENUM                opt_enum(const t_config_option_key &opt_key) const                      { return static_cast<ENUM>(this->option(opt_key)->get_int()); }

    bool                opt_bool(const t_config_option_key &opt_key) const                      { return this->option<ConfigOptionBool>(opt_key)->value != 0; }
    bool                opt_bool(const t_config_option_key &opt_key, size_t idx) const          { return this->option<ConfigOptionBools>(opt_key)->get_at(idx) != 0; }
    bool&               opt_bool(const t_config_option_key &opt_key)                            { return this->option<ConfigOptionBool>(opt_key)->value; }
    uint8_t&            opt_bool(const t_config_option_key &opt_key, size_t idx)          { return this->option<ConfigOptionBools>(opt_key)->get_at(idx); }

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

	// Set all the nullable values to nils.
    void null_nullables();

    static std::map<t_config_option_key, std::string> load_gcode_string_legacy(const char* str);
    static size_t load_from_gcode_string_legacy(ConfigBase& config, const char* str, ConfigSubstitutionContext& substitutions);

#ifdef _DEBUG
    //little dirty test to be sure it exists (not needed, but it's good for testing)
    int32_t m_exists = 0x55555555;
    bool    exists() { return m_exists == 0x55555555; }
    ~ConfigBase() override { m_exists = 0; }
#endif

private:
    // Set a configuration value from a string.
    bool set_deserialize_raw(const t_config_option_key& opt_key_src, const std::string& value, ConfigSubstitutionContext& substitutions, bool append);
};

// Configuration store with dynamic number of configuration values.
// In Slic3r, the dynamic config is mostly used at the user interface layer.
class DynamicConfig : public virtual ConfigBase
{
public:
    DynamicConfig() = default;
    DynamicConfig(const DynamicConfig &rhs) { *this = rhs; }
    DynamicConfig(DynamicConfig &&rhs) noexcept : options(std::move(rhs.options)) { rhs.options.clear(); }
	explicit DynamicConfig(const ConfigBase &rhs, const t_config_option_keys &keys);
	explicit DynamicConfig(const ConfigBase& rhs) : DynamicConfig(rhs, rhs.keys()) {}
	virtual ~DynamicConfig() override = default;

    // Copy a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def(). 
    DynamicConfig& operator=(const DynamicConfig &rhs) 
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        this->clear();
        for (const auto &kvp : rhs.options)
            this->options[kvp.first].reset(kvp.second->clone());
        return *this;
    }

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def(). 
    DynamicConfig& operator=(DynamicConfig &&rhs) noexcept
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        this->clear();
        this->options = std::move(rhs.options);
        rhs.options.clear();
        return *this;
    }

    // Add a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator+=(const DynamicConfig &rhs)
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        for (const auto &kvp : rhs.options) {
            auto it = this->options.find(kvp.first);
            if (it == this->options.end())
                this->options[kvp.first].reset(kvp.second->clone());
            else {
                assert(it->second->type() == kvp.second->type());
                if (it->second->type() == kvp.second->type())
                    *it->second = *kvp.second;
                else
                    it->second.reset(kvp.second->clone());
            }
        }
        return *this;
    }

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator+=(DynamicConfig &&rhs) 
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        for (auto &kvp : rhs.options) {
            auto it = this->options.find(kvp.first);
            if (it == this->options.end()) {
                this->options.insert(std::make_pair(kvp.first, std::move(kvp.second)));
            } else {
                assert(it->second->type() == kvp.second->type());
                it->second = std::move(kvp.second);
            }
        }
        rhs.options.clear();
        return *this;
    }

    bool           operator==(const DynamicConfig &rhs) const;
    bool           operator!=(const DynamicConfig &rhs) const { return ! (*this == rhs); }

    void swap(DynamicConfig &other) 
    { 
        std::swap(this->options, other.options);
    }

    void clear()
    { 
        this->options.clear(); 
    }

    bool erase(const t_config_option_key &opt_key)
    { 
        auto it = this->options.find(opt_key);
        if (it == this->options.end())
            return false;
        this->options.erase(it);
        return true;
    }

    // Remove options with all nil values, those are optional and it does not help to hold them.
    size_t remove_nil_options();

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
    bool                    set_key_value(const std::string &opt_key, ConfigOption *opt)
    {
        assert(opt != nullptr);
        auto it = this->options.find(opt_key);
        if (it == this->options.end()) {
            this->options[opt_key].reset(opt);
            return true;
        } else {
            it->second.reset(opt);
            return false;
        }
    }

    // Are the two configs equal? Ignoring options not present in both configs and phony fields.
    bool equals(const DynamicConfig &other, bool even_phony =true) const;
    // Returns options differing in the two configs, ignoring options not present in both configs and phony fields.
    t_config_option_keys diff(const DynamicConfig &other, bool even_phony=true) const;
    // Returns options being equal in the two configs, ignoring options not present in both configs.
    t_config_option_keys equal(const DynamicConfig &other) const;

    // Command line processing
    bool                read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys = nullptr);

    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cbegin() const { return options.cbegin(); }
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cend()   const { return options.cend(); }
    size_t                        												 size()   const { return options.size(); }

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

}

#endif
