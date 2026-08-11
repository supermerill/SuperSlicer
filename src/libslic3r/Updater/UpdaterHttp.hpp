///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// UpdaterHttp defines the small network contract used by repository updaters.
// Production code obtains a request from UpdaterHttpTransport, configures its
// callbacks, and starts it with perform() or perform_sync(). Tests may inject a
// transport which stores these requests and completes them explicitly, so no
// real network connection or background thread is required.

#ifndef slic3r_Updater_UpdaterHttp_hpp_
#define slic3r_Updater_UpdaterHttp_hpp_

#include <cstddef>
#include <functional>
#include <string>

namespace Slic3r {

class UpdaterHttpTransport;

// Describes one GET request independently from the concrete HTTP library. A
// request is move-only because its callbacks and execution belong to one
// transfer. Configure it with the fluent methods, then execute it exactly once.
class UpdaterHttpRequest
{
public:
    struct Progress {
        size_t dltotal;
        size_t dlnow;
        size_t ultotal;
        size_t ulnow;
        const std::string &buffer;
    };

    using CompleteFn = std::function<void(std::string /* body */, unsigned /* http_status */)>;
    using ErrorFn = std::function<void(std::string /* body */,
                                       std::string /* error */,
                                       unsigned /* http_status */)>;
    using ProgressFn = std::function<void(Progress, bool & /* cancel */)>;

    UpdaterHttpRequest(UpdaterHttpRequest &&other) noexcept = default;
    UpdaterHttpRequest &operator=(UpdaterHttpRequest &&other) noexcept = default;
    UpdaterHttpRequest(const UpdaterHttpRequest &) = delete;
    UpdaterHttpRequest &operator=(const UpdaterHttpRequest &) = delete;

    // Limits the response body retained in memory. Zero keeps Http's normal
    // default limit; updater callers normally set an explicit package limit.
    UpdaterHttpRequest &size_limit(size_t size_limit);

    // A successful HTTP response invokes on_complete. Network failures and
    // HTTP status codes of 400 or greater invoke on_error; the body may still
    // contain a server-provided diagnostic in that case.
    UpdaterHttpRequest &on_complete(CompleteFn callback);
    UpdaterHttpRequest &on_error(ErrorFn callback);

    // Progress callbacks may set cancel to stop the transfer. The buffer is a
    // borrowed view of the body accumulated by the transport and is valid only
    // for the duration of the callback.
    UpdaterHttpRequest &on_progress(ProgressFn callback);

    // Starts the request and returns immediately. The transport owns the
    // request state until it invokes one completion callback.
    void perform();

    // Runs the request on the caller's thread. The completion or error callback
    // is invoked before this method returns.
    void perform_sync();

    const std::string &url() const { return m_url; }
    size_t response_size_limit() const { return m_size_limit; }

private:
    friend class UpdaterHttpTransport;

    UpdaterHttpRequest(UpdaterHttpTransport &transport, std::string url);

    UpdaterHttpTransport *m_transport;
    std::string m_url;
    size_t m_size_limit = 0;
    CompleteFn m_complete;
    ErrorFn m_error;
    ProgressFn m_progress;
};

// Creates and executes updater requests. Custom transports normally retain an
// asynchronous request in perform_request(), then call complete_request() or
// fail_request() exactly once when their simulated or real operation finishes.
class UpdaterHttpTransport
{
public:
    virtual ~UpdaterHttpTransport() = default;

    UpdaterHttpRequest get(std::string url);

protected:
    // These helpers consume the selected callback before invoking it. This
    // prevents an accidental second transport notification from completing the
    // same logical updater operation twice.
    static void complete_request(UpdaterHttpRequest &request, std::string body, unsigned http_status);
    static void fail_request(UpdaterHttpRequest &request,
                             std::string body,
                             std::string error,
                             unsigned http_status);

    // Reports an intermediate transfer state without consuming the terminal
    // callbacks. A fake transport may use the returned cancel value to verify
    // how an updater reacts to cancellation requests.
    static void progress_request(UpdaterHttpRequest &request,
                                 size_t dltotal,
                                 size_t dlnow,
                                 size_t ultotal,
                                 size_t ulnow,
                                 const std::string &buffer,
                                 bool &cancel);

private:
    friend class UpdaterHttpRequest;

    virtual void perform_request(UpdaterHttpRequest request) = 0;
    virtual void perform_request_sync(UpdaterHttpRequest request) = 0;
};

// Returns the process-wide production transport. The object itself has no
// replaceable global test state; tests inject their own transport into an
// updater constructor instead.
UpdaterHttpTransport &default_updater_http_transport();

} // namespace Slic3r

#endif // slic3r_Updater_UpdaterHttp_hpp_
