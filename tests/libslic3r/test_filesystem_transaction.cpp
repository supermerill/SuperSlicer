///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// These tests exercise FilesystemTransaction as an independent filesystem
// primitive. Small temporary trees represent complete caller-prepared data;
// deterministic test hooks interrupt individual rename and cleanup boundaries
// so rollback behavior does not depend on platform-specific file locking.

#include <catch2/catch.hpp>

#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/FilesystemTransaction.hpp"
#include "libslic3r/FilesystemTransactionTest.hpp"

namespace {

class TemporaryFilesystemTree {
public:
    TemporaryFilesystemTree();
    ~TemporaryFilesystemTree();

    const boost::filesystem::path &path() const;

private:
    boost::filesystem::path m_path;
};

class ScopedFilesystemTransactionHook {
public:
    explicit ScopedFilesystemTransactionHook(Slic3r::FilesystemTransactionTestHook hook);
    ~ScopedFilesystemTransactionHook();
};

// Write one complete text file after creating its parent directory.
void write_text_file(const boost::filesystem::path &path, const std::string &contents);
// Read a test file without applying any text transformation.
std::string read_text_file(const boost::filesystem::path &path);
// Detect backup artifacts owned by the transaction under one directory.
bool has_transaction_backup(const boost::filesystem::path &directory);

TemporaryFilesystemTree::TemporaryFilesystemTree() :
    m_path(boost::filesystem::temp_directory_path() /
           boost::filesystem::unique_path("slic3r-filesystem-transaction-%%%%-%%%%"))
{
    boost::filesystem::create_directories(m_path);
}

TemporaryFilesystemTree::~TemporaryFilesystemTree()
{
    boost::system::error_code cleanup_error;
    boost::filesystem::remove_all(m_path, cleanup_error);
}

const boost::filesystem::path &TemporaryFilesystemTree::path() const
{
    return m_path;
}

ScopedFilesystemTransactionHook::ScopedFilesystemTransactionHook(
    Slic3r::FilesystemTransactionTestHook hook)
{
    Slic3r::set_filesystem_transaction_test_hook(std::move(hook));
}

ScopedFilesystemTransactionHook::~ScopedFilesystemTransactionHook()
{
    Slic3r::set_filesystem_transaction_test_hook({});
}

void write_text_file(const boost::filesystem::path &path, const std::string &contents)
{
    boost::filesystem::create_directories(path.parent_path());
    boost::nowide::ofstream stream(path.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << contents;
    REQUIRE(stream.good());
}

std::string read_text_file(const boost::filesystem::path &path)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool has_transaction_backup(const boost::filesystem::path &directory)
{
    if (!boost::filesystem::is_directory(directory))
        return false;
    for (boost::filesystem::directory_iterator it(directory), end; it != end; ++it)
        if (it->path().filename().string().find(".transaction-backup-") != std::string::npos)
            return true;
    return false;
}

TEST_CASE("An empty filesystem transaction commits successfully", "[filesystem][transaction]")
{
    Slic3r::FilesystemTransaction transaction;
    const Slic3r::FilesystemTransactionResult result = transaction.commit();

    CHECK(result.status == Slic3r::FilesystemTransactionStatus::Committed);
    CHECK_FALSE(result.failure.has_value());
    CHECK(result.rollback_errors.empty());
    CHECK(result.cleanup_warnings.empty());
}

TEST_CASE("A filesystem transaction publishes files directories and removals together",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path old_file = tree.path() / "file.txt";
    const boost::filesystem::path staged_file = tree.path() / ".file.stage";
    const boost::filesystem::path old_directory = tree.path() / "directory";
    const boost::filesystem::path staged_directory = tree.path() / ".directory.stage";
    const boost::filesystem::path removed_directory = tree.path() / "removed";
    const boost::filesystem::path new_file = tree.path() / "new.txt";
    const boost::filesystem::path staged_new_file = tree.path() / ".new.stage";
    write_text_file(old_file, "old file");
    write_text_file(staged_file, "new file");
    write_text_file(old_directory / "value.txt", "old directory");
    write_text_file(staged_directory / "value.txt", "new directory");
    write_text_file(removed_directory / "value.txt", "removed");
    write_text_file(staged_new_file, "new destination");

    Slic3r::FilesystemTransaction transaction;
    transaction.add_replacement(staged_file, old_file);
    transaction.add_replacement(staged_directory, old_directory);
    transaction.add_removal(removed_directory);
    transaction.add_replacement(staged_new_file, new_file);
    const Slic3r::FilesystemTransactionResult result = transaction.commit();

    CHECK(result.status == Slic3r::FilesystemTransactionStatus::Committed);
    CHECK(read_text_file(old_file) == "new file");
    CHECK(read_text_file(old_directory / "value.txt") == "new directory");
    CHECK(read_text_file(new_file) == "new destination");
    CHECK_FALSE(boost::filesystem::exists(removed_directory));
    CHECK_FALSE(boost::filesystem::exists(staged_file));
    CHECK_FALSE(boost::filesystem::exists(staged_directory));
    CHECK_FALSE(boost::filesystem::exists(staged_new_file));
    CHECK_FALSE(has_transaction_backup(tree.path()));
}

TEST_CASE("A filesystem transaction rejects invalid plans before mutation",
          "[filesystem][transaction]")
{
    SECTION("Missing staging") {
        TemporaryFilesystemTree tree;
        const boost::filesystem::path destination = tree.path() / "destination.txt";
        write_text_file(destination, "original");

        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(tree.path() / ".missing.stage", destination);
        const Slic3r::FilesystemTransactionResult result = transaction.commit();

        CHECK(result.status == Slic3r::FilesystemTransactionStatus::Rejected);
        CHECK(result.failure.has_value());
        CHECK(read_text_file(destination) == "original");
    }

    SECTION("Non-sibling staging") {
        TemporaryFilesystemTree tree;
        const boost::filesystem::path staging = tree.path() / "staging" / "value.txt";
        const boost::filesystem::path destination = tree.path() / "destination.txt";
        write_text_file(staging, "staged");
        write_text_file(destination, "original");

        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(staging, destination);
        const Slic3r::FilesystemTransactionResult result = transaction.commit();

        CHECK(result.status == Slic3r::FilesystemTransactionStatus::Rejected);
        CHECK(read_text_file(destination) == "original");
        CHECK_FALSE(boost::filesystem::exists(staging));
    }

    SECTION("Duplicate destination") {
        TemporaryFilesystemTree tree;
        const boost::filesystem::path first_staging = tree.path() / ".first.stage";
        const boost::filesystem::path second_staging = tree.path() / ".second.stage";
        const boost::filesystem::path destination = tree.path() / "destination.txt";
        write_text_file(first_staging, "first");
        write_text_file(second_staging, "second");
        write_text_file(destination, "original");

        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(first_staging, destination);
        transaction.add_replacement(second_staging, destination);
        const Slic3r::FilesystemTransactionResult result = transaction.commit();

        CHECK(result.status == Slic3r::FilesystemTransactionStatus::Rejected);
        CHECK(read_text_file(destination) == "original");
        CHECK_FALSE(boost::filesystem::exists(first_staging));
        CHECK_FALSE(boost::filesystem::exists(second_staging));
    }

    SECTION("Overlapping destinations") {
        TemporaryFilesystemTree tree;
        const boost::filesystem::path parent = tree.path() / "parent";
        write_text_file(parent / "child.txt", "original");

        Slic3r::FilesystemTransaction transaction;
        transaction.add_removal(parent);
        transaction.add_removal(parent / "child.txt");
        const Slic3r::FilesystemTransactionResult result = transaction.commit();

        CHECK(result.status == Slic3r::FilesystemTransactionStatus::Rejected);
        CHECK(read_text_file(parent / "child.txt") == "original");
    }

    SECTION("Symbolic-link staging when supported by the platform") {
        TemporaryFilesystemTree tree;
        const boost::filesystem::path target = tree.path() / "target.txt";
        const boost::filesystem::path staging = tree.path() / ".link.stage";
        const boost::filesystem::path destination = tree.path() / "destination.txt";
        write_text_file(target, "target");
        write_text_file(destination, "original");
        boost::system::error_code symlink_error;
        boost::filesystem::create_symlink(target, staging, symlink_error);
        if (symlink_error) {
            SUCCEED("Symbolic links are unavailable in this test environment.");
        } else {
            Slic3r::FilesystemTransaction transaction;
            transaction.add_replacement(staging, destination);
            const Slic3r::FilesystemTransactionResult result = transaction.commit();

            CHECK(result.status == Slic3r::FilesystemTransactionStatus::Rejected);
            CHECK(read_text_file(destination) == "original");
            CHECK(read_text_file(target) == "target");
        }
    }
}

TEST_CASE("Destroying an uncommitted filesystem transaction removes its staging",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path staging = tree.path() / ".value.stage";
    const boost::filesystem::path destination = tree.path() / "value.txt";
    write_text_file(staging, "new");
    write_text_file(destination, "old");
    {
        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(staging, destination);
    }

    CHECK_FALSE(boost::filesystem::exists(staging));
    CHECK(read_text_file(destination) == "old");
}

TEST_CASE("A publication failure restores replacements and removals",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path first = tree.path() / "a.txt";
    const boost::filesystem::path first_staging = tree.path() / ".a.stage";
    const boost::filesystem::path second = tree.path() / "b.txt";
    const boost::filesystem::path second_staging = tree.path() / ".b.stage";
    const boost::filesystem::path removed = tree.path() / "removed";
    write_text_file(first, "old a");
    write_text_file(first_staging, "new a");
    write_text_file(second, "old b");
    write_text_file(second_staging, "new b");
    write_text_file(removed / "value.txt", "old removed");

    Slic3r::FilesystemTransactionResult result;
    {
        ScopedFilesystemTransactionHook hook(
            [second](Slic3r::FilesystemTransactionTestPoint point,
                     const boost::filesystem::path &,
                     const boost::filesystem::path &destination) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePublishStaging &&
                    destination == second)
                    throw std::runtime_error("injected second publication failure");
            });
        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(first_staging, first);
        transaction.add_replacement(second_staging, second);
        transaction.add_removal(removed);
        result = transaction.commit();
    }

