///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena, David Kocík @kocikdav, Tomáš Mészáros @tamasmeszaros, Vojtěch Král @vojtechkral, Oleksandra Iushchenko @YuSanka
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
///|/
#include "Config.hpp"
#include "Flow.hpp"
#include "format.hpp"
#include "Preset.hpp"
#include "Utils.hpp"
#include "LocalesUtils.hpp"

#include <assert.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/config.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/nowide/cenv.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/format.hpp>
#include <string.h>

#include <LibBGCode/binarize/binarize.hpp>

//FIXME for GCodeFlavor and gcfMarlin (for forward-compatibility conversion)
// This is not nice, likely it would be better to pass the ConfigSubstitutionContext to handle_legacy().
#include "PrintConfig.hpp"

#define L(s) (s)

namespace Slic3r {


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
    case OptionCategory::fuzzy_skin : return L("Fuzzy skin");
    }
    return "error";
}

std::map<std::string, ConfigOptionMode> ConfigOptionDef::names_2_tag_mode = { {"Simple",comSimple},{"Advanced",comAdvanced},{"Expert",comExpert},{"Prusa",comPrusa},{"SuSi",comSuSi} };

// Escape \n, \r and backslash
std::string escape_string_cstyle(const std::string &str)
{
    // Allocate a buffer twice the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 2, 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++ i) {
        char c = str[i];
        if (c == '\r') {
            (*outptr ++) = '\\';
            (*outptr ++) = 'r';
        } else if (c == '\n') {
            (*outptr ++) = '\\';
            (*outptr ++) = 'n';
        } else if (c == '\\') {
            (*outptr ++) = '\\';
            (*outptr ++) = '\\';
        } else
            (*outptr ++) = c;
    }
    return std::string(out.data(), outptr - out.data());
}

std::string escape_strings_cstyle(const std::vector<std::string> &strs)
{
    return escape_strings_cstyle(strs, {});
}

std::string escape_strings_cstyle(const std::vector<std::string> &strs, const std::vector<bool> &enables)
{
    assert(strs.size() == enables.size() || enables.empty());
    // 1) Estimate the output buffer size to avoid buffer reallocation.
    size_t outbuflen = 0;
    for (size_t i = 0; i < strs.size(); ++ i)
        // Reserve space for every character escaped + quotes + semicolon + enable.
        outbuflen += strs[i].size() * 2 + ((enables.empty() || enables[i]) ? 3 : 4);
    // 2) Fill in the buffer.
    std::vector<char> out(outbuflen, 0);
    char *outptr = out.data();
    for (size_t j = 0; j < strs.size(); ++ j) {
        if (j > 0)
            // Separate the strings.
            (*outptr ++) = ';';
        if (!(enables.empty() || enables[j]))
            (*outptr ++) = '!';
        const std::string &str = strs[j];
        // Is the string simple or complex? Complex string contains spaces, tabs, new lines and other
        // escapable characters. Empty string shall be quoted as well, if it is the only string in strs.
        bool should_quote = strs.size() == 1 && str.empty();
        for (size_t i = 0; i < str.size(); ++ i) {
            char c = str[i];
            if (c == ' ' || c == ';' || c == ',' || c == '\t' || c == '\\' || c == '"' || c == '\r' || c == '\n') {
                should_quote = true;
                break;
            }
        }
        if (should_quote) {
            (*outptr ++) = '"';
            for (size_t i = 0; i < str.size(); ++ i) {
                char c = str[i];
                if (c == '\\' || c == '"') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = c;
                } else if (c == '\r') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = 'r';
                } else if (c == '\n') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = 'n';
                } else
                    (*outptr ++) = c;
            }
            (*outptr ++) = '"';
        } else {
            memcpy(outptr, str.data(), str.size());
            outptr += str.size();
        }
    }
    return std::string(out.data(), outptr - out.data());
}

// Unescape \n, \r and backslash
bool unescape_string_cstyle(const std::string &str, std::string &str_out)
{
    std::vector<char> out(str.size(), 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++ i) {
        char c = str[i];
        if (c == '\\') {
            if (++ i == str.size())
                return false;
            c = str[i];
            if (c == 'r')
                (*outptr ++) = '\r';
            else if (c == 'n')
                (*outptr ++) = '\n';
            else
                (*outptr ++) = c;
        } else
            (*outptr ++) = c;
    }
    str_out.assign(out.data(), outptr - out.data());
    return true;
}

