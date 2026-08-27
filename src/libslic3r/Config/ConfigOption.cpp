///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Lukáš Matěna
///@lukasmatena, David Kocík @kocikdav, Tomáš Mészáros @tamasmeszaros, Vojtěch Král @vojtechkral, Oleksandra
/// Iushchenko @YuSanka
///|/ Copyright (c) 2018 fredizzimo @fredizzimo
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
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
#include "ConfigDef.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <sstream>
#include <string_view>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/config.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/nowide/cenv.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include <LibBGCode/binarize/binarize.hpp>

#include "Flow.hpp"
#include "format.hpp"
#include "libslic3r.h"
#include "LocalesUtils.hpp"
#include "Preset.hpp"
#include "Utils.hpp"

#define L(s) (s)

namespace Slic3r {

namespace {

void trim_ascii(std::string &value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
}

bool iequals_ascii(const std::string &lhs, const char *rhs) {
    const std::string_view rhs_view(rhs);
    if (lhs.size() != rhs_view.size())
        return false;
    for (size_t idx = 0; idx < lhs.size(); ++idx)
        if (std::tolower(static_cast<unsigned char>(lhs[idx])) !=
            std::tolower(static_cast<unsigned char>(rhs_view[idx])))
            return false;
    return true;
}

} // namespace

namespace ConfigHelpers {

void trim(std::string &value) { trim_ascii(value); }

bool looks_like_enum_value(std::string value) {
    trim_ascii(value);
    if (value.empty() || value.size() > 64 || !std::isalpha(static_cast<unsigned char>(value.front())))
        return false;
    for (const char c : value)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return false;
    return true;
}

bool enum_looks_like_bool_value(std::string value) {
    trim_ascii(value);
    return iequals_ascii(value, "enabled") || iequals_ascii(value, "disabled") || iequals_ascii(value, "on") ||
        iequals_ascii(value, "off");
}

bool enum_looks_like_true_value(std::string value) {
    trim_ascii(value);
    return iequals_ascii(value, "enabled") || iequals_ascii(value, "on");
}

} // namespace ConfigHelpers

std::string ConfigOptionStringVersion::serialize() const {
    return escape_string_cstyle(std::string("SUSI_") + SLIC3R_VERSION_FULL);
}

std::string ConfigOptionFloat::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled()) {
        assert(this->can_be_disabled());
        ss << "!";
    }
    if (std::isfinite(this->value)) {
        ss << this->value;
    } else
        throw ConfigurationError("Serializing invalid number");
    assert(ss.str() != "!" && !ss.str().empty());
    return ss.str();
}

bool ConfigOptionFloat::deserialize(const std::string &str, bool append) {
    (void) append;
    if (!str.empty() && str.front() == '!') {
        this->set_enabled(false);
    } else {
        this->set_enabled(true);
    }
    std::istringstream iss(this->is_enabled() ? str : str.substr(1));
    iss >> this->value;

    return !iss.fail();
}

std::string ConfigOptionFloats::serialize() const {
    assert(this->m_values.size() == this->m_enabled.size());
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx > 0)
            ss << ",";
        this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    }
    return ss.str();
}

std::string ConfigOptionFloats::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    return ss.str();
}

bool ConfigOptionFloats::deserialize(const std::string &str, bool append) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    std::istringstream is(str);
    std::string item_str;
    while (std::getline(is, item_str, ',')) {
        ConfigHelpers::trim(item_str);
        bool enabled = true;
        if (!item_str.empty() && item_str.front() == '!') {
            enabled = false;
            item_str = item_str.substr(1);
            ConfigHelpers::trim(item_str);
            assert(this->can_be_disabled());
        }
        std::istringstream iss(item_str);
        double value;
        iss >> value;
        this->m_values.push_back(value);
        this->m_enabled.push_back(enabled);
    }
    set_default_enabled();
    assert(this->m_values.size() == this->m_enabled.size());
    return true;
}

void ConfigOptionFloats::serialize_single_value(std::ostringstream &ss, const double v, const bool enabled) const {
    if (!enabled) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    if (std::isfinite(v)) {
        ss << v;
    } else
        throw ConfigurationError("Serializing invalid number");
}

std::string ConfigOptionInt::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled()) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << this->value;

    return ss.str();
}

bool ConfigOptionInt::deserialize(const std::string &str, bool append) {
    (void) append;
    if (!str.empty() && str.front() == '!') {
        this->set_enabled(false);
        assert(this->can_be_disabled());
    } else {
        this->set_enabled(true);
    }
    std::istringstream iss(this->is_enabled() ? str : str.substr(1));

    iss >> this->value;

    return !iss.fail();
}

std::string ConfigOptionInts::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx > 0)
            ss << ",";
        this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
    }
    return ss.str();
}

std::string ConfigOptionInts::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    return ss.str();
}

