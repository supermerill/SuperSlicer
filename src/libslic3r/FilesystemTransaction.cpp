///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See FilesystemTransaction.hpp for the public contract. The implementation
// keeps transaction bookkeeping separate from content preparation: entries
// carry only paths and progress flags. All destinations are preserved before
// any staging path is published, which makes reverse-order rollback sufficient
// to recover the complete previously visible set.

#include "libslic3r/FilesystemTransaction.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/FilesystemTransactionTest.hpp"

namespace Slic3r {
namespace {

enum class FilesystemTransactionAction {
    Replace,
    Remove
};

// Progress flags record completed rename boundaries. Rollback relies on these
// flags instead of guessing transaction progress from the current filesystem.
struct FilesystemTransactionEntry {
    FilesystemTransactionAction action = FilesystemTransactionAction::Replace;
    boost::filesystem::path staging;
    boost::filesystem::path destination;
    boost::filesystem::path backup;
    bool destination_moved = false;
    bool staging_published = false;
};

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
FilesystemTransactionTestHook g_filesystem_transaction_test_hook;
#endif

// Convert one path to a stable absolute lexical representation used for all
// conflict and sibling checks.
boost::filesystem::path normalized_absolute_path(const boost::filesystem::path &path);
// Compare normalized path components using the host filesystem's case rules.
bool filesystem_paths_equal(const boost::filesystem::path &first,
                            const boost::filesystem::path &second);
// Return true when one path equals or contains the other by path components.
bool filesystem_paths_overlap(const boost::filesystem::path &first,
                              const boost::filesystem::path &second);
// Classify one existing entry without following symbolic-link ownership into
// data outside the transaction.
bool supported_filesystem_entry(const boost::filesystem::path &path,
                                std::string &error_message);
// Build one neutral diagnostic at a known filesystem boundary.
FilesystemTransactionFailure make_filesystem_transaction_failure(
    FilesystemTransactionOperation operation,
    const boost::filesystem::path &source,
    const boost::filesystem::path &destination,
    std::string detail);
// Preserve standard exception text while still reporting unknown failures.
std::string filesystem_transaction_exception_detail(std::exception_ptr exception);
// Return the stable display name used by the neutral transaction formatter.
const char *filesystem_transaction_operation_name(FilesystemTransactionOperation operation);
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
// Invoke the deterministic test seam immediately before a filesystem action.
void invoke_filesystem_transaction_test_hook(FilesystemTransactionTestPoint point,
                                             const boost::filesystem::path &source,
                                             const boost::filesystem::path &destination);
#endif

} // namespace

class FilesystemTransaction::Impl {
public:
    Impl() = default;
    ~Impl();

    // Record one caller-owned staging entry for later validation and commit.
    void add_replacement(const boost::filesystem::path &staging,
                         const boost::filesystem::path &destination);
    // Record one destination whose previous state must remain rollbackable.
    void add_removal(const boost::filesystem::path &destination);
    // Execute validation, preservation, publication, rollback and cleanup.
    FilesystemTransactionResult commit();

private:
    // Validate every path and generate non-conflicting sibling backup names
    // before the first visible destination is modified.
    std::optional<FilesystemTransactionFailure> validate_plan();
    // Restore published and preserved entries in reverse registration order.
    bool rollback(FilesystemTransactionResult &result);
    // Remove transaction-owned staging and backup artifacts after a stable
    // visible state has been reached.
    void cleanup_artifacts(FilesystemTransactionResult &result);
    // Remove registered stagings when validation never permitted a commit,
    // while protecting every path which may name a destination.
    void cleanup_uncommitted_stagings(
        std::vector<FilesystemTransactionFailure> *warnings);
    // Append one cleanup warning without allowing diagnostic construction to
    // escape a destructor.
    void report_destructor_cleanup_warning(const boost::filesystem::path &path,
                                           const std::string &detail) const noexcept;

