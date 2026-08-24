///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_plugin_cpp_gcode_GCodeFormatter_hpp_
#define slic3r_Api_plugin_cpp_gcode_GCodeFormatter_hpp_

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include "libslic3r/Api/plugin/c/slic3r_geometry.h"

/*
Plugin-side G-code formatter
============================

The formatter turns numerical axes into compact G-code text while enforcing
the configured XYZ and extrusion precision.
*/

namespace slic3r_api { namespace GCodeGeneration {


class GCodeFormatter
{
public:
    uint16_t m_gcode_precision_xyz = 3;
    uint16_t m_gcode_precision_e   = 5;
    GCodeFormatter(uint16_t gcode_precision_xyz, uint16_t gcode_precision_e)
        : m_gcode_precision_xyz(gcode_precision_xyz), m_gcode_precision_e(gcode_precision_e)
    {
        this->buf_end     = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }
    GCodeFormatter(const GCodeFormatter &precision)
        : m_gcode_precision_xyz(precision.m_gcode_precision_xyz), m_gcode_precision_e(precision.m_gcode_precision_e)
    {
        this->buf_end     = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }

    virtual void clear() {
        this->buf_end     = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }

    //GCodeFormatter(const GCodeFormatter &) = delete;
    //GCodeFormatter &operator=(const GCodeFormatter &) = delete;

    // At layer height 0.15mm, extrusion width 0.2mm and filament diameter 1.75mm,
    // the crossection of extrusion is 0.4 * 0.15 = 0.06mm2
    // and the filament crossection is 1.75^2 = 3.063mm2
    // thus the filament moves 3.063 / 0.6 = 51x slower than the XY axes
    // and we need roughly two decimal digits more on extruder than on XY.
#if 1
    // use gcode_precision_xyz and gcode_precision_e from conf
    // static constexpr const int XYZF_EXPORT_DIGITS = 3;
    // static constexpr const int E_EXPORT_DIGITS    = 5;
#else
    // order of magnitude smaller extrusion rate erros
    static constexpr const int XYZF_EXPORT_DIGITS = 4;
    static constexpr const int E_EXPORT_DIGITS    = 6;
    // excessive accuracy
//    static constexpr const int XYZF_EXPORT_DIGITS = 6;
//    static constexpr const int E_EXPORT_DIGITS    = 9;
#endif

    static constexpr const std::array<double, 10> pow_10{1.,      10.,      100.,      1000.,      10000.,
                                                         100000., 1000000., 10000000., 100000000., 1000000000.};
    static constexpr const std::array<double, 10> pow_10_inv{1. / 1.,         1. / 10.,        1. / 100.,     1. / 1000.,
                                                             1. / 10000.,     1. / 100000.,    1. / 1000000., 1. / 10000000.,
                                                             1. / 100000000., 1. / 1000000000.};

    // Quantize doubles to a resolution of the G-code.
    double quantize(double v, size_t ndigits) const { return std::round(v * pow_10[ndigits]) * pow_10_inv[ndigits]; }
    double quantize_xyzf(double v) const { return quantize(v, m_gcode_precision_xyz); }
    double quantize_e(double v) const { return quantize(v, m_gcode_precision_e); }
    c_vec2d  quantize(const c_vec2d &pt) const { return {quantize(pt.x, m_gcode_precision_xyz), quantize(pt.y, m_gcode_precision_xyz)}; }
    c_vec3d  quantize(const c_vec3d &pt) const
    {
        return {quantize(pt.x, m_gcode_precision_xyz), quantize(pt.y, m_gcode_precision_xyz), quantize(pt.z, m_gcode_precision_xyz)};
    }
    int quantize_int(double v, size_t ndigits = size_t(-1)) const;

    // retunr the pointer to the begining of the digit written (without the axis)
    // please don't use it but the other safer methods, if available.
    char* emit_axis(const char axis, const double v, size_t digits);

    // Machine and process commands use named parameters whose textual policy
    // differs from compact movement axes. These checked methods preserve the
    // requested representation and reject invalid values before writing.
    void emit_integer_parameter(char parameter, double value);
    void emit_fixed_parameter(char parameter, double value, uint16_t precision);

