#ifndef slic3r_GitHubBackup_hpp_
#define slic3r_GitHubBackup_hpp_

#include <string>

namespace Slic3r { namespace GitHubBackup {
inline bool valid_repository(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == value.size() ||
        value.find('/', slash + 1) != std::string::npos)
        return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/'))
            return false;
    const auto repo = value.substr(slash + 1);
    return repo != "." && repo != ".." && value.substr(0, slash) != "." && value.substr(0, slash) != "..";
}

// Connection information can include credentials embedded in host URLs.
inline bool excluded_option(const std::string &key)
{
    return key == "print_host" || key == "host_type" ||
        key.compare(0, 9, "printhost") == 0 || key.compare(0, 9, "octoprint") == 0;
}

inline std::string base64(const std::string &input)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (size_t i = 0; i < input.size(); i += 3) {
        unsigned value = static_cast<unsigned char>(input[i]) << 16;
        if (i + 1 < input.size()) value |= static_cast<unsigned char>(input[i + 1]) << 8;
        if (i + 2 < input.size()) value |= static_cast<unsigned char>(input[i + 2]);
        output += alphabet[(value >> 18) & 63];
        output += alphabet[(value >> 12) & 63];
        output += i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=';
        output += i + 2 < input.size() ? alphabet[value & 63] : '=';
    }
    return output;
}
}}
#endif