bool ConfigOptionInts::deserialize(const std::string &str, bool append) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    std::istringstream is(str);
    std::string item_str;
    while (std::getline(is, item_str, ',')) {
        bool enabled = true;
        ConfigHelpers::trim(item_str);
        if (!item_str.empty() && item_str.front() == '!') {
            enabled = false;
            item_str = item_str.substr(1);
            ConfigHelpers::trim(item_str);
            assert(this->can_be_disabled());
        }
        std::istringstream iss(item_str);
        int32_t value;
        iss >> value;
        this->m_values.push_back(value);
        this->m_enabled.push_back(enabled);
    }
    set_default_enabled();
    assert(this->m_values.size() == this->m_enabled.size());
    return true;
}

void ConfigOptionInts::serialize_single_value(std::ostringstream &ss, const int32_t v, bool enabled) const {
    if (!enabled) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << v;
}

std::string ConfigOptionString::serialize() const {
    if (!this->is_enabled())
        return std::string("!:") + escape_string_cstyle(this->value);
    return escape_string_cstyle(this->value);
}

bool ConfigOptionString::deserialize(const std::string &str, bool append) {
    (void) append;
    if (str.size() > 1 && str.front() == '!' && str[1] == ':') {
        this->set_enabled(false);
    } else {
        this->set_enabled(true);
    }
    return unescape_string_cstyle(this->is_enabled() ? str : str.substr(2), this->value);
}

std::string ConfigOptionStrings::serialize() const {
    if (this->m_enabled.empty() && !this->m_values.empty()) {
        std::vector<bool> filled;
        filled.resize(this->m_values.size(), ConfigOption::is_enabled(0));
        return escape_strings_cstyle(this->m_values, filled);
    }
    return escape_strings_cstyle(this->m_values, this->m_enabled);
}

std::string ConfigOptionStrings::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    if (!this->is_enabled(idx))
        return std::string("!:") + escape_string_cstyle(this->get_at(idx));
    return escape_string_cstyle(this->get_at(idx));
}

bool ConfigOptionStrings::deserialize(const std::string &str, bool append) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    assert(this->m_enabled.size() == this->m_values.size());
    bool success = unescape_strings_cstyle(str, this->m_values, this->m_enabled);
    if (success) {
        set_default_enabled();
    }
    assert(this->m_values.size() == this->m_enabled.size());
    return success;
}

std::string ConfigOptionPercent::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled()) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << this->value;
    std::string s(ss.str());
    s += "%";
    return s;
}

bool ConfigOptionPercent::deserialize(const std::string &str, bool append) {
    (void) append;
    if (!str.empty() && str.front() == '!') {
        this->set_enabled(false);
        assert(this->can_be_disabled());
    } else {
        this->set_enabled(true);
    }
    // don't try to parse the trailing % since it's optional
    std::istringstream iss(this->is_enabled() ? str : str.substr(1));
    iss >> this->value;
    return !iss.fail();
}

std::string ConfigOptionPercents::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx > 0)
            ss << ",";
        this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
        ss << "%";
    }
    std::string str = ss.str();
    return str;
}

std::string ConfigOptionPercents::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    return ss.str();
}

std::string ConfigOptionFloatOrPercent::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled()) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << this->value;
    std::string s(ss.str());
    if (this->percent)
        s += "%";
    return s;
}

bool ConfigOptionFloatOrPercent::deserialize(const std::string &str, bool append) {
    (void) append;
    if (!str.empty() && str.front() == '!') {
        this->set_enabled(false);
        assert(this->can_be_disabled());
    } else {
        this->set_enabled(true);
    }
    this->percent = str.find_first_of("%") != std::string::npos;
    std::istringstream iss(this->is_enabled() ? str : str.substr(1));
    iss >> this->value;
    return !iss.fail();
}

std::string ConfigOptionFloatsOrPercents::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx > 0)
            ss << ",";
        this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
    }
    return ss.str();
}

std::string ConfigOptionFloatsOrPercents::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    return ss.str();
}

bool ConfigOptionFloatsOrPercents::deserialize(const std::string &str, bool append) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    std::istringstream is(str);
    std::string item_str;
    while (std::getline(is, item_str, ',')) {
        ConfigHelpers::trim(item_str);
        bool enabled = true;
        if (!item_str.empty() && item_str.front() == '!') {
            enabled = false;
            item_str = item_str.substr(1);
            ConfigHelpers::trim(item_str);
            assert(this->can_be_disabled());
        }
        bool percent = item_str.find_first_of("%") != std::string::npos;
        std::istringstream iss(item_str);
        double value;
        iss >> value;
        this->m_values.push_back({value, percent});
        this->m_enabled.push_back(enabled);
    }
    set_default_enabled();
    assert(this->m_values.size() == this->m_enabled.size());
    return true;
}

FloatOrPercent ConfigOptionFloatsOrPercents::NIL_VALUE() {
    return FloatOrPercent{std::numeric_limits<double>::max(), false};
}

void ConfigOptionFloatsOrPercents::serialize_single_value(std::ostringstream &ss,
                                                          const FloatOrPercent &v,
                                                          bool enabled) const {
    if (!enabled) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    if (std::isfinite(v.value)) {
        ss << v.value;
        if (v.percent)
            ss << "%";
    } else
        throw ConfigurationError("Serializing invalid number");
}