    std::vector<FilesystemTransactionEntry> m_entries;
    bool m_finished = false;
    bool m_late_registration = false;
};

namespace {

boost::filesystem::path normalized_absolute_path(const boost::filesystem::path &path)
{
    return boost::filesystem::absolute(path).lexically_normal();
}

bool filesystem_paths_equal(const boost::filesystem::path &first,
                            const boost::filesystem::path &second)
{
    boost::filesystem::path::const_iterator first_it = first.begin();
    boost::filesystem::path::const_iterator second_it = second.begin();
    while (first_it != first.end() && second_it != second.end()) {
#ifdef _WIN32
        if (!boost::algorithm::iequals(first_it->wstring(), second_it->wstring()))
#else
        if (*first_it != *second_it)
#endif
            return false;
        ++first_it;
        ++second_it;
    }
    return first_it == first.end() && second_it == second.end();
}

bool filesystem_paths_overlap(const boost::filesystem::path &first,
                              const boost::filesystem::path &second)
{
    boost::filesystem::path::const_iterator first_it = first.begin();
    boost::filesystem::path::const_iterator second_it = second.begin();
    while (first_it != first.end() && second_it != second.end()) {
#ifdef _WIN32
        if (!boost::algorithm::iequals(first_it->wstring(), second_it->wstring()))
#else
        if (*first_it != *second_it)
#endif
            return false;
        ++first_it;
        ++second_it;
    }
    return first_it == first.end() || second_it == second.end();
}

bool supported_filesystem_entry(const boost::filesystem::path &path,
                                std::string &error_message)
{
    boost::system::error_code status_error;
    const boost::filesystem::file_status status = boost::filesystem::symlink_status(path, status_error);
    if (status_error) {
        error_message = "Cannot inspect filesystem entry '" + path.string() + "': " + status_error.message();
        return false;
    }
    if (boost::filesystem::is_symlink(status) ||
        (!boost::filesystem::is_regular_file(status) && !boost::filesystem::is_directory(status))) {
        error_message = "Filesystem entry '" + path.string() + "' is neither a regular file nor a directory.";
        return false;
    }
    return true;
}

FilesystemTransactionFailure make_filesystem_transaction_failure(
    FilesystemTransactionOperation operation,
    const boost::filesystem::path &source,
    const boost::filesystem::path &destination,
    std::string detail)
{
    FilesystemTransactionFailure failure;
    failure.operation = operation;
    failure.source = source;
    failure.destination = destination;
    failure.detail = std::move(detail);
    return failure;
}

std::string filesystem_transaction_exception_detail(std::exception_ptr exception)
{
    try {
        if (exception)
            std::rethrow_exception(exception);
    } catch (const std::exception &error) {
        return error.what();
    } catch (...) {
        return "Unknown filesystem transaction failure.";
    }
    return "Unknown filesystem transaction failure.";
}

const char *filesystem_transaction_operation_name(FilesystemTransactionOperation operation)
{
    switch (operation) {
    case FilesystemTransactionOperation::ValidatePlan:
        return "validate plan";
    case FilesystemTransactionOperation::PreserveDestination:
        return "preserve destination";
    case FilesystemTransactionOperation::PublishStaging:
        return "publish staging";
    case FilesystemTransactionOperation::WithdrawPublishedReplacement:
        return "withdraw published replacement";
    case FilesystemTransactionOperation::RestoreDestination:
        return "restore destination";
    case FilesystemTransactionOperation::CleanupArtifact:
        return "clean transaction artifact";
    }
    return "filesystem operation";
}

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
void invoke_filesystem_transaction_test_hook(FilesystemTransactionTestPoint point,
                                             const boost::filesystem::path &source,
                                             const boost::filesystem::path &destination)
{
    if (g_filesystem_transaction_test_hook)
        g_filesystem_transaction_test_hook(point, source, destination);
}
#endif

} // namespace

std::string format_filesystem_transaction_failure(
    const FilesystemTransactionFailure &failure)
{
    std::ostringstream stream;
    stream << filesystem_transaction_operation_name(failure.operation);
    if (!failure.source.empty())
        stream << " '" << failure.source.string() << "'";
    if (!failure.destination.empty())
        stream << " -> '" << failure.destination.string() << "'";
    if (!failure.detail.empty())
        stream << ": " << failure.detail;
    return stream.str();
}

std::string format_filesystem_transaction_error(
    const FilesystemTransactionResult &result)
{
    std::ostringstream stream;
    if (result.failure.has_value())
        stream << format_filesystem_transaction_failure(*result.failure);
    for (const FilesystemTransactionFailure &rollback_error : result.rollback_errors) {
        if (stream.tellp() > 0)
            stream << "\n";
        stream << "Rollback failed while "
               << format_filesystem_transaction_failure(rollback_error);
    }
    return stream.str();
}

FilesystemTransaction::Impl::~Impl()
{
    if (!m_finished || m_late_registration) {
        try {
            cleanup_uncommitted_stagings(nullptr);
        } catch (...) {
            report_destructor_cleanup_warning(
                boost::filesystem::path(),
                filesystem_transaction_exception_detail(std::current_exception()));
        }
    }
}

void FilesystemTransaction::Impl::add_replacement(const boost::filesystem::path &staging,
                                                  const boost::filesystem::path &destination)
{
    FilesystemTransactionEntry entry;
    entry.action = FilesystemTransactionAction::Replace;
    entry.staging = staging;
    entry.destination = destination;
    m_entries.emplace_back(std::move(entry));
    if (m_finished)
        m_late_registration = true;
}

void FilesystemTransaction::Impl::add_removal(const boost::filesystem::path &destination)
{
    FilesystemTransactionEntry entry;
    entry.action = FilesystemTransactionAction::Remove;
    entry.destination = destination;
    m_entries.emplace_back(std::move(entry));
    if (m_finished)
        m_late_registration = true;
}

std::optional<FilesystemTransactionFailure> FilesystemTransaction::Impl::validate_plan()
{
    if (m_finished)
        return make_filesystem_transaction_failure(
            FilesystemTransactionOperation::ValidatePlan, boost::filesystem::path(),
            boost::filesystem::path(), "A filesystem transaction may be committed only once.");

    try {
        // Normalize first so all subsequent validation and diagnostics refer to
        // the exact paths which will be passed to rename and remove operations.
        for (FilesystemTransactionEntry &entry : m_entries) {
            if (entry.destination.empty())
                return make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                    "A transaction destination path is empty.");
            entry.destination = normalized_absolute_path(entry.destination);
            if (entry.destination.filename().empty())
                return make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                    "A transaction cannot replace or remove a filesystem root.");

            if (entry.action == FilesystemTransactionAction::Replace) {
                if (entry.staging.empty())
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                        "A replacement staging path is empty.");
                entry.staging = normalized_absolute_path(entry.staging);
                if (filesystem_paths_equal(entry.staging, entry.destination) ||
                    !filesystem_paths_equal(entry.staging.parent_path(), entry.destination.parent_path()))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                        "A replacement staging path must be a distinct sibling of its destination.");
                if (!boost::filesystem::exists(entry.staging))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                        "A replacement staging entry does not exist.");
                std::string type_error;
                if (!supported_filesystem_entry(entry.staging, type_error))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging, entry.destination,
                        std::move(type_error));
            }

            if (boost::filesystem::exists(entry.destination)) {
                std::string type_error;
                if (!supported_filesystem_entry(entry.destination, type_error))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.destination,
                        boost::filesystem::path(), std::move(type_error));
            }
        }

        // Any equality or ancestor relation between registered destinations
        // would make commit order observable and rollback ambiguous.
        for (size_t first_idx = 0; first_idx < m_entries.size(); ++first_idx)
            for (size_t second_idx = first_idx + 1; second_idx < m_entries.size(); ++second_idx)
                if (filesystem_paths_overlap(m_entries[first_idx].destination,
                                             m_entries[second_idx].destination))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan,
                        m_entries[first_idx].destination, m_entries[second_idx].destination,
                        "Transaction destinations overlap.");

        // Stagings must also remain independent from every destination and from
        // each other, otherwise one rename could consume another entry's data.
        for (size_t entry_idx = 0; entry_idx < m_entries.size(); ++entry_idx) {
            const FilesystemTransactionEntry &entry = m_entries[entry_idx];
            if (entry.action != FilesystemTransactionAction::Replace)
                continue;
            for (size_t other_idx = 0; other_idx < m_entries.size(); ++other_idx)
                if (filesystem_paths_overlap(entry.staging, m_entries[other_idx].destination))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging,
                        m_entries[other_idx].destination,
                        "A replacement staging path overlaps a transaction destination.");
            for (size_t other_idx = entry_idx + 1; other_idx < m_entries.size(); ++other_idx)
                if (m_entries[other_idx].action == FilesystemTransactionAction::Replace &&
                    filesystem_paths_overlap(entry.staging, m_entries[other_idx].staging))
                    return make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::ValidatePlan, entry.staging,
                        m_entries[other_idx].staging,
                        "Replacement staging paths overlap.");
        }

        // Backups are generated only after all caller paths are accepted. Their
        // randomized sibling names are checked against every registered path.
        for (FilesystemTransactionEntry &entry : m_entries) {
            bool backup_available = false;
            for (unsigned int attempt = 0; attempt < 32 && !backup_available; ++attempt) {
                entry.backup = entry.destination.parent_path() /
                    boost::filesystem::unique_path(
                        "." + entry.destination.filename().string() + ".transaction-backup-%%%%-%%%%");
                backup_available = !boost::filesystem::exists(entry.backup);
                for (const FilesystemTransactionEntry &other : m_entries) {
                    if (filesystem_paths_overlap(entry.backup, other.destination) ||
                        (other.action == FilesystemTransactionAction::Replace &&
                         filesystem_paths_overlap(entry.backup, other.staging))) {
                        backup_available = false;
                        break;
                    }
                }
            }
            if (!backup_available)
                return make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::ValidatePlan, entry.destination,
                    entry.backup, "Cannot allocate a non-conflicting sibling backup path.");
        }
    } catch (...) {
        return make_filesystem_transaction_failure(
            FilesystemTransactionOperation::ValidatePlan, boost::filesystem::path(),
            boost::filesystem::path(),
            filesystem_transaction_exception_detail(std::current_exception()));
    }
    return std::nullopt;
}

