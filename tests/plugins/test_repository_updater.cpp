///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// These tests keep the updater data model independent from wxWidgets. They
// verify the common success/error result and that a vendor repository keeps
// local metadata while enriching it with data returned by the tag endpoint.

#include <catch2/catch.hpp>

#include <string>

#include "libslic3r/Updater/PresetUpdater.hpp"
#include "libslic3r/Updater/UpdaterError.hpp"

TEST_CASE("UpdaterError explicitly represents success and failure", "[plugins][updater]")
{
    const Slic3r::UpdaterError success;
    CHECK(success.succeeded());

    Slic3r::UpdaterError failure;
    failure.code = Slic3r::UpdaterError::Code::Network;
    failure.detail = "connection refused";
    CHECK_FALSE(failure.succeeded());
    CHECK(failure.detail == "connection refused");
}

TEST_CASE("VendorSync enriches matching cached versions from repository tags", "[plugins][updater]")
{
    Slic3r::VendorSync vendor;
    Slic3r::VendorAvailable cached;
    cached.config_version = *Slic3r::Semver::parse("1.2.3.4");
    cached.slicer_version = *Slic3r::Semver::parse("2.7.63.0");
    cached.local_file = "cached.ini";
    cached.tag = "1.2.3.4=2.7.63.0";
    vendor.available_profiles.emplace_back(cached);

    REQUIRE(vendor.parse_tags(
        "[{\"name\":\"1.2.3.4=2.7.63.0\",\"zipball_url\":\"zip\","
        "\"commit\":{\"sha\":\"sha\",\"url\":\"commit\"}}]"));
    REQUIRE(vendor.available_profiles.size() == 1);
    CHECK(vendor.available_profiles.front().local_file == "cached.ini");
    CHECK(vendor.available_profiles.front().url_zip == "zip");
    CHECK(vendor.available_profiles.front().commit_sha == "sha");
    CHECK(vendor.available_profiles.front().commit_url == "commit");
}
