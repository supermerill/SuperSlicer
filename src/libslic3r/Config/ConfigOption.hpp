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
#ifndef slic3r_ConfigOption_hpp_
#define slic3r_ConfigOption_hpp_

#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/any.hpp>
#include <cereal/access.hpp>
#include <cereal/types/base_class.hpp>

#include "libslic3r/Api/plugin/c/slic3r_config_option_type.h"

#include "../Exception.hpp"
#include "../Point.hpp"

namespace Slic3r {
    struct FloatOrPercent
    {
        double  value;
        bool    percent;

        double get_effective_value(double ratio_over) const {
            return this->percent ? (ratio_over * this->value / 100) : this->value;
        }
        double get_float(size_t idx = 0) const { return get_effective_value(1.); }
        bool is_percent(size_t idx = 0) const { return this->percent; }
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
        double inverse_interpolate(double y_value) const;

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
        // title to the graph window
        std::string title;
        // a text written in the graph window, to explain how to use it for hte specified setting
        std::string description;
        // label displayed on the left of the y axis (rotated)
        std::string y_label;
        // label displayed below the x axis
        std::string x_label;
        // what's displayed instead of the graph when there is no points
        std::string null_label;
        // default values for min & max x, it's enforced if label_min_x & label_max_x are empty
        double min_x, max_x, step_x;
        // default values for min & max y, it's enforced if label_min_y & label_max_y are empty
        double min_y, max_y, step_y;
        // label for the box that allow to change the min x (hidden if empty)
        std::string label_min_x;
        // label for the box that allow to change the max x (hidden if empty)
        std::string label_max_x;
        // label for the box that allow to change the min y (hidden if empty)
        std::string label_min_y;
        // label for the box that allow to change the max y (hidden if empty)
        std::string label_max_y;
        // the kinds of graph types allowed. the button that allow toc hange them is hidden if only one is available.
        std::vector<GraphData::GraphType> allowed_types;
        // the values when you click on the "reset" button (dynamically set to the current data stored in the setting)
        GraphData reset_vals;
        // min & max enforced points. if no values here, no enforced points.
        // if only one, only the min is enforced.
        // if the first value has nan x, only the max is enforced
        // if more than 2 values, other are ignored
        Pointfs enforced_values;
    };

    inline void config_hash_combine_value(std::size_t &seed, std::size_t value) noexcept {
        seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    template<class T> inline void config_hash_combine(std::size_t &seed, const T &value) noexcept {
        config_hash_combine_value(seed, std::hash<T>{}(value));
    }
}

namespace std {
    template<> struct hash<Slic3r::FloatOrPercent> {
        std::size_t operator()(const Slic3r::FloatOrPercent &v) const noexcept;
    };
    
    template<> struct hash<Slic3r::GraphData> {
        std::size_t operator()(const Slic3r::GraphData &v) const noexcept;
    };

    template<> struct hash<Slic3r::Vec2d> {
        std::size_t operator()(const Slic3r::Vec2d &v) const noexcept;
    };

    template<> struct hash<Slic3r::Vec3d> {
        std::size_t operator()(const Slic3r::Vec3d &v) const noexcept;
    };
}

namespace Slic3r {

// Name of the configuration option.
typedef std::string                 t_config_option_key;
typedef std::vector<std::string>    t_config_option_keys;

// Name of the configuration option.+ the idx of the element used (if only an elemnt is needed)
// idx is -1 if it's not a vector, or if the whole vector is used.
struct OptionKeyIdx
{
#ifdef __APPLE__
    // apple 'set<X> t2 = t1' needs the assignment operator, that needs to have no const data
    t_config_option_key key;
    int32_t idx;
#else
    const t_config_option_key key;
    // idx, -1 if the option is a scalar or it's for the whole vector and not a specific item
    const int32_t idx;
#endif

    //C++20
#if __cplusplus >= 202002L
    auto operator<=>(const OptionKeyIdx&) const = default;
#else
    auto tie() const { return std::tie(key,idx); }
    bool operator==(const OptionKeyIdx &other) const { return idx == other.idx && key == other.key; }
    bool operator<(const OptionKeyIdx& other) const { return tie() < other.tie(); }
#endif
    static inline OptionKeyIdx scalar(const t_config_option_key &key) {
        return OptionKeyIdx{key, -1};
    }
};
// hash for unordered_map
//inline void hash_combine(std::size_t& seed) { }
//template <typename T, typename... Rest>
//inline void hash_combine(size_t& seed, const T& v, Rest... rest)
//{
//    seed ^= ::qHash(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//    (hashCombine(seed, rest), ...);
//}
//namespace std {
//template<> struct hash<OptionKeyIdx>
//{
//    std::size_t operator()(const OptionKeyIdx &t) const {
//        std::size_t ret = 0;
//        hash_combine(ret, __VA_ARGS__);
//        return ret;
//    }
//};
//}
//typedef std::string                 t_config_option_key_id;

extern std::string  escape_string_cstyle(const std::string &str);
extern std::string  escape_strings_cstyle(const std::vector<std::string> &strs);
extern std::string  escape_strings_cstyle(const std::vector<std::string> &strs, const std::vector<bool> &enables);
extern bool         unescape_string_cstyle(const std::string &str, std::string &out);
extern bool         unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values);
extern bool         unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values, std::vector<bool> &out_enables);

extern std::string  escape_ampersand(const std::string& str);


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
void trim(std::string &value);
bool looks_like_enum_value(std::string value);
bool enum_looks_like_bool_value(std::string value);
bool enum_looks_like_true_value(std::string value);

enum class DeserializationSubstitution { Disabled, DefaultsToFalse, DefaultsToTrue };

enum class DeserializationResult {
    Loaded,
    Substituted,
    Failed,
};
} // namespace ConfigHelpers

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

// Type of a configuration value. The numeric values are defined once in the
// small C ABI header and reused here to keep host and plugin code in lockstep.
using ConfigOptionType = config_option_type;

