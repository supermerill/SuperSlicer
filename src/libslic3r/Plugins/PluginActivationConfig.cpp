///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file implements the integrity boundary around plugins/activated.ini.
// Parsing first builds a temporary document, then either returns a complete
// valid value or a sanitized value with diagnostics. Publication follows the
// inverse rule: a complete staging file is closed before the previous file is
// moved aside, and rollback remains possible until the replacement succeeds.

#include "PluginActivationConfig.hpp"

#include <cctype>
#include <exception>
#include <istream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/FilesystemTransaction.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

const char *const PLUGIN_DIRECTORY = "plugins";
const char *const ACTIVATED_PLUGINS_FILENAME = "activated.ini";
const char *const DEFAULT_ACTIVATED_PLUGINS_FILENAME = "default_activated.ini";

struct ActivationIniEntry {
    std::string section;
    std::string key;
    std::string value;
};

struct ActivationIniDocument {
    std::vector<ActivationIniEntry> entries;
    std::set<std::string> sections;
};

// Parse one activation token while rejecting values which would otherwise be
// silently interpreted as disabled.
bool parse_ini_enabled_value(const std::string &value, bool &enabled);
// Read the small INI grammar used by activated.ini and retain duplicate keys
// so semantic validation can report them instead of aborting at the first one.
bool parse_activation_ini(std::istream &stream,
                          ActivationIniDocument &document,
                          std::string &error_message);
// Flatten structured issues only for strict callers and logs. The tolerant
// reader preserves the original fields for the GUI.
std::string format_activation_config_issues(const boost::filesystem::path &config_path,
                                            const std::vector<PluginActivationConfigIssue> &issues);
// Locate the generated resource default independently from the user data path.
boost::filesystem::path default_plugin_activation_config_path();
// Copy a complete source through a sibling staging path before publication.
bool replace_file_with_copy(const boost::filesystem::path &source,
                            const boost::filesystem::path &destination,
                            std::string &error_message);
// Commit one prepared configuration staging and report cleanup warnings.
bool commit_plugin_activation_staging(FilesystemTransaction &transaction,
                                      const boost::filesystem::path &destination,
                                      std::string &error_message);

bool parse_ini_enabled_value(const std::string &value, bool &enabled)
{
    if (boost::algorithm::iequals(value, "1") || boost::algorithm::iequals(value, "true") ||
        boost::algorithm::iequals(value, "yes") || boost::algorithm::iequals(value, "on") ||
        boost::algorithm::iequals(value, "enabled")) {
        enabled = true;
        return true;
    }
    if (boost::algorithm::iequals(value, "0") || boost::algorithm::iequals(value, "false") ||
        boost::algorithm::iequals(value, "no") || boost::algorithm::iequals(value, "off") ||
        boost::algorithm::iequals(value, "disabled")) {
        enabled = false;
        return true;
    }
    return false;
}

bool parse_activation_ini(std::istream &stream,
                          ActivationIniDocument &document,
                          std::string &error_message)
{
    document = {};
    std::string current_section;
    std::string line;
    size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::string trimmed = boost::algorithm::trim_copy(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#')
            continue;

        // A malformed section changes the meaning of every following entry,
        // so it is a structural error rather than a recoverable entry issue.
        if (trimmed.front() == '[') {
            const size_t closing_bracket = trimmed.find(']');
            const std::string trailing = closing_bracket == std::string::npos ? std::string() :
                boost::algorithm::trim_copy(trimmed.substr(closing_bracket + 1));
            if (closing_bracket == std::string::npos || closing_bracket == 1 ||
                (!trailing.empty() && trailing.front() != ';' && trailing.front() != '#')) {
                error_message = "Invalid section declaration at line " + std::to_string(line_number) + ".";
                return false;
            }
            current_section = boost::algorithm::trim_copy(trimmed.substr(1, closing_bracket - 1));
            if (current_section.empty()) {
                error_message = "Empty section name at line " + std::to_string(line_number) + ".";
                return false;
            }
            if (!document.sections.insert(current_section).second) {
                error_message = "Duplicate section ['" + current_section + "'] at line " +
                                std::to_string(line_number) + ".";
                return false;
            }
            continue;
        }

        const size_t separator = trimmed.find('=');
        if (current_section.empty() || separator == std::string::npos) {
            error_message = "Invalid assignment at line " + std::to_string(line_number) + ".";
            return false;
        }
        const std::string key = boost::algorithm::trim_copy(trimmed.substr(0, separator));
        if (key.empty()) {
            error_message = "Empty key at line " + std::to_string(line_number) + ".";
            return false;
        }
        document.entries.push_back(ActivationIniEntry{
            current_section, key, boost::algorithm::trim_copy(trimmed.substr(separator + 1))});
    }
    if (!stream.eof()) {
        error_message = "Cannot finish reading the activation configuration.";
        return false;
    }
    return true;
}

