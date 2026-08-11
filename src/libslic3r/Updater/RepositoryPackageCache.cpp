///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryPackageCache.hpp. This implementation keeps every filesystem
// mutation behind staging directories so a failed import cannot damage a
// previously validated version.

#include "libslic3r/Updater/RepositoryPackageCache.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/Plugins/PluginBinaryMetadata.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#ifdef _WIN32
#include <Windows.h>
#pragma comment(lib, "version.lib")
#endif

namespace Slic3r {
namespace {

const char *const CACHE_LAYOUT_VERSION = "2";
const char *const CACHE_LAYOUT_FILENAME = ".layout_version";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const VERSION_FILENAME = "version.ini";
const char *const DEFAULT_VERSION = "1.0.0.0";

std::string package_type_directory(RepositoryPackageType type);
std::string package_section_name(RepositoryPackageType type);
std::string plugin_library_filename();
bool read_file(const boost::filesystem::path &path, std::string &contents);
bool repository_root_accepts_id(const boost::filesystem::path &descriptor,
                                RepositoryPackageType type,
                                const std::string &id,
                                std::string &error_message);
bool write_description_file(const boost::filesystem::path &path,
                            const RepositoryDescription &description,
                            std::string &error_message);
bool write_plugin_version_file(const boost::filesystem::path &path,
                               const RepositoryPackageVersion &version,
                               std::string &error_message);
bool read_plugin_version_file(const boost::filesystem::path &path,
                              RepositoryPackageVersion &version,
                              std::string &error_message);
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message);
bool descriptions_match(const RepositoryDescription &actual,
                        const RepositoryDescription &expected,
                        std::string &error_message);
bool versions_match(const RepositoryPackageVersion &actual,
                    const RepositoryPackageVersion &expected,
                    std::string &error_message);
void apply_description_defaults(RepositoryDescription &description);
void apply_version_defaults(RepositoryPackageVersion &version);
bool normalize_vendor_profile(const boost::filesystem::path &source,
                              const boost::filesystem::path &destination,
                              RepositoryDescription &description,
                              RepositoryPackageVersion &version,
                              std::string &error_message);
bool locate_vendor_profile(const boost::filesystem::path &source,
                           RepositoryPackageSource source_type,
                           boost::filesystem::path &profile_path,
                           std::string &error_message);
bool parse_version_directory_name(const std::string &name,
                                  std::string &package_version,
                                  std::string &slicer_version);
bool newer_version(const RepositoryPackageVersion &left, const RepositoryPackageVersion &right);
boost::filesystem::path find_plugin_python_entry(const boost::filesystem::path &source,
                                                 const std::string &id);
bool plugin_has_supported_payload(const boost::filesystem::path &source,
                                  const std::string &id,
                                  std::string &error_message);
bool read_python_plugin_version(const boost::filesystem::path &entry,
                                RepositoryPackageVersion &version,
                                std::string &error_message);
bool read_plugin_binary_version(const boost::filesystem::path &library,
                                RepositoryPackageVersion &version,
                                bool &found,
                                std::string &error_message);
bool read_native_plugin_version(const boost::filesystem::path &library,
                                RepositoryPackageVersion &version,
                                std::string &error_message);
bool merge_plugin_version_source(RepositoryPackageVersion &version,
                                 const RepositoryPackageVersion &source,
                                 const std::string &source_name,
                                 std::string &error_message);
bool resolve_plugin_version(const boost::filesystem::path &source,
                            const std::string &id,
                            const std::optional<RepositoryPackageExpectation> &expected,
                            RepositoryPackageVersion &version,
                            std::string &error_message);

class VendorRepositoryPackageCacheAdapter final : public RepositoryPackageCacheAdapter {
public:
    RepositoryPackageType package_type() const override { return RepositoryPackageType::Vendor; }
    bool inspect(const boost::filesystem::path &source,
                 RepositoryPackageSource source_type,
                 const std::optional<RepositoryPackageExpectation> &expected,
                 RepositoryDescription &description,
                 RepositoryPackageVersion &version,
                 std::string &error_message) const override;
    bool stage(const boost::filesystem::path &source,
               RepositoryPackageSource source_type,
               const RepositoryDescription &description,
               const RepositoryPackageVersion &version,
               const boost::filesystem::path &staging,
               std::string &error_message) const override;
    bool validate(const boost::filesystem::path &version_directory,
                  const RepositoryDescription &description,
                  const RepositoryPackageVersion &version,
                  std::string &error_message) const override;
};

class PluginRepositoryPackageCacheAdapter final : public RepositoryPackageCacheAdapter {
public:
    RepositoryPackageType package_type() const override { return RepositoryPackageType::Plugin; }
    bool inspect(const boost::filesystem::path &source,
                 RepositoryPackageSource source_type,
                 const std::optional<RepositoryPackageExpectation> &expected,
                 RepositoryDescription &description,
                 RepositoryPackageVersion &version,
                 std::string &error_message) const override;
    bool stage(const boost::filesystem::path &source,
               RepositoryPackageSource source_type,
               const RepositoryDescription &description,
               const RepositoryPackageVersion &version,
               const boost::filesystem::path &staging,
               std::string &error_message) const override;
    bool validate(const boost::filesystem::path &version_directory,
                  const RepositoryDescription &description,
                  const RepositoryPackageVersion &version,
                  std::string &error_message) const override;
};

std::string package_type_directory(RepositoryPackageType type)
{
    return type == RepositoryPackageType::Vendor ? "vendor" : "plugins";
}

std::string package_section_name(RepositoryPackageType type)
{
    return type == RepositoryPackageType::Vendor ? "vendor" : "plugin";
}

std::string plugin_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

bool read_file(const boost::filesystem::path &path, std::string &contents)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    if (!stream)
        return false;
    contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

// A sanitized directory may represent only one raw repository id. Check its
// descriptor before any version is staged so colliding ids cannot replace one
// another even when they use the same version numbers.
bool repository_root_accepts_id(const boost::filesystem::path &descriptor,
                                RepositoryPackageType type,
                                const std::string &id,
                                std::string &error_message)
{
    if (!boost::filesystem::is_regular_file(descriptor))
        return true;

    std::string contents;
    RepositoryDescription existing;
    if (!read_file(descriptor, contents) ||
        !parse_repository_description(contents, type, existing, error_message)) {
        if (error_message.empty())
            error_message = "Cannot read existing repository description '" + descriptor.string() + "'.";
        return false;
    }
    if (existing.id == id)
        return true;
    error_message = "Repository ids '" + existing.id + "' and '" + id +
                    "' map to the same cache directory.";
    return false;
}

// Descriptions contain only repository identity and display metadata. The
// same normalized format is used at repository root and inside each version.
bool write_description_file(const boost::filesystem::path &path,
                            const RepositoryDescription &description,
                            std::string &error_message)
{
    try {
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream stream(path.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot create repository description '" + path.string() + "'.";
            return false;
        }

        stream << '[' << package_section_name(description.type) << "]\n";
        stream << "id = " << description.id << "\n";
        stream << "name = " << description.name << "\n";
        stream << "full_name = " << description.full_name << "\n";
        stream << "description = " << description.description << "\n";
        stream << "config_update_rest = " << description.config_update_rest << "\n";
        stream << "slicer = " << description.slicer << "\n";
        if (!stream.good()) {
            error_message = "Cannot finish repository description '" + path.string() + "'.";
            return false;
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

// Plugin versions are deliberately stored apart from description.ini. This
// file travels with the payload and is the authoritative local version source.
bool write_plugin_version_file(const boost::filesystem::path &path,
                               const RepositoryPackageVersion &version,
                               std::string &error_message)
{
    try {
        boost::nowide::ofstream stream(path.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot create plugin version file '" + path.string() + "'.";
            return false;
        }
        stream << "[plugin]\n";
        stream << "package_version = " << version.package_version << "\n";
        stream << "slicer_version = " << version.slicer_version << "\n";
        if (!stream.good()) {
            error_message = "Cannot finish plugin version file '" + path.string() + "'.";
            return false;
        }
        return true;
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }
}

// Missing values remain empty here so metadata and repository expectations can
// provide them later. Syntax errors are rejected instead of being defaulted.
bool read_plugin_version_file(const boost::filesystem::path &path,
                              RepositoryPackageVersion &version,
                              std::string &error_message)
{
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(path.string(), tree);
        const boost::property_tree::ptree &plugin = tree.get_child("plugin");
        version.package_version = plugin.get<std::string>("package_version", std::string());
        version.slicer_version = plugin.get<std::string>("slicer_version", std::string());
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot read plugin version file '" + path.string() + "': " + error.what();
        return false;
    }
}

// Copy the complete package tree without following ownership outside source.
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message)
{
    try {
        if (!boost::filesystem::is_directory(source)) {
            error_message = "Package source '" + source.string() + "' is not a directory.";
            return false;
        }
        boost::filesystem::create_directories(destination);
        for (boost::filesystem::recursive_directory_iterator it(source), end; it != end; ++it) {
            const boost::filesystem::path relative = boost::filesystem::relative(it->path(), source);
            const boost::filesystem::path output = destination / relative;
            if (boost::filesystem::is_directory(it->path()))
                boost::filesystem::create_directories(output);
            else if (boost::filesystem::is_regular_file(it->path())) {
                boost::filesystem::create_directories(output.parent_path());
                boost::filesystem::copy_file(it->path(), output, boost::filesystem::copy_option::overwrite_if_exists);
            }
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool descriptions_match(const RepositoryDescription &actual,
                        const RepositoryDescription &expected,
                        std::string &error_message)
{
    if (!expected.id.empty() && actual.id != expected.id) {
        error_message = "Package id '" + actual.id + "' does not match expected id '" + expected.id + "'.";
        return false;
    }
    const std::pair<const std::string *, const std::string *> fields[] = {
        {&actual.name, &expected.name},
        {&actual.full_name, &expected.full_name},
        {&actual.description, &expected.description},
        {&actual.config_update_rest, &expected.config_update_rest},
        {&actual.slicer, &expected.slicer}
    };
    for (const std::pair<const std::string *, const std::string *> &field : fields) {
        if (!field.second->empty() && *field.first != *field.second) {
            error_message = "Package description metadata does not match its expected value.";
            return false;
        }
    }
    return true;
}

bool versions_match(const RepositoryPackageVersion &actual,
                    const RepositoryPackageVersion &expected,
                    std::string &error_message)
{
    if (!expected.package_version.empty() && actual.package_version != expected.package_version) {
        error_message = "Package version '" + actual.package_version + "' does not match expected version '" +
                        expected.package_version + "'.";
        return false;
    }
    if (!expected.slicer_version.empty() && actual.slicer_version != expected.slicer_version) {
        error_message = "Package slicer version '" + actual.slicer_version + "' does not match expected version '" +
                        expected.slicer_version + "'.";
        return false;
    }
    return true;
}

void apply_description_defaults(RepositoryDescription &description)
{
    if (description.name.empty())
        description.name = description.id;
    if (description.full_name.empty())
        description.full_name = description.name;
    if (description.slicer.empty())
        description.slicer = SLIC3R_APP_KEY;
}

void apply_version_defaults(RepositoryPackageVersion &version)
{
    if (version.package_version.empty())
        version.package_version = DEFAULT_VERSION;
    if (version.slicer_version.empty())
        version.slicer_version = DEFAULT_VERSION;
}

// Rewrite the vendor header in the staged copy so defaults are part of the
// installable profile itself, not only its generated description.ini.
bool normalize_vendor_profile(const boost::filesystem::path &source,
                              const boost::filesystem::path &destination,
                              RepositoryDescription &description,
                              RepositoryPackageVersion &version,
                              std::string &error_message)
{
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(source.string(), tree);
        boost::property_tree::ptree &vendor = tree.get_child("vendor");
        const std::string fallback_id = source.stem().string();
        description = {};
        version = {};
        description.type = RepositoryPackageType::Vendor;
        description.id = vendor.get<std::string>("id", fallback_id);
        description.name = vendor.get<std::string>("name", description.id);
        description.full_name = vendor.get<std::string>("full_name", description.name);
        description.description = vendor.get<std::string>("description", std::string());
        description.config_update_rest = vendor.get<std::string>("config_update_rest", std::string());
        description.slicer = vendor.get<std::string>("slicer", std::string());
        version.package_version = vendor.get<std::string>("config_version", std::string());
        version.slicer_version = vendor.get<std::string>("slicer_version", std::string());
        apply_description_defaults(description);
        apply_version_defaults(version);

        vendor.put("id", description.id);
        vendor.put("name", description.name);
        vendor.put("full_name", description.full_name);
        vendor.put("description", description.description);
        vendor.put("config_update_rest", description.config_update_rest);
        vendor.put("slicer", description.slicer);
        vendor.put("config_version", version.package_version);
        vendor.put("slicer_version", version.slicer_version);
        boost::filesystem::create_directories(destination.parent_path());
        boost::property_tree::write_ini(destination.string(), tree);
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot normalize vendor profile '" + source.string() + "': " + error.what();
        return false;
    }
}

bool locate_vendor_profile(const boost::filesystem::path &source,
                           RepositoryPackageSource source_type,
                           boost::filesystem::path &profile_path,
                           std::string &error_message)
{
    if (source_type == RepositoryPackageSource::Simple) {
        if (!boost::filesystem::is_regular_file(source)) {
            error_message = "The vendor source is not an INI file.";
            return false;
        }
        profile_path = source;
        return true;
    }

    const boost::filesystem::path profiles = source / "profiles";
    if (!boost::filesystem::is_directory(profiles)) {
        error_message = "The vendor package has no profiles directory.";
        return false;
    }
    for (boost::filesystem::directory_iterator it(profiles), end; it != end; ++it) {
        if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".ini")
            continue;
        if (!profile_path.empty()) {
            error_message = "A vendor package must contain exactly one profile INI.";
            return false;
        }
        profile_path = it->path();
    }
    if (profile_path.empty()) {
        error_message = "The vendor package contains no profile INI.";
        return false;
    }
    return true;
}

bool parse_version_directory_name(const std::string &name,
                                  std::string &package_version,
                                  std::string &slicer_version)
{
    const size_t separator = name.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == name.size() ||
        name.find('=', separator + 1) != std::string::npos)
        return false;
    package_version = name.substr(0, separator);
    slicer_version = name.substr(separator + 1);
    return Semver::parse(package_version).has_value() && Semver::parse(slicer_version).has_value();
}

bool newer_version(const RepositoryPackageVersion &left, const RepositoryPackageVersion &right)
{
    const std::optional<Semver> left_package = Semver::parse(left.package_version);
    const std::optional<Semver> right_package = Semver::parse(right.package_version);
    if (!left_package || !right_package)
        return false;
    if (*left_package != *right_package)
        return *left_package > *right_package;
    return *Semver::parse(left.slicer_version) > *Semver::parse(right.slicer_version);
}

// A package may use its id as the entry-point name, the conventional
// plugin.py name, or a single unambiguous Python file at its root.
boost::filesystem::path find_plugin_python_entry(const boost::filesystem::path &source,
                                                 const std::string &id)
{
    const boost::filesystem::path named_entry = source / (id + ".py");
    if (boost::filesystem::is_regular_file(named_entry))
        return named_entry;
    const boost::filesystem::path conventional_entry = source / "plugin.py";
    if (boost::filesystem::is_regular_file(conventional_entry))
        return conventional_entry;

    boost::filesystem::path unique_entry;
    if (!boost::filesystem::is_directory(source))
        return unique_entry;
    for (boost::filesystem::directory_iterator it(source), end; it != end; ++it) {
        if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".py")
            continue;
        if (!unique_entry.empty())
            return {};
        unique_entry = it->path();
    }
    return unique_entry;
}

bool plugin_has_supported_payload(const boost::filesystem::path &source,
                                  const std::string &id,
                                  std::string &error_message)
{
    if (boost::filesystem::is_regular_file(source / plugin_library_filename()) ||
        !find_plugin_python_entry(source, id).empty())
        return true;
    error_message = "Plugin package '" + id + "' must contain '" + plugin_library_filename() +
                    "' or one unambiguous Python entry point.";
    return false;
}

// Python metadata is parsed as literal single-line assignments. The package is
// never executed while being inspected, so importing a local archive is safe.
bool read_python_plugin_version(const boost::filesystem::path &entry,
                                RepositoryPackageVersion &version,
                                std::string &error_message)
{
    boost::nowide::ifstream stream(entry.string());
    if (!stream) {
        error_message = "Cannot read Python plugin entry '" + entry.string() + "'.";
        return false;
    }

    const std::regex package_pattern("^[[:space:]]*__version__[[:space:]]*=[[:space:]]*(['\"])([^'\"]+)\\1[[:space:]]*(?:#.*)?$");
    const std::regex slicer_pattern("^[[:space:]]*__slicer_version__[[:space:]]*=[[:space:]]*(['\"])([^'\"]+)\\1[[:space:]]*(?:#.*)?$");
    std::string line;
    std::smatch match;
    while (std::getline(stream, line)) {
        if (std::regex_match(line, match, package_pattern)) {
            if (!version.package_version.empty() && version.package_version != match[2].str()) {
                error_message = "Python plugin declares __version__ more than once with different values.";
                return false;
            }
            version.package_version = match[2].str();
        } else if (std::regex_match(line, match, slicer_pattern)) {
            if (!version.slicer_version.empty() && version.slicer_version != match[2].str()) {
                error_message = "Python plugin declares __slicer_version__ more than once with different values.";
                return false;
            }
            version.slicer_version = match[2].str();
        }
    }
    return true;
}

// Scan the binary in bounded chunks for the portable fixed-size record. The
// overlap preserves a record split across two reads, while field terminators
// and the format version reject accidental occurrences of the magic bytes.
bool read_plugin_binary_version(const boost::filesystem::path &library,
                                RepositoryPackageVersion &version,
                                bool &found,
                                std::string &error_message)
{
    found = false;
    boost::nowide::ifstream stream(library.string(), std::ios::in | std::ios::binary);
    if (!stream) {
        error_message = "Cannot read embedded metadata from plugin library '" + library.string() + "'.";
        return false;
    }

    constexpr size_t chunk_size = 64 * 1024;
    constexpr size_t overlap_size = sizeof(PluginBinaryMetadata) - 1;
    std::vector<unsigned char> bytes(chunk_size + overlap_size);
    size_t carried = 0;
    for (;;) {
        stream.read(reinterpret_cast<char *>(bytes.data() + carried), std::streamsize(chunk_size));
        const size_t read_size = size_t(stream.gcount());
        const size_t available = carried + read_size;

        for (size_t offset = 0; offset + sizeof(PluginBinaryMetadata) <= available; ++offset) {
            if (std::memcmp(bytes.data() + offset, PLUGIN_BINARY_METADATA_MAGIC.data(),
                            PLUGIN_BINARY_METADATA_MAGIC.size()) != 0)
                continue;

            PluginBinaryMetadata candidate;
            std::memcpy(&candidate, bytes.data() + offset, sizeof(candidate));
            if (candidate.format_version != PLUGIN_BINARY_METADATA_FORMAT_VERSION)
                continue;
            const char *package_end = static_cast<const char *>(std::memchr(
                candidate.package_version, '\0', sizeof(candidate.package_version)));
            const char *slicer_end = static_cast<const char *>(std::memchr(
                candidate.slicer_version, '\0', sizeof(candidate.slicer_version)));
            if (package_end == nullptr || slicer_end == nullptr)
                continue;

            RepositoryPackageVersion candidate_version;
            candidate_version.package_version.assign(
                candidate.package_version, size_t(package_end - candidate.package_version));
            candidate_version.slicer_version.assign(
                candidate.slicer_version, size_t(slicer_end - candidate.slicer_version));
            if (candidate_version.package_version.empty() && candidate_version.slicer_version.empty())
                continue;
            if (!merge_plugin_version_source(version, candidate_version,
                                             "embedded binary metadata", error_message))
                return false;
            found = true;
        }

        if (stream.bad()) {
            error_message = "Cannot finish reading plugin library '" + library.string() + "'.";
            return false;
        }
        if (read_size == 0)
            break;

        carried = std::min(available, overlap_size);
        std::memmove(bytes.data(), bytes.data() + available - carried, carried);
    }
    return true;
}

// VERSIONINFO is the standard Windows source. The portable record is then
// merged as a cross-platform fallback and as a consistency check when both are
// present. Neither source requires loading or executing the plugin library.
bool read_native_plugin_version(const boost::filesystem::path &library,
                                RepositoryPackageVersion &version,
                                std::string &error_message)
{
#ifdef _WIN32
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(library.wstring().c_str(), &ignored);
    if (size != 0) {
        std::vector<unsigned char> data(size);
        if (!GetFileVersionInfoW(library.wstring().c_str(), 0, size, data.data())) {
            error_message = "Cannot read VERSIONINFO from plugin library '" + library.string() + "'.";
            return false;
        }

        struct Translation { WORD language; WORD code_page; };
        Translation *translations = nullptr;
        UINT translation_bytes = 0;
        if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                           reinterpret_cast<void **>(&translations), &translation_bytes) &&
            translation_bytes >= sizeof(Translation)) {
            const wchar_t *keys[] = {L"ProductVersion", L"SlicerVersion"};
            std::string *outputs[] = {&version.package_version, &version.slicer_version};
            for (size_t idx = 0; idx < 2; ++idx) {
                wchar_t query[128];
                swprintf(query, sizeof(query) / sizeof(query[0]), L"\\StringFileInfo\\%04x%04x\\%ls",
                         translations[0].language, translations[0].code_page, keys[idx]);
                wchar_t *value = nullptr;
                UINT value_size = 0;
                if (VerQueryValueW(data.data(), query, reinterpret_cast<void **>(&value), &value_size) &&
                    value != nullptr && value_size > 1)
                    *outputs[idx] = boost::nowide::narrow(value);
            }
        }
    }
