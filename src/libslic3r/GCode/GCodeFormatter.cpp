///|/ Copyright (c) SuperSlicer 2020 - 2024 Durand Remi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav
///|/
///|/ PrusaSlicer & SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GCodeFormatter.hpp"

namespace Slic3r {


bool GCodeFormatter::emit_xy(const Vec2d &point, std::string &old_x, std::string &old_y)
{
    char* start_digit = this->emit_axis('X', point.x(), m_gcode_precision_xyz);
    std::string x_str = std::string(start_digit, this->ptr_err.ptr);
    start_digit = this->emit_axis('Y', point.y(), m_gcode_precision_xyz);
    std::string y_str = std::string(start_digit, this->ptr_err.ptr);
    bool same_point = (x_str == old_x && y_str == old_y);
    // update str
    old_x = x_str;
    old_y = y_str;
    return !same_point;
}

// return the de that isn't emmited as it's truncated
double GCodeFormatter::emit_e(const std::string_view axis, double v)
{
    if (!axis.empty()) {
        // not gcfNoExtrusion
        char* start_digit = this->emit_axis(axis[0], v, m_gcode_precision_e);
#ifdef _DEBUG
        double written_e = atof(std::string(start_digit, this->ptr_err.ptr).c_str());
        assert(std::abs(v - written_e) < 0.0000001);  // shoulde be already taken into account by m_tool->extrude
        return v - written_e;
#endif
    }
    return 0;
}

char* GCodeFormatter::emit_axis(const char axis, const double value, size_t digits) {
    assert(digits <= 9);
    static constexpr const std::array<int, 10> pow_10{1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    *ptr_err.ptr++ = ' '; *ptr_err.ptr++ = axis;

    char *base_ptr = this->ptr_err.ptr;
    auto  v_int    = int64_t(std::round(value * pow_10[digits]));
    // Older stdlib on macOS doesn't support std::from_chars at all, so it is used boost::spirit::karma::generate instead of it.
    // That is a little bit slower than std::to_chars but not much.
#ifdef __APPLE__
    boost::spirit::karma::generate(this->ptr_err.ptr, boost::spirit::karma::int_generator<int64_t>(), v_int);
#else
    // this->buf_end minus 1 because we need space for adding the extra decimal point.
    this->ptr_err = std::to_chars(this->ptr_err.ptr, this->buf_end - 1, v_int);
#endif
    size_t writen_digits = (this->ptr_err.ptr - base_ptr) - (v_int < 0 ? 1 : 0);
    if (writen_digits < digits) {
        // Number is smaller than 10^digits, so that we will pad it with zeros.
        size_t remaining_digits = digits - writen_digits;
        // Move all newly inserted chars by remaining_digits to allocate space for padding with zeros.
        for (char *from_ptr = this->ptr_err.ptr - 1, *to_ptr = from_ptr + remaining_digits; from_ptr >= this->ptr_err.ptr - writen_digits; --to_ptr, --from_ptr)
            *to_ptr = *from_ptr;

        memset(this->ptr_err.ptr - writen_digits, '0', remaining_digits);
        this->ptr_err.ptr += remaining_digits;
    }

    // Move all newly inserted chars by one to allocate space for a decimal point.
    for (char *to_ptr = this->ptr_err.ptr, *from_ptr = to_ptr - 1; from_ptr >= this->ptr_err.ptr - digits; --to_ptr, --from_ptr)
        *to_ptr = *from_ptr;

    *(this->ptr_err.ptr - digits) = '.';
    for (size_t i = 0; i < digits; ++i) {
        if (*this->ptr_err.ptr != '0')
            break;
        this->ptr_err.ptr--;
    }
    if (*this->ptr_err.ptr == '.')
        this->ptr_err.ptr--;
    if ((this->ptr_err.ptr + 1) == base_ptr || *this->ptr_err.ptr == '-')
        *(++this->ptr_err.ptr) = '0';
    this->ptr_err.ptr++;

#ifndef NDEBUG
    {
        assert(this->ptr_err.ptr - base_ptr >= 1); // not empty
        assert(v_int != 0 || (this->ptr_err.ptr - base_ptr == 1 && base_ptr[0] == '0')); //check zero
        assert(value >= 0 || base_ptr[0] == '-' || v_int == 0 ); // minus if negative (and not zero)
        assert(value < 0 || base_ptr[0] != '-'); // no minus if positive
        assert(*(this->ptr_err.ptr-1) != '.'); // do not add an extra '.' at the end
        char* position_dot = std::find(base_ptr, this->ptr_err.ptr, '.');
        assert(position_dot == this->ptr_err.ptr || *(this->ptr_err.ptr-1) != '0'); // if has '.', do not end with a zero
        assert(base_ptr[0] != '-' || this->ptr_err.ptr - base_ptr > 2 || (this->ptr_err.ptr - base_ptr == 2 && base_ptr[1] != '0')); // no "Z-0"
        // Verify that the optimized formatter produces the same result as the standard sprintf().
        double v1 = atof(std::string(base_ptr, this->ptr_err.ptr).c_str());
        char buf[2048];
        sprintf(buf, "%.*lf", int(digits), value);
        double v2 = atof(buf);
        // Numbers may differ when rounding at exactly or very close to 0.5 due to numerical issues when scaling the double to an integer.
        // Thus the complex assert.
//        assert(v1 == v2);
        assert(std::abs(v1 - value) * pow_10[digits] < 0.50001);
        assert(std::abs(v2 - value) * pow_10[digits] < 0.50001);
    }
#endif // NDEBUG
    return base_ptr;
}


} /* namespace Slic3r */