std::string format_activation_config_issues(const boost::filesystem::path &config_path,
                                            const std::vector<PluginActivationConfigIssue> &issues)
{
    std::ostringstream message;
    message << "Plugin configuration '" << config_path.string() << "' contains invalid entries:";
    for (const PluginActivationConfigIssue &issue : issues) {
        message << " [" << issue.section << "]";
        if (!issue.key.empty())
            message << " " << issue.key;
        message << ": " << issue.reason;
    }
    return message.str();
}

boost::filesystem::path default_plugin_activation_config_path()
{
    return boost::filesystem::path(resources_dir()) / PLUGIN_DIRECTORY / DEFAULT_ACTIVATED_PLUGINS_FILENAME;
}

bool replace_file_with_copy(const boost::filesystem::path &source,
                            const boost::filesystem::path &destination,
                            std::string &error_message)
{
    const boost::filesystem::path parent = destination.parent_path();
    const std::string filename = destination.filename().string();
    const boost::filesystem::path staging = parent /
        boost::filesystem::unique_path("." + filename + ".replacement-%%%%-%%%%");
    FilesystemTransaction transaction;
    transaction.add_replacement(staging, destination);

    try {
        if (!parent.empty())
            boost::filesystem::create_directories(parent);
        boost::filesystem::copy_file(source, staging);
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot stage plugin configuration '" + destination.string() + "': " + error.what();
        return false;
    }

    return commit_plugin_activation_staging(transaction, destination, error_message);
}

bool commit_plugin_activation_staging(FilesystemTransaction &transaction,
                                      const boost::filesystem::path &destination,
                                      std::string &error_message)
{
    try {
        // This application file may be absent or regular, but a directory at
        // the same path is configuration damage rather than replaceable data.
        if (boost::filesystem::exists(destination) && !boost::filesystem::is_regular_file(destination)) {
            error_message = "Cannot replace plugin configuration '" + destination.string() +
                            "' because it is not a regular file.";
            return false;
        }

        const FilesystemTransactionResult result = transaction.commit();
        for (const FilesystemTransactionFailure &warning : result.cleanup_warnings)
            BOOST_LOG_TRIVIAL(warning) << "Plugin activation configuration cleanup warning: "
                                       << format_filesystem_transaction_failure(warning);
        if (result.status != FilesystemTransactionStatus::Committed) {
            error_message = "Cannot replace plugin configuration '" + destination.string() + "': " +
                            format_filesystem_transaction_error(result);
            return false;
        }
        error_message.clear();
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot replace plugin configuration '" + destination.string() + "': " + error.what();
        return false;
    }
}

} // namespace

bool is_valid_plugin_package_name(const std::string &package_name)
{
    if (package_name.empty() || package_name.front() == '.' || package_name == "." || package_name == ".." ||
        package_name.find("..") != std::string::npos)
        return false;

    for (const unsigned char character : package_name)
        if (character > 0x7f || (!std::isalnum(character) && character != '.' && character != '_' && character != '-'))
            return false;
    return true;
}

bool is_valid_plugin_package_version(const std::string &version)
{
    return Semver::parse(version).has_value();
}

bool migrate_plugin_activation_id(PluginActivationConfig &config,
                                  const std::string &obsolete_id,
                                  std::initializer_list<std::string> replacement_ids)
{
    bool changed = config.plugin_packages.erase(obsolete_id) > 0;
    const std::map<std::string, bool>::iterator obsolete = config.activated.find(obsolete_id);
    if (obsolete == config.activated.end())
        return changed;

    // Preserve explicit successor choices. A missing successor inherits the
    // old state, including false, before the default activations are merged.
    const bool enabled = obsolete->second;
    for (const std::string &replacement_id : replacement_ids)
        config.activated.emplace(replacement_id, enabled);
    config.activated.erase(obsolete);
    return true;
}

boost::filesystem::path plugin_activation_config_path(const boost::filesystem::path &data_directory)
{
    return data_directory / PLUGIN_DIRECTORY / ACTIVATED_PLUGINS_FILENAME;
}