#endif

    if (version.package_version.empty() || version.slicer_version.empty()) {
        RepositoryPackageVersion binary_version;
        bool binary_found = false;
        return read_plugin_binary_version(library, binary_version, binary_found, error_message) &&
               (!binary_found || merge_plugin_version_source(
                    version, binary_version, "embedded binary metadata", error_message));
    }
    return true;
}

// Merge one provenance source without silently choosing a winner. A conflict
// means the package cannot identify which version would actually be installed.
bool merge_plugin_version_source(RepositoryPackageVersion &version,
                                 const RepositoryPackageVersion &source,
                                 const std::string &source_name,
                                 std::string &error_message)
{
    if (!source.package_version.empty()) {
        if (!version.package_version.empty() && version.package_version != source.package_version) {
            error_message = "Plugin package version conflict with " + source_name + ": '" +
                            version.package_version + "' versus '" + source.package_version + "'.";
            return false;
        }
        version.package_version = source.package_version;
    }
    if (!source.slicer_version.empty()) {
        if (!version.slicer_version.empty() && version.slicer_version != source.slicer_version) {
            error_message = "Plugin slicer version conflict with " + source_name + ": '" +
                            version.slicer_version + "' versus '" + source.slicer_version + "'.";
            return false;
        }
        version.slicer_version = source.slicer_version;
    }
    return true;
}

