///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ParallelFor_hpp_
#define slic3r_Api_plugin_cpp_ParallelFor_hpp_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_utils.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api {
namespace detail {

template<class Fn, class... Args>
struct ParallelForStorageWithProgressData
{
    const plugin_run_context *run_ctx = nullptr;
    PluginProgress *progress = nullptr;
    Fn fn;
    std::tuple<Args...> args;
};

template<class Fn, class... Args, std::size_t... Indices>
void call_parallel_for_storage_worker(ParallelForStorageWithProgressData<Fn, Args...> &data,
                                      const uint32_t index,
                                      storage_handle *scratch_storage,
                                      std::index_sequence<Indices...>)
{
    data.fn(index, scratch_storage, std::get<Indices>(data.args)...);
}

template<class Fn, class... Args>
void parallel_for_storage_with_progress_bridge(const uint32_t index,
                                               storage_handle *scratch_storage,
                                               void *user_data)
{
    ParallelForStorageWithProgressData<Fn, Args...> *data =
        static_cast<ParallelForStorageWithProgressData<Fn, Args...> *>(user_data);
    assert(data != nullptr);
    assert(scratch_storage != nullptr);

    throw_if_cancelled(data->run_ctx);
    call_parallel_for_storage_worker(*data, index, scratch_storage, std::index_sequence_for<Args...>{});
    if (data->progress != nullptr)
        data->progress->increment();
}

} // namespace detail

// Run independent jobs in parallel while hiding the C ABI plumbing.
//
// The worker function must have this shape:
//
//     void worker(uint32_t index, storage_handle *scratch_storage, Args... args);
//
// The helper checks cancellation before each job, gives the worker a temporary
// scratch storage that belongs only to that job, and increments progress after
// the worker returns. Extra arguments are copied into a small shared descriptor
// before the loop starts. Pass pointers for shared objects, mutexes, or large
// views so the helper does not copy them and so ownership remains obvious.
template<class Fn, class... Args>
void parallel_for_storage_with_progress(const uint32_t begin,
                                        const uint32_t end,
                                        const plugin_run_context *run_ctx,
                                        PluginProgress *progress,
                                        Fn &&fn,
                                        Args&&... args)
{
    using StoredFn = std::decay_t<Fn>;
    using Data = detail::ParallelForStorageWithProgressData<StoredFn, std::decay_t<Args>...>;

    Data data {
        run_ctx,
        progress,
        std::forward<Fn>(fn),
        std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)
    };

    slic3r_parallel_for_storage(
        begin,
        end,
        &data,
        &detail::parallel_for_storage_with_progress_bridge<StoredFn, std::decay_t<Args>...>);
}

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_ParallelFor_hpp_
