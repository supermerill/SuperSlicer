///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See UpdaterOperationExecutor.hpp. This implementation owns only scheduling
// and exception containment; repository-specific work remains in each updater.

#include "libslic3r/Updater/UpdaterOperationExecutor.hpp"

#include <cassert>
#include <exception>
#include <utility>

#include <boost/log/trivial.hpp>

namespace Slic3r {

thread_local const UpdaterOperationExecutor *UpdaterOperationExecutor::s_callback_executor = nullptr;

UpdaterOperationExecutor::UpdaterOperationExecutor()
    : m_worker(&UpdaterOperationExecutor::worker_loop, this)
    , m_worker_thread_id(m_worker.get_id())
{
}

UpdaterOperationExecutor::~UpdaterOperationExecutor()
{
    shutdown_and_wait();
}

UpdaterOperationExecutor::AsyncOperationToken::AsyncOperationToken(UpdaterOperationExecutor &executor)
    : m_executor(executor)
{
}

UpdaterOperationExecutor::AsyncOperationToken::~AsyncOperationToken()
{
    m_executor.release_async_operation();
}

void UpdaterOperationExecutor::AsyncOperationToken::run_callback(std::function<void()> callback) const
{
    if (!callback)
        return;

    // Preserve an outer callback context because one updater callback may
    // synchronously invoke another updater using a different executor.
    const UpdaterOperationExecutor *previous_executor = UpdaterOperationExecutor::s_callback_executor;
    UpdaterOperationExecutor::s_callback_executor = &m_executor;
    try {
        callback();
    } catch (...) {
        UpdaterOperationExecutor::s_callback_executor = previous_executor;
        throw;
    }
    UpdaterOperationExecutor::s_callback_executor = previous_executor;
}

bool UpdaterOperationExecutor::enqueue(Operation operation, Completion completion)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_stopping)
        return false;

    // The queued closure owns the complete operation lifecycle. Converting
    // exceptions here gives every caller the same structured failure path.
    m_operations.emplace_back(
        [operation = std::move(operation), completion = std::move(completion)]() mutable {
            UpdaterError result;
            try {
                result = operation ? operation() :
                    make_updater_error(UpdaterError::Code::Unexpected, "The updater operation is empty.");
            } catch (...) {
                result = make_updater_error_from_exception(std::current_exception());
            }

            if (completion)
                completion(std::move(result));
        });
    m_condition.notify_one();
    return true;
}

UpdaterOperationExecutor::AsyncOperation UpdaterOperationExecutor::retain_async_operation()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_stopping)
        return AsyncOperation();

    // Allocate the token before changing the counter so allocation failure
    // cannot leave shutdown waiting for an operation that was never returned.
    AsyncOperation operation(new AsyncOperationToken(*this));
    ++m_async_operations;
    return operation;
}

bool UpdaterOperationExecutor::can_shutdown_from_current_thread() const
{
    return std::this_thread::get_id() != m_worker_thread_id && s_callback_executor != this;
}

void UpdaterOperationExecutor::shutdown_and_wait()
{
    if (!can_shutdown_from_current_thread()) {
        const bool from_worker = std::this_thread::get_id() == m_worker_thread_id;
        BOOST_LOG_TRIVIAL(fatal)
            << "Updater shutdown requested from its own "
            << (from_worker ? "operation worker" : "asynchronous callback")
            << ". Continuing would deadlock or destroy state still in use.";
        std::terminate();
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();

    if (m_worker.joinable()) {
        assert(m_worker.get_id() != std::this_thread::get_id());
        m_worker.join();
    }

    // HTTP callbacks may finish after the worker has stopped. They can no
    // longer enqueue filesystem work, but the updater remains alive until each
    // terminal callback has observed that rejection and released its token.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this] { return m_async_operations == 0; });
}

void UpdaterOperationExecutor::wait_until_idle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this] { return m_operations.empty() && !m_executing; });
}

void UpdaterOperationExecutor::release_async_operation()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        assert(m_async_operations != 0);
        --m_async_operations;
    }
    m_condition.notify_all();
}

void UpdaterOperationExecutor::worker_loop()
{
    for (;;) {
        std::function<void()> operation;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopping || !m_operations.empty(); });
            if (m_operations.empty()) {
                if (m_stopping)
                    return;
                continue;
            }
            operation = std::move(m_operations.front());
            m_operations.pop_front();
            m_executing = true;
        }

        // A completion supplied by application code may itself throw. The
        // worker logs that programming error and remains available to drain
        // later operations instead of terminating the process.
        try {
            operation();
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(error) << "Unhandled updater completion exception: " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Unhandled unknown updater completion exception.";
        }

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_executing = false;
        }
        m_condition.notify_all();
    }
}

} // namespace Slic3r