std::string ConfigOptionPoint::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled())
        ss << "!";
    ss << this->value(0);
    ss << "x";
    ss << this->value(1);
    return ss.str();
}

bool ConfigOptionPoint::deserialize(const std::string &str, bool append) {
    (void) append;
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
    this->value = point;
    return true;
}

bool ConfigOptionPoints::operator<(const ConfigOptionPoints &rhs) const throw() {
    return this->m_enabled < rhs.m_enabled ||
        (this->m_enabled == rhs.m_enabled &&
         std::lexicographical_compare(
             this->m_values.begin(), this->m_values.end(), rhs.m_values.begin(), rhs.m_values.end(),
             [](const Vec2d &l, const Vec2d &r) { return l < r; }));
}

std::string ConfigOptionPoints::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx != 0)
            ss << ",";
        assert(m_enabled.size() == m_values.size());
        if (!m_enabled[idx]) {
            ss << "!";
            assert(this->can_be_disabled());
        }
        ss << this->m_values[idx].x();
        ss << "x";
        ss << this->m_values[idx].y();
    }
    return ss.str();
}

std::string ConfigOptionPoints::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    if (!m_enabled[idx]) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << this->m_values[idx].x();
    ss << "x";
    ss << this->m_values[idx].y();
    return ss.str();
}

