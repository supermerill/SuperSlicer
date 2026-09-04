// Read package-authored release notes without loading any plugin code. Strict
// JSON validation rejects ambiguous duplicate keys before selecting a version;
// callers can still fall back to repository notes when this optional file fails.
#include "PluginPackageChangelog.hpp"

#include <set>
#include <regex>
#include <stdexcept>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/Semver.hpp"

namespace Slic3r {
namespace {
constexpr size_t MAX_CHANGELOG_SIZE = 1024 * 1024;
}

bool parse_plugin_package_changelog(const std::string &text, const std::string &version,
                                    std::optional<std::string> &notes, std::string &error)
{
    notes.reset();
    error.clear();
    try {
        if (text.size() > MAX_CHANGELOG_SIZE)
            throw std::runtime_error("Changelog exceeds the 1 MiB limit.");
        // JSON objects normally overwrite duplicate keys. Track each active
        // object while parsing so malformed release tables cannot be accepted.
        std::vector<std::set<std::string>> keys;
        const nlohmann::json document = nlohmann::json::parse(text,
            [&keys](int, nlohmann::json::parse_event_t event, nlohmann::json &value) {
                if (event == nlohmann::json::parse_event_t::object_start)
                    keys.emplace_back();
                else if (event == nlohmann::json::parse_event_t::object_end)
                    keys.pop_back();
                else if (event == nlohmann::json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    throw std::runtime_error("Duplicate changelog key: " + value.get<std::string>());
                return true;
            });
        if (!document.is_object() || document.size() != 2 ||
            !document.contains("format_version") || !document["format_version"].is_number_integer() ||
            document["format_version"] != 1 || !document.contains("versions") || !document["versions"].is_object())
            throw std::runtime_error("Expected format_version = 1 and a versions object.");
        for (const auto &[release, entries] : document["versions"].items()) {
            static const std::regex version_pattern("[0-9]+(\\.[0-9]+){1,3}(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?");
            if (release.size() > 128 || !std::regex_match(release, version_pattern) || !Semver::parse(release))
                throw std::runtime_error("Invalid package version: " + release);
            if (!entries.is_array())
                throw std::runtime_error("Version '" + release + "' must contain an array of strings.");
            std::string rendered;
            for (const nlohmann::json &entry : entries) {
                if (!entry.is_string())
                    throw std::runtime_error("Version '" + release + "' contains a non-string note.");
                if (!rendered.empty())
                    rendered += '\n';
                rendered += "- " + entry.get<std::string>();
            }
            if (release == version)
                notes = std::move(rendered);
        }
        return true;
    } catch (const std::exception &exception) {
        notes.reset();
        error = exception.what();
        return false;
    }
}

bool read_plugin_package_changelog(const std::string &path, const std::string &version,
                                   std::optional<std::string> &notes, std::string &error)
{
    notes.reset();
    error.clear();
    try {
        if (!boost::filesystem::exists(path))
            return true;
        boost::nowide::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Cannot open file.");
        // Bounded reading also protects against a file growing after stat().
        std::string contents(MAX_CHANGELOG_SIZE + 1, '\0');
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        contents.resize(static_cast<size_t>(stream.gcount()));
        if (stream.bad())
            throw std::runtime_error("Cannot read file.");
        if (parse_plugin_package_changelog(contents, version, notes, error))
            return true;
    } catch (const std::exception &exception) {
        error = exception.what();
    }
    error = "Changelog '" + path + "': " + error;
    return false;
}
}
