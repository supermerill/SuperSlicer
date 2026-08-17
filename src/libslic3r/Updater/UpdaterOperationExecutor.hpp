///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// UpdaterOperationExecutor serializes filesystem mutations on one background
// thread. Call enqueue() with work returning UpdaterError and a terminal
// callback; exceptions from either side are contained at the worker boundary.
// Updater owners must call shutdown_and_wait() in their derived destructor
// before members captured by queued operations begin to disappear.

#ifndef slic3r_Updater_UpdaterOperationExecutor_hpp_
#define slic3r_Updater_UpdaterOperationExecutor_hpp_

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {

class UpdaterOperationExecutor
{
public:
    using Operation = std::function<UpdaterError()>;
    using Completion = std::function<void(UpdaterError)>;

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

    // Stop accepting work, drain accepted tasks, then join the worker. This is
    // idempotent so derived and base destructors may both enforce the lifetime
    // rule without coordinating ownership details.
    void shutdown_and_wait();

    // Wait until every accepted task and its completion have returned while
    // keeping the executor available. Headless callers and deterministic tests
    // use this when they need a synchronous observation point.
    void wait_until_idle();

private:
    void worker_loop();

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_operations;
    bool m_stopping = false;
    bool m_executing = false;
    std::thread m_worker;
};

} // namespace Slic3r

#endif // slic3r_Updater_UpdaterOperationExecutor_hpp_
