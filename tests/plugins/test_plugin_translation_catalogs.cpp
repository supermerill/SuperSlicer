#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

TEST_CASE("Plugin translation catalogs validate package-relative directories", "[plugins][translation]")
{
    Slic3r::Orchestrator &orchestrator = Slic3r::Orchestrator::instance();
    const boost::filesystem::path package_root =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("slic3r-translation-%%%%-%%%%");
    const boost::filesystem::path locale_directory = package_root / "locale";
    const boost::filesystem::path alternate_directory = package_root / "alternate";
    boost::filesystem::create_directories(locale_directory);
    boost::filesystem::create_directories(alternate_directory);

    const std::string domain = "tests.translation." + boost::filesystem::unique_path("%%%%-%%%%").string();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);

    {
        // The loader owns this scope in production. The test establishes the
        // same package root so the C entry point can validate relative paths.
        Slic3r::Orchestrator::PluginRegistrationScope scope =
            orchestrator.plugin_registration_scope(package_root.string(), true);
        CHECK(orchestrator_register_translation_catalog(handle, nullptr, "locale") == -1);
        CHECK(orchestrator_register_translation_catalog(handle, "", "locale") == -1);
        CHECK(orchestrator_register_translation_catalog(handle, domain.c_str(), nullptr) == -1);
        CHECK(orchestrator_register_translation_catalog(handle, "tests.translation.absolute",
                                                         locale_directory.string().c_str()) == -1);
        CHECK(orchestrator_register_translation_catalog(handle, domain.c_str(), "locale") == 1);
        CHECK(orchestrator_register_translation_catalog(handle, domain.c_str(), "locale") == 0);
        CHECK(orchestrator_register_translation_catalog(handle, domain.c_str(), "alternate") == -2);
        CHECK(orchestrator_register_translation_catalog(handle, "invalid/domain", "locale") == -1);
        CHECK(orchestrator_register_translation_catalog(handle, "tests.translation.escape", "../outside") == -1);
    }

    const std::vector<Slic3r::Orchestrator::TranslationCatalog> &catalogs = orchestrator.translation_catalogs();
    const Slic3r::Orchestrator::TranslationCatalog *registered = nullptr;
    for (const Slic3r::Orchestrator::TranslationCatalog &catalog : catalogs)
        if (catalog.domain == domain)
            registered = &catalog;

    REQUIRE(registered != nullptr);
    CHECK(registered->locale_directory == locale_directory.generic_string());
    CHECK(registered->package_root == package_root.generic_string());

    CHECK(orchestrator_register_translation_catalog(handle, "tests.translation.no_scope", "locale") == -1);

    boost::filesystem::remove_all(package_root);
}
