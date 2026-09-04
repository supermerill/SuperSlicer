// Validate the common ABI rules without loading a library. Package parsing
// must preserve literal header paths and never infer ABI from build versions.
#include <catch2/catch.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include "libslic3r/Plugins/PluginApiCompatibility.hpp"

using namespace Slic3r;

TEST_CASE("Package ABI diagnostics identify the manifest section key and invalid value", "[plugins][abi][repository]")
{
    const std::string declaration = GENERATE(std::string(""),
        std::string("[abi]\n"),
        std::string("[abi]\nslic3r_plugin_types.h=bad.version\n"),
        std::string("[abi]\nslic3r_plugin_types.h=1.0\nslic3r_plugin_types.h=1.0\n"));
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("plugin-version-%%%%-%%%%.ini");
    {
        boost::nowide::ofstream stream(path.string());
        stream << "[plugin]\npackage_version=1.0.0\nslicer_version=1.0.0\n" << declaration;
    }
    PluginPackageMetadata metadata;
    std::string error;
    const bool parsed = read_plugin_package_metadata(path.string(), metadata, error);
    boost::filesystem::remove(path);
    REQUIRE(parsed);
    const std::string diagnostic = metadata.compatibility.message();
    CHECK(diagnostic.find(path.string()) != std::string::npos);
    CHECK(diagnostic.find("[abi]") != std::string::npos);
    CHECK(diagnostic.find("slic3r_plugin_types.h") != std::string::npos);
    if (declaration.empty())
        CHECK(diagnostic.find("Missing [abi] section") != std::string::npos);
    if (declaration.find("bad.version") != std::string::npos) {
        CHECK(diagnostic.find("bad.version") != std::string::npos);
        CHECK(diagnostic.find("major.minor") != std::string::npos);
    }
    if (declaration.find("1.0") != std::string::npos) {
        CHECK(diagnostic.find("line 6") != std::string::npos);
        CHECK(diagnostic.find("slic3r_plugin_types.h=1.0") != std::string::npos);
    }
}

TEST_CASE("Package ABI uses literal header names and ignores the build version", "[plugins][abi][repository]")
{
    for (const std::string &slicer : {std::string("1.0.0"), std::string("9999.0.0")}) {
        PluginPackageMetadata metadata;
        std::string error;
        REQUIRE(parse_plugin_package_metadata("[plugin]\npackage_version=1.0.0\nslicer_version=" + slicer +
            "\n[abi]\nslic3r_plugin_types.h=1.0\nsteps/slic3r_step_post_perimeter.h=1.0\nfuture.h=0.0\n", metadata, error));
        CHECK(metadata.compatibility.compatible());
        REQUIRE(metadata.abi.size() == 3);
        CHECK(metadata.abi[1].header == "steps/slic3r_step_post_perimeter.h");
        CHECK(metadata.slicer_version == slicer);
    }
}

TEST_CASE("Package ABI rejects missing invalid and conflicting requirements", "[plugins][abi][repository]")
{
    const std::string abi = GENERATE(std::string(""), std::string("[abi]\n"),
        std::string("[abi]\nslic3r_plugin_types.h=0.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=2.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=1.65535\n"),
        std::string("[abi]\nslic3r_plugin_types.h=1.0\nfuture.h=1.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=1.0\nslic3r_plugin_types.h=1.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=1.0.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=-1.0\n"),
        std::string("[abi]\nslic3r_plugin_types.h=65536.0\n"));
    PluginPackageMetadata metadata;
    std::string error;
    parse_plugin_package_metadata("[plugin]\npackage_version=1.0.0\nslicer_version=1.0.0\n" + abi, metadata, error);
    CHECK(metadata.compatibility.status == PluginApiCompatibilityStatus::Incompatible);
    CHECK_FALSE(metadata.compatibility.message().empty());
}

TEST_CASE("DLL and package validators share the same contract registry", "[plugins][abi][loader]")
{
    const std::vector<PluginApiRequirement> &host = host_plugin_api_contracts();
    std::vector<slic3r_major_minor_version> table;
    for (const PluginApiRequirement &contract : host)
        table.push_back(contract.version);
    CHECK(validate_plugin_api_table(table).compatible());
    CHECK(validate_plugin_api_requirements(host).compatible());
    table.resize(1);
    CHECK(validate_plugin_api_table(table).compatible());
    table.resize(host.size() + 10);
    CHECK(validate_plugin_api_table(table).compatible());
    table.back() = {1, 0};
    CHECK_FALSE(validate_plugin_api_table(table).compatible());
    table.assign(1, {0, 0});
    CHECK_FALSE(validate_plugin_api_table(table).compatible());
    CHECK(PluginPackageMetadata{}.compatibility.status == PluginApiCompatibilityStatus::NotChecked);
}