FilesystemTransactionResult FilesystemTransaction::Impl::commit()
{
    FilesystemTransactionResult result;
    const std::optional<FilesystemTransactionFailure> validation_failure = validate_plan();
    if (validation_failure.has_value()) {
        result.status = FilesystemTransactionStatus::Rejected;
        result.failure = validation_failure;
        m_finished = true;
        try {
            cleanup_uncommitted_stagings(&result.cleanup_warnings);
        } catch (...) {
            result.cleanup_warnings.emplace_back(make_filesystem_transaction_failure(
                FilesystemTransactionOperation::CleanupArtifact,
                boost::filesystem::path(), boost::filesystem::path(),
                filesystem_transaction_exception_detail(std::current_exception())));
        }
        m_late_registration = false;
        return result;
    }

    m_finished = true;
    FilesystemTransactionOperation active_operation = FilesystemTransactionOperation::PreserveDestination;
    boost::filesystem::path active_source;
    boost::filesystem::path active_destination;
    try {
        // Preserve every existing destination before exposing a replacement.
        // Removals therefore remain recoverable until the global commit ends.
        for (FilesystemTransactionEntry &entry : m_entries) {
            if (!boost::filesystem::exists(entry.destination))
                continue;
            active_operation = FilesystemTransactionOperation::PreserveDestination;
            active_source = entry.destination;
            active_destination = entry.backup;
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
            invoke_filesystem_transaction_test_hook(
                FilesystemTransactionTestPoint::BeforePreserveDestination,
                active_source, active_destination);
#endif
            boost::filesystem::rename(entry.destination, entry.backup);
            entry.destination_moved = true;
        }

        // Publishing uses sibling renames, so a staging entry becomes visible
        // as one complete filesystem object rather than as a partial copy.
        for (FilesystemTransactionEntry &entry : m_entries) {
            if (entry.action != FilesystemTransactionAction::Replace)
                continue;
            active_operation = FilesystemTransactionOperation::PublishStaging;
            active_source = entry.staging;
            active_destination = entry.destination;
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
            invoke_filesystem_transaction_test_hook(
                FilesystemTransactionTestPoint::BeforePublishStaging,
                active_source, active_destination);
#endif
            boost::filesystem::rename(entry.staging, entry.destination);
            entry.staging_published = true;
        }
    } catch (...) {
        result.failure = make_filesystem_transaction_failure(
            active_operation, active_source, active_destination,
            filesystem_transaction_exception_detail(std::current_exception()));
        if (rollback(result)) {
            result.status = FilesystemTransactionStatus::RolledBack;
            cleanup_artifacts(result);
        } else {
            result.status = FilesystemTransactionStatus::RollbackFailed;
        }
        return result;
    }

    result.status = FilesystemTransactionStatus::Committed;
    cleanup_artifacts(result);
    return result;
}