    REQUIRE(result.status == Slic3r::FilesystemTransactionStatus::RolledBack);
    REQUIRE(result.failure.has_value());
    CHECK(result.failure->operation == Slic3r::FilesystemTransactionOperation::PublishStaging);
    CHECK(result.failure->detail.find("injected second publication failure") != std::string::npos);
    CHECK(result.rollback_errors.empty());
    CHECK(read_text_file(first) == "old a");
    CHECK(read_text_file(second) == "old b");
    CHECK(read_text_file(removed / "value.txt") == "old removed");
    CHECK_FALSE(boost::filesystem::exists(first_staging));
    CHECK_FALSE(boost::filesystem::exists(second_staging));
    CHECK_FALSE(has_transaction_backup(tree.path()));
}

TEST_CASE("Rollback removes a published destination which had no previous value",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path added = tree.path() / "a-new.txt";
    const boost::filesystem::path added_staging = tree.path() / ".a-new.stage";
    const boost::filesystem::path failed = tree.path() / "b-failed.txt";
    const boost::filesystem::path failed_staging = tree.path() / ".b-failed.stage";
    write_text_file(added_staging, "new value");
    write_text_file(failed_staging, "failed value");

    Slic3r::FilesystemTransactionResult result;
    {
        ScopedFilesystemTransactionHook hook(
            [failed](Slic3r::FilesystemTransactionTestPoint point,
                     const boost::filesystem::path &,
                     const boost::filesystem::path &destination) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePublishStaging &&
                    destination == failed)
                    throw std::runtime_error("injected publication failure");
            });
        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(added_staging, added);
        transaction.add_replacement(failed_staging, failed);
        result = transaction.commit();
    }

    CHECK(result.status == Slic3r::FilesystemTransactionStatus::RolledBack);
    CHECK_FALSE(boost::filesystem::exists(added));
    CHECK_FALSE(boost::filesystem::exists(failed));
    CHECK_FALSE(has_transaction_backup(tree.path()));
}

