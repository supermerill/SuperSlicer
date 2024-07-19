///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GFormatter_hpp_
#define slic3r_GFormatter_hpp_

#include "../libslic3r.h"

#include <string>
#include <string_view>
#include <vector>
#include <charconv>

namespace Slic3r {

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
    Vec2d  quantize(const Vec2d &pt) const { return {quantize(pt.x(), m_gcode_precision_xyz), quantize(pt.y(), m_gcode_precision_xyz)}; }
    Vec3d  quantize(const Vec3d &pt) const
    {
        return {quantize(pt.x(), m_gcode_precision_xyz), quantize(pt.y(), m_gcode_precision_xyz), quantize(pt.z(), m_gcode_precision_xyz)};
    }

    // retunr the pointer to the begining of the digit written (without the axis)
    char* emit_axis(const char axis, const double v, size_t digits);
    
    void emit_xy(const Vec2d &point)
    {
        this->emit_axis('X', point.x(), m_gcode_precision_xyz);
        this->emit_axis('Y', point.y(), m_gcode_precision_xyz);
    }

    // update old_x & old_y with new strings. Return false if they are both the same.
    bool emit_xy(const Vec2d &point, std::string &old_x, std::string &old_y);

    void emit_xyz(const Vec3d &point)
    {
        this->emit_axis('X', point.x(), m_gcode_precision_xyz);
        this->emit_axis('Y', point.y(), m_gcode_precision_xyz);
        this->emit_z(point.z());
    }

    void emit_z(const double z) { this->emit_axis('Z', z, m_gcode_precision_xyz); }

    void emit_ij(const Vec2d &point)
    {
        if (point.x() != 0)
            this->emit_axis('I', point.x(), m_gcode_precision_xyz);
        if (point.y() != 0)
            this->emit_axis('J', point.y(), m_gcode_precision_xyz);
    }

    // return the de that isn't emmited as it's truncated
    double emit_e(const std::string_view axis, double v);

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
};

class GCodeG2G3Formatter : public GCodeFormatter {
public:
    GCodeG2G3Formatter(int gcode_precision_xyz, int gcode_precision_e, bool ccw)
        : GCodeFormatter(gcode_precision_xyz, gcode_precision_e)
    {
        this->buf[0] = 'G';
        this->buf[1] = ccw ? '3' : '2';
        this->ptr_err.ptr += 2;
    }
    GCodeG2G3Formatter(const GCodeFormatter &precision, bool ccw)
        : GCodeFormatter(precision)
    {
        this->buf[0] = 'G';
        this->buf[1] = ccw ? '3' : '2';
        this->ptr_err.ptr += 2;
    }

    GCodeG2G3Formatter(const GCodeG2G3Formatter&) = delete;
    GCodeG2G3Formatter& operator=(const GCodeG2G3Formatter&) = delete;
};

} /* namespace Slic3r */

#endif /* slic3r_GFormatter_hpp_ */