static constexpr ConfigOptionType coVectorType = SLIC3R_CONFIG_OPTION_VECTOR_TYPE; // 16384
static constexpr ConfigOptionType coNone = SLIC3R_CONFIG_OPTION_NONE;
static constexpr ConfigOptionType coFloat = SLIC3R_CONFIG_OPTION_FLOAT;
static constexpr ConfigOptionType coFloats = SLIC3R_CONFIG_OPTION_FLOATS;
static constexpr ConfigOptionType coInt = SLIC3R_CONFIG_OPTION_INT;
static constexpr ConfigOptionType coInts = SLIC3R_CONFIG_OPTION_INTS;
static constexpr ConfigOptionType coString = SLIC3R_CONFIG_OPTION_STRING;
static constexpr ConfigOptionType coStrings = SLIC3R_CONFIG_OPTION_STRINGS;
static constexpr ConfigOptionType coPercent = SLIC3R_CONFIG_OPTION_PERCENT;
static constexpr ConfigOptionType coPercents = SLIC3R_CONFIG_OPTION_PERCENTS;
static constexpr ConfigOptionType coFloatOrPercent = SLIC3R_CONFIG_OPTION_FLOAT_OR_PERCENT;
static constexpr ConfigOptionType coFloatsOrPercents = SLIC3R_CONFIG_OPTION_FLOATS_OR_PERCENTS;
static constexpr ConfigOptionType coPoint = SLIC3R_CONFIG_OPTION_POINT;
static constexpr ConfigOptionType coPoints = SLIC3R_CONFIG_OPTION_POINTS;
static constexpr ConfigOptionType coPoint3 = SLIC3R_CONFIG_OPTION_POINT3;
//    coPoint3s = coPoint3 + coVectorType;
static constexpr ConfigOptionType coBool = SLIC3R_CONFIG_OPTION_BOOL;
static constexpr ConfigOptionType coBools = SLIC3R_CONFIG_OPTION_BOOLS;
static constexpr ConfigOptionType coEnum = SLIC3R_CONFIG_OPTION_ENUM;
static constexpr ConfigOptionType coGraph = SLIC3R_CONFIG_OPTION_GRAPH;
static constexpr ConfigOptionType coGraphs = SLIC3R_CONFIG_OPTION_GRAPHS;

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

PrinterTechnology parse_printer_technology(const std::string &);
std::string to_string(PrinterTechnology);

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

class ConfigDef;
class ConfigOption;

//note: is_nil is replaced by is_enabled

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
        FCO_CAN_DISABLED = 1 << 4,
    };

    ConfigOption() : flags(uint32_t(FCO_ENABLED)) { assert(this->flags != 0); }

    virtual ~ConfigOption() {}

    virtual ConfigOptionType    type() const = 0;
    virtual std::string         serialize() const = 0;
    virtual bool                deserialize(const std::string &str, bool append = false) = 0;
    virtual ConfigOption*       clone() const = 0;
    // Set a value from a ConfigOption. The two options should be compatible.
    virtual void                set(const ConfigOption &option, int32_t idx = -1) = 0;
    // DEPRECATED: please use set()
    ConfigOption&               operator=(const ConfigOption *opt) { this->set(*opt); return *this; }
    // Getters, idx is ignored if it's a scalar value.
    virtual bool                get_bool(size_t idx = 0)       const { throw BadOptionTypeException("Calling ConfigOption::get_bool on a non-boolean ConfigOption"); }
    virtual int32_t             get_int(size_t idx = 0)        const { throw BadOptionTypeException("Calling ConfigOption::get_int on a non-int ConfigOption"); }
    virtual double              get_float(size_t idx = 0)      const { throw BadOptionTypeException("Calling ConfigOption::get_float on a non-float ConfigOption"); }
    virtual double              get_effective_value(double ratio_over, size_t idx = 0) const { return this->get_float(idx); }
    virtual bool                is_percent(size_t idx = 0)     const { return false; }
    virtual void                set_bool(bool /* val */, size_t idx = 0) { throw BadOptionTypeException("Calling ConfigOption::set_int on a non-bool ConfigOption"); }
    virtual void                set_int(int32_t /* val */, size_t idx = 0) { throw BadOptionTypeException("Calling ConfigOption::set_int on a non-int ConfigOption"); }
    virtual void                set_float(double /* val */, size_t idx = 0) { throw BadOptionTypeException("Calling ConfigOption::set_float on a non-float ConfigOption"); }
    virtual void                set_percent(double val, size_t idx = 0) { set_float(val*0.01, idx); }
    // If scalar, idx is ignore, else: if idx < 0 return the vector; if idx >=0 then return the value at theis index; if idx >= size(), then return the first value or a default one.
    virtual boost::any          get_any(int32_t idx = -1)      const { throw BadOptionTypeException("Calling ConfigOption::get_any on a raw ConfigOption"); }
    virtual void                set_any(boost::any, int32_t idx = -1) { throw BadOptionTypeException("Calling ConfigOption::set_any on a raw ConfigOption"); }
    virtual bool                is_enabled(int32_t idx = -1)    const { return (flags & FCO_ENABLED) != 0; }
    virtual ConfigOption*       set_enabled(bool enabled, int32_t idx = -1)
    {
        assert(enabled || can_be_disabled());
        if (enabled)
            this->flags |= FCO_ENABLED;
        else
            this->flags &= uint32_t(0xFF ^ FCO_ENABLED);
        return this;
    }
    // Like FCO_EXTRUDER_ARRAY that is a copy of the def, this is the copy of the def to replicate is_nullable()
    bool                        can_be_disabled() const { return (flags & FCO_CAN_DISABLED) != 0; }
    // should only be set by configdef when a new item is requested. It also set it as disabled if you set the arg to true.
    ConfigOption*               set_can_be_disabled(bool force_disabled = false) { this->flags |= FCO_CAN_DISABLED; if(force_disabled) set_enabled(false); return this; }

    virtual bool                operator==(const ConfigOption &rhs) const = 0;
    bool                        operator!=(const ConfigOption &rhs) const { return ! (*this == rhs); }
    virtual bool                operator<(const ConfigOption &rhs) const = 0;
    virtual size_t              hash()          const throw() = 0;
    bool                        is_scalar()     const { return (int(this->type()) & int(coVectorType)) == 0; }
    bool                        is_vector()     const { return ! this->is_scalar(); }
    // Size of the vector it contains, or 1 if it's a scalar value.
    virtual size_t              size()          const = 0;
    bool                        is_phony()      const { return (flags & FCO_PHONY) != 0; }
    ConfigOption*               set_phony(bool phony) { if (phony) this->flags |= FCO_PHONY; else this->flags &= uint8_t(0xFF ^ FCO_PHONY); return this; }
    // Is this option overridden by another option?
    // An option overrides another option if enabled and not equal or the other isn't enabled.
    virtual bool                overriden_by(const ConfigOption *rhs, int32_t idx = -1) const {
        return rhs->is_enabled() && (*this != *rhs || !this->is_enabled());
    }
    // Apply an override option
    virtual bool                apply_override(const ConfigOption *rhs, int32_t idx = -1) {
        if (!rhs->is_enabled()) {
            return false;
        }
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

// Value of a single valued option (bool, int, float, string, point, enum)
template <class T>
class ConfigOptionSingle : public ConfigOption {
public:
    T value;
    explicit ConfigOptionSingle(T value) : value(std::move(value)) {}
    operator T() const { return this->value; }
    boost::any get_any(int32_t idx = -1) const override { return boost::any(value); }
    void       set_any(boost::any anyval, int32_t idx = -1) override { value = boost::any_cast<T>(anyval); }
    size_t     size() const override { return 1; }
    
    void set(const ConfigOption &rhs, int32_t idx = -1) override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionSingle: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionSingle*>(&rhs));
        this->value = static_cast<const ConfigOptionSingle&>(rhs).value;
        this->flags = rhs.flags;
    }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type()) {
            throw ConfigurationError("ConfigOptionSingle: Comparing incompatible types");
        }
        assert(dynamic_cast<const ConfigOptionSingle<T>*>(&rhs));
        return this->value == static_cast<const ConfigOptionSingle<T>*>(&rhs)->value 
            && this->is_enabled() == rhs.is_enabled()
            && this->is_phony() == rhs.is_phony();
        // should compare all flags?
    }

    bool operator<(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type()) {
            throw ConfigurationError("ConfigOptionSingle: Comparing incompatible types");
        }
        assert(dynamic_cast<const ConfigOptionSingle<T> *>(&rhs));
        return this->is_enabled() < rhs.is_enabled() ||
            (this->is_enabled() == rhs.is_enabled() &&
             this->value < static_cast<const ConfigOptionSingle<T> *>(&rhs)->value);
    }

    bool operator==(const T &rhs) const throw() { return this->value == rhs; }
    bool operator!=(const T &rhs) const throw() { return this->value != rhs; }
    bool operator< (const T &rhs) const throw() { return this->value < rhs; }

    size_t hash() const throw() override {
        std::hash<T> hasher;
        size_t seed = 0;
        config_hash_combine(seed, this->is_enabled());
        config_hash_combine_value(seed, hasher(this->value));
        return seed;
    }

    // Is this option overridden by another option?
    // An option overrides another option if it is enabled and not equal.
    bool overriden_by(const ConfigOption *rhs, int32_t idx = -1) const override {
        //if (this->nullable())
        //    throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionSingle.overriden_by() applied to different types.");
        auto rhs_co = static_cast<const ConfigOptionSingle*>(rhs);
        return rhs_co->is_enabled() && (this->value != rhs_co->value || !this->is_enabled());
    }
    // Apply an override option, possibly a disabled one.
    bool apply_override(const ConfigOption *rhs, int32_t idx = -1) override {
        //if (this->nullable())
        //    throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionSingle.apply_override() applied to different types.");
        if (!rhs->is_enabled()) {
            return false;
        }
        auto rhs_co = static_cast<const ConfigOptionSingle*>(rhs);
        bool modified = false;
        if (this->value != rhs_co->value) {
            this->value = rhs_co->value;
            modified = true;
        }
        if (!this->is_enabled()) {
            this->set_enabled(true);
            modified = true;
        }
        return modified;
    }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(this->flags);
        ar(this->value);
    }
};

