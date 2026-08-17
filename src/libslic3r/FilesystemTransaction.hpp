///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// FilesystemTransaction publishes a set of already prepared filesystem
// entries as one logical operation. Callers create and validate disposable
// staging files or directories beside their destinations, then register
// replacements and removals. commit() first preserves every existing
// destination, publishes every staging entry, and only then discards backups.
// If publication fails, the transaction restores the previous visible state
// in reverse order and reports whether that restoration was complete.
//
// The class deliberately knows nothing about the contents being published. It
// does not copy, parse or validate application data, and it does not recover
// artifacts left by a process interruption. It owns only the staging and
// backup paths associated with its own lifetime.

#ifndef slic3r_FilesystemTransaction_hpp_
#define slic3r_FilesystemTransaction_hpp_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// The status distinguishes a plan rejected before mutation from a publication
// failure whose changes were either fully or only partially restored.
enum class FilesystemTransactionStatus {
    Committed,
    Rejected,
    RolledBack,
    RollbackFailed
};

// Operations identify the filesystem boundary which produced a diagnostic.
// They remain independent from the type or meaning of the stored data.
enum class FilesystemTransactionOperation {
    ValidatePlan,
    PreserveDestination,
    PublishStaging,
    WithdrawPublishedReplacement,
    RestoreDestination,
    CleanupArtifact
};

// A diagnostic names both sides of an attempted operation when applicable.
// destination is empty for single-path operations such as cleanup.
struct FilesystemTransactionFailure {
    FilesystemTransactionOperation operation = FilesystemTransactionOperation::ValidatePlan;
    boost::filesystem::path source;
    boost::filesystem::path destination;
    std::string detail;
};

// The primary failure explains why commit stopped. Rollback errors describe
// visible paths which could not be restored; cleanup warnings concern only
// disposable staging or backup artifacts and never change the status.
struct FilesystemTransactionResult {
    FilesystemTransactionStatus status = FilesystemTransactionStatus::Rejected;
    std::optional<FilesystemTransactionFailure> failure;
    std::vector<FilesystemTransactionFailure> rollback_errors;
    std::vector<FilesystemTransactionFailure> cleanup_warnings;
};

class FilesystemTransaction {
public:
    FilesystemTransaction();
    ~FilesystemTransaction();

    FilesystemTransaction(FilesystemTransaction &&other) noexcept;
    FilesystemTransaction &operator=(FilesystemTransaction &&other) noexcept;

    FilesystemTransaction(const FilesystemTransaction &) = delete;
    FilesystemTransaction &operator=(const FilesystemTransaction &) = delete;

    // Register a complete disposable staging entry to replace destination.
    // Both paths must be distinct siblings. Ownership of staging transfers to
    // the transaction, which consumes or removes it even when commit rejects
    // the complete plan. Files and directories are both accepted.
    void add_replacement(const boost::filesystem::path &staging,
                         const boost::filesystem::path &destination);

    // Register a destination which should be absent after a successful commit.
    // A missing destination is a valid no-op. Existing entries must be regular
    // files or directories and remain recoverable until the global commit.
    void add_removal(const boost::filesystem::path &destination);

    // Validate and execute the complete plan exactly once. This method catches
    // filesystem and unexpected implementation exceptions and represents them
    // in the result. RollbackFailed means callers must stop before consuming
    // the destinations.
    FilesystemTransactionResult commit();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r

#endif // slic3r_FilesystemTransaction_hpp_