bool resolve_plugin_version(const boost::filesystem::path &source,
                            const std::string &id,
                            const std::optional<RepositoryPackageExpectation> &expected,
                            RepositoryPackageVersion &version,
                            std::string &error_message)
{
    version = {};
    const boost::filesystem::path version_path = source / VERSION_FILENAME;
    if (boost::filesystem::is_regular_file(version_path) &&
        !read_plugin_version_file(version_path, version, error_message))
        return false;

    RepositoryPackageVersion native_version;
    const boost::filesystem::path library = source / plugin_library_filename();
    if (boost::filesystem::is_regular_file(library) &&
        !read_native_plugin_version(library, native_version, error_message))
        return false;
    const boost::filesystem::path python_entry = find_plugin_python_entry(source, id);
    if (!python_entry.empty()) {
        RepositoryPackageVersion python_version;
        if (!read_python_plugin_version(python_entry, python_version, error_message) ||
            !merge_plugin_version_source(native_version, python_version, "Python metadata", error_message))
            return false;
    }
    if (!merge_plugin_version_source(version, native_version, "native metadata", error_message))
        return false;
    if (expected && !merge_plugin_version_source(version, expected->version, "repository tag", error_message))
        return false;

    apply_version_defaults(version);
    if (!Semver::parse(version.package_version) || !Semver::parse(version.slicer_version)) {
        error_message = "Plugin package contains an invalid package or slicer version.";
        return false;
    }
    return true;
}