// Value of a vector valued option (bools, ints, floats, strings, points)
class ConfigOptionVectorBase : public ConfigOption {
public:
    virtual std::string         serialize() const override = 0;
    virtual bool                deserialize(const std::string &str, bool append = false) override = 0;
    // Currently used only to initialize the PlaceholderParser.
    virtual std::string         serialize_at(int idx = 0) const = 0;
    // Set from a vector of ConfigOptions. 
    // If the rhs ConfigOption is scalar, then its value is used,
    // otherwise for each of rhs, the first value of a vector is used.
    // This function is useful to collect values for multiple extrder / filament settings.
    virtual void set(const std::vector<const ConfigOption*> &rhs) = 0;
    // Set a single vector item from either a scalar option or the first value of a vector option.vector of ConfigOptions. 
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    virtual void set_at(const ConfigOption &rhs, size_t i, size_t j) = 0;
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
    // Get if the size of this vector is/should be the same as nozzle_diameter
    bool is_extruder_size() const { return (flags & FCO_EXTRUDER_ARRAY) != 0; }
    // should only be set by configdef when a new item is requested.
    ConfigOptionVectorBase* set_is_extruder_size(bool is_extruder_size = true) {
        if (is_extruder_size) this->flags |= FCO_EXTRUDER_ARRAY; else this->flags &= uint8_t(0xFF ^ FCO_EXTRUDER_ARRAY);
        assert(this->flags != 0);
        return this;
    }

    
    bool is_enabled(int32_t idx = -1) const override {
        assert (m_enabled.size() == size());
        return idx >= 0 && idx < m_enabled.size() ? m_enabled[idx] : ConfigOption::is_enabled();
    }
    
    bool has_same_enabled(const ConfigOptionVectorBase &rhs) const
    {
        return this->m_enabled == rhs.m_enabled;
    }