    // update old_x & old_y with new strings. Return false if they are both the same.
    bool emit_xy(const c_vec2d &point, std::string &old_x, std::string &old_y);
    bool emit_z(const double z, std::string &old_z);

    void emit_ij(const c_vec2d &point)
    {
        if (point.x != 0)
            this->emit_axis('I', point.x, m_gcode_precision_xyz);
        if (point.y != 0)
            this->emit_axis('J', point.y, m_gcode_precision_xyz);
    }

    void emit_e(const std::string_view axis, double v);

    void emit_f(double speed) { this->emit_axis('F', speed, m_gcode_precision_xyz); }

    void emit_string(const std::string_view s)
    {
        // Be aware that std::string_view::data() returns a pointer to a buffer that is not necessarily null-terminated.
        memcpy(ptr_err.ptr, s.data(), s.size());
        ptr_err.ptr += s.size();
    }

    void emit_comment(bool allow_comments, const std::string_view comment)
    {
        if (allow_comments && !comment.empty()) {
            *ptr_err.ptr++ = ' ';
            *ptr_err.ptr++ = ';';
            *ptr_err.ptr++ = ' ';
            this->emit_string(comment);
        }
    }

    std::string string()
    {
        *ptr_err.ptr++ = '\n';
#ifdef _DEBUG
        // no 'Z-0'
        std::string to_check(this->buf, ptr_err.ptr - buf);
        assert(to_check.find("X-0 ") == std::string::npos);
        assert(to_check.find("X-0\n") == std::string::npos);
        assert(to_check.find("Y-0 ") == std::string::npos);
        assert(to_check.find("Y-0\n") == std::string::npos);
        assert(to_check.find("Z-0 ") == std::string::npos);
        assert(to_check.find("Z-0\n") == std::string::npos);
        assert(to_check.find("E-0 ") == std::string::npos);
        assert(to_check.find("E-0\n") == std::string::npos);
        assert(to_check.find("F-0 ") == std::string::npos);
        assert(to_check.find("F-0\n") == std::string::npos);
        assert(to_check.find("S-0 ") == std::string::npos);
        assert(to_check.find("S-0\n") == std::string::npos);
#endif
        return std::string(this->buf, ptr_err.ptr - buf);
    }

protected:
    static constexpr const size_t buflen = 256;
    char                          buf[buflen];
    char *                        buf_end;
    std::to_chars_result          ptr_err;
};

class GCodeG1Formatter : public GCodeFormatter {
public:
    GCodeG1Formatter(int gcode_precision_xyz, int gcode_precision_e)
            : GCodeFormatter(gcode_precision_xyz, gcode_precision_e)
    {
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->ptr_err.ptr += 2;
    }

    GCodeG1Formatter(const GCodeFormatter &precision) : GCodeFormatter(precision)
    {
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->ptr_err.ptr += 2;
    }

    GCodeG1Formatter(const GCodeG1Formatter&) = delete;
    GCodeG1Formatter& operator=(const GCodeG1Formatter&) = delete;

    void clear() override {
        GCodeFormatter::clear();
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->ptr_err.ptr += 2;
    }
};

class GCodeG2G3Formatter : public GCodeFormatter {
    const bool m_ccw;
public:
    GCodeG2G3Formatter(int gcode_precision_xyz, int gcode_precision_e, bool ccw)
        : GCodeFormatter(gcode_precision_xyz, gcode_precision_e), m_ccw(ccw)
    {
        this->buf[0] = 'G';
        this->buf[1] = m_ccw ? '3' : '2';
        this->ptr_err.ptr += 2;
    }
    GCodeG2G3Formatter(const GCodeFormatter &precision, bool ccw)
        : GCodeFormatter(precision), m_ccw(ccw)
    {
        this->buf[0] = 'G';
        this->buf[1] = m_ccw ? '3' : '2';
        this->ptr_err.ptr += 2;
    }

    GCodeG2G3Formatter(const GCodeG2G3Formatter&) = delete;
    GCodeG2G3Formatter& operator=(const GCodeG2G3Formatter&) = delete;

    void clear() override {
        GCodeFormatter::clear();
        this->buf[0] = 'G';
        this->buf[1] = m_ccw ? '3' : '2';
        this->ptr_err.ptr += 2;
    }
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_GCodeFormatter_hpp_