bool VendorRepositoryPackageCacheAdapter::inspect(
    const boost::filesystem::path &source,
    RepositoryPackageSource source_type,
    const std::optional<RepositoryPackageExpectation> &expected,
    RepositoryDescription &description,
    RepositoryPackageVersion &version,
    std::string &error_message) const
{
    boost::filesystem::path profile_path;
    if (!locate_vendor_profile(source, source_type, profile_path, error_message))
        return false;

    const boost::filesystem::path temporary = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path(".vendor-description-%%%%-%%%%.ini");
    const bool normalized = normalize_vendor_profile(profile_path, temporary, description, version, error_message);
    boost::system::error_code ignored_error;
    boost::filesystem::remove(temporary, ignored_error);
    if (!normalized)
        return false;

    // Complete archives may repeat generic display metadata in description.ini.
    // Version-looking keys in that file are ignored; only the profile owns the
    // vendor and slicer versions.
    const boost::filesystem::path manifest_path = source / DESCRIPTION_FILENAME;
    if (source_type == RepositoryPackageSource::Package &&
        boost::filesystem::is_regular_file(manifest_path)) {
        std::string contents;
        RepositoryDescription manifest;
        if (!read_file(manifest_path, contents) ||
            !parse_repository_description(contents, RepositoryPackageType::Vendor, manifest, error_message))
            return false;
        if (!descriptions_match(description, manifest, error_message))
            return false;
    }
    if (expected && (expected->type != RepositoryPackageType::Vendor ||
                     description.id != expected->id)) {
        error_message = "Vendor package identity does not match its repository tag.";
        return false;
    }
    return !expected || versions_match(version, expected->version, error_message);
}