    ConfigOption *set_enabled(bool enabled, int32_t idx = -1) override
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
        if (idx >= size()) {
            assert(false);
            return this;
        }
        // set our value
        m_enabled[idx] = enabled;
        return this;
    }

    bool has_enabled() const {
        for (bool enabled : m_enabled)
            if (enabled)
                return true;
        return false;
    }

    // We just overloaded and hid two base class virtual methods.
    // Let's show it was intentional (warnings).
    using ConfigOption::set;


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
    template<class Archive> void serialize(Archive &ar) {
        ar(this->flags);
        ar(this->m_enabled);
    }
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

    void set(const ConfigOption &rhs, int32_t idx = -1) override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionVector: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionVector<T>*>(&rhs));
        assert(idx < int32_t(size()));
        if (idx < 0) {
            this->m_values = static_cast<const ConfigOptionVector<T>&>(rhs).m_values;
            this->m_enabled = static_cast<const ConfigOptionVector<T>&>(rhs).m_enabled;
        } else {
            this->set_at(rhs, idx, idx);
        }
        this->flags = rhs.flags;
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
    void set_at(const ConfigOption &rhs, size_t i, size_t j) override
    {
        // Fill with default value up to the needed position
        if (this->m_values.size() <= i) {
            // Resize this vector, fill in the new vector fields with the copy of the first field.
            this->m_values.resize(i + 1, this->default_value);
            this->m_enabled.resize(i + 1, ConfigOption::is_enabled());
        }
        if (rhs.type() == this->type()) {
            // Assign the first value of the rhs vector.
            const ConfigOptionVector<T> &other = static_cast<const ConfigOptionVector<T>&>(rhs);
            if (other.empty())
                throw ConfigurationError("ConfigOptionVector::set_at(): Assigning from an empty vector");
            this->m_values[i] = other.get_at(j);
            this->m_enabled[i] = other.is_enabled(j);
            ConfigOption::set_enabled(other.is_enabled(-1));
        } else if (rhs.type() == this->scalar_type()) {
            const ConfigOptionSingle<T> &other = static_cast<const ConfigOptionSingle<T>&>(rhs);
            this->m_values[i] = other.value;
            this->m_enabled[i] = other.is_enabled();
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
           assert(idx < size());
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
                if (opt_default == nullptr) {
                    if (this->m_values.size() == 0) {
                        this->m_values.resize(n, this->default_value);
                    } else {
                        this->m_values.resize(n, this->m_values.front());
                    }
                } else {
                    if (opt_default->type() != this->type()) {
                        throw ConfigurationError(
                            "ConfigOptionVector::resize(): Extending with an incompatible type.");
                    } else if (auto other = static_cast<const ConfigOptionVector<T> *>(opt_default);
                               other->m_values.empty()) {
                        this->m_values.resize(n, other->default_value);
                    } else {
                        this->m_values.resize(n, other->get_at(0));
                    }
                }
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

    bool operator<(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type()) {
            throw ConfigurationError("ConfigOptionVector: Comparing incompatible types");
        }
        assert(dynamic_cast<const ConfigOptionVector<T> *>(&rhs));
        return this->m_values < static_cast<const ConfigOptionVector<T> *>(&rhs)->m_values ||
            (this->m_values == static_cast<const ConfigOptionVector<T> *>(&rhs)->m_values &&
             this->m_enabled < static_cast<const ConfigOptionVector<T> *>(&rhs)->m_enabled);
        // should compare all flags?
    }

    bool operator==(const std::vector<T> &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->m_values == rhs; }
    bool operator!=(const std::vector<T> &rhs) const throw() { return this->is_enabled() != rhs.is_enabled() || this->m_values != rhs; }

    size_t hash() const throw() override {
        std::hash<T> hasher;
        std::hash<bool> hasher_b;
        size_t seed = 0;
        for (const auto &v : this->m_values)
            config_hash_combine_value(seed, hasher(v));
        for (bool b : this->m_enabled)
            config_hash_combine_value(seed, hasher_b(b));
        return seed;
    }

    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal
    bool overriden_by(const ConfigOption *rhs, int32_t idx = -1) const override {
        // if (this->nullable())
        //   throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.overriden_by() applied to different types.");
        auto rhs_vec = static_cast<const ConfigOptionVector<T> *>(rhs);
        assert(this->size() == rhs_vec->size());
        if (idx < 0 || idx >= size()) {
            if (this->empty()) {
                assert(false);
                return rhs_vec->has_enabled() && (this->m_values != rhs_vec->m_values || this->m_enabled != rhs_vec->m_enabled);;
            }
            // has at least one value to override.
            for (size_t i = 0; i < this->size(); ++i) {
                if (rhs_vec->m_enabled[i] && (this->m_values[i] != rhs_vec->m_values[i] || !this->m_enabled[i])) {
                    return true;
                }
            }
            return false;
        } else {
            return rhs_vec->m_enabled[idx] && (this->m_values[idx] != rhs_vec->m_values[idx] || !this->m_enabled[idx]);
        }
    }

    // Apply an override option, possibly a nullable one.
    bool apply_override(const ConfigOption *rhs, int32_t idx = -1) override {
        // if (this->nullable())
        //   throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.apply_override() applied to different types.");
        auto rhs_vec = static_cast<const ConfigOptionVector<T> *>(rhs);
        assert(this->size() == rhs_vec->size());
        bool modified = false;
        if (idx >= 0 && idx < this->size()) {
            if (rhs_vec->m_enabled[idx]) {
                if (this->m_values[idx] != rhs_vec->m_values[idx]) {
                    this->m_values[idx] = rhs_vec->m_values[idx];
                    modified = true;
                }
                if (!this->m_enabled[idx]) {
                    this->m_enabled[idx] = true;
                    modified = true;
                }
            }
        } else {
            // do it value per value
            for (int i = 0; i < this->size(); ++i) {
                if (rhs_vec->m_enabled[i]) {
                    if (this->m_values[i] != rhs_vec->m_values[i]) {
                        this->m_values[i] = rhs_vec->m_values[i];
                        modified = true;
                    }
                    if (!this->m_enabled[i]) {
                        this->m_enabled[i] = true;
                        modified = true;
                    }
                }
            }
        }
        return modified;
    }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(this->m_values);
        ar(cereal::base_class<ConfigOptionVectorBase>(this));
    }
};

class ConfigOptionFloat : public ConfigOptionSingle<double>
{
public:
    ConfigOptionFloat() : ConfigOptionSingle<double>(0) {}
    explicit ConfigOptionFloat(double _value) : ConfigOptionSingle<double>(_value) {}

    static ConfigOptionType static_type() { return coFloat; }
    ConfigOptionType        type()      const override { return static_type(); }
    bool                    get_bool(size_t idx = 0) const override { return this->value != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return int32_t(this->value); }
    double                  get_float(size_t idx = 0) const override { return this->value; }
    void                    set_bool(bool value, size_t idx = 0) override { this->value = value ? 1. : 0.; }
    void                    set_int(int32_t value, size_t idx = 0) override { this->value = value; }
    void                    set_float(double value, size_t idx = 0) override { this->value = value; }
    ConfigOption*           clone()     const override { return new ConfigOptionFloat(*this); }
    bool                    operator==(const ConfigOptionFloat &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionFloat &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<double>>(this)); }
};

