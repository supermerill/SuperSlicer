///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file adapts the updater's injectable request contract to Http. The
// adapter deliberately contains no repository logic: URLs, limits and response
// handling remain visible in PresetUpdater and PluginUpdater.

#include "libslic3r/Updater/UpdaterHttp.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "libslic3r/Updater/Http.hpp"

namespace Slic3r {
namespace {

// Executes updater requests with the application's regular Http backend. An
// asynchronous request shares its callback state with both Http callbacks so
// it stays alive until the network transfer has selected a terminal result.
class DefaultUpdaterHttpTransport final : public UpdaterHttpTransport
{
private:
    void perform_request(UpdaterHttpRequest request) override;
    void perform_request_sync(UpdaterHttpRequest request) override;
};

void DefaultUpdaterHttpTransport::perform_request(UpdaterHttpRequest request)
{
    const std::shared_ptr<UpdaterHttpRequest> state =
        std::make_shared<UpdaterHttpRequest>(std::move(request));

    Http::get(state->url())
        .size_limit(state->response_size_limit())
        .on_error([state](std::string body, std::string error, unsigned http_status) {
            fail_request(*state, std::move(body), std::move(error), http_status);
        })
        .on_complete([state](std::string body, unsigned http_status) {
            complete_request(*state, std::move(body), http_status);
        })
        .on_progress([state](Http::Progress progress, bool &cancel) {
            progress_request(*state,
                             progress.dltotal,
                             progress.dlnow,
                             progress.ultotal,
                             progress.ulnow,
                             progress.buffer,
                             cancel);
        })
        .perform();
}

void DefaultUpdaterHttpTransport::perform_request_sync(UpdaterHttpRequest request)
{
    Http::get(request.url())
        .size_limit(request.response_size_limit())
        .on_error([&request](std::string body, std::string error, unsigned http_status) {
            fail_request(request, std::move(body), std::move(error), http_status);
        })
        .on_complete([&request](std::string body, unsigned http_status) {
            complete_request(request, std::move(body), http_status);
        })
        .on_progress([&request](Http::Progress progress, bool &cancel) {
            progress_request(request,
                             progress.dltotal,
                             progress.dlnow,
                             progress.ultotal,
                             progress.ulnow,
                             progress.buffer,
                             cancel);
        })
        .perform_sync();
}

} // namespace

UpdaterHttpRequest::UpdaterHttpRequest(UpdaterHttpTransport &transport, std::string url)
    : m_transport(&transport), m_url(std::move(url))
{
}

UpdaterHttpRequest &UpdaterHttpRequest::size_limit(size_t size_limit)
{
    m_size_limit = size_limit;
    return *this;
}

UpdaterHttpRequest &UpdaterHttpRequest::on_complete(CompleteFn callback)
{
    m_complete = std::move(callback);
    return *this;
}

UpdaterHttpRequest &UpdaterHttpRequest::on_error(ErrorFn callback)
{
    m_error = std::move(callback);
    return *this;
}

UpdaterHttpRequest &UpdaterHttpRequest::on_progress(ProgressFn callback)
{
    m_progress = std::move(callback);
    return *this;
}

void UpdaterHttpRequest::perform()
{
    if (m_transport == nullptr)
        throw std::logic_error("An updater HTTP request may only be performed once.");

    UpdaterHttpTransport *transport = m_transport;
    m_transport = nullptr;
    transport->perform_request(std::move(*this));
}

void UpdaterHttpRequest::perform_sync()
{
    if (m_transport == nullptr)
        throw std::logic_error("An updater HTTP request may only be performed once.");

    UpdaterHttpTransport *transport = m_transport;
    m_transport = nullptr;
    transport->perform_request_sync(std::move(*this));
}

UpdaterHttpRequest UpdaterHttpTransport::get(std::string url)
{
    return UpdaterHttpRequest(*this, std::move(url));
}

void UpdaterHttpTransport::complete_request(UpdaterHttpRequest &request,
                                            std::string body,
                                            unsigned http_status)
{
    UpdaterHttpRequest::CompleteFn callback = std::move(request.m_complete);
    request.m_error = UpdaterHttpRequest::ErrorFn();
    request.m_progress = UpdaterHttpRequest::ProgressFn();
    if (callback)
        callback(std::move(body), http_status);
}

void UpdaterHttpTransport::fail_request(UpdaterHttpRequest &request,
                                        std::string body,
                                        std::string error,
                                        unsigned http_status)
{
    UpdaterHttpRequest::ErrorFn callback = std::move(request.m_error);
    request.m_complete = UpdaterHttpRequest::CompleteFn();
    request.m_progress = UpdaterHttpRequest::ProgressFn();
    if (callback)
        callback(std::move(body), std::move(error), http_status);
}

void UpdaterHttpTransport::progress_request(UpdaterHttpRequest &request,
                                            size_t dltotal,
                                            size_t dlnow,
                                            size_t ultotal,
                                            size_t ulnow,
                                            const std::string &buffer,
                                            bool &cancel)
{
    if (request.m_progress)
        request.m_progress(UpdaterHttpRequest::Progress{dltotal, dlnow, ultotal, ulnow, buffer}, cancel);
}

UpdaterHttpTransport &default_updater_http_transport()
{
    static DefaultUpdaterHttpTransport transport;
    return transport;
}

} // namespace Slic3r
