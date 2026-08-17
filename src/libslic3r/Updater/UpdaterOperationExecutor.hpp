///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// UpdaterOperationExecutor serializes filesystem mutations on one background
// thread and tracks asynchronous operations executed by external transports.
// Call enqueue() for worker tasks and retain_async_operation() before starting
// a callback-based operation. shutdown_and_wait() keeps the updater alive until
// both kinds of work have released every callback capture.

#ifndef slic3r_Updater_UpdaterOperationExecutor_hpp_
#define slic3r_Updater_UpdaterOperationExecutor_hpp_

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class UpdaterOperationExecutor
{
public:
    using Operation = std::function<UpdaterError()>;
    using Completion = std::function<void(UpdaterError)>;

    // An asynchronous callback chain shares this token until its terminal
    // callback returns. Releasing the final reference wakes shutdown waiters.
    class AsyncOperationToken
    {
    public:
        ~AsyncOperationToken();

    private:
        friend class UpdaterOperationExecutor;
        explicit AsyncOperationToken(UpdaterOperationExecutor &executor);

        UpdaterOperationExecutor &m_executor;
    };
    using AsyncOperation = std::shared_ptr<AsyncOperationToken>;

    UpdaterOperationExecutor();
    ~UpdaterOperationExecutor();
    UpdaterOperationExecutor(const UpdaterOperationExecutor &) = delete;
    UpdaterOperationExecutor(UpdaterOperationExecutor &&) = delete;
    UpdaterOperationExecutor &operator=(const UpdaterOperationExecutor &) = delete;
    UpdaterOperationExecutor &operator=(UpdaterOperationExecutor &&) = delete;

    // Queue one operation and invoke completion once on the worker after the
    // operation has returned or thrown. False means shutdown already started;
    // in that case no callback is retained or invoked by the executor.
    bool enqueue(Operation operation, Completion completion);

    // Register one logical callback-based operation. Every callback belonging
    // to that operation must retain a copy until the terminal callback has
    // returned. An empty result means shutdown has started and no new external
    // work may capture the updater.
    AsyncOperation retain_async_operation();

    // Stop accepting work, drain accepted tasks, join the worker, then wait for
    // callback-based operations to release their tokens. This is idempotent so
    // derived and base destructors may both enforce the lifetime rule.
    void shutdown_and_wait();

    // Wait until every worker task and its completion have returned while
    // keeping the executor available. Network callbacks are deliberately not
    // included because they may enqueue a later worker task in a sequence.
    void wait_until_idle();

private:
    void release_async_operation();
    void worker_loop();

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_operations;
    size_t m_async_operations = 0;
    bool m_stopping = false;
    bool m_executing = false;
    std::thread m_worker;
};

} // namespace Slic3r

#endif // slic3r_Updater_UpdaterOperationExecutor_hpp_