bool ConfigOptionPoints::deserialize(const std::string &str, bool append) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    std::istringstream is(str);
    std::string point_str;
    while (std::getline(is, point_str, ',')) {
        ConfigHelpers::trim(point_str);
        bool enabled = true;
        if (!point_str.empty() && point_str.front() == '!') {
            enabled = false;
            point_str = point_str.substr(1);
            assert(this->can_be_disabled());
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

std::string ConfigOptionPoint3::serialize() const {
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

bool ConfigOptionPoint3::deserialize(const std::string &str_raw, bool append) {
    (void) append;
    if (!str_raw.empty() && str_raw.front() == '!') {
        this->set_enabled(false);
    } else {
        this->set_enabled(true);
    }

    Vec2d point(Vec2d::Zero());
    std::string str = (this->is_enabled() ? str_raw : str_raw.substr(1));
    char dummy;
    return sscanf(str.data(), " %lf , %lf , %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) ==
        3 ||
        sscanf(str.data(), " %lf x %lf x %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) == 3;
}

std::string ConfigOptionGraph::serialize() const {
    std::ostringstream ss;
    if (!this->is_enabled())
        ss << "!";
    ss << this->value.serialize();
    return ss.str();
}

bool ConfigOptionGraph::deserialize(const std::string &str, bool append) {
    (void) append;
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

bool ConfigOptionGraphs::operator<(const ConfigOptionGraphs &rhs) const throw() {
    return this->m_enabled < rhs.m_enabled ||
        (this->m_enabled == rhs.m_enabled &&
         std::lexicographical_compare(
             this->m_values.begin(), this->m_values.end(), rhs.m_values.begin(), rhs.m_values.end(),
             [](const GraphData &l, const GraphData &r) { return l < r; }));
}

std::string ConfigOptionGraphs::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < size(); ++idx) {
        const GraphData &graph = this->m_values[idx];
        if (idx != 0)
            ss << ",";
        if (!this->is_enabled(idx)) {
            ss << "!";
            assert(this->can_be_disabled());
        }
        ss << graph.serialize();
    }
    return ss.str();
}

std::string ConfigOptionGraphs::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    if (!this->is_enabled(idx)) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << this->m_values[idx].serialize();
    return ss.str();
}

bool ConfigOptionGraphs::deserialize(const std::string &str, bool append) {
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
        ConfigHelpers::trim(graph_str);
        bool enabled = true;
        if (!graph_str.empty() && graph_str.front() == '!') {
            enabled = false;
            graph_str = graph_str.substr(1);
            ConfigHelpers::trim(graph_str);
            assert(this->can_be_disabled());
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

std::string ConfigOptionBool::serialize() const {
    return std::string(this->is_enabled() ? "" : "!") + std::string(this->value ? "1" : "0");
}

bool ConfigOptionBool::deserialize(const std::string &str, bool append) {
    (void) append;
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

std::string ConfigOptionBools::serialize() const {
    std::ostringstream ss;
    for (size_t idx = 0; idx < this->m_values.size(); ++idx) {
        if (idx > 0)
            ss << ",";
        this->serialize_single_value(ss, this->m_values[idx], this->is_enabled(idx));
    }
    return ss.str();
}

std::string ConfigOptionBools::serialize_at(int idx) const {
    assert(idx >= 0 && idx < size());
    std::ostringstream ss;
    this->serialize_single_value(ss, this->m_values[idx], this->m_enabled[idx]);
    return ss.str();
}

ConfigHelpers::DeserializationResult ConfigOptionBools::deserialize_with_substitutions(
    const std::string &str, bool append, ConfigHelpers::DeserializationSubstitution substitution) {
    if (!append) {
        this->m_values.clear();
        this->m_enabled.clear();
    }
    std::istringstream is(str);
    std::string item_str;
    bool substituted = false;
    while (std::getline(is, item_str, ',')) {
        ConfigHelpers::trim(item_str);
        bool enabled = true;
        if (!item_str.empty() && item_str.front() == '!') {
            enabled = false;
            item_str = item_str.substr(1);
            ConfigHelpers::trim(item_str);
            assert(this->can_be_disabled());
        }
        unsigned char new_value = 0;
        if (item_str == "1") {
            new_value = true;
        } else if (item_str == "0") {
            new_value = false;
        } else if (substitution != ConfigHelpers::DeserializationSubstitution::Disabled &&
                   ConfigHelpers::looks_like_enum_value(item_str)) {
            new_value = ConfigHelpers::enum_looks_like_true_value(item_str) ||
                substitution == ConfigHelpers::DeserializationSubstitution::DefaultsToTrue;
            substituted = true;
        } else
            return ConfigHelpers::DeserializationResult::Failed;
        this->m_values.push_back(new_value);
        this->m_enabled.push_back(enabled);
    }
    set_default_enabled();
    assert(this->m_values.size() == this->m_enabled.size());
    return substituted ? ConfigHelpers::DeserializationResult::Substituted :
                         ConfigHelpers::DeserializationResult::Loaded;
}

bool ConfigOptionBools::deserialize(const std::string &str, bool append) {
    return this->deserialize_with_substitutions(str, append, ConfigHelpers::DeserializationSubstitution::Disabled) ==
        ConfigHelpers::DeserializationResult::Loaded;
}

void ConfigOptionBools::serialize_single_value(std::ostringstream &ss, const unsigned char v, bool enabled) const {
    if (!enabled) {
        ss << "!";
        assert(this->can_be_disabled());
    }
    ss << (v ? "1" : "0");
}

std::string ConfigOptionEnumGeneric::serialize() const {
    std::string prefix;
    if (!this->is_enabled())
        prefix = "!";
    for (const auto &kvp : *this->keys_map)
        if (kvp.second == this->value)
            return prefix + kvp.first;
    return prefix;
}

bool ConfigOptionEnumGeneric::deserialize(const std::string &str, bool append) {
    (void) append;
    auto it = this->keys_map->find(str);
    if (it == this->keys_map->end())
        return false;
    this->value = it->second;
    return true;
}

PrinterTechnology parse_printer_technology(const std::string &technology) {
    if (technology == "FFF")
        return PrinterTechnology::ptFFF;
    else if (technology == "SLA")
        return PrinterTechnology::ptSLA;
    else if (technology == "SLS")
        return PrinterTechnology::ptSLS;
    else if (technology == "MILL")
        return PrinterTechnology::ptMill;
    else if (technology == "LASER")
        return PrinterTechnology::ptLaser;
    return PrinterTechnology::ptUnknown;
}

std::string to_string(PrinterTechnology tech) {
    if (tech == PrinterTechnology::ptFFF)
        return "FFF";
    else if (tech == PrinterTechnology::ptSLA)
        return "SLA";
    else if (tech == PrinterTechnology::ptSLS)
        return "SLS";
    else if (tech == PrinterTechnology::ptMill)
        return "MILL";
    else if (tech == PrinterTechnology::ptLaser)
        return "LASER";
    return "Unknown";
}

std::string toString(OptionCategory opt) {
    switch (opt) {
    case OptionCategory::none: return "";
    case OptionCategory::perimeter: return L("Perimeters & Shell");
    case OptionCategory::slicing: return L("Slicing");
    case OptionCategory::infill: return L("Infill");
    case OptionCategory::ironing: return L("Ironing PP");
    case OptionCategory::skirtBrim: return L("Skirt & Brim");
    case OptionCategory::support: return L("Support material");
    case OptionCategory::width: return L("Width & Flow");
    case OptionCategory::speed: return L("Speed");
    case OptionCategory::extruders: return L("Multiple extruders");
    case OptionCategory::output: return L("Output options");
    case OptionCategory::notes: return L("Notes");
    case OptionCategory::dependencies: return L("Dependencies");
    case OptionCategory::filament: return L("Filament");
    case OptionCategory::cooling: return L("Cooling");
    case OptionCategory::advanced: return L("Advanced");
    case OptionCategory::filoverride: return L("Filament overrides");
    case OptionCategory::customgcode: return L("Custom G-code");
    case OptionCategory::general: return L("General");
    case OptionCategory::limits: return L("Machine limits"); // if not used, no need ot ask for translation
    case OptionCategory::mmsetup: return L("Single Extruder MM Setup");
    case OptionCategory::firmware: return L("Firmware");
    case OptionCategory::pad: return L("Pad");
    case OptionCategory::padSupp: return L("Pad and Support");
    case OptionCategory::wipe: return L("Wipe Options");
    case OptionCategory::milling: return L("Milling");
    case OptionCategory::hollowing: return "Hollowing";
    case OptionCategory::milling_extruders: return L("Milling tools");
    case OptionCategory::fuzzy_skin: return L("Fuzzy skin");
    }
    return "error";
}

// Escape \n, \r and backslash
std::string escape_string_cstyle(const std::string &str) {
    // Allocate a buffer twice the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 2, 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '\r') {
            (*outptr++) = '\\';
            (*outptr++) = 'r';
        } else if (c == '\n') {
            (*outptr++) = '\\';
            (*outptr++) = 'n';
        } else if (c == '\\') {
            (*outptr++) = '\\';
            (*outptr++) = '\\';
        } else
            (*outptr++) = c;
    }
    return std::string(out.data(), outptr - out.data());
}

std::string escape_strings_cstyle(const std::vector<std::string> &strs) { return escape_strings_cstyle(strs, {}); }

std::string escape_strings_cstyle(const std::vector<std::string> &strs, const std::vector<bool> &enables) {
    assert(strs.size() == enables.size() || enables.empty());
    // 1) Estimate the output buffer size to avoid buffer reallocation.
    size_t outbuflen = 0;
    for (size_t i = 0; i < strs.size(); ++i)
        // Reserve space for every character escaped + quotes + semicolon + enable.
        outbuflen += strs[i].size() * 2 + ((enables.empty() || enables[i]) ? 3 : 4);
    // 2) Fill in the buffer.
    std::vector<char> out(outbuflen, 0);
    char *outptr = out.data();
    for (size_t j = 0; j < strs.size(); ++j) {
        if (j > 0)
            // Separate the strings.
            (*outptr++) = ';';
        if (!(enables.empty() || enables[j])) {
            (*outptr++) = '!';
            (*outptr++) = ':';
        }
        const std::string &str = strs[j];
        // Is the string simple or complex? Complex string contains spaces, tabs, new lines and other
        // escapable characters. Empty string shall be quoted as well, if it is the only string in strs.
        bool should_quote = strs.size() == 1 && str.empty();
        for (size_t i = 0; i < str.size(); ++i) {
            char c = str[i];
            if (c == ' ' || c == ';' || c == ',' || c == '\t' || c == '\\' || c == '"' || c == '\r' || c == '\n') {
                should_quote = true;
                break;
            }
        }
        if (should_quote) {
            (*outptr++) = '"';
            for (size_t i = 0; i < str.size(); ++i) {
                char c = str[i];
                if (c == '\\' || c == '"') {
                    (*outptr++) = '\\';
                    (*outptr++) = c;
                } else if (c == '\r') {
                    (*outptr++) = '\\';
                    (*outptr++) = 'r';
                } else if (c == '\n') {
                    (*outptr++) = '\\';
                    (*outptr++) = 'n';
                } else
                    (*outptr++) = c;
            }
            (*outptr++) = '"';
        } else {
            memcpy(outptr, str.data(), str.size());
            outptr += str.size();
        }
    }
    return std::string(out.data(), outptr - out.data());
}

// Unescape \n, \r and backslash
bool unescape_string_cstyle(const std::string &str, std::string &str_out) {
    std::vector<char> out(str.size(), 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '\\') {
            if (++i == str.size())
                return false;
            c = str[i];
            if (c == 'r')
                (*outptr++) = '\r';
            else if (c == 'n')
                (*outptr++) = '\n';
            else
                (*outptr++) = c;
        } else
            (*outptr++) = c;
    }
    str_out.assign(out.data(), outptr - out.data());
    return true;
}

bool unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values) {
    std::vector<bool> useless;
    return unescape_strings_cstyle(str, out_values, useless);
}
bool unescape_strings_cstyle(const std::string &str,
                             std::vector<std::string> &out_values,
                             std::vector<bool> &out_enables) {
    if (str.empty())
        return true;

    size_t i = 0;
    for (;;) {
        // Skip white spaces.
        char c = str[i];
        while (c == ' ' || c == '\t') {
            if (++i == str.size())
                return true;
            c = str[i];
        }
        bool enable = true;
        if (c == '!' && str.size() > i + 1 && str[i + 1] == ':') {
            enable = false;
            ++i;
            c = str[++i];
        }
        // Start of a word.
        std::vector<char> buf;
        buf.reserve(16);
        // Is it enclosed in quotes?
        c = str[i];
        if (c == '"') {
            // Complex case, string is enclosed in quotes.
            for (++i; i < str.size(); ++i) {
                c = str[i];
                if (c == '"') {
                    // End of string.
                    break;
                }
                if (c == '\\') {
                    if (++i == str.size())
                        return false;
                    c = str[i];
                    if (c == 'r')
                        c = '\r';
                    else if (c == 'n')
                        c = '\n';
                }
                buf.push_back(c);
            }
            if (i == str.size())
                return false;
            ++i;
        } else {
            for (; i < str.size(); ++i) {
                c = str[i];
                if (c == ';' || c == ',')
                    break;
                buf.push_back(c);
            }
        }
        // Store the string into the output vector.
        out_values.push_back(std::string(buf.data(), buf.size()));
        out_enables.push_back(enable);
        if (i == str.size())
            return true;
        // Skip white spaces.
        c = str[i];
        while (c == ' ' || c == '\t') {
            if (++i == str.size())
                // End of string. This is correct.
                return true;
            c = str[i];
        }
        if (c != ';' && c != ',')
            return false;
        if (++i == str.size()) {
            // Emit one additional empty string.
            out_values.push_back(std::string());
            out_enables.push_back(true);
            return true;
        }
    }
}

std::string escape_ampersand(const std::string &str) {
    // Allocate a buffer 2 times the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 6, 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '&') {
            (*outptr++) = '&';
            (*outptr++) = '&';
        } else
            (*outptr++) = c;
    }
    return std::string(out.data(), outptr - out.data());
}