class ConfigOptionFloats : public ConfigOptionVector<double>
{
public:
    ConfigOptionFloats() : ConfigOptionVector<double>() {}
    explicit ConfigOptionFloats(double default_value) : ConfigOptionVector<double>(default_value) { assert(std::abs(default_value) < 1000000000 && (std::abs(default_value) > 0.000000001 || default_value == 0));}
    explicit ConfigOptionFloats(size_t n, double value) : ConfigOptionVector<double>(n, value) {assert(std::abs(default_value) < 1000000000 && (std::abs(default_value) > 0.000000001 || default_value == 0));}
    explicit ConfigOptionFloats(std::initializer_list<double> il) : ConfigOptionVector<double>(std::move(il)) {assert(std::abs(default_value) < 1000000000 && (std::abs(default_value) > 0.000000001 || default_value == 0));}
    explicit ConfigOptionFloats(const std::vector<double> &vec) : ConfigOptionVector<double>(vec) {assert(std::abs(default_value) < 1000000000 && (std::abs(default_value) > 0.000000001 || default_value == 0));}
    explicit ConfigOptionFloats(std::vector<double> &&vec) : ConfigOptionVector<double>(std::move(vec)) {assert(std::abs(default_value) < 1000000000 && (std::abs(default_value) > 0.000000001 || default_value == 0));}

    static ConfigOptionType static_type() { return coFloats; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionFloats(*this); }
    bool                    operator==(const ConfigOptionFloats &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool operator<(const ConfigOptionFloats &rhs) const throw()
        { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }
    bool                    get_bool(size_t idx = 0) const override { return this->get_at(idx) != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return int32_t(this->get_at(idx)); }
    double                  get_float(size_t idx = 0) const override { return this->get_at(idx); }
    void                    set_bool(bool value, size_t idx = 0) override { this->set_at(value ? 1. : 0., idx); }
    void                    set_int(int32_t value, size_t idx = 0) override { this->set_at(double(value), idx); }
    void                    set_float(double value, size_t idx = 0) override { this->set_at(value, idx); }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

protected:
    void serialize_single_value(std::ostringstream &ss, const double v, const bool enabled) const;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<double>>(this)); }
};


class ConfigOptionInt : public ConfigOptionSingle<int32_t>
{
public:
    ConfigOptionInt() : ConfigOptionSingle<int32_t>(0) {}
    explicit ConfigOptionInt(int32_t value) : ConfigOptionSingle<int32_t>(value) {}
    explicit ConfigOptionInt(double _value) : ConfigOptionSingle<int32_t>(int32_t(floor(_value + 0.5))) {}
    
    static ConfigOptionType static_type() { return coInt; }
    ConfigOptionType        type()   const override { return static_type(); }
    bool                    get_bool(size_t idx = 0) const override { return this->value != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return this->value; }
    double                  get_float(size_t idx = 0) const override { return double(this->value); }
    void                    set_bool(bool value, size_t idx = 0) override { this->value = value ? 1 : 0; }
    void                    set_int(int32_t value, size_t idx = 0) override { this->value = value; }
    void                    set_float(double value, size_t idx = 0) override { this->value = int32_t(value); }
    ConfigOption*           clone()  const override { return new ConfigOptionInt(*this); }
    bool                    operator==(const ConfigOptionInt &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator<(const ConfigOptionInt &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<int32_t>>(this)); }
};

class ConfigOptionInts : public ConfigOptionVector<int32_t>
{
public:
    ConfigOptionInts() : ConfigOptionVector<int32_t>() {}
    explicit ConfigOptionInts(int32_t default_value) : ConfigOptionVector<int32_t>(default_value) {assert(std::abs(default_value) < 1000000000);}
    explicit ConfigOptionInts(size_t n, int32_t value) : ConfigOptionVector<int32_t>(n, value) {assert(std::abs(default_value) < 1000000000);}
    explicit ConfigOptionInts(std::initializer_list<int32_t> &&il) : ConfigOptionVector<int32_t>(std::move(il)) {assert(std::abs(default_value) < 1000000000);}
    //explicit ConfigOptionInts(const std::vector<int> &v) : ConfigOptionVector<int>(v) {}
    //explicit ConfigOptionInts(std::vector<int> &&v) : ConfigOptionVector<int>(std::move(v)) {}

    static ConfigOptionType static_type() { return coInts; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionInts(*this); }
    bool                    operator==(const ConfigOptionInts &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionInts &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }
    bool                    get_bool(size_t idx = 0) const override { return this->get_at(idx) != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return this->get_at(idx); }
    double                  get_float(size_t idx = 0) const override { return double(this->get_at(idx)); }
    void                    set_bool(bool value, size_t idx = 0) override { this->set_at(value ? 1 : 0, idx); }
    void                    set_int(int32_t value, size_t idx = 0) override { this->set_at(value, idx); }
    void                    set_float(double value, size_t idx = 0) override { this->set_at(int32_t(value), idx); }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    void serialize_single_value(std::ostringstream &ss, const int32_t v, bool enabled) const;

    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<int32_t>>(this)); }
};

class ConfigOptionString : public ConfigOptionSingle<std::string>
{
public:
    ConfigOptionString() : ConfigOptionSingle<std::string>(std::string{}) {}
    explicit ConfigOptionString(std::string value) : ConfigOptionSingle<std::string>(std::move(value)) {}

    static ConfigOptionType static_type() { return coString; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionString(*this); }
    bool                    operator==(const ConfigOptionString &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionString &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }
    bool                    empty() const { return this->value.empty(); }

    bool                    get_bool(size_t idx = 0) const override { return !this->value.empty() && this->value != "0"; }
    int32_t                 get_int(size_t idx = 0) const override { try { return std::stoi(this->value); } catch (...) { return 0; } }
    double                  get_float(size_t idx = 0) const override { try { return std::stod(this->value); } catch (...) { return 0.0; } }
    void                    set_bool(bool value, size_t idx = 0) override { this->value = value ? "1" : "0"; }
    void                    set_int(int32_t value, size_t idx = 0) override { this->value = std::to_string(value); }
    void                    set_float(double value, size_t idx = 0) override { this->value = std::to_string(value); }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(cereal::base_class<ConfigOptionSingle<std::string>>(this));
    }
};

class ConfigOptionStringVersion : public ConfigOptionString
{
public:
    ConfigOptionStringVersion() : ConfigOptionString(std::string{}) { this->set_phony(false); }
    explicit ConfigOptionStringVersion(std::string value) : ConfigOptionString(std::move(value)) { this->set_phony(false); }
    ConfigOption*           clone() const override { return new ConfigOptionStringVersion(*this); }

    std::string serialize() const override;
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
    bool                    operator==(const ConfigOptionStrings &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionStrings &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }

    bool                    get_bool(size_t idx = 0) const override { return !this->get_at(idx).empty() && this->get_at(idx) != "0"; }
    int32_t                 get_int(size_t idx = 0) const override { try { return std::stoi(this->get_at(idx)); } catch (...) { return 0; } }
    double                  get_float(size_t idx = 0) const override { try { return std::stod(this->get_at(idx)); } catch (...) { return 0.0; } }
    void                    set_bool(bool value, size_t idx = 0) override { this->set_at(value ? "1" : "0", idx); }
    void                    set_int(int32_t value, size_t idx = 0) override { this->set_at(std::to_string(value), idx); }
    void                    set_float(double value, size_t idx = 0) override { this->set_at(std::to_string(value), idx); }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(cereal::base_class<ConfigOptionVector<std::string>>(this));
    }
};

class ConfigOptionPercent : public ConfigOptionFloat
{
public:
    ConfigOptionPercent() : ConfigOptionFloat(0) {}
    explicit ConfigOptionPercent(double _value) : ConfigOptionFloat(_value) {}
    
    static ConfigOptionType static_type() { return coPercent; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPercent(*this); }
    bool                    operator==(const ConfigOptionPercent &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPercent &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value < rhs.value); }

    double                  get_effective_value(double ratio_over, size_t idx = 0) const override { return ratio_over * this->value / 100.; }
    bool                    get_bool(size_t idx) const override { return this->value != 0.; }
    int32_t                 get_int(size_t idx) const override { return int32_t(this->value); }
    double                  get_float(size_t idx) const override { return this->value / 100; }
    bool                    is_percent(size_t idx = 0) const override { return true; }
    void                    set_bool(bool value, size_t idx = 0) override { this->value = value ? 1. : 0.; }
    void                    set_int(int32_t value, size_t idx = 0) override { this->value = value; }
    void                    set_float(double scalar_value, size_t idx = 0) override { this->value = value * 100; }
    void                    set_percent(double percent_value, size_t idx = 0) override {  this->value = percent_value; }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloat>(this)); }
};

class ConfigOptionPercents : public ConfigOptionFloats
{
public:
    ConfigOptionPercents() : ConfigOptionFloats() {}
    explicit ConfigOptionPercents(double default_value) : ConfigOptionFloats(default_value) {}
    explicit ConfigOptionPercents(size_t n, double value) : ConfigOptionFloats(n, value) {}
    explicit ConfigOptionPercents(std::initializer_list<double> il) : ConfigOptionFloats(std::move(il)) {}
    explicit ConfigOptionPercents(const std::vector<double>& vec) : ConfigOptionFloats(vec) {}
    explicit ConfigOptionPercents(std::vector<double>&& vec) : ConfigOptionFloats(std::move(vec)) {}

    static ConfigOptionType static_type() { return coPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionPercents(*this); }
    bool operator==(const ConfigOptionPercents &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool operator<(const ConfigOptionPercents &rhs) const throw()
        { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }

    double                  get_effective_value(double ratio_over, size_t idx = 0) const override { return ratio_over * this->get_at(idx) / 100.; }
    bool                    get_bool(size_t idx) const override { return this->get_at(idx) != 0.; }
    int32_t                 get_int(size_t idx) const override { return int32_t(this->get_at(idx)); }
    double                  get_float(size_t idx) const override { return this->get_at(idx) / 100.; }
    bool                    is_percent(size_t idx = 0) const override { return true; }
    void                    set_bool(bool value, size_t idx = 0) override { this->set_at(value ? 1. : 0., idx); }
    void                    set_int(int32_t value, size_t idx = 0) override { this->set_at(value, idx); }
    void                    set_float(double value, size_t idx = 0) override { this->set_at(value * 100., idx); }
    void                    set_percent(double percent_value, size_t idx = 0) override {  this->set_at(percent_value, idx); }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    // The float's deserialize function shall ignore the trailing optional %.
    // bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloats>(this)); }
};

// note: maybe should be a ConfigOptionSingle<FloatOrPercent>
class ConfigOptionFloatOrPercent : public ConfigOptionPercent
{
public:
    bool percent;
    ConfigOptionFloatOrPercent() : ConfigOptionPercent(0), percent(false) {}
    explicit ConfigOptionFloatOrPercent(double _value, bool _percent) : ConfigOptionPercent(_value), percent(_percent) {}

    static ConfigOptionType     static_type() { return coFloatOrPercent; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption*               clone() const override { return new ConfigOptionFloatOrPercent(*this); }
    bool                        operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent*>(&rhs));
        return *this == *static_cast<const ConfigOptionFloatOrPercent*>(&rhs);
    }
    bool operator<(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent *>(&rhs));
        return *this < *static_cast<const ConfigOptionFloatOrPercent *>(&rhs);
    }
    bool                        operator==(const ConfigOptionFloatOrPercent &rhs) const throw()
        { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value && this->percent == rhs.percent; }
    size_t                      hash() const throw() override 
        { if(!is_enabled()) return 0; size_t seed = std::hash<double>{}(this->value); return this->percent ? seed ^ 0x9e3779b9 : seed; }
    bool                        operator< (const ConfigOptionFloatOrPercent &rhs) const throw() {
        bool this_enabled = this->is_enabled();
        bool rhs_enabled = this->is_enabled();
        return std::tie(this_enabled, this->value, this->percent) < std::tie(rhs_enabled, rhs.value, rhs.percent);
    }

    double                      get_effective_value(double ratio_over, size_t idx = 0) const override
        { return this->percent ? (ratio_over * this->value / 100) : this->value; }
    bool                        get_bool(size_t idx = 0) const override { return this->value != 0; }
    int32_t                     get_int(size_t idx = 0) const override { return int32_t(this->value); }
    double                      get_float(size_t idx = 0) const override { return get_effective_value(1., idx); }
    bool                        is_percent(size_t idx = 0) const override { return this->percent; }
    void                        set_bool(bool value, size_t idx = 0) override { this->value = value ? 1. : 0.; }
    void                        set_int(int32_t value, size_t idx = 0) override { this->value = double(value); }
    void                        set_float(double value, size_t idx = 0) override { this->value = value; this->percent = false; }
    void                        set_percent(double percent_value, size_t idx = 0) override { this->value = percent_value; this->percent = true; }
    // special case for get/set any: use a FloatOrPercent like for FloatsOrPercents, to have the is_percent
    boost::any get_any(int32_t idx = 0) const override { return boost::any(FloatOrPercent{value, percent}); }
    void       set_any(boost::any anyval, int32_t idx = -1) override
    {
        auto fl_or_per = boost::any_cast<FloatOrPercent>(anyval);
        this->value    = fl_or_per.value;
        this->percent  = fl_or_per.percent;
    }

    void set(const ConfigOption &rhs, int32_t idx = -1) override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionSingle*>(&rhs));
        this->value = static_cast<const ConfigOptionFloatOrPercent&>(rhs).value;
        this->percent = static_cast<const ConfigOptionFloatOrPercent&>(rhs).percent;
        this->flags = rhs.flags;
    }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(cereal::base_class<ConfigOptionPercent>(this), percent);
    }
};