bool VendorRepositoryPackageCacheAdapter::stage(
    const boost::filesystem::path &source,
    RepositoryPackageSource source_type,
    const RepositoryDescription &description,
    const RepositoryPackageVersion &version,
    const boost::filesystem::path &staging,
    std::string &error_message) const
{
    boost::filesystem::path profile_path;
    if (!locate_vendor_profile(source, source_type, profile_path, error_message))
        return false;

    try {
        if (source_type == RepositoryPackageSource::Package &&
            !copy_directory_tree(source, staging, error_message))
            return false;
        boost::filesystem::create_directories(staging / "profiles");

        RepositoryDescription normalized;
        RepositoryPackageVersion normalized_version;
        const boost::filesystem::path normalized_profile = staging / "profiles" / (description.id + ".ini");
        if (!normalize_vendor_profile(profile_path, normalized_profile,
                                      normalized, normalized_version, error_message))
            return false;
        if (!descriptions_match(normalized, description, error_message) ||
            !versions_match(normalized_version, version, error_message) ||
            !versions_match(version, normalized_version, error_message))
            return false;

        // A package may name its input profile after an archive wrapper. Keep
        // one canonical profile named after the raw vendor id in the cached
        // payload so discovery cannot observe two competing INI files.
        if (source_type == RepositoryPackageSource::Package) {
            const boost::filesystem::path copied_profile = staging / "profiles" / profile_path.filename();
            if (copied_profile != normalized_profile)
                boost::filesystem::remove(copied_profile);
        }

        // A bare profile may have a sibling icon directory named after its id.
        if (source_type == RepositoryPackageSource::Simple) {
            const boost::filesystem::path icons = source.parent_path() / description.id;
            if (boost::filesystem::is_directory(icons) &&
                !copy_directory_tree(icons, staging / "profiles" / description.id, error_message))
                return false;
        }
        return write_description_file(staging / DESCRIPTION_FILENAME, description, error_message);
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool VendorRepositoryPackageCacheAdapter::validate(
    const boost::filesystem::path &version_directory,
    const RepositoryDescription &description,
    const RepositoryPackageVersion &version,
    std::string &error_message) const
{
    const boost::filesystem::path profile_path = version_directory / "profiles" / (description.id + ".ini");
    if (!boost::filesystem::is_regular_file(profile_path)) {
        error_message = "Vendor version has no profile '" + profile_path.string() + "'.";
        return false;
    }
    try {
        const VendorProfile profile = VendorProfile::from_ini(profile_path, true);
        if (profile.id != description.id || profile.config_version.to_string() != version.package_version ||
            profile.slicer_version.to_string() != version.slicer_version) {
            error_message = "Vendor profile metadata does not match its cached version directory.";
            return false;
        }
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }
    std::string contents;
    RepositoryDescription actual;
    if (!read_file(version_directory / DESCRIPTION_FILENAME, contents) ||
        !parse_repository_description(contents, RepositoryPackageType::Vendor, actual, error_message))
        return false;
    return descriptions_match(actual, description, error_message) &&
           descriptions_match(description, actual, error_message);
}

bool PluginRepositoryPackageCacheAdapter::inspect(
    const boost::filesystem::path &source,
    RepositoryPackageSource,
    const std::optional<RepositoryPackageExpectation> &expected,
    RepositoryDescription &description,
    RepositoryPackageVersion &version,
    std::string &error_message) const
{
    if (!boost::filesystem::is_directory(source)) {
        error_message = "Plugin package source is not a directory.";
        return false;
    }

    const boost::filesystem::path description_path = source / DESCRIPTION_FILENAME;
    if (boost::filesystem::is_regular_file(description_path)) {
        std::string contents;
        if (!read_file(description_path, contents) ||
            !parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message))
            return false;
    } else {
        // A live plugin copied back after a layout purge may predate package
        // descriptions. Its requested activation version is authoritative in
        // that case; a manually imported folder still derives its identity
        // from the directory name and receives local-package defaults.
        description = {};
        description.type = RepositoryPackageType::Plugin;
        description.id = expected ? expected->id : source.filename().string();
    }
    apply_description_defaults(description);
    if (expected && (expected->type != RepositoryPackageType::Plugin || description.id != expected->id)) {
        error_message = "Plugin package identity does not match its repository tag.";
        return false;
    }
    return plugin_has_supported_payload(source, description.id, error_message) &&
           resolve_plugin_version(source, description.id, expected, version, error_message);
}

bool PluginRepositoryPackageCacheAdapter::stage(
    const boost::filesystem::path &source,
    RepositoryPackageSource,
    const RepositoryDescription &description,
    const RepositoryPackageVersion &version,
    const boost::filesystem::path &staging,
    std::string &error_message) const
{
    if (!copy_directory_tree(source, staging, error_message))
        return false;
    return write_description_file(staging / DESCRIPTION_FILENAME, description, error_message) &&
           write_plugin_version_file(staging / VERSION_FILENAME, version, error_message);
}

bool PluginRepositoryPackageCacheAdapter::validate(
    const boost::filesystem::path &version_directory,
    const RepositoryDescription &description,
    const RepositoryPackageVersion &version,
    std::string &error_message) const
{
    if (!plugin_has_supported_payload(version_directory, description.id, error_message))
        return false;
    std::string contents;
    RepositoryDescription actual;
    if (!read_file(version_directory / DESCRIPTION_FILENAME, contents) ||
        !parse_repository_description(contents, RepositoryPackageType::Plugin, actual, error_message))
        return false;
    RepositoryPackageVersion actual_version;
    if (!read_plugin_version_file(version_directory / VERSION_FILENAME, actual_version, error_message))
        return false;
    return descriptions_match(actual, description, error_message) &&
           descriptions_match(description, actual, error_message) &&
           versions_match(actual_version, version, error_message) &&
           versions_match(version, actual_version, error_message);
}

} // namespace