bool GraphData::operator<(const GraphData &rhs) const {
    if (this->data_size() == rhs.data_size()) {
        const Pointfs my_data = this->data();
        const Pointfs other_data = rhs.data();
        assert(my_data.size() == other_data.size());
        auto it_this = my_data.begin();
        auto it_other = other_data.begin();
        while (it_this != my_data.end()) {
            if (it_this->x() != it_other->x())
                return it_this->x() < it_other->x();
            if (it_this->y() != it_other->y())
                return it_this->y() < it_other->y();
            ++it_this;
            ++it_other;
        }
        return this->type < rhs.type;
    }
    return this->data_size() < rhs.data_size();
}

bool GraphData::operator>(const GraphData &rhs) const {
    if (this->data_size() == rhs.data_size()) {
        const Pointfs my_data = this->data();
        const Pointfs other_data = rhs.data();
        assert(my_data.size() == other_data.size());
        auto it_this = my_data.begin();
        auto it_other = other_data.begin();
        while (it_this != my_data.end()) {
            if (it_this->x() != it_other->x())
                return it_this->x() > it_other->x();
            if (it_this->y() != it_other->y())
                return it_this->y() > it_other->y();
            ++it_this;
            ++it_other;
        }
        return this->type > rhs.type;
    }
    return this->data_size() > rhs.data_size();
}

} // namespace Slic3r

