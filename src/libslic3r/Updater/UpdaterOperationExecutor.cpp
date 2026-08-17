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

UpdaterOperationExecutor::UpdaterOperationExecutor()
    : m_worker(&UpdaterOperationExecutor::worker_loop, this)
{
}

UpdaterOperationExecutor::~UpdaterOperationExecutor()
{
    shutdown_and_wait();
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

void UpdaterOperationExecutor::shutdown_and_wait()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stopping = true;
    }
    m_condition.notify_all();

    if (m_worker.joinable()) {
        assert(m_worker.get_id() != std::this_thread::get_id());
        m_worker.join();
    }
}

void UpdaterOperationExecutor::wait_until_idle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_condition.wait(lock, [this] { return m_operations.empty() && !m_executing; });
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