TEST_CASE("A rollback failure is reported without hiding recovery artifacts",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path first = tree.path() / "a.txt";
    const boost::filesystem::path first_staging = tree.path() / ".a.stage";
    const boost::filesystem::path second = tree.path() / "b.txt";
    const boost::filesystem::path second_staging = tree.path() / ".b.stage";
    write_text_file(first, "old a");
    write_text_file(first_staging, "new a");
    write_text_file(second, "old b");
    write_text_file(second_staging, "new b");

    Slic3r::FilesystemTransactionResult result;
    {
        ScopedFilesystemTransactionHook hook(
            [first, second](Slic3r::FilesystemTransactionTestPoint point,
                            const boost::filesystem::path &source,
                            const boost::filesystem::path &destination) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforePublishStaging &&
                    destination == second)
                    throw std::runtime_error("injected commit failure");
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforeWithdrawPublishedReplacement &&
                    source == first)
                    throw std::runtime_error("injected rollback failure");
            });
        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(first_staging, first);
        transaction.add_replacement(second_staging, second);
        result = transaction.commit();
    }

    REQUIRE(result.status == Slic3r::FilesystemTransactionStatus::RollbackFailed);
    REQUIRE(result.failure.has_value());
    CHECK(result.failure->detail.find("injected commit failure") != std::string::npos);
    REQUIRE_FALSE(result.rollback_errors.empty());
    CHECK(result.rollback_errors.front().detail.find("injected rollback failure") != std::string::npos);
    CHECK(read_text_file(first) == "new a");
    CHECK(read_text_file(second) == "old b");
    CHECK(has_transaction_backup(tree.path()));
}