PluginActivationConfigReadResult read_plugin_activation_config_tolerant(
    const boost::filesystem::path &config_path)
{
    PluginActivationConfigReadResult result;
    boost::nowide::ifstream stream(config_path.string());
    if (!stream) {
        result.error_message = "Cannot read plugin configuration '" + config_path.string() + "'.";
        return result;
    }

    ActivationIniDocument document;
    std::string syntax_error;
    if (!parse_activation_ini(stream, document, syntax_error)) {
        result.error_message = "Cannot parse plugin configuration '" + config_path.string() + "': " + syntax_error;
        return result;
    }
    if (document.sections.find("installed") == document.sections.end()) {
        result.issues.push_back(PluginActivationConfigIssue{
            "installed", std::string(), "The mandatory desired-package section is missing."});
        result.error_message = format_activation_config_issues(config_path, result.issues);
        return result;
    }

    std::map<std::string, size_t> installed_key_counts;
    std::map<std::string, size_t> activated_key_counts;
    std::map<std::string, size_t> package_key_counts;
    for (const ActivationIniEntry &entry : document.entries) {
        if (entry.section == "installed")
            ++installed_key_counts[entry.key];
        else if (entry.section == "activated")
            ++activated_key_counts[entry.key];
        else if (entry.section == "plugin_packages")
            ++package_key_counts[entry.key];
    }

    const std::string slicer_suffix = ".slicer_version";
    std::map<std::string, std::string> package_versions;
    std::map<std::string, std::string> slicer_versions;
    std::set<std::string> rejected_packages;
    std::set<std::string> reported_duplicate_installed;
    for (const ActivationIniEntry &entry : document.entries) {
        if (entry.section != "installed")
            continue;

        const bool is_slicer_entry = entry.key.size() > slicer_suffix.size() &&
            entry.key.compare(entry.key.size() - slicer_suffix.size(), slicer_suffix.size(), slicer_suffix) == 0;
        const std::string package_name = is_slicer_entry ?
            entry.key.substr(0, entry.key.size() - slicer_suffix.size()) : entry.key;
        if (installed_key_counts[entry.key] > 1) {
            if (reported_duplicate_installed.insert(entry.key).second)
                result.issues.push_back(PluginActivationConfigIssue{
                    "installed", entry.key, "The entry is duplicated and its value is ambiguous."});
            if (is_valid_plugin_package_name(package_name))
                rejected_packages.insert(package_name);
            continue;
        }
        if (!is_valid_plugin_package_name(package_name)) {
            result.issues.push_back(PluginActivationConfigIssue{
                "installed", entry.key, "The package name is not safe for a repository directory."});
            continue;
        }
        if (!is_valid_plugin_package_version(entry.value)) {
            result.issues.push_back(PluginActivationConfigIssue{
                "installed", entry.key, is_slicer_entry ?
                    "The slicer version is not a valid semantic version." :
                    "The package version is not a valid semantic version."});
            rejected_packages.insert(package_name);
            continue;
        }
        if (is_slicer_entry)
            slicer_versions.emplace(package_name, entry.value);
        else
            package_versions.emplace(package_name, entry.value);
    }

    // A compatibility version has no meaning without the package version it
    // qualifies, so both the orphan and its package are rejected together.
    for (const std::pair<const std::string, std::string> &entry : slicer_versions)
        if (package_versions.find(entry.first) == package_versions.end()) {
            result.issues.push_back(PluginActivationConfigIssue{
                "installed", entry.first + slicer_suffix,
                "The slicer version has no matching package-version entry."});
            rejected_packages.insert(entry.first);
        }

    for (const std::pair<const std::string, std::string> &entry : package_versions) {
        if (rejected_packages.find(entry.first) != rejected_packages.end())
            continue;
        const std::map<std::string, std::string>::const_iterator slicer = slicer_versions.find(entry.first);
        PluginInstalledVersion version;
        version.package_version = entry.second;
        // Older files contained one version because package and slicer versions
        // were identical when that format was produced.
        version.slicer_version = slicer == slicer_versions.end() ? entry.second : slicer->second;
        result.config.installed.emplace(entry.first, std::move(version));
    }

    std::set<std::string> reported_duplicate_activations;
    for (const ActivationIniEntry &entry : document.entries) {
        if (entry.section != "activated")
            continue;
        if (activated_key_counts[entry.key] > 1) {
            if (reported_duplicate_activations.insert(entry.key).second)
                result.issues.push_back(PluginActivationConfigIssue{
                    "activated", entry.key, "The activation entry is duplicated and ambiguous."});
            continue;
        }
        bool enabled = false;
        if (entry.key.empty() || !parse_ini_enabled_value(entry.value, enabled)) {
            result.issues.push_back(PluginActivationConfigIssue{
                "activated", entry.key, "The activation value is not recognized."});
            continue;
        }
        result.config.activated.emplace(entry.key, enabled);
    }

    std::set<std::string> reported_duplicate_packages;
    for (const ActivationIniEntry &entry : document.entries) {
        if (entry.section != "plugin_packages")
            continue;
        if (package_key_counts[entry.key] > 1) {
            if (reported_duplicate_packages.insert(entry.key).second)
                result.issues.push_back(PluginActivationConfigIssue{
                    "plugin_packages", entry.key, "The package association is duplicated and ambiguous."});
            continue;
        }
        if (entry.key.empty() || !is_valid_plugin_package_name(entry.value)) {
            result.issues.push_back(PluginActivationConfigIssue{
                "plugin_packages", entry.key, "The associated package name is invalid."});
            continue;
        }
        if (rejected_packages.find(entry.value) != rejected_packages.end()) {
            result.issues.push_back(PluginActivationConfigIssue{
                "plugin_packages", entry.key, "The associated package was rejected from [installed]."});
            result.config.activated.erase(entry.key);
            continue;
        }
        result.config.plugin_packages.emplace(entry.key, entry.value);
    }

    result.rejected_packages.assign(rejected_packages.begin(), rejected_packages.end());
    result.status = result.issues.empty() ? PluginActivationConfigStatus::Valid :
                                           PluginActivationConfigStatus::PartiallyValid;
    if (!result.issues.empty())
        result.error_message = format_activation_config_issues(config_path, result.issues);
    return result;
}

