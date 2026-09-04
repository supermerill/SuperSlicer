// Central source of compatibility decisions for installation and execution.
// Read all C contracts only here, not in every consumer. INI parsing preserves
// header names literally and never treats the informational slicer version as
// an ABI constraint.
#include "PluginApiCompatibility.hpp"
#include <algorithm>
#include <charconv>
#include <iterator>
#include <set>
#include <sstream>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include "libslic3r/Api/plugin/c/slic3r_bridge_detector.h"
#include "libslic3r/Api/plugin/c/slic3r_clipper.h"
#include "libslic3r/Api/plugin/c/slic3r_config.h"
#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_config_option.h"
#include "libslic3r/Api/plugin/c/slic3r_config_option_type.h"
#include "libslic3r/Api/plugin/c/slic3r_config_types.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/slic3r_def.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_polyline.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusions.h"
#include "libslic3r/Api/plugin/c/slic3r_gcode_firmware.h"
#include "libslic3r/Api/plugin/c/slic3r_gcode_script.h"
#include "libslic3r/Api/plugin/c/slic3r_geometry.h"
#include "libslic3r/Api/plugin/c/slic3r_plugin.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_plugin_run_context.h"
#include "libslic3r/Api/plugin/c/slic3r_printing_plan.h"
#include "libslic3r/Api/plugin/c/slic3r_slicing_step.h"
#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/c/slic3r_volume.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_bridge_detector.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_common.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_simplification.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode_firmware.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill_group.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_height.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_stiching.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_ordering.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_infill.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_perimeter.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_slicing.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_pre_gcode.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_pre_infill.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_pre_perimeter.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_seam_placer.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_slicing.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_support.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_support_demand.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_support_spot.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_wipetower.h"

namespace Slic3r {
namespace {
// Parse exactly two decimal uint16 components, without signs or suffixes.
bool parse_api_version(const std::string &text, slic3r_major_minor_version &version);
// Format the version consistently in UI and native-loader diagnostics.
std::string version_text(slic3r_major_minor_version version);

bool parse_api_version(const std::string &text, slic3r_major_minor_version &version)
{
    const size_t dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size())
        return false;
    unsigned major = 0, minor = 0;
    const char *begin = text.data();
    const std::from_chars_result a = std::from_chars(begin, begin + dot, major);
    const std::from_chars_result b = std::from_chars(begin + dot + 1, begin + text.size(), minor);
    if (a.ec != std::errc() || a.ptr != begin + dot ||
        b.ec != std::errc() || b.ptr != begin + text.size() || major > 65535 || minor > 65535)
        return false;
    version = {uint16_t(major), uint16_t(minor)};
    return true;
}
std::string version_text(slic3r_major_minor_version version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor);
}
}

