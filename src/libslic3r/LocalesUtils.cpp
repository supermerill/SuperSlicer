///|/ Copyright (c) Prusa Research 2021 - 2022 Tomáš Mészáros @tamasmeszaros, Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "LocalesUtils.hpp"

#ifdef _WIN32
    #include <charconv>
#endif
#include <stdexcept>
#include <sstream>

#include <fast_float/fast_float.h>

#include <boost/lexical_cast.hpp>

namespace Slic3r {


CNumericLocalesSetter::CNumericLocalesSetter()
{
#ifdef _WIN32
    _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    m_orig_numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
#elif __APPLE__
    m_original_locale = uselocale((locale_t)0);
    m_new_locale = newlocale(LC_NUMERIC_MASK, "C", m_original_locale);
    uselocale(m_new_locale);
#else // linux / BSD
    m_original_locale = uselocale((locale_t)0);
    m_new_locale = duplocale(m_original_locale);
    m_new_locale = newlocale(LC_NUMERIC_MASK, "C", m_new_locale);
    uselocale(m_new_locale);
#endif
}



CNumericLocalesSetter::~CNumericLocalesSetter()
{
#ifdef _WIN32
    std::setlocale(LC_NUMERIC, m_orig_numeric_locale.data());
#else
    uselocale(m_original_locale);
    freelocale(m_new_locale);
#endif
}



bool is_decimal_separator_point()
{
    char str[5] = "";
    sprintf(str, "%.1f", 0.5f);
    return str[1] == '.';
}

template<class T>
static T string_to_floating_decimal_point(const std::string_view str, size_t* pos /* = nullptr*/)
{
    T out;
    size_t p = fast_float::from_chars(str.data(), str.data() + str.size(), out).ptr - str.data();
    if (pos)
        *pos = p;
    return out;
}

double string_to_double_decimal_point(const std::string_view str, size_t* pos /* = nullptr*/)
{
    return string_to_floating_decimal_point<double>(str, pos);
}

float string_to_float_decimal_point(const std::string_view str, size_t* pos /* = nullptr*/)
{
    return string_to_floating_decimal_point<float>(str, pos);
}

std::string to_string_nozero(double value, int32_t max_precision) {
    double intpart;
    if (modf(value, &intpart) == 0.0) {
        //shortcut for int
        return std::to_string(int64_t(intpart));
    } else {
        std::stringstream ss;
        //first, get the int part, to see how many digit it takes
        int long10 = 0;
        if (intpart > 9)
            long10 = (int)std::floor(std::log10(std::abs(intpart)));
        //set the usable precision: there is only 15-16 decimal digit in a double
        ss << std::fixed << std::setprecision(int(std::min(15 - long10, int(max_precision)))) << value;
        std::string ret = ss.str();
        uint8_t nb_del = 0;
        if (ret.find('.') != std::string::npos) {
            uint8_t idx_char;
            for (idx_char = uint8_t(ss.tellp()) - 1; idx_char > 0; idx_char--) {
                if (ret[idx_char] == '0')
                    nb_del++;
                else
                    break;
            }
            // remove the '.' at the end of the int
            if (idx_char > 0 && ret[idx_char] == '.')
                nb_del++;
        }

        if (nb_del > 0)
            return ret.substr(0, ret.size() - nb_del);
        else
            return ret;
    }
}

std::string float_to_string_decimal_point(double value, int precision/* = -1*/)
{
    // merill: this fail on 'float_to_string_decimal_point(0.2)' because the 0.2 is a 0.200000001 (from a float->double conversion probably)
    // so i  revert this to my slow but trusty to_string_nozero
//    // Our Windows build server fully supports C++17 std::to_chars. Let's use it.
//    // Other platforms are behind, fall back to slow stringstreams for now.
//#ifdef _WIN32
//    constexpr size_t SIZE = 20;
//    char out[SIZE] = "";
//    std::to_chars_result res;
//    if (precision >=0)
//        res = std::to_chars(out, out+SIZE, value, std::chars_format::fixed, precision);
//    else
//        res = std::to_chars(out, out+SIZE, value, std::chars_format::general, 6);
//    if (res.ec == std::errc::value_too_large)
//        throw std::invalid_argument("float_to_string_decimal_point conversion failed.");
//    return std::string(out, res.ptr - out);
//#else
//    std::stringstream buf;
//    if (precision >= 0)
//        buf << std::fixed << std::setprecision(precision);
//    buf << value;
//    return buf.str();
//#endif

    return to_string_nozero(value, precision < 0 ? 6 : precision);
}

void remove_not_ascii(std::string &tomodify) {
    size_t pos_read = 0;
    bool previous_ascii = true;
    //skip until a not-ascii character
    while (pos_read < tomodify.length() && ((tomodify[pos_read] & 0x80) == 0)) { ++pos_read; }
    size_t pos_write = pos_read;
    //then modify the string
    while (pos_read < tomodify.length()) {
        if ((tomodify[pos_read] & 0x80) == 0) {
            //ascii, write
            tomodify[pos_write] = tomodify[pos_read];
            ++pos_write;
            previous_ascii = true;
        } else {
            //not-ascii, remove
            if (previous_ascii) {
                tomodify[pos_write] = '_';
                ++pos_write;
            }
            previous_ascii = false;
        }
        ++pos_read;
    }
    //remove extra bits
    tomodify.resize(pos_write);
}

} // namespace Slic3r

