///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests build minimal installed-package directories and pass them to the
// production loader. They verify that failures remain structured data suitable
// for Plugin updates instead of existing only as startup log messages.

#include <catch2/catch.hpp>

#include <string>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Plugins/PluginLoader.hpp"

namespace {

const char *plugin_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

void write_package_metadata(const boost::filesystem::path &package_root,
                            const std::string &package_id)
{
    boost::filesystem::create_directories(package_root);
    {
        boost::nowide::ofstream stream((package_root / "description.ini").string());
        stream << "[plugin]\nid = " << package_id << "\nname = " << package_id
               << "\nfull_name = " << package_id << "\nslicer = SuperSlicer\n";
    }
    {
        boost::nowide::ofstream stream((package_root / "version.ini").string());
        stream << "[plugin]\npackage_version = 1.0.0\nslicer_version = 2.7.63.0\n";
    }
}

const Slic3r::PluginPackageLoadReport &load_fixture(const boost::filesystem::path &library,
                                                    const std::string &package_id)
{
    const boost::filesystem::path repository = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("slic3r-plugin-loader-%%%%-%%%%");
    const boost::filesystem::path package_root = repository / package_id;
    write_package_metadata(package_root, package_id);
    boost::filesystem::copy_file(library, package_root / plugin_library_filename());

    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.clear_plugin_package_load_reports();
    Slic3r::load_plugin_packages_from_repository(repository, orchestrator);
    const Slic3r::PluginPackageLoadReport *report = orchestrator.plugin_package_load_report(package_id);
    REQUIRE(report != nullptr);

    // Failed libraries are unloaded before the loader returns, so the fixture
    // repository can be removed immediately even on Windows.
    boost::filesystem::remove_all(repository);
    return *report;
}

} // namespace

TEST_CASE("Plugin loader reports incompatible package ABI",
          "[plugins][loader][diagnostics]")
{
    const Slic3r::PluginPackageLoadReport &report = load_fixture(
        boost::filesystem::path(SLIC3R_TEST_PLUGIN_BAD_ABI_DLL), "diagnostic.bad_abi");
    REQUIRE(report.issues.size() == 1);
    CHECK(report.state == Slic3r::PluginPackageLoadState::Failed);
    CHECK(report.issues.front().code == Slic3r::PluginPackageLoadErrorCode::AbiMismatch);
    CHECK(report.issues.front().plugin_abi != report.issues.front().host_abi);
}

TEST_CASE("Plugin loader reports a missing registration export",
          "[plugins][loader][diagnostics]")
{
    const Slic3r::PluginPackageLoadReport &report = load_fixture(
        boost::filesystem::path(SLIC3R_TEST_PLUGIN_MISSING_REGISTER_DLL), "diagnostic.no_register");
    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == Slic3r::PluginPackageLoadErrorCode::MissingRegistrationExport);
}

TEST_CASE("Plugin loader reports a missing ABI export",
          "[plugins][loader][diagnostics]")
{
    const Slic3r::PluginPackageLoadReport &report = load_fixture(
        boost::filesystem::path(SLIC3R_TEST_PLUGIN_MISSING_ABI_DLL), "diagnostic.no_abi");
    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == Slic3r::PluginPackageLoadErrorCode::MissingAbiExport);
}

TEST_CASE("Plugin loader reports a registration exception",
          "[plugins][loader][diagnostics]")
{
    const Slic3r::PluginPackageLoadReport &report = load_fixture(
        boost::filesystem::path(SLIC3R_TEST_PLUGIN_THROW_REGISTER_DLL), "diagnostic.throw_register");
    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == Slic3r::PluginPackageLoadErrorCode::RegistrationFailed);
    CHECK(report.issues.front().detail.find("Registration fixture failure") != std::string::npos);
}