const std::vector<PluginApiRequirement> &host_plugin_api_contracts()
{
    static_assert(SLIC3R_PLUGIN_API_PLUGIN_TYPES == 0, "Vtable contract must be first.");
    static const std::vector<PluginApiRequirement> contracts = [] {
        std::vector<PluginApiRequirement> result(SLIC3R_PLUGIN_API_COUNT);
#define SLIC3R_PLUGIN_API_VERSION_ENTRY(name, header) \
        result[SLIC3R_PLUGIN_API_##name] = {header, {uint16_t(SLIC3R_PLUGIN_API_##name##_MAJOR), uint16_t(SLIC3R_PLUGIN_API_##name##_MINOR)}};
#include "libslic3r/Api/plugin/c/slic3r_plugin_api_version_entries.inc"
#undef SLIC3R_PLUGIN_API_VERSION_ENTRY
        return result;
    }();
    return contracts;
}

std::string PluginApiCompatibility::message() const
{
    std::string text;
    for (const PluginApiIssue &issue : issues) {
        if (!text.empty())
            text += "\n";
        text += issue.detail;
    }
    return text;
}

PluginApiCompatibility validate_plugin_api_requirements(const std::vector<PluginApiRequirement> &requirements)
{
    PluginApiCompatibility result;
    result.status = PluginApiCompatibilityStatus::Compatible;
    const std::vector<PluginApiRequirement> &host = host_plugin_api_contracts();
    bool has_vtable = false;
    std::set<std::string> seen;
    for (const PluginApiRequirement &entry : requirements) {
        if (entry.header == "slic3r_plugin_types.h" && entry.version.major != 0)
            has_vtable = true;
        if (!seen.insert(entry.header).second) {
            result.issues.push_back({entry.header, entry.version, std::nullopt, "Duplicate ABI contract: " + entry.header});
            continue;
        }
        if (entry.version.major == 0 && entry.version.minor == 0)
            continue;
        const std::vector<PluginApiRequirement>::const_iterator found = std::find_if(
            host.begin(), host.end(), [&entry](const PluginApiRequirement &candidate) { return candidate.header == entry.header; });
        if (found == host.end()) {
            result.issues.push_back({entry.header, entry.version, std::nullopt,
                "Unknown API header '" + entry.header + "' requires " + version_text(entry.version) + "."});
        } else if (entry.version.major == 0 || entry.version.major != found->version.major || entry.version.minor > found->version.minor) {
            result.issues.push_back({entry.header, entry.version, found->version,
                "API header '" + entry.header + "' requires " + version_text(entry.version) +
                ", but the host provides " + version_text(found->version) + "."});
        }
    }
    if (!has_vtable)
        result.issues.push_back({"slic3r_plugin_types.h", {}, host.front().version,
            "The mandatory contract 'slic3r_plugin_types.h' is missing or unused (0.0). "
            "The host provides " + version_text(host.front().version) +
            "; a nonzero requirement with the same major and a supported minor is required."});
    if (!result.issues.empty())
        result.status = PluginApiCompatibilityStatus::Incompatible;
    return result;
}

PluginApiCompatibility validate_plugin_api_table(const std::vector<slic3r_major_minor_version> &versions)
{
    const std::vector<PluginApiRequirement> &host = host_plugin_api_contracts();
    std::vector<PluginApiRequirement> requirements;
    requirements.reserve(versions.size());
    for (size_t idx = 0; idx < versions.size(); ++idx)
        requirements.push_back({idx < host.size() ? host[idx].header : "unknown-id-" + std::to_string(idx), versions[idx]});
    return validate_plugin_api_requirements(requirements);
}

bool parse_plugin_package_metadata(const std::string &text, PluginPackageMetadata &metadata, std::string &error)
{
    metadata = {};
    error.clear();
    try {
        // Parse ABI separately so a malformed/duplicate ABI entry does not
        // destroy the package identity needed to keep it inspectable in cache.
        // Section boundaries alone are scanned here; INI keys and values are
        // still parsed by property_tree, including duplicate-key rejection.
        std::ostringstream identity_text, abi_text;
        std::istringstream input(text);
        bool in_abi = false;
        size_t abi_sections = 0;
        std::string line;
        while (std::getline(input, line)) {
            const std::string trimmed = boost::algorithm::trim_copy(line);
            if (!trimmed.empty() && trimmed.front() == '[') {
                const size_t closing = trimmed.find(']');
                in_abi = closing != std::string::npos &&
                    boost::algorithm::trim_copy(trimmed.substr(1, closing - 1)) == "abi";
                if (in_abi)
                    ++abi_sections;
            }
            // Keep original line numbers in both streams for parser diagnostics.
            abi_text << (in_abi ? line : "") << '\n';
            identity_text << (in_abi ? "" : line) << '\n';
        }
        boost::property_tree::ptree tree;
        std::istringstream identity_input(identity_text.str());
        boost::property_tree::read_ini(identity_input, tree);
        const boost::property_tree::ptree &plugin = tree.get_child("plugin");
        metadata.package_version = plugin.get<std::string>("package_version", "");
        metadata.slicer_version = plugin.get<std::string>("slicer_version", "");
        if (abi_sections == 0) {
            metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
            metadata.compatibility.issues.push_back({"slic3r_plugin_types.h", {}, host_plugin_api_contracts().front().version,
                "Missing [abi] section in version.ini. It must declare the mandatory key "
                "'slic3r_plugin_types.h' and the API versions required by this package. "
                "Rebuild the package with ABI manifest generation enabled; slicer_version is not an ABI declaration."});
            return true;
        }
        if (abi_sections > 1) {
            metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
            metadata.compatibility.issues.push_back({{}, {}, std::nullopt, "Duplicate [abi] section."});
            return true;
        }
        boost::property_tree::ptree abi_tree;
        try {
            std::istringstream abi_input(abi_text.str());
            boost::property_tree::read_ini(abi_input, abi_tree);
        } catch (const boost::property_tree::ini_parser_error &exception) {
            // Include the offending key/value even when parsing stopped before
            // property_tree could construct an entry (for example duplicates).
            std::istringstream lines(text);
            std::string source;
            for (unsigned long idx = 0; idx < exception.line() && std::getline(lines, source); ++idx) {}
            metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
            metadata.compatibility.issues.push_back({{}, {}, std::nullopt,
                "Malformed [abi] section at line " + std::to_string(exception.line()) +
                ": " + exception.message() + ". Source: " + source});
            return true;
        }
        const boost::optional<boost::property_tree::ptree &> abi = abi_tree.get_child_optional("abi");
        if (abi) {
            // Iteration is intentional: get("slic3r_plugin_types.h") would split
            // the filename at its dot instead of reading the literal INI key.
            for (const boost::property_tree::ptree::value_type &entry : *abi) {
                slic3r_major_minor_version version{};
                if (!parse_api_version(entry.second.get_value<std::string>(), version)) {
                    metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
                    metadata.compatibility.issues.push_back({entry.first, {}, std::nullopt,
                        "Invalid value '" + entry.second.get_value<std::string>() + "' for [abi] key '" + entry.first +
                        "'. Expected major.minor, with two decimal integers between 0 and 65535 (for example 1.0)."});
                    return true;
                }
                metadata.abi.push_back({entry.first, version});
            }
        }
        metadata.compatibility = validate_plugin_api_requirements(metadata.abi);
        for (PluginApiIssue &issue : metadata.compatibility.issues)
            issue.detail = "Section [abi], key '" + issue.header + "': " + issue.detail;
        return true;
    } catch (const std::exception &exception) {
        error = "Cannot read plugin version.ini: " + std::string(exception.what());
        metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
        metadata.compatibility.issues.push_back({{}, {}, std::nullopt, error});
        return false;
    }
}

bool read_plugin_package_metadata(const std::string &path, PluginPackageMetadata &metadata, std::string &error)
{
    boost::nowide::ifstream input(path);
    if (!input) {
        metadata = {};
        error = "Cannot read plugin version.ini: " + path;
        metadata.compatibility.status = PluginApiCompatibilityStatus::Incompatible;
        metadata.compatibility.issues.push_back({{}, {}, std::nullopt, error});
        return false;
    }
    const bool parsed = parse_plugin_package_metadata(std::string(std::istreambuf_iterator<char>(input), {}), metadata, error);
    // Preserve the actual installed/cache file location through metadata copies
    // so tooltips and installation errors identify the manifest to investigate.
    const std::string location = "Manifest: " + path + "\n";
    for (PluginApiIssue &issue : metadata.compatibility.issues)
        issue.detail = location + issue.detail;
    if (!error.empty())
        error = location + error;
    return parsed;
}
}