bool FilesystemTransaction::Impl::rollback(FilesystemTransactionResult &result)
{
    // Reverse order undoes publication dependencies and restores removals only
    // after later entries have vacated any potentially related destinations.
    for (std::vector<FilesystemTransactionEntry>::reverse_iterator it = m_entries.rbegin();
         it != m_entries.rend(); ++it) {
        FilesystemTransactionEntry &entry = *it;
        if (entry.staging_published) {
            try {
                if (boost::filesystem::exists(entry.destination)) {
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
                    invoke_filesystem_transaction_test_hook(
                        FilesystemTransactionTestPoint::BeforeWithdrawPublishedReplacement,
                        entry.destination, entry.staging);
#endif
                    boost::filesystem::rename(entry.destination, entry.staging);
                }
                entry.staging_published = false;
            } catch (...) {
                result.rollback_errors.emplace_back(make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::WithdrawPublishedReplacement,
                    entry.destination, entry.staging,
                    filesystem_transaction_exception_detail(std::current_exception())));
            }
        }

        if (entry.destination_moved) {
            try {
                if (boost::filesystem::exists(entry.destination)) {
                    result.rollback_errors.emplace_back(make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::RestoreDestination,
                        entry.backup, entry.destination,
                        "The destination is occupied while restoring its backup."));
                    continue;
                }
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
                invoke_filesystem_transaction_test_hook(
                    FilesystemTransactionTestPoint::BeforeRestoreDestination,
                    entry.backup, entry.destination);
#endif
                boost::filesystem::rename(entry.backup, entry.destination);
                entry.destination_moved = false;
            } catch (...) {
                result.rollback_errors.emplace_back(make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::RestoreDestination,
                    entry.backup, entry.destination,
                    filesystem_transaction_exception_detail(std::current_exception())));
            }
        }
    }
    return result.rollback_errors.empty();
}