bool unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values)
{
    std::vector<bool> useless;
    return unescape_strings_cstyle(str, out_values, useless);
}
bool unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out_values, std::vector<bool> &out_enables)
{
    if (str.empty())
        return true;

    size_t i = 0;
    for (;;) {
        // Skip white spaces.
        char c = str[i];
        while (c == ' ' || c == '\t') {
            if (++ i == str.size())
                return true;
            c = str[i];
        }
        bool enable = true;
        if (c == '!') {
            enable = false;
            c = str[++i];
        }
        // Start of a word.
        std::vector<char> buf;
        buf.reserve(16);
        // Is it enclosed in quotes?
        c = str[i];
        if (c == '"') {
            // Complex case, string is enclosed in quotes.
            for (++ i; i < str.size(); ++ i) {
                c = str[i];
                if (c == '"') {
                    // End of string.
                    break;
                }
                if (c == '\\') {
                    if (++ i == str.size())
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
            ++ i;
        } else {
            for (; i < str.size(); ++ i) {
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
            if (++ i == str.size())
                // End of string. This is correct.
                return true;
            c = str[i];
        }
        if (c != ';' && c != ',')
            return false;
        if (++ i == str.size()) {
            // Emit one additional empty string.
            out_values.push_back(std::string());
            out_enables.push_back(true);
            return true;
        }
    }
}

std::string escape_ampersand(const std::string& str)
{
    // Allocate a buffer 2 times the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 6, 0);
    char* outptr = out.data();
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

bool GraphData::operator<(const GraphData &rhs) const
{
    if (this->data_size() == rhs.data_size()) {
        const Pointfs my_data = this->data();
        const Pointfs other_data = rhs.data();
        assert(my_data.size() == other_data.size());
        auto it_this = my_data.begin();
        auto it_other = other_data.begin();
        while (it_this != my_data.end()) {
            if(it_this->x() != it_other->x())
                return it_this->x() < it_other->x();
            if(it_this->y() != it_other->y())
                return it_this->y() < it_other->y();
            ++it_this;
            ++it_other;
        }
        return this->type < rhs.type;
    }
    return this->data_size() < rhs.data_size();
}

bool GraphData::operator>(const GraphData &rhs) const
{
    if (this->data_size() == rhs.data_size()) {
        const Pointfs my_data = this->data();
        const Pointfs other_data = rhs.data();
        assert(my_data.size() == other_data.size());
        auto it_this = my_data.begin();
        auto it_other = other_data.begin();
        while (it_this != my_data.end()) {
            if(it_this->x() != it_other->x())
                return it_this->x() > it_other->x();
            if(it_this->y() != it_other->y())
                return it_this->y() > it_other->y();
            ++it_this;
            ++it_other;
        }
        return this->type > rhs.type;
    }
    return this->data_size() > rhs.data_size();
}

Pointfs GraphData::data() const
{
    assert(validate());
    return Pointfs(this->graph_points.begin() + this->begin_idx, this->graph_points.begin() + this->end_idx);
}

size_t GraphData::data_size() const
{
    assert(validate());
    return this->end_idx - this->begin_idx;
}

double GraphData::interpolate(double x_value) const{
    double y_value = 0.;
    if (this->data_size() < 1) {
        // nothing
    } else if (this->graph_points.size() == 1 || this->graph_points.front().x() >= x_value) {
        y_value = this->graph_points.front().y();
    } else if (this->graph_points.back().x() <= x_value) {
        y_value = this->graph_points.back().y();
    } else {
        // find first and second datapoint
        for (size_t idx = this->begin_idx; idx < this->end_idx; ++idx) {
            const auto &data_point = this->graph_points[idx];
            if (data_point.x() == x_value) {
                // lucky point
                y_value = data_point.y();
                break;
            } else if (data_point.x() < x_value) {
                // not yet, iterate
            } else if (idx == 0) {
                y_value = data_point.y();
                break;
            } else {
                // interpolate
                const auto &data_point_before = this->graph_points[idx - 1];
                assert(data_point.x() > data_point_before.x());
                assert(data_point_before.x() < x_value);
                assert(data_point.x() > x_value);
                if (this->type == GraphData::GraphType::SQUARE) {
                    y_value = data_point_before.y();
                } else if (this->type == GraphData::GraphType::LINEAR) {
                    const double interval     = data_point.x() - data_point_before.x();
                    const double ratio_before = (x_value - data_point_before.x()) / interval;
                    double mult = data_point_before.y() * (1 - ratio_before) + data_point.y() * ratio_before;
                    y_value = mult;
                } else if (this->type == GraphData::GraphType::SPLINE) {
                    // Cubic spline interpolation: see https://en.wikiversity.org/wiki/Cubic_Spline_Interpolation#Methods
                    const bool boundary_first_derivative = true; // true - first derivative is 0 at the leftmost and
                                                                 // rightmost point false - second ---- || -------
                    // TODO: cache (if the caller use my cache).
                    const int N = end_idx - begin_idx - 1; // last point can be accessed as N, we have N+1 total points
                    std::vector<float> diag(N + 1);
                    std::vector<float> mu(N + 1);
                    std::vector<float> lambda(N + 1);
                    std::vector<float> h(N + 1);
                    std::vector<float> rhs(N + 1);

                    // let's fill in inner equations
                    for (int i = 1 + begin_idx; i <= N + begin_idx; ++i) h[i] = this->graph_points[i].x() - this->graph_points[i - 1].x();
                    std::fill(diag.begin(), diag.end(), 2.f);
                    for (int i = 1 + begin_idx; i <= N + begin_idx - 1; ++i) {
                        mu[i]     = h[i] / (h[i] + h[i + 1]);
                        lambda[i] = 1.f - mu[i];
                        rhs[i]    = 6 * (float(this->graph_points[i + 1].y() - this->graph_points[i].y()) /
                                          (h[i + 1] * (this->graph_points[i + 1].x() - this->graph_points[i - 1].x())) -
                                      float(this->graph_points[i].y() - this->graph_points[i - 1].y()) /
                                          (h[i] * (this->graph_points[i + 1].x() - this->graph_points[i - 1].x())));
                    }

                    // now fill in the first and last equations, according to boundary conditions:
                    if (boundary_first_derivative) {
                        const float endpoints_derivative = 0;
                        lambda[0]                        = 1;
                        mu[N]                            = 1;
                        rhs[0] = (6.f / h[1]) * (float(this->graph_points[begin_idx].y() - this->graph_points[1 + begin_idx].y()) /
                                                     (this->graph_points[begin_idx].x() - this->graph_points[1 + begin_idx].x()) - endpoints_derivative);
                        rhs[N] = (6.f / h[N]) * (endpoints_derivative - float(this->graph_points[N + begin_idx - 1].y() - this->graph_points[N + begin_idx].y()) /
                                                                            (this->graph_points[N + begin_idx - 1].x() - this->graph_points[N + begin_idx].x()));
                    } else {
                        lambda[0] = 0;
                        mu[N]     = 0;
                        rhs[0]    = 0;
                        rhs[N]    = 0;
                    }

                    // the trilinear system is ready to be solved:
                    for (int i = 1; i <= N; ++i) {
                        float multiple = mu[i] / diag[i - 1]; // let's subtract proper multiple of above equation
                        diag[i] -= multiple * lambda[i - 1];
                        rhs[i] -= multiple * rhs[i - 1];
                    }
                    // now the back substitution (vector mu contains invalid values from now on):
                    rhs[N] = rhs[N] / diag[N];
                    for (int i = N - 1; i >= 0; --i) rhs[i] = (rhs[i] - lambda[i] * rhs[i + 1]) / diag[i];

                    //now interpolate at our point
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
                break;
            }
        }
    }
    return y_value;
}

bool GraphData::validate() const
{
    if (this->begin_idx < 0 || this->end_idx < 0 || this->end_idx < this->begin_idx)
        return false;
    if (this->end_idx > this->graph_points.size() && !this->graph_points.empty())
        return false;
    if(this->graph_points.empty())
        return this->end_idx == 0 && this->begin_idx == 0;
    for (size_t i = 1; i < this->graph_points.size(); ++i)
        if (this->graph_points[i - 1].x() > this->graph_points[i].x())
            return false;
    return true;
}

std::string GraphData::serialize() const
{
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
    
bool GraphData::deserialize(const std::string &str)
{
    if (size_t pos = str.find('|'); pos != std::string::npos) {
        // old format
        assert(str.size() > pos + 2);
        assert(str[pos+1] == ' ');
        assert(str[pos+2] != ' ');
        if (str.size() > pos + 1) {
            std::string buttons = str.substr(pos + 2);
            size_t start = 0;
            size_t end_x = buttons.find(' ', start);
            size_t end_y= buttons.find(' ', end_x + 1);
            while (end_x != std::string::npos && end_y != std::string::npos) {
                this->graph_points.emplace_back();
                Vec2d &data_point = this->graph_points.back();
                data_point.x() = std::stod(buttons.substr(start, end_x));
                data_point.y() = std::stod(buttons.substr(end_x + 1, end_y));
                start = end_y + 1;
                end_x = buttons.find(' ', start);
                end_y= buttons.find(' ', end_x + 1);
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
        //maybe a coStrings with 0,0 values inside, like a coPoints but worse (used by orca's small_area_infill_flow_compensation_model)
        std::vector<std::string> args;
        boost::split(args, str, boost::is_any_of(","));
        if (args.size() % 2 == 0) {
            for (size_t i = 0; i < args.size(); i += 2) {
                this->graph_points.emplace_back();
                Vec2d &data_point = this->graph_points.back();
                args[i].erase(std::remove(args[i].begin(), args[i].end(), '\n'), args[i].end());
                args[i].erase(std::remove(args[i].begin(), args[i].end(), '"'), args[i].end());
                data_point.x() = std::stod(args[i]);
                args[i+1].erase(std::remove(args[i+1].begin(), args[i+1].end(), '\n'), args[i+1].end());
                args[i+1].erase(std::remove(args[i+1].begin(), args[i+1].end(), '"'), args[i+1].end());
                data_point.y() = std::stod(args[i+1]);
            }
        }
        this->begin_idx = 0;
        this->end_idx = this->graph_points.size();
        this->type = GraphType::SPLINE;
    } else {
        std::istringstream iss(str);
        std::string              item;
        char                     sep_point = 'x';
        char                     sep       = ':';
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
            std::string                s_point;
            std::istringstream         isspoint(item);
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
    //check if data is okay
    if (!this->validate()) return false;
    return true;
}

void ConfigOptionDeleter::operator()(ConfigOption* p) {
    delete p;
}

std::vector<std::string> ConfigOptionDef::cli_args(const std::string &key) const
{
	std::vector<std::string> args;
	if (this->cli != ConfigOptionDef::nocli) {
        const std::string &cli = this->cli;
        //FIXME What was that for? Check the "readline" documentation.
        // Neither '=' nor '!' is used in any of the cli parameters currently defined by PrusaSlicer.
//        std::string cli = this->cli.substr(0, this->cli.find("="));
//        boost::trim_right_if(cli, boost::is_any_of("!"));
		if (cli.empty()) {
            // Convert an option key to CLI argument by replacing underscores with dashes.
            std::string opt = key;
            boost::replace_all(opt, "_", "-");
            args.emplace_back(std::move(opt));
        } else
			boost::split(args, cli, boost::is_any_of("|"));
    }
    return args;
}

ConfigOption* ConfigOptionDef::create_empty_option() const
{
	if (this->nullable) {
	    switch (this->type) {
        case coFloat:           return new ConfigOptionFloatNullable();
        case coInt:             return new ConfigOptionIntNullable();
	    case coFloats:          return new ConfigOptionFloatsNullable();
	    case coInts:            return new ConfigOptionIntsNullable();
	    case coPercents:        return new ConfigOptionPercentsNullable();
        case coFloatsOrPercents: return new ConfigOptionFloatsOrPercentsNullable();
	    case coBools:           return new ConfigOptionBoolsNullable();
	    default:                throw ConfigurationError(std::string("Unknown option type for nullable option ") + this->label);
	    }
	} else {
	    switch (this->type) {
	    case coFloat:           return new ConfigOptionFloat();
	    case coFloats:          return new ConfigOptionFloats();
	    case coInt:             return new ConfigOptionInt();
	    case coInts:            return new ConfigOptionInts();
	    case coString:          return new ConfigOptionString();
	    case coStrings:         return new ConfigOptionStrings();
	    case coPercent:         return new ConfigOptionPercent();
	    case coPercents:        return new ConfigOptionPercents();
	    case coFloatOrPercent:  return new ConfigOptionFloatOrPercent();
        case coFloatsOrPercents: return new ConfigOptionFloatsOrPercents();
	    case coPoint:           return new ConfigOptionPoint();
	    case coPoints:          return new ConfigOptionPoints();
	    case coPoint3:          return new ConfigOptionPoint3();
	//    case coPoint3s:         return new ConfigOptionPoint3s();
	    case coGraph:           return new ConfigOptionGraph();
	    case coGraphs:          return new ConfigOptionGraphs();
	    case coBool:            return new ConfigOptionBool();
	    case coBools:           return new ConfigOptionBools();
	    case coEnum:            return new ConfigOptionEnumGeneric(this->enum_def->m_enum_keys_map);
	    default:                throw ConfigurationError(std::string("Unknown option type for option ") + this->label);
	    }
	}
}

ConfigOption* ConfigOptionDef::create_default_option() const
{
    if (this->default_value)
        return (this->default_value->type() == coEnum) ?
            // Special case: For a DynamicConfig, convert a templated enum to a generic enum.
            new ConfigOptionEnumGeneric(this->enum_def->m_enum_keys_map, this->default_value->get_int()) :
            this->default_value->clone();
    return this->create_empty_option();
}

// Assignment of the serialization IDs is not thread safe. The Defs shall be initialized from the main thread!
ConfigOptionDef* ConfigDef::add(const t_config_option_key &opt_key, ConfigOptionType type)
{
	static size_t serialization_key_ordinal_last = 0;
    ConfigOptionDef *opt = &this->options[opt_key];
    opt->opt_key = opt_key;
    opt->type = type;
    opt->serialization_key_ordinal = ++ serialization_key_ordinal_last;
    this->by_serialization_key_ordinal[opt->serialization_key_ordinal] = opt;
    return opt;
}

ConfigOptionDef* ConfigDef::add_nullable(const t_config_option_key &opt_key, ConfigOptionType type)
{
	ConfigOptionDef *def = this->add(opt_key, type);
	def->nullable = true;
	return def;
}

void ConfigDef::finalize()
{
    // Validate & finalize open & closed enums.
    for (std::pair<const t_config_option_key, ConfigOptionDef> &kvp : options) {
        ConfigOptionDef& def = kvp.second;
        if (def.type == coEnum) {
            assert(def.enum_def);
            assert(def.enum_def->is_valid_closed_enum());
            assert(! def.is_gui_type_enum_open());
            def.enum_def->finalize_closed_enum();
        } else if (def.is_gui_type_enum_open()) {
            assert(def.enum_def);
            assert(def.enum_def->is_valid_open_enum());
            assert(def.gui_type != ConfigOptionDef::GUIType::i_enum_open || def.type == coInt || def.type == coInts);
            assert(def.gui_type != ConfigOptionDef::GUIType::f_enum_open || def.type == coFloat || def.type == coPercent || def.type == coFloatOrPercent);
            assert(def.gui_type != ConfigOptionDef::GUIType::select_open || def.type == coString || def.type == coStrings);
        } else {
            assert(! def.enum_def);
        }
    }
}

std::ostream& ConfigDef::print_cli_help(std::ostream& out, bool show_defaults, std::function<bool(const ConfigOptionDef &)> filter) const
{
    // prepare a function for wrapping text
    auto wrap = [](const std::string& text, size_t line_length) -> std::string {
        std::istringstream words(text);
        std::ostringstream wrapped;
        std::string word;
 
        if (words >> word) {
            wrapped << word;
            size_t space_left = line_length - word.length();
            while (words >> word) {
                if (space_left < word.length() + 1) {
                    wrapped << '\n' << word;
                    space_left = line_length - word.length();
                } else {
                    wrapped << ' ' << word;
                    space_left -= word.length() + 1;
                }
            }
        }
        return wrapped.str();
    };

    // get the unique categories
    std::set<OptionCategory> categories;
    for (const auto& opt : this->options) {
        const ConfigOptionDef& def = opt.second;
        if (filter(def))
            categories.insert(def.category);
    }
    
    for (OptionCategory category : categories) {
        if (category != OptionCategory::none) {
            out << toString(category) << ":" << std::endl;
        } else if (categories.size() > 1) {
            out << "Misc options:" << std::endl;
        }
        
        for (const auto& opt : this->options) {
            const ConfigOptionDef& def = opt.second;
			if (def.category != category || def.cli == ConfigOptionDef::nocli || !filter(def))
                continue;
            
            // get all possible variations: --foo, --foobar, -f...
            std::vector<std::string> cli_args = def.cli_args(opt.first);
			if (cli_args.empty())
				continue;

            for (auto& arg : cli_args) {
                arg.insert(0, (arg.size() == 1) ? "-" : "--");
                if (def.type == coFloat || def.type == coInt || def.type == coFloatOrPercent
                    || def.type == coFloats || def.type == coInts) {
                    arg += " N";
                } else if (def.type == coPoint) {
                    arg += " X,Y";
                } else if (def.type == coPoint3) {
                    arg += " X,Y,Z";
                } else if (def.type == coString || def.type == coStrings) {
                    arg += " ABCD";
                }
            }
            
            // left: command line options
            const std::string cli = boost::algorithm::join(cli_args, ", ");
            out << " " << std::left << std::setw(20) << cli;
            
            // right: option description
            std::string descr = def.tooltip;
            bool show_defaults_this = show_defaults || def.opt_key == "config_compatibility";
            if (show_defaults_this && def.default_value && def.type != coBool
                && (def.type != coString || !def.default_value->serialize().empty())) {
                descr += " (";
                if (!def.sidetext.empty()) {
                    descr += def.sidetext + ", ";
                } else if (def.enum_def && def.enum_def->has_values()) {
                    descr += boost::algorithm::join(def.enum_def->values(), ", ") + "; ";
                }
                descr += "default: " + def.default_value->serialize() + ")";
            }
            
            // wrap lines of description
            descr = wrap(descr, 80);
            std::vector<std::string> lines;
            boost::split(lines, descr, boost::is_any_of("\n"));
            
            // if command line options are too long, print description in new line
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i == 0 && cli.size() > 19)
                    out << std::endl;
                if (i > 0 || cli.size() > 19)
                    out << std::string(21, ' ');
                out << lines[i] << std::endl;
            }
        }
    }
    return out;
}

// Look up a closed enum value of this combo box based on an index of the combo box value / label.
// Such a mapping should always succeed.
int ConfigOptionEnumDef::index_to_enum(int index) const
{
    // It has to be a closed enum, thus values have to be defined.
    assert(this->is_valid_closed_enum());
    assert(index >= 0 && index < int(m_values.size()));
    if (m_values_ordinary)
        return index;
    else {
        auto it = m_enum_keys_map->find(m_values[index]);
        assert(it != m_enum_keys_map->end());
        return it->second;
    }
}

// Look up an index of value / label of this combo box based on enum value.
// Such a mapping may fail, thus an optional is returned.
std::optional<int> ConfigOptionEnumDef::enum_to_index(int enum_val) const
{
    assert(this->is_valid_closed_enum());
    assert(enum_val >= 0 && enum_val < int(m_enum_names->size()));
    if (m_values_ordinary)
        return {enum_val};
    else {
        auto it = std::find(m_values.begin(), m_values.end(), (*m_enum_names)[enum_val]);
        return it == m_values.end() ? std::optional<int>{} : std::optional<int>{int(it - m_values.begin())};
    }
}

// Look up an index of value / label of this combo box based on value string.
std::optional<int> ConfigOptionEnumDef::value_to_index(const std::string &value) const
{
    assert(this->is_valid_open_enum() || this->is_valid_closed_enum());
    auto it = std::find(m_values.begin(), m_values.end(), value);
    return it == m_values.end() ? std::optional<int>{} : std::optional<int>{it - m_values.begin()};
}

// Look up an index of label of this combo box. Used for open enums.
std::optional<int> ConfigOptionEnumDef::label_to_index(const std::string &value) const
{
    assert(is_valid_open_enum());
    const auto &ls = this->labels();
    auto        it = std::find(ls.begin(), ls.end(), value);
    return it == ls.end() ? std::optional<int>{} : std::optional<int>{it - ls.begin()};
}

std::optional<std::reference_wrapper<const std::string>> ConfigOptionEnumDef::enum_to_value(int enum_val) const
{
    assert(this->is_valid_closed_enum());
    auto opt = this->enum_to_index(enum_val);
    return opt.has_value() ? std::optional<std::reference_wrapper<const std::string>>{this->value(*opt)} :
                             std::optional<std::reference_wrapper<const std::string>>{};
}

std::optional<std::reference_wrapper<const std::string>> ConfigOptionEnumDef::enum_to_label(int enum_val) const
{
    assert(this->is_valid_closed_enum());
    auto opt = this->enum_to_index(enum_val);
    return opt.has_value() ? std::optional<std::reference_wrapper<const std::string>>{this->label(*opt)} :
                             std::optional<std::reference_wrapper<const std::string>>{};
}

bool ConfigOptionEnumDef::is_valid_closed_enum() const
{
    return m_enum_names != nullptr && m_enum_keys_map != nullptr && !m_values.empty() &&
           (m_labels.empty() || m_values.size() == m_labels.size());
}
#ifndef NDEBUG
bool ConfigOptionEnumDef::is_valid_open_enum() const
{
    return m_enum_names == nullptr && m_enum_keys_map == nullptr && (!m_values.empty() || !m_labels.empty()) &&
           (m_values.empty() || m_labels.empty() || m_values.size() == m_labels.size());
}
#endif // NDEBUG

void ConfigOptionEnumDef::clear()
{
    m_values_ordinary = false;
    m_enum_names      = nullptr;
    m_enum_keys_map   = nullptr;
    m_values.clear();
    m_labels.clear();
    m_enum_keys_map_storage_for_script.reset();
}

void ConfigOptionEnumDef::set_values(const std::vector<std::string> &v)
{
    m_values = v;
    assert(m_labels.empty() || m_labels.size() == m_values.size());
}
void ConfigOptionEnumDef::set_values(const std::initializer_list<std::string_view> il)
{
    m_values.clear();
    m_values.reserve(il.size());
    for (const std::string_view &p : il)
        m_values.emplace_back(p);
    assert(m_labels.empty() || m_labels.size() == m_values.size());
}
void ConfigOptionEnumDef::set_values(const std::initializer_list<std::pair<std::string_view, std::string_view>> il)
{
    m_values.clear();
    m_values.reserve(il.size());
    m_labels.clear();
    m_labels.reserve(il.size());
    for (const std::pair<std::string_view, std::string_view> &p : il) {
        m_values.emplace_back(p.first);
        m_labels.emplace_back(p.second);
    }
}
void ConfigOptionEnumDef::set_values(const std::vector<std::pair<std::string, std::string>> il)
{
    m_values.clear();
    m_values.reserve(il.size());
    m_labels.clear();
    m_labels.reserve(il.size());
    for (const std::pair<std::string, std::string> &p : il) {
        m_values.emplace_back(p.first);
        m_labels.emplace_back(p.second);
    }
}
void ConfigOptionEnumDef::set_labels(const std::initializer_list<std::string_view> il)
{
    m_labels.clear();
    m_labels.reserve(il.size());
    for (const std::string_view &p : il)
        m_labels.emplace_back(p);
    assert(m_values.empty() || m_labels.size() == m_values.size());
}
void ConfigOptionEnumDef::finalize_closed_enum()
{
    assert(this->is_valid_closed_enum());
    // Check whether def.enum_values contains all the values of def.enum_keys_map and
    // that they are sorted by their ordinary values.
    m_values_ordinary = true;
    for (const auto &[enum_name, enum_int] : *m_enum_keys_map) {
        assert(enum_int >= 0);
        if (enum_int >= int(this->values().size()) || this->value(enum_int) != enum_name) {
            m_values_ordinary = false;
            break;
        }
    }
}

void ConfigOptionDef::set_enum_values(const std::vector<std::string> il)
{
    this->enum_def_new();
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(const std::initializer_list<std::string_view> il)
{
    this->enum_def_new();
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(const std::initializer_list<std::pair<std::string_view, std::string_view>> il)
{
    this->enum_def_new();
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(const std::vector<std::pair<std::string, std::string>> il)
{
    this->enum_def_new();
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(GUIType gui_type, const std::initializer_list<std::string_view> il)
{
    this->enum_def_new();
    assert(is_gui_type_enum_open(gui_type));
    this->gui_type = gui_type;
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_as_closed_for_scripted_enum(const std::vector<std::pair<std::string, std::string>> il)
{
    set_enum_values(il);
    gui_type                                     = GUIType::undefined; // closed enum
    enum_def->m_enum_names                       = &enum_def->m_values;
    enum_def->m_enum_keys_map_storage_for_script = std::make_unique<t_config_enum_values>();
    enum_def->m_enum_keys_map                    = enum_def->m_enum_keys_map_storage_for_script.get();
    for (size_t i = 0; i < enum_def->m_values.size(); i++) {
        (*enum_def->m_enum_keys_map_storage_for_script)[enum_def->m_values[i]] = i;
    }
    enum_def->finalize_closed_enum();
    assert(enum_def->m_values_ordinary);
}

void ConfigOptionDef::set_enum_values(GUIType gui_type, const std::initializer_list<std::pair<std::string_view, std::string_view>> il)
{
    this->enum_def_new();
    assert(gui_type == GUIType::i_enum_open || gui_type == GUIType::f_enum_open);
    this->gui_type = gui_type;
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(GUIType gui_type, const std::vector<std::pair<std::string, std::string>> il)
{
    this->enum_def_new();
    assert(gui_type == GUIType::i_enum_open || gui_type == GUIType::f_enum_open || gui_type == GUIType::select_close);
    this->gui_type = gui_type;
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_values(GUIType gui_type, const std::vector<std::string> il)
{
    this->enum_def_new();
    assert(gui_type == GUIType::select_open || gui_type == GUIType::color || gui_type == ConfigOptionDef::GUIType::select_close);
    this->gui_type = gui_type;
    enum_def->set_values(il);
}

void ConfigOptionDef::set_enum_labels(GUIType gui_type, const std::initializer_list<std::string_view> il)
{
    this->enum_def_new();
    assert(gui_type == GUIType::i_enum_open || gui_type == GUIType::f_enum_open || gui_type == ConfigOptionDef::GUIType::select_close);
    this->gui_type = gui_type;
    enum_def->set_labels(il);
}

bool ConfigOptionDef::has_enum_value(const std::string &value) const {
    if (!value.empty() && value.front() == '!')
        return has_enum_value(value.substr(1));
    return enum_def && enum_def->value_to_index(value).has_value();
}

std::string ConfigBase::SetDeserializeItem::format(std::initializer_list<int> values)
{
    std::string out;
    int i = 0;
    for (int v : values) {
        if (i ++ > 0)
            out += ", ";
        out += std::to_string(v);
    }
    return out;
}

std::string ConfigBase::SetDeserializeItem::format(std::initializer_list<float> values)
{
    std::string out;
    int i = 0;
    for (float v : values) {
        if (i ++ > 0)
            out += ", ";
        out += float_to_string_decimal_point(double(v));
    }
    return out;
}

std::string ConfigBase::SetDeserializeItem::format(std::initializer_list<double> values)
{
    std::string out;
    int i = 0;
    for (float v : values) {
        if (i ++ > 0)
            out += ", ";
        out += float_to_string_decimal_point(v);
    }
    return out;
}

void ConfigBase::apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent)
{
    // loop through options and apply them
    for (const t_config_option_key &opt_key : keys) {
        // Create a new option with default value for the key.
        // If the key is not in the parameter definition, or this ConfigBase is a static type and it does not support the parameter,
        // an exception is thrown if not ignore_nonexistent.
        ConfigOption *my_opt = this->option(opt_key, true);
        // If we didn't find an option, look for any other option having this as an alias.
        if (my_opt == nullptr) {
            const ConfigDef       *def = this->def();
            for (const auto &opt : def->options) {
                for (const t_config_option_key &opt_key2 : opt.second.aliases) {
                    if (opt_key2 == opt_key) {
                        my_opt = this->option(opt.first, true);
                        break;
                    }
                }
                if (my_opt != nullptr)
                    break;
            }
        }
        if (my_opt == nullptr) {
            // opt_key does not exist in this ConfigBase and it cannot be created, because it is not defined by this->def().
            // This is only possible if other is of DynamicConfig type.
            if (ignore_nonexistent)
                continue;
            throw UnknownOptionException(opt_key);
        }
		const ConfigOption *other_opt = other.option(opt_key);
		if (other_opt == nullptr) {
            // The key was not found in the source config, therefore it will not be initialized!
//			printf("Not found, therefore not initialized: %s\n", opt_key.c_str());
        } else {
            try {
                my_opt->set(other_opt);
            } catch (ConfigurationException& e) {
                throw ConfigurationException(std::string(e.what()) + ", when ConfigBase::apply_only on " + opt_key);
            }
        }
    }
}

// Are the two configs equal? Ignoring options not present in both configs.
bool ConfigBase::equals(const ConfigBase &other) const
{ 
    if(this->keys().size() != other.keys().size())
        return false;
    for (const t_config_option_key &opt_key : this->keys()) {
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt != *other_opt)
            return false;
    }
    return true;
}

// Returns options differing in the two configs, ignoring options not present in both configs.
t_config_option_keys ConfigBase::diff(const ConfigBase &other, bool even_phony /*=true*/) const
{
    t_config_option_keys diff;
    for (const t_config_option_key &opt_key : this->keys()) {
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        //dirty if both exist, they aren't both phony and value is different
        if (this_opt != nullptr && other_opt != nullptr 
            && (even_phony || !(this_opt->is_phony() && other_opt->is_phony()))
            && ((*this_opt != *other_opt) || (this_opt->is_phony() != other_opt->is_phony())))
            diff.emplace_back(opt_key);
    }
    return diff;
}

// Returns options being equal in the two configs, ignoring options not present in both configs.
t_config_option_keys ConfigBase::equal(const ConfigBase &other) const
{
    t_config_option_keys equal;
    for (const t_config_option_key &opt_key : this->keys()) {
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt == *other_opt)
            equal.emplace_back(opt_key);
    }
    return equal;
}

std::string ConfigBase::opt_serialize(const t_config_option_key &opt_key) const
{
    const ConfigOption* opt = this->option(opt_key);
    assert(opt != nullptr);
    if (opt->is_phony())
        return "";
    return opt->serialize();
}

void ConfigBase::set(const std::string &opt_key, int32_t value, bool create)
{
    ConfigOption *opt = this->option_throw(opt_key, create);
    switch (opt->type()) {
    	case coInt:    static_cast<ConfigOptionInt*>(opt)->value = value; break;
    	case coFloat:  static_cast<ConfigOptionFloat*>(opt)->value = value; break;
		case coFloatOrPercent:  static_cast<ConfigOptionFloatOrPercent*>(opt)->value = value; static_cast<ConfigOptionFloatOrPercent*>(opt)->percent = false; break;
		case coString: static_cast<ConfigOptionString*>(opt)->value = std::to_string(value); break;
    	default: throw BadOptionTypeException("Configbase::set() - conversion from int not possible");
    }
}

void ConfigBase::set(const std::string &opt_key, double value, bool create)
{
    ConfigOption *opt = this->option_throw(opt_key, create);
    switch (opt->type()) {
    	case coFloat:  			static_cast<ConfigOptionFloat*>(opt)->value = value; break;
    	case coFloatOrPercent:  static_cast<ConfigOptionFloatOrPercent*>(opt)->value = value; static_cast<ConfigOptionFloatOrPercent*>(opt)->percent = false; break;
        case coString: 			static_cast<ConfigOptionString*>(opt)->value = float_to_string_decimal_point(value); break;
    	default: throw BadOptionTypeException("Configbase::set() - conversion from float not possible");
    }
}

bool ConfigBase::set_deserialize_nothrow(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
    t_config_option_key opt_key = opt_key_src;
    std::string         value   = value_src;
    //note: should be done BEFORE calling set_deserialize
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    this->handle_legacy(opt_key, value);
    if (opt_key.empty()) {
        assert(false);
        // Ignore the option.
        return true;
    }
    assert(opt_key == opt_key_src);
    assert(value == value_src);
    return this->set_deserialize_raw(opt_key, value, substitutions_ctxt, append);
}

void ConfigBase::set_deserialize(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
    if (!this->set_deserialize_nothrow(opt_key_src, value_src, substitutions_ctxt, append)) {
        if (substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Disable) {
            throw BadOptionValueException(format("Invalid value provided for parameter %1%: %2%", opt_key_src, value_src));
        } else if (substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Enable) {
            const ConfigDef* def = this->def();
            if (def == nullptr)
                throw UnknownOptionException(opt_key_src);
            const ConfigOptionDef* optdef = def->get(opt_key_src);
            t_config_option_key opt_key = opt_key_src;
            if (optdef == nullptr) {
                // If we didn't find an option, look for any other option having this as an alias.
                for (const auto& opt : def->options) {
                    for (const t_config_option_key& opt_key2 : opt.second.aliases) {
                        if (opt_key2 == opt_key_src) {
                            opt_key = opt.first;
                            optdef = &opt.second;
                            break;
                        }
                    }
                    if (optdef != nullptr)
                        break;
                }
                if (optdef == nullptr)
                    throw UnknownOptionException(opt_key_src);
            }
            substitutions_ctxt.add(ConfigSubstitution{ optdef, value_src, ConfigOptionUniquePtr(optdef->default_value->clone()) });
        }
    }
}

void ConfigBase::set_deserialize(std::initializer_list<SetDeserializeItem> items, ConfigSubstitutionContext& substitutions_ctxt)
{
	for (const SetDeserializeItem &item : items)
		this->set_deserialize(item.opt_key, item.opt_value, substitutions_ctxt, item.append);
}

bool ConfigBase::set_deserialize_raw(const t_config_option_key &opt_key_src, const std::string &value, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
    t_config_option_key    opt_key = opt_key_src;
    // Try to deserialize the option by its name.
    const ConfigDef       *def     = this->def();
    if (def == nullptr)
        throw NoDefinitionException(opt_key);
    const ConfigOptionDef *optdef  = def->get(opt_key);
    if (optdef == nullptr) {
        // If we didn't find an option, look for any other option having this as an alias.
        for (const auto &opt : def->options) {
            for (const t_config_option_key &opt_key2 : opt.second.aliases) {
                if (opt_key2 == opt_key) {
                    opt_key = opt.first;
                    optdef = &opt.second;
                    break;
                }
            }
            if (optdef != nullptr)
                break;
        }
        if (optdef == nullptr)
            throw UnknownOptionException(opt_key);
    }
    
    if (! optdef->shortcut.empty()) {
        // Aliasing for example "solid_layers" to "top_solid_layers" and "bottom_solid_layers".
        for (const t_config_option_key &shortcut : optdef->shortcut)
            // Recursive call.
            if (! this->set_deserialize_raw(shortcut, value, substitutions_ctxt, append))
                return false;
        return true;
    }
    
    ConfigOption *opt = this->option(opt_key, true);
    if (opt == nullptr)
        throw new UnknownOptionException(opt_key);
    bool success;
    if (optdef->can_phony && value.empty()) {
        success = true;
    } else {
        bool substituted = false;
        if (optdef->type == coBools && substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable) {
            //FIXME Special handling of vectors of bools, quick and not so dirty solution before PrusaSlicer 2.3.2 release.
            bool nullable = opt->nullable();
            ConfigHelpers::DeserializationSubstitution default_value = ConfigHelpers::DeserializationSubstitution::DefaultsToFalse;
            if (optdef->default_value) {
                // Default value for vectors of booleans used in a "per extruder" context, thus the default contains just a single value.
                assert(dynamic_cast<const ConfigOptionVector<unsigned char>*>(optdef->default_value.get()));
                const auto &values = static_cast<const ConfigOptionVector<unsigned char>*>(optdef->default_value.get())->get_values();
                if (values.size() == 1 && values.front() == 1)
                    default_value = ConfigHelpers::DeserializationSubstitution::DefaultsToTrue;
            }
            auto result = nullable ?
                static_cast<ConfigOptionBoolsNullable*>(opt)->deserialize_with_substitutions(value, append, default_value) :
                static_cast<ConfigOptionBools*>(opt)->deserialize_with_substitutions(value, append, default_value);
            success     = result != ConfigHelpers::DeserializationResult::Failed;
            substituted = result == ConfigHelpers::DeserializationResult::Substituted;
        } else {
            success = opt->deserialize(value, append);
            if (!success && substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                //special check for booleans with abnormal string.
                if ((optdef->type == coEnum || optdef->type == coBool) && ConfigHelpers::enum_looks_like_bool_value(value)) {
                    // Deserialize failed, try to substitute with a default value.
                    //assert(substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Enable || substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::EnableSilent);
                    if (optdef->type == coBool) {
                        static_cast<ConfigOptionBool*>(opt)->value = ConfigHelpers::enum_looks_like_true_value(value);
                    } else {
                        // Just use the default of the option.
                        opt->set(optdef->default_value.get());
                    }
                } else {
                    // Deserialize failed, substitute with a default value.
                    opt->set(optdef->default_value.get());
                }
                success = true;
                substituted = true;
            }
        }

        if (substituted && (substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Enable ||
                            substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)) {
            substitutions_ctxt.emplace(optdef, std::string(value), ConfigOptionUniquePtr(opt->clone()));
        }
    }
    //set phony status
    if (optdef->can_phony)
        if(value.empty())
            opt->set_phony(true);
        else
            opt->set_phony(false);
    else
        opt->set_phony(false);

    if (optdef->is_vector_extruder)
        static_cast<ConfigOptionVectorBase*>(opt)->set_is_extruder_size(true);
    return success;
}

const ConfigOptionDef* ConfigBase::get_option_def(const t_config_option_key& opt_key) const {
    // Get option definition.
    const ConfigDef* def = this->def();
    if (def == nullptr)
        throw NoDefinitionException(opt_key);
    const ConfigOptionDef* opt_def = def->get(opt_key);
    if(opt_def == nullptr && parent != nullptr)
        opt_def = parent->get_option_def(opt_key);
    return opt_def;
}

// Return an absolute value of a possibly relative config variable.
// For example, return absolute infill extrusion width, either from an absolute value, or relative to the layer height.
double ConfigBase::get_computed_value(const t_config_option_key &opt_key, int extruder_id) const
{
    // Get stored option value.
    const ConfigOption *raw_opt = this->option(opt_key);
    if (raw_opt == nullptr) {
        std::stringstream ss; ss << "You can't define an option that need " << opt_key << " without defining it!";
        throw std::runtime_error(ss.str());
    }

    if (!raw_opt->is_vector()) {
        if (raw_opt->type() == coFloat)
            return static_cast<const ConfigOptionFloat*>(raw_opt)->value;
        if (raw_opt->type() == coInt)
            return static_cast<const ConfigOptionInt*>(raw_opt)->value;
        if (raw_opt->type() == coBool)
            return static_cast<const ConfigOptionBool*>(raw_opt)->value ? 1 : 0;
        const ConfigOptionPercent* cast_opt = nullptr;
        if (raw_opt->type() == coFloatOrPercent) {
            auto cofop = static_cast<const ConfigOptionFloatOrPercent*>(raw_opt);
            if (cofop->value == 0 && boost::ends_with(opt_key, "_extrusion_width")) {
                 return Flow::extrusion_width(opt_key, *this, extruder_id);
            }
            if (!cofop->percent)
                return cofop->value;
            cast_opt = cofop;
        }
        if (raw_opt->type() == coPercent) {
            cast_opt = static_cast<const ConfigOptionPercent*>(raw_opt);
        }
        const ConfigOptionDef* opt_def = this->get_option_def(opt_key);
        if (opt_def == nullptr) // maybe a placeholder?
            return cast_opt->get_abs_value(1);
        //if over no other key, it's most probably a simple %
        if (opt_def->ratio_over == "")
            return cast_opt->get_abs_value(1);
        // Compute absolute value over the absolute value of the base option.
        //FIXME there are some ratio_over chains, which end with empty ratio_with.
        // For example, XXX_extrusion_width parameters are not handled by get_abs_value correctly.
        if (!opt_def->ratio_over.empty() && opt_def->ratio_over != "depends")
            return cast_opt->get_abs_value(this->get_computed_value(opt_def->ratio_over, extruder_id));

        std::stringstream ss; ss << "ConfigBase::get_abs_value(): " << opt_key << " has no valid ratio_over to compute of";
        throw ConfigurationError(ss.str());
    } else {
        // check if it's an extruder_id array
        const ConfigOptionVectorBase* vector_opt = static_cast<const ConfigOptionVectorBase*>(raw_opt);
        int idx = -1;
        if (vector_opt->is_extruder_size()) {
            idx = extruder_id;
            if (extruder_id < 0) {
                const ConfigOption* opt_extruder_id = nullptr;
                if ((opt_extruder_id = this->option("extruder")) == nullptr)
                    if ((opt_extruder_id = this->option("current_extruder")) == nullptr
                        || opt_extruder_id->get_int() < 0 || opt_extruder_id->get_int() >= vector_opt->size()) {
                        std::stringstream ss; ss << "ConfigBase::get_abs_value(): " << opt_key << " need to has the extuder id to get the right value, but it's not available";
                        throw ConfigurationError(ss.str());
                    }
                extruder_id = opt_extruder_id->get_int();
                idx = extruder_id;
            }
        } else {
            t_config_option_keys machine_limits = Preset::machine_limits_options();
            if (std::find(machine_limits.begin(), machine_limits.end(), opt_key) != machine_limits.end()) {
                idx = 0;
            }
        }
        if (idx >= 0) {
            if (raw_opt->type() == coFloats || raw_opt->type() == coInts || raw_opt->type() == coBools)
                return vector_opt->get_float(idx);
            if (raw_opt->type() == coFloatsOrPercents) {
                const ConfigOptionFloatsOrPercents* opt_fl_per = static_cast<const ConfigOptionFloatsOrPercents*>(raw_opt);
                if (!opt_fl_per->get_at(idx).percent)
                    return opt_fl_per->get_at(idx).value;

                const ConfigOptionDef* opt_def = this->get_option_def(opt_key);
                if (opt_def == nullptr) // maybe a placeholder?
                    return opt_fl_per->get_abs_value(extruder_id, 1);
                if (opt_def->ratio_over.empty())
                    return opt_fl_per->get_abs_value(idx, 1);
                if (opt_def->ratio_over != "depends")
                    return opt_fl_per->get_abs_value(idx, this->get_computed_value(opt_def->ratio_over, idx));
                std::stringstream ss; ss << "ConfigBase::get_abs_value(): " << opt_key << " has no valid ratio_over to compute of";
                throw ConfigurationError(ss.str());
            }
            if (raw_opt->type() == coPercents) {
                const ConfigOptionPercents* opt_per = static_cast<const ConfigOptionPercents*>(raw_opt);
                const ConfigOptionDef* opt_def = this->get_option_def(opt_key);
                if (opt_def == nullptr) // maybe a placeholder?
                    return opt_per->get_abs_value(extruder_id, 1);
                if (opt_def->ratio_over.empty())
                    return opt_per->get_abs_value(idx, 1);
                if (opt_def->ratio_over != "depends")
                    return opt_per->get_abs_value(idx, this->get_computed_value(opt_def->ratio_over, idx));
                std::stringstream ss; ss << "ConfigBase::get_abs_value(): " << opt_key << " has no valid ratio_over to compute of";
                throw ConfigurationError(ss.str());
            }
        } 
    }
    std::stringstream ss; ss << "ConfigBase::get_abs_value(): "<< opt_key<<" has not a valid option type for get_abs_value()";
    throw ConfigurationError(ss.str());
}

// Return an absolute value of a possibly relative config variable.
// For example, return absolute infill extrusion width, either from an absolute value, or relative to a provided value.
double ConfigBase::get_abs_value(const t_config_option_key &opt_key, double ratio_over) const 
{
    // Get stored option value.
    const ConfigOption *raw_opt = this->option(opt_key);
    assert(raw_opt != nullptr);
    if (raw_opt->type() != coFloatOrPercent) {
        if(raw_opt->type() != coPercent)
            throw ConfigurationError("ConfigBase::get_abs_value(): opt_key is not of coFloatOrPercent");
        return static_cast<const ConfigOptionPercent*>(raw_opt)->get_abs_value(ratio_over);
    }
    // Compute absolute value.
    return static_cast<const ConfigOptionFloatOrPercent*>(raw_opt)->get_abs_value(ratio_over);
}

void ConfigBase::setenv_() const
{
    t_config_option_keys opt_keys = this->keys();
    for (t_config_option_keys::const_iterator it = opt_keys.begin(); it != opt_keys.end(); ++it) {
        // prepend the SLIC3R_ prefix
        std::ostringstream ss;
        ss << "SLIC3R_";
        ss << *it;
        std::string envname = ss.str();
        
        // capitalize environment variable name
        for (size_t i = 0; i < envname.size(); ++i)
            envname[i] = (envname[i] <= 'z' && envname[i] >= 'a') ? envname[i]-('a'-'A') : envname[i];
        
        boost::nowide::setenv(envname.c_str(), this->opt_serialize(*it).c_str(), 1);
    }
}

ConfigSubstitutions ConfigBase::load(const std::string& filename, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    enum class EFileType
    {
        Ini,
        AsciiGCode,
        BinaryGCode
    };

    EFileType file_type;

    if (is_gcode_file(filename)) {
        FILE* file = boost::nowide::fopen(filename.c_str(), "rb");
        if (file == nullptr)
            throw Slic3r::RuntimeError(format("Error opening file %1%", filename));

        std::vector<std::byte> cs_buffer(65536);
        using namespace bgcode::core;
        file_type = (is_valid_binary_gcode(*file, true, cs_buffer.data(), cs_buffer.size()) == EResult::Success) ? EFileType::BinaryGCode : EFileType::AsciiGCode;
        fclose(file);
    }
    else 
        file_type = EFileType::Ini;

    switch (file_type)
    {
    case EFileType::Ini:         { return this->load_from_ini(filename, compatibility_rule); }
    case EFileType::AsciiGCode:  { return this->load_from_gcode_file(filename, compatibility_rule);}
    case EFileType::BinaryGCode: { return this->load_from_binary_gcode_file(filename, compatibility_rule);}
    default:                     { throw Slic3r::RuntimeError(format("Invalid file %1%", filename)); }
    }
}

ConfigSubstitutions ConfigBase::load_from_ini(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    try {
        boost::property_tree::ptree tree;
        boost::nowide::ifstream ifs(file);
        boost::property_tree::read_ini(ifs, tree);
        return this->load(tree, compatibility_rule);
    } catch (const ConfigurationError &e) {
        throw ConfigurationError(format("Failed loading configuration file \"%1%\": %2%", file, e.what()));
    }
}

ConfigSubstitutions ConfigBase::load_from_ini_string(const std::string &data, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    boost::property_tree::ptree tree;
    std::istringstream iss(data);
    boost::property_tree::read_ini(iss, tree);
    return this->load(tree, compatibility_rule);
}

// Loading a "will be one day a legacy format" of configuration stored into 3MF or AMF.
// Accepts the same data as load_from_ini_string(), only with each configuration line possibly prefixed with a semicolon (G-code comment).
ConfigSubstitutions ConfigBase::load_from_ini_string_commented(std::string &&data, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // Convert the "data" string into INI format by removing the semi-colons at the start of a line.
    // Also the "; generated by PrusaSlicer ..." comment line will be removed.
    size_t j = 0;
    for (size_t i = 0; i < data.size();)
        if (i == 0 || data[i] == '\n') {
            // Start of a line.
            if (data[i] == '\n') {
                // Consume LF, don't keep empty lines.
                if (j > 0 && data[j - 1] != '\n')
                    data[j ++] = data[i];
                ++ i;
            }
            // Skip all leading spaces;
            for (; i < data.size() && (data[i] == ' ' || data[i] == '\t'); ++ i) ;
            // Skip the semicolon (comment indicator).
            if (i < data.size() && data[i] == ';')
                ++ i;
            // Skip all leading spaces after semicolon.
            for (; i < data.size() && (data[i] == ' ' || data[i] == '\t'); ++ i) ;
            if (strncmp(data.data() + i, "generated by ", 13) == 0) {
                // Skip the "; generated by ..." line.
                for (; i < data.size() && data[i] != '\n'; ++ i);
            }
        } else if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') {
            // Skip CR.
            ++ i;
        } else {
            // Consume the rest of the data.
            data[j ++] = data[i ++];
        }
    data.erase(data.begin() + j, data.end());

    return this->load_from_ini_string(data, compatibility_rule);
}

ConfigSubstitutions ConfigBase::load(const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    std::vector<std::pair<t_config_option_key, std::string>> opt_deleted;
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    for (const boost::property_tree::ptree::value_type &v : tree) {
        t_config_option_key opt_key = v.first;
        try {
            std::string value = v.second.get_value<std::string>();
            t_config_option_key saved_key = opt_key;
            PrintConfigDef::handle_legacy(opt_key, value, false);
            if (!opt_key.empty()) {
                if (!PrintConfigDef::is_defined(opt_key)) {
                    if (substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                        substitutions_ctxt.add(ConfigSubstitution(v.first, value));
                    }
                } else {
                    this->set_deserialize(opt_key, value, substitutions_ctxt);
                }
            } else {
                opt_deleted.emplace_back(saved_key, value);
            }
        } catch (UnknownOptionException & /* e */) {
            // ignore
            assert(false);
        } catch (BadOptionValueException & e) {
            if (compatibility_rule == ForwardCompatibilitySubstitutionRule::Disable)
                throw e;
            // log the error
            const ConfigDef* def = this->def();
            if (def == nullptr) throw e;
            const ConfigOptionDef* optdef = def->get(opt_key);
            substitutions_ctxt.emplace(optdef, v.second.get_value<std::string>(), ConfigOptionUniquePtr(optdef->default_value->clone()));
        }
    }
    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    this->handle_legacy_composite(opt_deleted);
    return std::move(substitutions_ctxt).data();
}

// Load the config keys from the given string.
std::map<t_config_option_key, std::string> ConfigBase::load_gcode_string_legacy(const char* str)
{
    std::map<t_config_option_key, std::string> opt_key_values;
    if (str == nullptr)
        return opt_key_values;

    // Walk line by line in reverse until a non-configuration key appears.
    const char *data_start = str;
    // boost::nowide::ifstream seems to cook the text data somehow, so less then the 64k of characters may be retrieved.
    const char *end = data_start + strlen(str);
    for (;;) {
        // Extract next line.
        for (--end; end > data_start && (*end == '\r' || *end == '\n'); --end)
            ;
        if (end == data_start)
            break;
        const char *start = end ++;
        for (; start > data_start && *start != '\r' && *start != '\n'; --start)
            ;
        if (start == data_start)
            break;
        // Extracted a line from start to end. Extract the key = value pair.
        if (end - (++ start) < 10 || start[0] != ';' || start[1] != ' ')
            break;
        const char *key = start + 2;
        if (!((*key >= 'a' && *key <= 'z') || (*key >= 'A' && *key <= 'Z')))
            // A key must start with a letter.
            break;
        const char *sep = key;
        for (; sep != end && *sep != '='; ++sep)
            ;
        if (sep == end || sep[-1] != ' ' || sep[1] != ' ')
            break;
        const char *value = sep + 2;
        if (value > end)
            break;
        const char *key_end = sep - 1;
        if (key_end - key < 3)
            break;
        // The key may contain letters, digits and underscores.
        for (const char *c = key; c != key_end; ++ c)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) {
                key = nullptr;
                break;
            }
        if (key == nullptr)
            break;
        opt_key_values.emplace(std::string(key, key_end), std::string(value, end));
        end = start;
    }
    return opt_key_values;
}

    
size_t ConfigBase::load_from_gcode_string_legacy(ConfigBase& config, const char* str, ConfigSubstitutionContext& substitutions)
{
    if (str == nullptr)
        return 0;
    
    std::vector<std::pair<t_config_option_key, std::string>> opt_deleted;
    // Walk line by line in reverse until a non-configuration key appears.
    const char *data_start = str;
    // boost::nowide::ifstream seems to cook the text data somehow, so less then the 64k of characters may be retrieved.
    const char *end = data_start + strlen(str);
    size_t num_key_value_pairs = 0;
    for (auto [key, value] : load_gcode_string_legacy(str)) {
        try {
            std::string opt_key = key;
            t_config_option_key saved_key = opt_key;
            PrintConfigDef::handle_legacy(opt_key, value, false);
            if (!opt_key.empty()) {
                if (!PrintConfigDef::is_defined(opt_key)) {
                    if (substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                        substitutions.add(ConfigSubstitution(key, value));
                    }
                } else {
                    config.set_deserialize(opt_key, value, substitutions);
                    ++num_key_value_pairs;
                }
            } else {
                opt_deleted.emplace_back(saved_key, value);
            }
        }
        catch (UnknownOptionException & /* e */) {
            // log & ignore
            if (substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable)
                substitutions.add(ConfigSubstitution(key, value));
        } catch (BadOptionValueException & e) {
            if (substitutions.rule == ForwardCompatibilitySubstitutionRule::Disable)
                throw e;
            // log the error
            const ConfigDef* def = config.def();
            if (def == nullptr) throw e;
            const ConfigOptionDef* optdef = def->get(key);
            substitutions.emplace(optdef, std::move(value), ConfigOptionUniquePtr(optdef->default_value->clone()));
        }
    }

    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    config.handle_legacy_composite(opt_deleted);

    return num_key_value_pairs;
}

// Reading a config from G-code back to front for performance reasons: We don't want to scan
// hundreds of MB file for a short config block, which we expect to find at the end of the G-code.
class ReverseLineReader
{
public:
    using pos_type = boost::nowide::ifstream::pos_type;

    // Stop at file_start
    ReverseLineReader(boost::nowide::ifstream &ifs, pos_type file_start) : m_ifs(ifs), m_file_start(file_start)
    {
        m_ifs.seekg(0, m_ifs.end);
        m_file_pos = m_ifs.tellg();
        m_block.assign(m_block_size, 0);
    }

    bool getline(std::string &out) {
        out.clear();
        for (;;) {
            if (m_block_len == 0) {
                // Read the next block.
                m_block_len = size_t(std::min<std::fstream::pos_type>(m_block_size, m_file_pos - m_file_start));
                if (m_block_len == 0)
                    return false;
                m_file_pos -= m_block_len;
                m_ifs.seekg(m_file_pos, m_ifs.beg);
                if (! m_ifs.read(m_block.data(), m_block_len))
                    return false;
                assert(m_block_len == m_ifs.gcount());
            }

            assert(m_block_len > 0);
            // Non-empty buffer. Find another LF.
            int i = int(m_block_len) - 1;
            for (; i >= 0; -- i)
                if (m_block[i] == '\n')
                    break;
            // i is position of LF or -1 if not found.
            if (i == -1) {
                // LF not found. Just make a backup of the buffer and continue.
                out.insert(out.begin(), m_block.begin(), m_block.begin() + m_block_len);
                m_block_len = 0;
            } else {
                assert(i >= 0);
                // Copy new line to the output. It may be empty.
                out.insert(out.begin(), m_block.begin() + i + 1, m_block.begin() + m_block_len);
                // Block length without the newline.
                m_block_len = i;
                // Remove CRLF from the end of the block.
                if (m_block_len > 0 && m_block[m_block_len - 1] == '\r')
                    -- m_block_len;
                return true;
            }
        }
        assert(false);
        return false;
    }

private:
    boost::nowide::ifstream &m_ifs;
    std::vector<char>        m_block;
    size_t                   m_block_size = 65536;
    size_t                   m_block_len  = 0;
    pos_type                 m_file_start;
    pos_type                 m_file_pos   = 0;
};

// Load the config keys from the tail of a G-code file.
ConfigSubstitutions ConfigBase::load_from_gcode_file(const std::string &filename, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // Read a 64k block from the end of the G-code.
    boost::nowide::ifstream ifs(filename, std::ifstream::binary);
    // Look for Slic3r-like header.
    // Look for the header across the whole file as the G-code may have been extended at the start by a post-processing script or the user.
    bool has_delimiters = false;
    {
        //allow variant with wharacter after the base name (like Slic3rPE or Slic3r++ or SuperSlicer-Vertex)
        static constexpr const char slic3r_gcode_header[] = "; generated by Slic3r";
        static constexpr const char superslicer_gcode_header[] = "; generated by SuperSlicer";
        static constexpr const char prusaslicer_gcode_header[] = "; generated by PrusaSlicer";
        static constexpr const char this_gcode_header[] = "; generated by " SLIC3R_APP_KEY;
        std::string header;
        bool        header_found = false;
        while (std::getline(ifs, header)) {
            if (strncmp(prusaslicer_gcode_header, header.c_str(), strlen(prusaslicer_gcode_header)) == 0
                    || strncmp(slic3r_gcode_header, header.c_str(), strlen(slic3r_gcode_header)) == 0
                    || strncmp(superslicer_gcode_header, header.c_str(), strlen(superslicer_gcode_header)) == 0
                    || strncmp(this_gcode_header, header.c_str(), strlen(this_gcode_header)) == 0) {
                // Parse slic3r version.
                size_t i = strlen("; generated by Sl");
                //go to end of the key
                for (; i < header.size() && header[i] != ' '; ++i);
                //go to the start of the version
                for (; i < header.size() && header[i] == ' '; ++ i) ;
                size_t j = i;
                //go to the end of the version
                for (; j < header.size() && header[j] != ' '; ++ j) ;
                try {
                    Semver semver(header.substr(i, j - i));
                    has_delimiters = semver >= Semver(2, 4, 0, 0, nullptr, "alpha0");
                } catch (const RuntimeError &) {
                }
                header_found = true;
                break;
            }
        }
        if (! header_found)
            throw Slic3r::RuntimeError("Not a Slic3r/ SuperSlicer / PrusaSlicer generated g-code.");
    }

    auto                      header_end_pos = ifs.tellg();
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    size_t                    key_value_pairs = 0;
    
    std::vector<std::pair<t_config_option_key, std::string>> opt_deleted;
    if (has_delimiters)
    {
        // Slic3r starting with 2.4.0 (and Prusaslicer from 2.4.0-alpha0) delimits the config section stored into G-code with 
        // ; SLIC3R_NAME_config = begin
        // ...
        // ; SLIC3R_NAME_config = end
        // The begin / end tags look like any other key / value pairs on purpose to be compatible with older G-code viewer.
        // Read the file in reverse line by line.
        ReverseLineReader reader(ifs, header_end_pos);
        // Read the G-code file by 64k blocks back to front.
        bool begin_found = false;
        bool end_found   = false;
        std::string line;
        while (reader.getline(line))
            if (boost::algorithm::ends_with(line, "r_config = end")) {
                end_found = true;
                break;
            }
        if (! end_found)
            throw Slic3r::RuntimeError(format("Configuration block closing tag \"; (.+)r_config = end\" not found when reading %1%", filename));
        std::string key, value;
        while (reader.getline(line)) {
            if (boost::algorithm::ends_with(line, "r_config = begin")) {
                begin_found = true;
                break;
            }
            // line should be a valid key = value pair.
            auto pos = line.find('=');
            if (pos != std::string::npos && pos > 1 && line.front() == ';') {
                key   = line.substr(1, pos - 1);
                value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    std::string opt_key = key;
                    PrintConfigDef::handle_legacy(opt_key, value, false);
                    if (!opt_key.empty()) {
                        if (!PrintConfigDef::is_defined(opt_key)) {
                            if (substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                                substitutions_ctxt.add(ConfigSubstitution(key, value));
                            }
                        } else {
                            this->set_deserialize(opt_key, value, substitutions_ctxt);
                            ++ key_value_pairs;
                        }
                    } else {
                        opt_deleted.emplace_back(key, value);
                    }
                } catch (UnknownOptionException & /* e */) {
                    // ignore
                    assert(false);
                }
            }
        }
        if (! begin_found) 
            throw Slic3r::RuntimeError(format("Configuration block opening tag \"; (.+)r_config = begin\" not found when reading %1%", filename));
    }
    else
    {
        // Slic3r or PrusaSlicer older than 2.4.0-alpha0 do not emit any delimiter.
        // Try a heuristics reading the G-code from back.
        ifs.seekg(0, ifs.end);
        auto file_length = ifs.tellg();
    	auto data_length = std::min<std::fstream::pos_type>(65535, file_length - header_end_pos);
    	ifs.seekg(file_length - data_length, ifs.beg);
        std::vector<char> data(size_t(data_length) + 1, 0);
        ifs.read(data.data(), data_length);
        ifs.close();
        key_value_pairs = load_from_gcode_string_legacy(*this, data.data(), substitutions_ctxt);
    }

    if (key_value_pairs < 80)
        throw Slic3r::RuntimeError(format("Suspiciously low number of configuration values extracted from %1%: %2%", filename, key_value_pairs));

    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    this->handle_legacy_composite(opt_deleted);
    return std::move(substitutions_ctxt).data();
}

ConfigSubstitutions ConfigBase::load_from_binary_gcode_file(const std::string& filename, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);

    FilePtr file{ boost::nowide::fopen(filename.c_str(), "rb") };
    if (file.f == nullptr)
        throw Slic3r::RuntimeError(format("Error opening file %1%", filename));

    using namespace bgcode::core;
    using namespace bgcode::binarize;
    std::vector<std::byte> cs_buffer(65536);
    EResult res = is_valid_binary_gcode(*file.f, true, cs_buffer.data(), cs_buffer.size());
    if (res != EResult::Success)
        throw Slic3r::RuntimeError(format("File %1% does not contain a valid binary gcode\nError: %2%", filename,
            std::string(translate_result(res))));

    FileHeader file_header;
    res = read_header(*file.f, file_header, nullptr);
    if (res != EResult::Success)
        throw Slic3r::RuntimeError(format("Error while reading file %1%: %2%", filename, std::string(translate_result(res))));

    // searches for config block
    BlockHeader block_header;
    res = read_next_block_header(*file.f, file_header, block_header, EBlockType::SlicerMetadata, cs_buffer.data(), cs_buffer.size());
    if (res != EResult::Success)
        throw Slic3r::RuntimeError(format("Error while reading file %1%: %2%", filename, std::string(translate_result(res))));
    if ((EBlockType)block_header.type != EBlockType::SlicerMetadata)
        throw Slic3r::RuntimeError(format("Unable to find slicer metadata block in file %1%", filename));
    SlicerMetadataBlock slicer_metadata_block;
    res = slicer_metadata_block.read_data(*file.f, file_header, block_header);
    if (res != EResult::Success)
        throw Slic3r::RuntimeError(format("Error while reading file %1%: %2%", filename, std::string(translate_result(res))));
    
    std::vector<std::pair<t_config_option_key, std::string>> opt_deleted;
    // extracts data from block
    for (const auto& [key, value] : slicer_metadata_block.raw_data) {
        t_config_option_key test_key = key;
        std::string test_val = value;
        PrintConfigDef::handle_legacy(test_key, test_val, true);
        if (test_key.empty()) {
            opt_deleted.emplace_back(key, test_val);
        }

        this->set_deserialize(key, value, substitutions_ctxt);
    }

    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    this->handle_legacy_composite(opt_deleted);
    return std::move(substitutions_ctxt).data();
}

void ConfigBase::save(const std::string &file, bool to_prusa) const
{
    boost::nowide::ofstream c;
    c.open(file, std::ios::out | std::ios::trunc);
    c << "# " << Slic3r::header_slic3r_generated() << std::endl;
    if (to_prusa)
        for (std::string opt_key : this->keys()) {
            std::string value = this->opt_serialize(opt_key);
            this->to_prusa(opt_key, value);
            if(!opt_key.empty())
                c << opt_key << " = " << value << std::endl;
        }
    else
        for (const std::string &opt_key : this->keys())
            c << opt_key << " = " << this->opt_serialize(opt_key) << std::endl;
    c.close();
}

// Set all the nullable values to nils.
void ConfigBase::null_nullables()
{
    for (const std::string &opt_key : this->keys()) {
        ConfigOption *opt = this->optptr(opt_key, false);
        assert(opt != nullptr);
        if (opt->nullable())
        	opt->deserialize("nil", ForwardCompatibilitySubstitutionRule::Disable);
    }
}

DynamicConfig::DynamicConfig(const ConfigBase& rhs, const t_config_option_keys& keys)
{
	for (const t_config_option_key& opt_key : keys)
		this->options[opt_key] = std::unique_ptr<ConfigOption>(rhs.option(opt_key)->clone());
}

bool DynamicConfig::operator==(const DynamicConfig &rhs) const
{
    auto it1     = this->options.begin();
    auto it1_end = this->options.end();
    auto it2     = rhs.options.begin();
    auto it2_end = rhs.options.end();
    for (; it1 != it1_end && it2 != it2_end; ++ it1, ++ it2)
		if (it1->first != it2->first || *it1->second != *it2->second)
			// key or value differ
			return false;
    return it1 == it1_end && it2 == it2_end;
}

// Remove options with all nil values, those are optional and it does not help to hold them.
size_t DynamicConfig::remove_nil_options()
{
	size_t cnt_removed = 0;
	for (auto it = options.begin(); it != options.end();)
		if (it->second->is_nil()) {
			it = options.erase(it);
			++ cnt_removed;
		} else
			++ it;
	return cnt_removed;
}

ConfigOption* DynamicConfig::optptr(const t_config_option_key &opt_key, bool create)
{
    if (create) {
        // Use lower_bound instead of find for emplace_hint
        auto it = options.lower_bound(opt_key);
        if (it != options.end() && it->first == opt_key)
            // Option was found.
            return it->second.get();
        // Try to create a new ConfigOption.
        const ConfigDef *def = this->def();
        if (def == nullptr)
            throw NoDefinitionException(opt_key);
        const ConfigOptionDef *optdef = def->get(opt_key);
        if (optdef == nullptr)
            //        throw ConfigurationError(std::string("Invalid option name: ") + opt_key);
            // Let the parent decide what to do if the opt_key is not defined by this->def().
            return nullptr;
        ConfigOption *opt = optdef->create_default_option();
        this->options.emplace_hint(it, opt_key, std::unique_ptr<ConfigOption>(opt));
        return opt;
    } else {
        auto it = options.find(opt_key);
        if (it != options.end())
            // Option was found.
            return it->second.get();
        // Option was not found and a new option shall not be created.
        return nullptr;
    }
}

const ConfigOption* DynamicConfig::optptr(const t_config_option_key &opt_key) const
{
    auto it = options.find(opt_key);
    if (it == options.end()) {
        //if not find, try with the parent config.
        if (parent != nullptr)
            return parent->option(opt_key);
        else
            return nullptr;
    }else 
        return it->second.get();
}

bool DynamicConfig::read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys)
{
    // cache the CLI option => opt_key mapping
    std::map<std::string,std::string> opts;
    for (const auto &oit : this->def()->options)
        for (const std::string &t : oit.second.cli_args(oit.first))
            opts[t] = oit.first;
    
    bool parse_options = true;
    for (size_t i = 1; i < argc; ++ i) {
        std::string token = argv[i];
        // Store non-option arguments in the provided vector.
        if (! parse_options || ! boost::starts_with(token, "-")) {
            extra->push_back(token);
            continue;
        }
#ifdef __APPLE__
        if (boost::starts_with(token, "-psn_"))
            // OSX launcher may add a "process serial number", for example "-psn_0_989382" to the command line.
            // While it is supposed to be dropped since OSX 10.9, we will rather ignore it.
            continue;
#endif /* __APPLE__ */
        // Stop parsing tokens as options when -- is supplied.
        if (token == "--") {
            parse_options = false;
            continue;
        }
        // Remove leading dashes (one or two).
        token.erase(token.begin(), token.begin() + (boost::starts_with(token, "--") ? 2 : 1));
        // Read value when supplied in the --key=value form.
        std::string value;
        {
            size_t equals_pos = token.find("=");
            if (equals_pos != std::string::npos) {
                value = token.substr(equals_pos+1);
                token.erase(equals_pos);
            }
        }
        // Look for the cli -> option mapping.
        auto it = opts.find(token);
        bool no = false;
        if (it == opts.end()) {
            // Remove the "no-" prefix used to negate boolean options.
            std::string yes_token;
            if (boost::starts_with(token, "no-")) {
                yes_token = token.substr(3);
                it = opts.find(yes_token);
                no = true;
            }
            if (it == opts.end()) {
                boost::nowide::cerr << "Unknown option --" << token.c_str() << std::endl;
                return false;
            }
            if (no)
                token = yes_token;
        }

        const t_config_option_key &opt_key = it->second;
        const ConfigOptionDef     &optdef  = *this->option_def(opt_key);

        // If the option type expects a value and it was not already provided,
        // look for it in the next token.
        if (value.empty() && optdef.type != coBool && optdef.type != coBools) {
            if (i == argc-1) {
                boost::nowide::cerr << "No value supplied for --" << token.c_str() << std::endl;
                return false;
            }
            value = argv[++ i];
        }

        if (no) {
            assert(optdef.type == coBool || optdef.type == coBools);
            if (! value.empty()) {
                boost::nowide::cerr << "Boolean options negated by the --no- prefix cannot have a value." << std::endl;
                return false;
            }
        }

        // Store the option value.
        const bool               existing   = this->has(opt_key);
        if (keys != nullptr && ! existing) {
            // Save the order of detected keys.
            keys->push_back(opt_key);
        }
        ConfigOption            *opt_base   = this->option(opt_key, true);
        ConfigOptionVectorBase  *opt_vector = opt_base->is_vector() ? static_cast<ConfigOptionVectorBase*>(opt_base) : nullptr;
        if (opt_vector) {
			if (! existing)
				// remove the default values
				opt_vector->clear();
            // Vector values will be chained. Repeated use of a parameter will append the parameter or parameters
            // to the end of the value.
            if (opt_base->type() == coBools && value.empty())
                static_cast<ConfigOptionBools*>(opt_base)->set_at(!no, opt_vector->size());
            else
                // Deserialize any other vector value (ConfigOptionInts, Floats, Percents, Points) the same way
                // they get deserialized from an .ini file. For ConfigOptionStrings, that means that the C-style unescape
                // will be applied for values enclosed in quotes, while values non-enclosed in quotes are left to be
                // unescaped by the calling shell.
				opt_vector->deserialize(value, true);
        } else if (opt_base->type() == coBool) {
            if (value.empty())
                static_cast<ConfigOptionBool*>(opt_base)->value = !no;
            else
                opt_base->deserialize(value);
        } else if (opt_base->type() == coString) {
            // Do not unescape single string values, the unescaping is left to the calling shell.
            static_cast<ConfigOptionString*>(opt_base)->value = value;
        } else {
            // Just bail out if the configuration value is not understood.
            ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Disable);
            // Any scalar value of a type different from Bool and String.
            // here goes int options, like loglevel.
            if (! this->set_deserialize_nothrow(opt_key, value, context, false)) {
				boost::nowide::cerr << "Invalid value supplied for --" << token.c_str() << std::endl;
				return false;
			}
        }
    }
    return true;
}

t_config_option_keys DynamicConfig::keys() const
{
    t_config_option_keys keys;
    keys.reserve(this->options.size());
    for (const auto &opt : this->options)
        keys.emplace_back(opt.first);
    return keys;
}

void StaticConfig::set_defaults()
{
    // use defaults from definition
    auto *defs = this->def();
    if (defs != nullptr) {
        for (const std::string &key : this->keys()) {
            const ConfigOptionDef   *def = defs->get(key);
            ConfigOption            *opt = this->option(key);
            if (def != nullptr && opt != nullptr && def->default_value)
                opt->set(def->default_value.get());
        }
    }
}

t_config_option_keys StaticConfig::keys() const 
{
    t_config_option_keys keys;
    assert(this->def() != nullptr);
    for (const auto &opt_def : this->def()->options)
        if (this->option(opt_def.first) != nullptr) 
            keys.push_back(opt_def.first);
    return keys;
}

// Iterate over the pairs of options with equal keys, call the fn.
// Returns true on early exit by fn().
template<typename Fn>
static inline bool dynamic_config_iterate(const DynamicConfig &lhs, const DynamicConfig &rhs, Fn fn)
{
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator i = lhs.cbegin();
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator j = rhs.cbegin();
    while (i != lhs.cend() && j != rhs.cend())
        if (i->first < j->first)
            ++ i;
        else if (i->first > j->first)
            ++ j;
        else {
            assert(i->first == j->first);
            if (fn(i->first, i->second.get(), j->second.get()))
                // Early exit by fn.
                return true;
            ++ i;
            ++ j;
        }
    // Finished to the end.
    return false;
}

// Are the two configs equal? Ignoring options not present in both configs and phony fields.
bool DynamicConfig::equals(const DynamicConfig &other, bool even_phony /*=true*/) const
{ 
    return ! dynamic_config_iterate(*this, other, 
        [even_phony](const t_config_option_key & /* key */, const ConfigOption *l, const ConfigOption *r) {
            return (l != nullptr && r != nullptr
                && (even_phony || !(r->is_phony() && l->is_phony()))
                && ((*r != *l) || (r->is_phony() != l->is_phony())));
        });
}

// Returns options differing in the two configs, ignoring options not present in both configs.
t_config_option_keys DynamicConfig::diff(const DynamicConfig &other, bool even_phony /*=true*/) const
{
    t_config_option_keys diff;
    dynamic_config_iterate(*this, other, 
        [&diff, even_phony](const t_config_option_key &key, const ConfigOption *l, const ConfigOption *r) {
            //if (*l != *r)
            if (l != nullptr && r != nullptr
                && (even_phony || !(r->is_phony() && l->is_phony()))
                && ((*r != *l) || (r->is_phony() != l->is_phony())))
                diff.emplace_back(key);
            // Continue iterating.
            return false; 
        });
    return diff;
}

// Returns options being equal in the two configs, ignoring options not present in both configs.
t_config_option_keys DynamicConfig::equal(const DynamicConfig &other) const
{
    t_config_option_keys equal;
    dynamic_config_iterate(*this, other, 
        [&equal](const t_config_option_key &key, const ConfigOption *l, const ConfigOption *r) {
            if (*l == *r)
                equal.emplace_back(key);
            // Continue iterating.
            return false;
        });
    return equal;
}

}

#include <cereal/types/polymorphic.hpp>
CEREAL_REGISTER_TYPE(Slic3r::ConfigOption)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<int32_t>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<bool>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingleNullable<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingleNullable<int>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingleNullable<bool>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVectorBase)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<int32_t>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInt)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionIntNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInts)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionIntsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionString)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercentsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsOrPercentsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionGraph)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionGraphs)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBool)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBools)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBoolsNullable)
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
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingleNullable<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingleNullable<int>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingleNullable<bool>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionVectorBase) 
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<int32_t>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<Slic3r::GraphData>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<double>, Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingleNullable<double>, Slic3r::ConfigOptionFloatNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<double>, Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<double>, Slic3r::ConfigOptionFloatsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<int32_t>, Slic3r::ConfigOptionInt)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingleNullable<int32_t>, Slic3r::ConfigOptionIntNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<int32_t>, Slic3r::ConfigOptionInts)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<int32_t>, Slic3r::ConfigOptionIntsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<std::string>, Slic3r::ConfigOptionString)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<std::string>, Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloat, Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloats, Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloats, Slic3r::ConfigOptionPercentsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionPercent, Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>, Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>, Slic3r::ConfigOptionFloatsOrPercentsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>, Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::Vec2d>, Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>, Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::GraphData>, Slic3r::ConfigOptionGraph)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::GraphData>, Slic3r::ConfigOptionGraphs)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<bool>, Slic3r::ConfigOptionBool)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<unsigned char>, Slic3r::ConfigOptionBools)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<unsigned char>, Slic3r::ConfigOptionBoolsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionInt, Slic3r::ConfigOptionEnumGeneric)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigBase, Slic3r::DynamicConfig)