bool read_plugin_activation_config(const boost::filesystem::path &config_path,
                                   PluginActivationConfig &config,
                                   std::string &error_message)
{
    PluginActivationConfigReadResult result = read_plugin_activation_config_tolerant(config_path);
    if (result.status != PluginActivationConfigStatus::Valid) {
        config = {};
        error_message = std::move(result.error_message);
        return false;
    }
    config = std::move(result.config);
    error_message.clear();
    return true;
}

bool write_plugin_activation_config(const boost::filesystem::path &config_path,
                                    const PluginActivationConfig &config,
                                    std::string &error_message)
{
    const boost::filesystem::path parent = config_path.parent_path();
    const std::string filename = config_path.filename().string();
    const boost::filesystem::path staging = parent /
        boost::filesystem::unique_path("." + filename + ".replacement-%%%%-%%%%");
    FilesystemTransaction transaction;
    transaction.add_replacement(staging, config_path);

    try {
        if (!parent.empty())
            boost::filesystem::create_directories(parent);

        // The live file remains untouched until all sections have been written
        // and the sibling staging stream has closed successfully.
        boost::nowide::ofstream stream(staging.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot write plugin configuration '" + config_path.string() + "'.";
            return false;
        }
        stream << "[installed]\n";
        for (const auto &[package_name, version] : config.installed) {
            stream << package_name << " = " << version.package_version << "\n";
            stream << package_name << ".slicer_version = " << version.slicer_version << "\n";
        }
        stream << "\n[activated]\n";
        for (const auto &[plugin_id, enabled] : config.activated)
            stream << plugin_id << " = " << (enabled ? "1" : "0") << "\n";
        stream << "\n[plugin_packages]\n";
        for (const auto &[plugin_id, package_name] : config.plugin_packages)
            if (!plugin_id.empty() && is_valid_plugin_package_name(package_name))
                stream << plugin_id << " = " << package_name << "\n";
        stream.flush();
        if (!stream) {
            error_message = "Cannot finish writing plugin configuration '" + config_path.string() + "'.";
            stream.close();
            return false;
        }

        // Successful close confirms that no buffered output remains before the
        // staging file is allowed to replace the durable configuration.
        stream.close();
        if (!stream) {
            error_message = "Cannot close plugin configuration staging file '" + staging.string() + "'.";
            return false;
        }
    } catch (const std::exception &error) {
        error_message = "Cannot stage plugin configuration '" + config_path.string() + "': " + error.what();
        return false;
    }

    return commit_plugin_activation_staging(transaction, config_path, error_message);
}

bool replace_plugin_activation_config_with_defaults(const boost::filesystem::path &config_path,
                                                    std::string &error_message)
{
    const boost::filesystem::path default_config_path = default_plugin_activation_config_path();
    PluginActivationConfig default_config;
    std::string validation_error;

    // Validate resources before touching the damaged user file. The staged
    // copy remains byte-identical to the complete shipped configuration.
    if (!read_plugin_activation_config(default_config_path, default_config, validation_error)) {
        error_message = "Cannot use default plugin configuration '" + default_config_path.string() + "': " +
                        validation_error;
        return false;
    }
    return replace_file_with_copy(default_config_path, config_path, error_message);
}

bool ensure_plugin_activation_config(const boost::filesystem::path &data_directory,
                                     PluginActivationConfig &config,
                                     bool &from_user_config,
                                     std::string &error_message)
{
    from_user_config = false;
    if (data_directory.empty())
        return read_plugin_activation_config(default_plugin_activation_config_path(), config, error_message);

    const boost::filesystem::path config_path = plugin_activation_config_path(data_directory);
    try {
        // Initial creation uses the normal atomic publication protocol, so
        // startup observes either no file or one complete default file.
        if (!boost::filesystem::exists(config_path) &&
            !replace_file_with_copy(default_plugin_activation_config_path(), config_path, error_message))
            return false;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin configuration '" + config_path.string() + "': " + error.what();
        return false;
    }
    from_user_config = true;
    return read_plugin_activation_config(config_path, config, error_message);
}

} // namespace Slic3r