class ConfigOptionFloatsOrPercents : public ConfigOptionVector<FloatOrPercent>
{
public:
    ConfigOptionFloatsOrPercents() : ConfigOptionVector<FloatOrPercent>() {}
    explicit ConfigOptionFloatsOrPercents(FloatOrPercent default_value) : ConfigOptionVector<FloatOrPercent>(default_value) {assert(std::abs(default_value.value) < 1000000000 && (std::abs(default_value.value) > 0.000000001 || default_value.value == 0));}
    explicit ConfigOptionFloatsOrPercents(size_t n, FloatOrPercent value) : ConfigOptionVector<FloatOrPercent>(n, value) {assert(std::abs(default_value.value) < 1000000000 && (std::abs(default_value.value) > 0.000000001 || default_value.value == 0));}
    explicit ConfigOptionFloatsOrPercents(std::initializer_list<FloatOrPercent> il) : ConfigOptionVector<FloatOrPercent>(std::move(il)) {assert(std::abs(default_value.value) < 1000000000 && (std::abs(default_value.value) > 0.000000001 || default_value.value == 0));}
    explicit ConfigOptionFloatsOrPercents(const std::vector<FloatOrPercent> &vec) : ConfigOptionVector<FloatOrPercent>(vec) {assert(std::abs(default_value.value) < 1000000000 && (std::abs(default_value.value) > 0.000000001 || default_value.value == 0));}
    explicit ConfigOptionFloatsOrPercents(std::vector<FloatOrPercent> &&vec) : ConfigOptionVector<FloatOrPercent>(std::move(vec)) {assert(std::abs(default_value.value) < 1000000000 && (std::abs(default_value.value) > 0.000000001 || default_value.value == 0));}

    static ConfigOptionType static_type() { return coFloatsOrPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionFloatsOrPercents(*this); }
    bool                    operator==(const ConfigOptionFloatsOrPercents &rhs) const throw()
        { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator<(const ConfigOptionFloatsOrPercents &rhs) const throw()
        { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values < rhs.m_values); }
    double                  get_effective_value(double ratio_over, size_t idx = 0) const override{
        const FloatOrPercent& data = this->get_at(idx);
        if (data.percent) return ratio_over * data.value / 100;
        return data.value;
    }
    bool                    get_bool(size_t idx = 0) const override { return this->get_at(idx).value != 0.; }
    int32_t                 get_int(size_t idx = 0) const override { return int32_t(this->get_at(idx).value); }
    double                  get_float(size_t idx = 0) const override { return get_effective_value(1., idx); }
    bool                    is_percent(size_t idx = 0) const override { return this->get_at(idx).percent; }
    void                    set_bool(bool value, size_t idx = 0) override { this->get_at(idx).value = value ? 1. : 0.; }
    void                    set_int(int32_t value, size_t idx = 0) override { this->get_at(idx).value = double(value); }
    void                    set_float(double scalar_value, size_t idx = 0) override { this->set_at(FloatOrPercent{scalar_value, false}, idx); }
    void                    set_percent(double percent_value, size_t idx = 0) override { this->set_at(FloatOrPercent{percent_value, true}, idx); }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

protected:
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static FloatOrPercent NIL_VALUE();

    void serialize_single_value(std::ostringstream &ss, const FloatOrPercent &v, bool enabled) const;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<FloatOrPercent>>(this)); }
};

class ConfigOptionPoint : public ConfigOptionSingle<Vec2d>
{
public:
    ConfigOptionPoint() : ConfigOptionSingle<Vec2d>(Vec2d(0,0)) {}
    explicit ConfigOptionPoint(const Vec2d &value) : ConfigOptionSingle<Vec2d>(value) {}
    
    static ConfigOptionType static_type() { return coPoint; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoint(*this); }
    bool                    operator==(const ConfigOptionPoint &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }
    
    bool                    get_bool(size_t idx = 0) const override { assert(idx < 2); return idx == 0 ? this->value.x() != 0 : this->value.y() != 0; }
    int32_t                 get_int(size_t idx = 0) const override { assert(idx < 2); return int32_t(idx == 0 ? this->value.x() : this->value.y()); }
    double                  get_float(size_t idx = 0) const override { assert(idx < 2); return double(idx == 0 ? this->value.x() : this->value.y()); }
    void                    set_bool(bool value, size_t idx = 0) override { assert(idx < 2); (idx == 0 ? this->value.x() : this->value.y()) = value ? 1 : 0; }
    void                    set_int(int32_t value, size_t idx = 0) override { assert(idx < 2); (idx == 0 ? this->value.x() : this->value.y()) = double(value); }
    void                    set_float(double value, size_t idx = 0) override { assert(idx < 2); (idx == 0 ? this->value.x() : this->value.y()) = value; }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

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
    bool                    operator==(const ConfigOptionPoints &rhs) const throw()
    {
        return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values;
    }
    bool operator<(const ConfigOptionPoints &rhs) const throw();