TEST_CASE("Plugin loader reports an invalid native library",
          "[plugins][loader][diagnostics]")
{
    const boost::filesystem::path invalid_library = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("invalid-plugin-%%%%.bin");
    {
        boost::nowide::ofstream stream(invalid_library.string(), std::ios::binary);
        stream << "not a dynamic library";
    }
    const Slic3r::PluginPackageLoadReport &report = load_fixture(invalid_library, "diagnostic.invalid_library");
    boost::filesystem::remove(invalid_library);
    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == Slic3r::PluginPackageLoadErrorCode::LibraryOpenFailed);
    CHECK_FALSE(report.issues.front().detail.empty());
}

TEST_CASE("Plugin load report distinguishes empty and partial registration",
          "[plugins][loader][diagnostics]")
{
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.clear_plugin_package_load_reports();
    orchestrator.begin_plugin_package_load("diagnostic.empty", "plugins/diagnostic.empty");
    orchestrator.finish_plugin_package_load("diagnostic.empty");
    const Slic3r::PluginPackageLoadReport *empty =
        orchestrator.plugin_package_load_report("diagnostic.empty");
    REQUIRE(empty != nullptr);
    REQUIRE(empty->issues.size() == 1);
    CHECK(empty->issues.front().code == Slic3r::PluginPackageLoadErrorCode::NoPluginsRegistered);

    orchestrator.begin_plugin_package_load("diagnostic.partial", "plugins/diagnostic.partial");
    Slic3r::PluginPackageLoadReport *partial = const_cast<Slic3r::PluginPackageLoadReport *>(
        orchestrator.plugin_package_load_report("diagnostic.partial"));
    REQUIRE(partial != nullptr);
    partial->registered_plugin_ids.push_back("diagnostic.partial.loaded");
    Slic3r::PluginPackageLoadIssue issue;
    issue.code = Slic3r::PluginPackageLoadErrorCode::RegistrationFailed;
    issue.plugin_id = "diagnostic.partial.missing";
    issue.detail = "The second plugin registration failed.";
    orchestrator.report_plugin_package_load_issue("diagnostic.partial", std::move(issue));
    orchestrator.finish_plugin_package_load("diagnostic.partial");
    CHECK(partial->state == Slic3r::PluginPackageLoadState::LoadedWithErrors);
}

TEST_CASE("Secondary runtimes report package errors through the host ABI",
          "[plugins][loader][diagnostics][python]")
{
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.clear_plugin_package_load_reports();
    const std::string package_root = "plugins/diagnostic.python";
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    orchestrator_begin_plugin_package_load(handle, package_root.c_str());
    orchestrator_report_plugin_package_load_error(
        handle, package_root.c_str(), RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_COMPILE_FAILED,
        "SyntaxError: invalid syntax\n  File plugin.py, line 3");
    orchestrator_finish_plugin_package_load(handle, package_root.c_str());

    const Slic3r::PluginPackageLoadReport *report =
        orchestrator.plugin_package_load_report("diagnostic.python");
    REQUIRE(report != nullptr);
    REQUIRE(report->issues.size() == 1);
    CHECK(report->issues.front().code == Slic3r::PluginPackageLoadErrorCode::PythonCompileFailed);
    CHECK(report->issues.front().detail.find("SyntaxError") != std::string::npos);
}

TEST_CASE("Python package reports an unavailable runtime",
          "[plugins][loader][diagnostics][python]")
{
    const boost::filesystem::path repository = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("slic3r-python-runtime-missing-%%%%-%%%%");
    const std::string package_id = "diagnostic.python.runtime_missing";
    const boost::filesystem::path package_root = repository / package_id;
    write_package_metadata(package_root, package_id);
    {
        boost::nowide::ofstream stream((package_root / "plugin.py").string());
        stream << "def register_plugin(api):\n    return None\n";
    }

    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    orchestrator.clear_plugin_package_load_reports();
    Slic3r::load_plugin_packages_from_repository(repository, orchestrator);

    const Slic3r::PluginPackageLoadReport *report =
        orchestrator.plugin_package_load_report(package_id);
    REQUIRE(report != nullptr);
    REQUIRE(report->issues.size() == 1);
    CHECK(report->issues.front().code ==
          Slic3r::PluginPackageLoadErrorCode::PythonRuntimeUnavailable);
    boost::filesystem::remove_all(repository);
}