void FilesystemTransaction::Impl::cleanup_artifacts(FilesystemTransactionResult &result)
{
    for (const FilesystemTransactionEntry &entry : m_entries) {
        const boost::filesystem::path artifacts[] = {entry.staging, entry.backup};
        for (const boost::filesystem::path &artifact : artifacts) {
            if (artifact.empty())
                continue;
            try {
                if (!boost::filesystem::exists(artifact))
                    continue;
#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
                invoke_filesystem_transaction_test_hook(
                    FilesystemTransactionTestPoint::BeforeCleanupArtifact,
                    artifact, boost::filesystem::path());
#endif
                boost::system::error_code cleanup_error;
                boost::filesystem::remove_all(artifact, cleanup_error);
                if (cleanup_error)
                    result.cleanup_warnings.emplace_back(make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::CleanupArtifact, artifact,
                        boost::filesystem::path(), cleanup_error.message()));
            } catch (...) {
                result.cleanup_warnings.emplace_back(make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::CleanupArtifact, artifact,
                    boost::filesystem::path(),
                    filesystem_transaction_exception_detail(std::current_exception())));
            }
        }
    }
}

void FilesystemTransaction::Impl::cleanup_uncommitted_stagings(
    std::vector<FilesystemTransactionFailure> *warnings)
{
    // Protect every destination before deleting owned staging data. This is
    // required even for an invalid plan where two registered paths conflict.
    std::vector<boost::filesystem::path> destinations;
    try {
        for (const FilesystemTransactionEntry &entry : m_entries)
            if (!entry.destination.empty())
                destinations.emplace_back(normalized_absolute_path(entry.destination));
    } catch (...) {
        report_destructor_cleanup_warning(
            boost::filesystem::path(),
            "Cannot normalize transaction destinations while cleaning staging entries.");
        return;
    }

    for (const FilesystemTransactionEntry &entry : m_entries) {
        if (entry.action != FilesystemTransactionAction::Replace || entry.staging.empty())
            continue;
        try {
            const boost::filesystem::path staging = normalized_absolute_path(entry.staging);
            bool conflicts_with_destination = false;
            for (const boost::filesystem::path &destination : destinations)
                if (filesystem_paths_overlap(staging, destination)) {
                    conflicts_with_destination = true;
                    break;
                }
            if (conflicts_with_destination) {
                const FilesystemTransactionFailure warning = make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::CleanupArtifact, staging,
                    boost::filesystem::path(),
                    "The staging path was not removed because it overlaps a transaction destination.");
                if (warnings != nullptr)
                    warnings->emplace_back(warning);
                else
                    report_destructor_cleanup_warning(staging, warning.detail);
                continue;
            }

            boost::system::error_code cleanup_error;
            boost::filesystem::remove_all(staging, cleanup_error);
            if (cleanup_error) {
                if (warnings != nullptr)
                    warnings->emplace_back(make_filesystem_transaction_failure(
                        FilesystemTransactionOperation::CleanupArtifact, staging,
                        boost::filesystem::path(), cleanup_error.message()));
                else
                    report_destructor_cleanup_warning(staging, cleanup_error.message());
            }
        } catch (...) {
            const std::string detail = filesystem_transaction_exception_detail(std::current_exception());
            if (warnings != nullptr)
                warnings->emplace_back(make_filesystem_transaction_failure(
                    FilesystemTransactionOperation::CleanupArtifact, entry.staging,
                    boost::filesystem::path(), detail));
            else
                report_destructor_cleanup_warning(entry.staging, detail);
        }
    }
}