namespace std {

std::size_t hash<Slic3r::FloatOrPercent>::operator()(const Slic3r::FloatOrPercent &v) const noexcept {
    std::size_t seed = std::hash<double>{}(v.value);
    return v.percent ? seed ^ 0x9e3779b9 : seed;
}

std::size_t hash<Slic3r::GraphData>::operator()(const Slic3r::GraphData &v) const noexcept {
    std::size_t seed = 0;
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.begin_idx));
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.end_idx));
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.type));
    for (const Slic3r::Vec2d &pt : v.graph_points) {
        Slic3r::config_hash_combine_value(seed, std::hash<double>{}(pt.x()));
        Slic3r::config_hash_combine_value(seed, std::hash<double>{}(pt.y()));
    }
    return seed;
}

std::size_t hash<Slic3r::Vec2d>::operator()(const Slic3r::Vec2d &v) const noexcept {
    std::size_t seed = std::hash<double>{}(v.x());
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.y()));
    return seed;
}

std::size_t hash<Slic3r::Vec3d>::operator()(const Slic3r::Vec3d &v) const noexcept {
    std::size_t seed = std::hash<double>{}(v.x());
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.y()));
    Slic3r::config_hash_combine_value(seed, std::hash<double>{}(v.z()));
    return seed;
}

} // namespace std

