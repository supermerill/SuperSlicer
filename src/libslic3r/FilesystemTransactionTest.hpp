///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// This header exposes deterministic failure boundaries only to test builds.
// Production code must use FilesystemTransaction solely through its public
// result contract and must not depend on these callbacks.

#ifndef slic3r_FilesystemTransactionTest_hpp_
#define slic3r_FilesystemTransactionTest_hpp_

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING

#include <functional>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

enum class FilesystemTransactionTestPoint {
    BeforePreserveDestination,
    BeforePublishStaging,
    BeforeWithdrawPublishedReplacement,
    BeforeRestoreDestination,
    BeforeCleanupArtifact
};

using FilesystemTransactionTestHook = std::function<void(
    FilesystemTransactionTestPoint,
    const boost::filesystem::path &,
    const boost::filesystem::path &)>;

// Tests install one process-wide hook while executing synchronously. The hook
// is intentionally not thread-safe and must be cleared before a test ends.
void set_filesystem_transaction_test_hook(FilesystemTransactionTestHook hook);

} // namespace Slic3r

#endif // SLIC3R_FILESYSTEM_TRANSACTION_TESTING

#endif // slic3r_FilesystemTransactionTest_hpp_