    bool                    get_bool(size_t idx = 0) const override { assert(idx < size() * 2); return idx%2 == 0 ? this->get_at(idx/2).x() != 0 : this->get_at(idx/2).y() != 0; }
    int32_t                 get_int(size_t idx = 0) const override { assert(idx < size() * 2); return int32_t(idx%2 == 0 ? this->get_at(idx/2).x() : this->get_at(idx/2).y()); }
    double                  get_float(size_t idx = 0) const override { assert(idx < size() * 2); return double(idx%2 == 0 ? this->get_at(idx/2).x() : this->get_at(idx/2).y()); }
    void                    set_bool(bool value, size_t idx = 0) override { assert(idx < size() * 2); (idx%2 == 0 ? this->get_at(idx/2).x() : this->get_at(idx/2).y()) = value ? 1 : 0; }
    void                    set_int(int32_t value, size_t idx = 0) override { assert(idx < size() * 2); (idx%2 == 0 ? this->get_at(idx/2).x() : this->get_at(idx/2).y()) = double(value); }
    void                    set_float(double value, size_t idx = 0) override { assert(idx < size() * 2); (idx%2 == 0 ? this->get_at(idx/2).x() : this->get_at(idx/2).y()) = value; }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void save(Archive &archive) const {
        archive(flags);
        size_t cnt = this->m_values.size();
        archive(cnt);
        archive.saveBinary((const char *) this->m_values.data(), sizeof(Vec2d) * cnt);
    }
    template<class Archive> void load(Archive &archive) {
        archive(flags);
        size_t cnt;
        archive(cnt);
        this->m_values.assign(cnt, Vec2d());
        archive.loadBinary((char *) this->m_values.data(), sizeof(Vec2d) * cnt);
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
    bool                    operator==(const ConfigOptionPoint3 &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint3 &rhs) const throw() 
    {
        bool this_enabled = this->is_enabled();
        bool rhs_enabled  = this->is_enabled();
        return std::tie(this_enabled, this->value.x(), this->value.y()) < std::tie(rhs_enabled, rhs.value.x(), rhs.value.y());
    }

    std::string serialize() const override;

    bool deserialize(const std::string &str_raw, bool append = false) override;

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
    bool                    operator==(const ConfigOptionGraph &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionGraph &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

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
    bool                    operator==(const ConfigOptionGraphs &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool operator<(const ConfigOptionGraphs &rhs) const throw();

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    bool deserialize(const std::string &str, bool append = false) override;

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
    bool                    operator==(const ConfigOptionBool &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() &&this->value == rhs.value; }
    bool                    operator< (const ConfigOptionBool &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && int(this->value) < int(rhs.value)); }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<bool>>(this)); }
};

class ConfigOptionBools : public ConfigOptionVector<unsigned char>
{
public:
    ConfigOptionBools() : ConfigOptionVector<unsigned char>() {}
    explicit ConfigOptionBools(bool default_value) : ConfigOptionVector<unsigned char>(default_value) {}
    explicit ConfigOptionBools(size_t n, bool value) : ConfigOptionVector<unsigned char>(n, (unsigned char)value) {}
    explicit ConfigOptionBools(std::initializer_list<bool> il)
    {
        this->m_values.reserve(il.size());
        for (bool b : il) this->m_values.emplace_back((unsigned char) b);
        this->m_enabled.resize(this->m_values.size(), ConfigOption::is_enabled());
        assert(m_enabled.size() == size());
    }
    explicit ConfigOptionBools(std::initializer_list<unsigned char> il)
    {
        this->m_values.reserve(il.size());
        for (unsigned char b : il) this->m_values.emplace_back(b);
        this->m_enabled.resize(this->m_values.size(), ConfigOption::is_enabled());
        assert(m_enabled.size() == size());
    }
    explicit ConfigOptionBools(const std::vector<unsigned char> &vec) : ConfigOptionVector<unsigned char>(vec) {}
    explicit ConfigOptionBools(std::vector<unsigned char> &&vec)
        : ConfigOptionVector<unsigned char>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coBools; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { assert(this->m_values.size() == this->m_enabled.size()); return new ConfigOptionBools(*this); }
    bool                    operator==(const ConfigOptionBools &rhs) const throw() { return this->m_enabled == rhs.m_enabled && this->m_values == rhs.m_values; }
    bool                    operator< (const ConfigOptionBools &rhs) const throw() { return this->m_enabled < rhs.m_enabled || (this->m_enabled == rhs.m_enabled && this->m_values <  rhs.m_values); }
    bool                    get_bool(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0; }
    int32_t                 get_int(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0 ? 1 : 0; }
    double                  get_float(size_t idx = 0) const override { return ConfigOptionVector<unsigned char>::get_at(idx) != 0 ? 1. : 0.; }

    std::string serialize() const override;

    std::string serialize_at(int idx) const override;

    ConfigHelpers::DeserializationResult deserialize_with_substitutions(
        const std::string &str, bool append, ConfigHelpers::DeserializationSubstitution substitution);

    bool deserialize(const std::string &str, bool append = false) override;

protected:
    void serialize_single_value(std::ostringstream &ss, const unsigned char v, bool enabled) const;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) {
        ar(cereal::base_class<ConfigOptionVector<unsigned char>>(this));
    }
};

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
    bool                    operator==(const ConfigOptionEnum<T> &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                    operator< (const ConfigOptionEnum<T> &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && int(this->value) < int(rhs.value)); }
    int32_t                 get_int(size_t idx = 0) const override { return int32_t(this->value); }
    void                    set_int(int32_t val, size_t idx = 0) override { this->value = T(val); }
    // special case for get/set any: use a int like for ConfigOptionEnumGeneric, to simplify
    boost::any get_any(int32_t idx = -1) const override { return boost::any(get_int()); }
    void       set_any(boost::any anyval, int32_t idx = -1) override { set_int(boost::any_cast<int32_t>(anyval)); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() == rhs.is_enabled() && this->value == (T)rhs.get_int();
    }

    bool operator<(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() < rhs.is_enabled() ||
            (this->is_enabled() == rhs.is_enabled() && this->value < (T) rhs.get_int());
    }

    void set(const ConfigOption &rhs, int32_t idx = -1) override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = (T)rhs.get_int();
        this->flags = rhs.flags;
    }

    std::string serialize() const override
    {
        const t_config_enum_names& names = ConfigOptionEnum<T>::get_enum_names();
        assert(static_cast<int>(this->value) < int(names.size()));
        return std::string(this->is_enabled() ? "" : "!") + names[static_cast<int>(this->value)];
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        (void) append;
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
    bool                        operator==(const ConfigOptionEnumGeneric &rhs) const throw() { return this->is_enabled() == rhs.is_enabled() && this->value == rhs.value; }
    bool                        operator< (const ConfigOptionEnumGeneric &rhs) const throw() { return this->is_enabled() < rhs.is_enabled() || (this->is_enabled() == rhs.is_enabled() && this->value <  rhs.value); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() == rhs.is_enabled() && this->value == rhs.get_int();
    }

    bool operator<(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->is_enabled() < rhs.is_enabled() ||
            (this->is_enabled() == rhs.is_enabled() && this->value < rhs.get_int());
    }

    void set(const ConfigOption &rhs, int32_t idx = -1) override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = rhs.get_int();
        this->flags = rhs.flags;
    }

    std::string serialize() const override;

    bool deserialize(const std::string &str, bool append = false) override;

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionInt>(this)); }
};

} // namespace Slic3r

#endif