namespace Slic3r {

Pointfs GraphData::data() const {
    assert(validate());
    return Pointfs(this->graph_points.begin() + this->begin_idx, this->graph_points.begin() + this->end_idx);
}

size_t GraphData::data_size() const {
    assert(validate());
    return this->end_idx - this->begin_idx;
}

double GraphData::interpolate(double x_value) const {
    double y_value = 1.0f;
    if (this->data_size() < 1) {
        // nothing
    } else if (this->graph_points.size() == 1 || this->graph_points[begin_idx].x() >= x_value) {
        y_value = this->graph_points.front().y();
    } else if (this->graph_points[end_idx - 1].x() <= x_value) {
        y_value = this->graph_points[end_idx - 1].y();
    } else {
        // find first and second datapoint
        for (size_t idx = this->begin_idx; idx < this->end_idx; ++idx) {
            const auto &data_point = this->graph_points[idx];
            if (is_approx(data_point.x(), x_value)) {
                // lucky point
                return data_point.y();
            } else if (data_point.x() < x_value) {
                // not yet, iterate
            } else if (idx == 0) {
                return data_point.y();
            } else {
                // interpolate
                const auto &data_point_before = this->graph_points[idx - 1];
                assert(data_point.x() > data_point_before.x());
                assert(data_point_before.x() < x_value);
                assert(data_point.x() > x_value);
                if (this->type == GraphData::GraphType::SQUARE) {
                    y_value = data_point_before.y();
                } else if (this->type == GraphData::GraphType::LINEAR) {
                    const double interval = data_point.x() - data_point_before.x();
                    const double ratio_before = (x_value - data_point_before.x()) / interval;
                    double mult = data_point_before.y() * (1 - ratio_before) + data_point.y() * ratio_before;
                    y_value = mult;
                } else if (this->type == GraphData::GraphType::SPLINE) {
                    // Cubic spline interpolation: see https://en.wikiversity.org/wiki/Cubic_Spline_Interpolation#Methods
                    const bool boundary_first_derivative = true; // true - first derivative is 0 at the leftmost and
                                                                 // rightmost point false - second ---- || -------
                    // TODO: cache (if the caller use my cache).
                    const int N = end_idx - begin_idx -
                        1; // last point can be accessed as N, we have N+1 total points
                    std::vector<float> diag(N + 1);
                    std::vector<float> mu(N + 1);
                    std::vector<float> lambda(N + 1);
                    std::vector<float> h(N + 1);
                    std::vector<float> rhs(N + 1);

                    // let's fill in inner equations
                    for (int i = 1 + begin_idx; i <= N + begin_idx; ++i)
                        h[i] = this->graph_points[i].x() - this->graph_points[i - 1].x();
                    std::fill(diag.begin(), diag.end(), 2.f);
                    for (int i = 1 + begin_idx; i <= N + begin_idx - 1; ++i) {
                        mu[i] = h[i] / (h[i] + h[i + 1]);
                        lambda[i] = 1.f - mu[i];
                        rhs[i] = 6 *
                            (float(this->graph_points[i + 1].y() - this->graph_points[i].y()) /
                                 (h[i + 1] * (this->graph_points[i + 1].x() - this->graph_points[i - 1].x())) -
                             float(this->graph_points[i].y() - this->graph_points[i - 1].y()) /
                                 (h[i] * (this->graph_points[i + 1].x() - this->graph_points[i - 1].x())));
                    }

                    // now fill in the first and last equations, according to boundary conditions:
                    if (boundary_first_derivative) {
                        const float endpoints_derivative = 0;
                        lambda[0] = 1;
                        mu[N] = 1;
                        rhs[0] = (6.f / h[1]) *
                            (float(this->graph_points[begin_idx].y() - this->graph_points[1 + begin_idx].y()) /
                                 (this->graph_points[begin_idx].x() - this->graph_points[1 + begin_idx].x()) -
                             endpoints_derivative);
                        rhs[N] = (6.f / h[N]) *
                            (endpoints_derivative -
                             float(this->graph_points[N + begin_idx - 1].y() - this->graph_points[N + begin_idx].y()) /
                                 (this->graph_points[N + begin_idx - 1].x() - this->graph_points[N + begin_idx].x()));
                    } else {
                        lambda[0] = 0;
                        mu[N] = 0;
                        rhs[0] = 0;
                        rhs[N] = 0;
                    }

                    // the trilinear system is ready to be solved:
                    for (int i = 1; i <= N; ++i) {
                        float multiple = mu[i] / diag[i - 1]; // let's subtract proper multiple of above equation
                        diag[i] -= multiple * lambda[i - 1];
                        rhs[i] -= multiple * rhs[i - 1];
                    }
                    // now the back substitution (vector mu contains invalid values from now on):
                    rhs[N] = rhs[N] / diag[N];
                    for (int i = N - 1; i >= 0; --i)
                        rhs[i] = (rhs[i] - lambda[i] * rhs[i + 1]) / diag[i];

                    // now interpolate at our point
                    size_t curr_idx = idx - begin_idx;
                    y_value = (rhs[curr_idx - 1] * pow(this->graph_points[idx].x() - x_value, 3) +
                               rhs[curr_idx] * pow(x_value - this->graph_points[idx - 1].x(), 3)) /
                            (6 * h[curr_idx]) +
                        (this->graph_points[idx - 1].y() - rhs[curr_idx - 1] * h[curr_idx] * h[curr_idx] / 6.f) *
                            (this->graph_points[idx].x() - x_value) / h[curr_idx] +
                        (this->graph_points[idx].y() - rhs[curr_idx] * h[curr_idx] * h[curr_idx] / 6.f) *
                            (x_value - this->graph_points[idx - 1].x()) / h[curr_idx];
                } else {
                    assert(false);
                }
                return y_value;
            }
        }
    }
    return y_value;
}

double GraphData::inverse_interpolate(double y_value) const {
    GraphData inverse = *this;
    // inverse x & y
    for (Vec2d &data_point : inverse.graph_points) {
        std::swap(data_point.x(), data_point.y());
    }
    return inverse.interpolate(y_value);
}

bool GraphData::validate() const {
    if (this->begin_idx < 0 || this->end_idx < 0 || this->end_idx < this->begin_idx)
        return false;
    if (this->end_idx > this->graph_points.size() && !this->graph_points.empty())
        return false;
    if (this->graph_points.empty())
        return this->end_idx == 0 && this->begin_idx == 0;
    for (size_t i = 1; i < this->graph_points.size(); ++i)
        if (this->graph_points[i - 1].x() > this->graph_points[i].x())
            return false;
    return true;
}

std::string GraphData::serialize() const {
    std::ostringstream ss;
    ss << this->begin_idx;
    ss << ":";
    ss << this->end_idx;
    ss << ":";
    ss << uint16_t(this->type);
    for (const Vec2d &graph_point : this->graph_points) {
        ss << ":";
        ss << graph_point.x();
        ss << "x";
        ss << graph_point.y();
    }
    return ss.str();
}

bool GraphData::deserialize(const std::string &str) {
    if (size_t pos = str.find('|'); pos != std::string::npos) {
        // old format
        assert(str.size() > pos + 2);
        assert(str[pos + 1] == ' ');
        assert(str[pos + 2] != ' ');
        if (str.size() > pos + 1) {
            std::string buttons = str.substr(pos + 2);
            size_t start = 0;
            size_t end_x = buttons.find(' ', start);
            size_t end_y = buttons.find(' ', end_x + 1);
            while (end_x != std::string::npos && end_y != std::string::npos) {
                this->graph_points.emplace_back();
                Vec2d &data_point = this->graph_points.back();
                data_point.x() = std::stod(buttons.substr(start, end_x));
                data_point.y() = std::stod(buttons.substr(end_x + 1, end_y));
                start = end_y + 1;
                end_x = buttons.find(' ', start);
                end_y = buttons.find(' ', end_x + 1);
            }
            if (end_x != std::string::npos && end_x + 1 < buttons.size()) {
                this->graph_points.emplace_back();
                Vec2d &data_point = this->graph_points.back();
                data_point.x() = std::stod(buttons.substr(start, end_x));
                data_point.y() = std::stod(buttons.substr(end_x + 1, buttons.size()));
            }
        }
        this->begin_idx = 0;
        this->end_idx = this->graph_points.size();
        this->type = GraphType::SPLINE;
    } else if (size_t pos = str.find(','); pos != std::string::npos) {
        // maybe a coStrings with 0,0 values inside, like a coPoints but worse (used by orca's
        // small_area_infill_flow_compensation_model)
        std::vector<std::string> args;
        boost::split(args, str, boost::is_any_of(","));
        if (args.size() % 2 == 0) {
            for (size_t i = 0; i < args.size(); i += 2) {
                this->graph_points.emplace_back();
                Vec2d &data_point = this->graph_points.back();
                args[i].erase(std::remove(args[i].begin(), args[i].end(), '\n'), args[i].end());
                args[i].erase(std::remove(args[i].begin(), args[i].end(), '"'), args[i].end());
                data_point.x() = std::stod(args[i]);
                args[i + 1].erase(std::remove(args[i + 1].begin(), args[i + 1].end(), '\n'), args[i + 1].end());
                args[i + 1].erase(std::remove(args[i + 1].begin(), args[i + 1].end(), '"'), args[i + 1].end());
                data_point.y() = std::stod(args[i + 1]);
            }
        }
        this->begin_idx = 0;
        this->end_idx = this->graph_points.size();
        this->type = GraphType::SPLINE;
    } else {
        std::istringstream iss(str);
        std::string item;
        char sep_point = 'x';
        char sep = ':';
        std::vector<std::string> values_str;
        // get begin_idx
        if (std::getline(iss, item, sep)) {
            std::istringstream(item) >> this->begin_idx;
        } else
            return false;
        // get end_idx
        if (std::getline(iss, item, sep)) {
            std::istringstream(item) >> this->end_idx;
        } else
            return false;
        // get type
        if (std::getline(iss, item, sep)) {
            uint16_t int_type;
            std::istringstream(item) >> int_type;
            this->type = GraphType(int_type);
        } else
            return false;
        // get points
        while (std::getline(iss, item, sep)) {
            this->graph_points.emplace_back();
            Vec2d &data_point = this->graph_points.back();
            std::string s_point;
            std::istringstream isspoint(item);
            if (std::getline(isspoint, s_point, sep_point)) {
                std::istringstream(s_point) >> data_point.x();
            } else
                return false;
            if (std::getline(isspoint, s_point, sep_point)) {
                std::istringstream(s_point) >> data_point.y();
            } else
                return false;
        }
    }
    // check if data is okay
    if (!this->validate())
        return false;
    return true;
}

// TODO: replace ConfigOptionDef* by ConfigOptionDef&
} // namespace Slic3r