RepositoryPackageCache::RepositoryPackageCache(boost::filesystem::path data_directory,
                                               const RepositoryPackageCacheAdapter &adapter)
    : m_data_directory(std::move(data_directory)), m_adapter(adapter)
{
}

bool RepositoryPackageCache::prepare_layout(bool &purged, std::string &error_message) const
{
    purged = false;
    const boost::filesystem::path root = type_directory();
    const boost::filesystem::path marker = root / CACHE_LAYOUT_FILENAME;
    try {
        std::string current_version;
        if (read_file(marker, current_version) && current_version == CACHE_LAYOUT_VERSION)
            return true;

        // Missing marker means callers must repopulate durable sources even
        // when the cache directory itself does not exist yet.
        purged = true;
        if (boost::filesystem::exists(root)) {
            boost::filesystem::remove_all(root);
        }
        boost::filesystem::create_directories(root);
        boost::nowide::ofstream stream(marker.string(), std::ios::out | std::ios::trunc);
        stream << CACHE_LAYOUT_VERSION;
        if (!stream.good()) {
            error_message = "Cannot write repository cache layout marker.";
            return false;
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

boost::filesystem::path RepositoryPackageCache::type_directory() const
{
    return m_data_directory / "cache" / package_type_directory(m_adapter.package_type());
}

boost::filesystem::path RepositoryPackageCache::repository_directory(const std::string &id) const
{
    return type_directory() / safe_id(id);
}

boost::filesystem::path RepositoryPackageCache::repository_description_path(const std::string &id) const
{
    return repository_directory(id) / DESCRIPTION_FILENAME;
}

boost::filesystem::path RepositoryPackageCache::repository_tags_path(const std::string &id) const
{
    return repository_directory(id) / "tags.json";
}

boost::filesystem::path RepositoryPackageCache::repository_logs_directory(const std::string &id) const
{
    return repository_directory(id) / "logs";
}

boost::filesystem::path RepositoryPackageCache::version_directory(const std::string &id,
                                                                  const std::string &package_version,
                                                                  const std::string &slicer_version) const
{
    return repository_directory(id) / version_directory_name(package_version, slicer_version);
}

bool RepositoryPackageCache::save_repository_description(const RepositoryDescription &description,
                                                         std::string &error_message) const
{
    if (description.type != m_adapter.package_type() || description.id.empty()) {
        error_message = "Repository description has the wrong package type or no id.";
        return false;
    }

    try {
        const boost::filesystem::path root = repository_directory(description.id);
        const boost::filesystem::path descriptor = repository_description_path(description.id);
        if (!repository_root_accepts_id(descriptor, m_adapter.package_type(), description.id, error_message))
            return false;
        boost::filesystem::create_directories(root);
        const boost::filesystem::path staging = root /
            boost::filesystem::unique_path(".description-%%%%-%%%%.ini");
        const boost::filesystem::path backup = root /
            boost::filesystem::unique_path(".description-previous-%%%%-%%%%.ini");
        if (!write_description_file(staging, description, error_message)) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return false;
        }

        // Windows cannot rename over an existing file. Keep the previous
        // descriptor beside the staging file until the new descriptor is in
        // place, then remove it without making cleanup failure fatal.
        const bool replacing = boost::filesystem::is_regular_file(descriptor);
        try {
            if (replacing)
                boost::filesystem::rename(descriptor, backup);
            boost::filesystem::rename(staging, descriptor);
        } catch (...) {
            if (boost::filesystem::is_regular_file(backup) && !boost::filesystem::exists(descriptor))
                boost::filesystem::rename(backup, descriptor);
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            throw;
        }
        if (replacing) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous repository description '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::cache_simple(const boost::filesystem::path &source,
                                          RepositoryCachedVersion &cached,
                                          std::string &error_message) const
{
    return cache_source(source, RepositoryPackageSource::Simple, std::nullopt, cached, error_message);
}

bool RepositoryPackageCache::cache_archive(const boost::filesystem::path &archive_path,
                                           const std::optional<RepositoryPackageExpectation> &expected,
                                           RepositoryCachedVersion &cached,
                                           std::string &error_message) const
{
    const boost::filesystem::path extraction = type_directory() /
        boost::filesystem::unique_path(".extract-%%%%-%%%%");
    try {
        if (!extract_repository_archive(archive_path, extraction, error_message)) {
            boost::filesystem::remove_all(extraction);
            return false;
        }

        RepositoryDescription ignored_description;
        RepositoryPackageVersion ignored_version;
        std::string direct_error;
        boost::filesystem::path package_root = extraction;
        if (!m_adapter.inspect(package_root, RepositoryPackageSource::Package, expected,
                               ignored_description, ignored_version, direct_error)) {
            boost::filesystem::directory_iterator it(extraction), end;
            if (it == end || !boost::filesystem::is_directory(it->path())) {
                boost::filesystem::remove_all(extraction);
                error_message = std::move(direct_error);
                return false;
            }
            package_root = it->path();
            ++it;
            if (it != end) {
                boost::filesystem::remove_all(extraction);
                error_message = "Package archive contains more than one root entry.";
                return false;
            }
        }

        const bool succeeded = cache_source(package_root, RepositoryPackageSource::Package,
                                            expected, cached, error_message);
        boost::filesystem::remove_all(extraction);
        return succeeded;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(extraction, ignored_error);
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::cache_package_directory(
    const boost::filesystem::path &package_directory,
    const std::optional<RepositoryPackageExpectation> &expected,
    RepositoryCachedVersion &cached,
    std::string &error_message) const
{
    return cache_source(package_directory, RepositoryPackageSource::Package, expected, cached, error_message);
}

bool RepositoryPackageCache::cache_source(const boost::filesystem::path &source,
                                          RepositoryPackageSource source_type,
                                          const std::optional<RepositoryPackageExpectation> &expected,
                                          RepositoryCachedVersion &cached,
                                          std::string &error_message) const
{
    RepositoryDescription description;
    RepositoryPackageVersion version;
    if (!m_adapter.inspect(source, source_type, expected, description, version, error_message))
        return false;
    apply_description_defaults(description);
    apply_version_defaults(version);
    if (!Semver::parse(version.package_version) || !Semver::parse(version.slicer_version)) {
        error_message = "Repository package contains an invalid package or slicer version.";
        return false;
    }
    return publish(source, source_type, description, version, cached, error_message);
}

bool RepositoryPackageCache::publish(const boost::filesystem::path &source,
                                     RepositoryPackageSource source_type,
                                     const RepositoryDescription &description,
                                     const RepositoryPackageVersion &version,
                                     RepositoryCachedVersion &cached,
                                     std::string &error_message) const
{
    const boost::filesystem::path root = repository_directory(description.id);
    const boost::filesystem::path destination = version_directory(
        description.id, version.package_version, version.slicer_version);
    const boost::filesystem::path staging = root /
        boost::filesystem::unique_path(".publish-%%%%-%%%%");
    const boost::filesystem::path backup = root /
        boost::filesystem::unique_path(".previous-%%%%-%%%%");
    bool backup_created = false;
    bool published = false;
    try {
        if (!repository_root_accepts_id(repository_description_path(description.id),
                                        m_adapter.package_type(), description.id, error_message))
            return false;
        boost::filesystem::create_directories(root);
        if (!m_adapter.stage(source, source_type, description, version, staging, error_message) ||
            !m_adapter.validate(staging, description, version, error_message)) {
            boost::filesystem::remove_all(staging);
            return false;
        }

        const bool replacing = boost::filesystem::exists(destination);
        if (replacing) {
            boost::filesystem::rename(destination, backup);
            backup_created = true;
        }
        try {
            boost::filesystem::rename(staging, destination);
            published = true;
        } catch (...) {
            if (replacing && !boost::filesystem::exists(destination))
                boost::filesystem::rename(backup, destination);
            throw;
        }
        if (!refresh_repository_description(description.id, description, version, error_message)) {
            // The root descriptor is part of publication. Restore the previous
            // exact version when it existed, or remove the newly added version,
            // so callers never observe a half-published package.
            boost::filesystem::remove_all(destination);
            if (replacing)
                boost::filesystem::rename(backup, destination);
            return false;
        }
        if (replacing) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove_all(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous cached version '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        cached.description = description;
        cached.version = version;
        cached.directory = destination;
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::system::error_code ignored_error;
        if (published)
            boost::filesystem::remove_all(destination, ignored_error);
        if (backup_created && !boost::filesystem::exists(destination)) {
            try {
                boost::filesystem::rename(backup, destination);
            } catch (const boost::filesystem::filesystem_error &restore_error) {
                BOOST_LOG_TRIVIAL(error) << "Cannot restore cached repository version '"
                                         << destination.string() << "': " << restore_error.what();
            }
        }
        boost::filesystem::remove_all(staging, ignored_error);
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::refresh_repository_description(
    const std::string &id,
    const RepositoryDescription &fallback,
    const RepositoryPackageVersion &fallback_version,
    std::string &error_message) const
{
    RepositoryDescription selected = fallback;
    RepositoryPackageVersion selected_version = fallback_version;
    RepositoryDescription existing;
    std::string contents;
    const boost::filesystem::path descriptor = repository_description_path(id);
    if (read_file(descriptor, contents)) {
        std::string parse_error;
        if (parse_repository_description(contents, m_adapter.package_type(), existing, parse_error) &&
            !existing.config_update_rest.empty() && selected.config_update_rest.empty())
            selected.config_update_rest = existing.config_update_rest;
    }

    const boost::filesystem::path root = repository_directory(id);
    for (boost::filesystem::directory_iterator it(root), end; it != end; ++it) {
        if (!boost::filesystem::is_directory(it->path()))
            continue;
        std::string package_version;
        std::string slicer_version;
        if (!parse_version_directory_name(it->path().filename().string(), package_version, slicer_version))
            continue;
        std::string version_contents;
        RepositoryDescription candidate;
        std::string parse_error;
        if (!read_file(it->path() / DESCRIPTION_FILENAME, version_contents) ||
            !parse_repository_description(version_contents, m_adapter.package_type(), candidate, parse_error))
            continue;
        RepositoryPackageVersion candidate_version;
        candidate_version.package_version = package_version;
        candidate_version.slicer_version = slicer_version;
        if (!m_adapter.validate(it->path(), candidate, candidate_version, parse_error))
            continue;
        if (newer_version(candidate_version, selected_version)) {
            const std::string preserved_url = selected.config_update_rest;
            selected = std::move(candidate);
            selected_version = std::move(candidate_version);
            if (selected.config_update_rest.empty())
                selected.config_update_rest = preserved_url;
        }
    }
    return save_repository_description(selected, error_message);
}

std::vector<RepositoryCachedEntry> RepositoryPackageCache::scan() const
{
    std::vector<RepositoryCachedEntry> entries;
    if (!boost::filesystem::is_directory(type_directory()))
        return entries;

    try {
        for (boost::filesystem::directory_iterator root_it(type_directory()), root_end;
             root_it != root_end; ++root_it) {
            if (!boost::filesystem::is_directory(root_it->path()) ||
                root_it->path().filename().string().front() == '.')
                continue;

            const std::string filesystem_id = root_it->path().filename().string();
            std::string contents;
            RepositoryDescription root_description;
            std::string error_message;
            if (!read_file(root_it->path() / DESCRIPTION_FILENAME, contents) ||
                !parse_repository_description(contents, m_adapter.package_type(), root_description, error_message) ||
                safe_id(root_description.id) != filesystem_id) {
                if (error_message.empty())
                    error_message = "Repository descriptor id does not match its sanitized directory name.";
                BOOST_LOG_TRIVIAL(warning) << "Ignoring repository cache '" << root_it->path().string()
                                           << "': " << error_message;
                continue;
            }

            RepositoryCachedEntry entry;
            entry.description = root_description;
            entry.directory = root_it->path();
            for (boost::filesystem::directory_iterator version_it(root_it->path()), version_end;
                 version_it != version_end; ++version_it) {
                if (!boost::filesystem::is_directory(version_it->path()))
                    continue;
                std::string package_version;
                std::string slicer_version;
                if (!parse_version_directory_name(version_it->path().filename().string(),
                                                  package_version, slicer_version))
                    continue;
                RepositoryDescription version_description;
                std::string version_contents;
                if (!read_file(version_it->path() / DESCRIPTION_FILENAME, version_contents) ||
                    !parse_repository_description(version_contents, m_adapter.package_type(),
                                                  version_description, error_message) ||
                    version_description.id != root_description.id ||
                    !m_adapter.validate(version_it->path(), version_description,
                                        RepositoryPackageVersion{package_version, slicer_version}, error_message)) {
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring repository package '"
                                               << version_it->path().string() << "': " << error_message;
                    continue;
                }
                RepositoryPackageVersion version;
                version.package_version = std::move(package_version);
                version.slicer_version = std::move(slicer_version);
                entry.versions.push_back({std::move(version_description), std::move(version), version_it->path()});
            }
            std::sort(entry.versions.begin(), entry.versions.end(),
                      [](const RepositoryCachedVersion &left, const RepositoryCachedVersion &right) {
                          return newer_version(left.version, right.version);
                      });
            entries.emplace_back(std::move(entry));
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot finish scanning repository cache '"
                                   << type_directory().string() << "': " << error.what();
    }
    return entries;
}

std::string RepositoryPackageCache::safe_id(const std::string &id)
{
    std::string safe;
    safe.reserve(id.size());
    for (const unsigned char character : id)
        safe.push_back(character < 0x80 && (std::isalnum(character) || character == '.' ||
                                           character == '_' || character == '-') ?
                       static_cast<char>(character) : '-');
    while (safe.find("..") != std::string::npos)
        safe.replace(safe.find(".."), 2, "--");
    if (safe.empty() || safe == "." || safe == "..")
        safe = "repository";

    // Leading/trailing dots and DOS device names are not portable directory
    // names. Prefixing keeps sanitization deterministic without changing the
    // raw id stored in repository descriptors.
    if (safe.front() == '.' || safe.back() == '.')
        safe.insert(safe.begin(), '_');
    std::string uppercase = safe;
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    const bool reserved_device = uppercase == "CON" || uppercase == "PRN" || uppercase == "AUX" ||
                                 uppercase == "NUL" ||
                                 (uppercase.size() == 4 &&
                                  (uppercase.compare(0, 3, "COM") == 0 || uppercase.compare(0, 3, "LPT") == 0) &&
                                  uppercase[3] >= '1' && uppercase[3] <= '9');
    if (reserved_device)
        safe.insert(safe.begin(), '_');
    return safe;
}

std::string RepositoryPackageCache::version_directory_name(const std::string &package_version,
                                                           const std::string &slicer_version)
{
    return package_version + '=' + slicer_version;
}

const RepositoryPackageCacheAdapter &vendor_repository_cache_adapter()
{
    static const VendorRepositoryPackageCacheAdapter adapter;
    return adapter;
}

const RepositoryPackageCacheAdapter &plugin_repository_cache_adapter()
{
    static const PluginRepositoryPackageCacheAdapter adapter;
    return adapter;
}

} // namespace Slic3r