void FilesystemTransaction::Impl::report_destructor_cleanup_warning(
    const boost::filesystem::path &path,
    const std::string &detail) const noexcept
{
    try {
        BOOST_LOG_TRIVIAL(warning) << "Cannot clean uncommitted filesystem transaction staging path '"
                                   << path.string() << "': " << detail;
    } catch (...) {
        // Destruction must never propagate a logging or path-conversion error.
    }
}

FilesystemTransaction::FilesystemTransaction() : m_impl(std::make_unique<Impl>())
{
}

FilesystemTransaction::~FilesystemTransaction() = default;

FilesystemTransaction::FilesystemTransaction(FilesystemTransaction &&other) noexcept = default;

FilesystemTransaction &FilesystemTransaction::operator=(FilesystemTransaction &&other) noexcept = default;

void FilesystemTransaction::add_replacement(const boost::filesystem::path &staging,
                                            const boost::filesystem::path &destination)
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    m_impl->add_replacement(staging, destination);
}

void FilesystemTransaction::add_removal(const boost::filesystem::path &destination)
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    m_impl->add_removal(destination);
}

FilesystemTransactionResult FilesystemTransaction::commit()
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    return m_impl->commit();
}

#ifdef SLIC3R_FILESYSTEM_TRANSACTION_TESTING
void set_filesystem_transaction_test_hook(FilesystemTransactionTestHook hook)
{
    g_filesystem_transaction_test_hook = std::move(hook);
}
#endif

} // namespace Slic3r