#include <cereal/types/polymorphic.hpp>
CEREAL_REGISTER_TYPE(Slic3r::ConfigOption)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<int32_t>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<bool>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVectorBase)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<int32_t>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInt)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInts)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionString)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionGraph)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionGraphs)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBool)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBools)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionEnumGeneric)
CEREAL_REGISTER_TYPE(Slic3r::ConfigBase)
CEREAL_REGISTER_TYPE(Slic3r::DynamicConfig)

CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<int32_t>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<std::string>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<Slic3r::Vec2d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<Slic3r::Vec3d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<Slic3r::GraphData>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<bool>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionVectorBase)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<int32_t>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<Slic3r::GraphData>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<double>, Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<double>, Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<int32_t>, Slic3r::ConfigOptionInt)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<int32_t>, Slic3r::ConfigOptionInts)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<std::string>, Slic3r::ConfigOptionString)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<std::string>, Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloat, Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloats, Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionPercent, Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>,
                                     Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>, Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::Vec2d>, Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>, Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::GraphData>, Slic3r::ConfigOptionGraph)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::GraphData>, Slic3r::ConfigOptionGraphs)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<bool>, Slic3r::ConfigOptionBool)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<unsigned char>, Slic3r::ConfigOptionBools)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionInt, Slic3r::ConfigOptionEnumGeneric)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigBase, Slic3r::DynamicConfig)