TEST_CASE("Cleanup failures are warnings after a successful commit",
          "[filesystem][transaction]")
{
    TemporaryFilesystemTree tree;
    const boost::filesystem::path destination = tree.path() / "value.txt";
    const boost::filesystem::path staging = tree.path() / ".value.stage";
    write_text_file(destination, "old");
    write_text_file(staging, "new");

    Slic3r::FilesystemTransactionResult result;
    {
        ScopedFilesystemTransactionHook hook(
            [](Slic3r::FilesystemTransactionTestPoint point,
               const boost::filesystem::path &source,
               const boost::filesystem::path &) {
                if (point == Slic3r::FilesystemTransactionTestPoint::BeforeCleanupArtifact &&
                    source.filename().string().find(".transaction-backup-") != std::string::npos)
                    throw std::runtime_error("injected cleanup failure");
            });
        Slic3r::FilesystemTransaction transaction;
        transaction.add_replacement(staging, destination);
        result = transaction.commit();
    }

    CHECK(result.status == Slic3r::FilesystemTransactionStatus::Committed);
    CHECK(read_text_file(destination) == "new");
    REQUIRE(result.cleanup_warnings.size() == 1);
    CHECK(result.cleanup_warnings.front().operation ==
          Slic3r::FilesystemTransactionOperation::CleanupArtifact);
    CHECK(result.cleanup_warnings.front().detail.find("injected cleanup failure") != std::string::npos);
    CHECK(has_transaction_backup(tree.path()));
}

} // namespace
