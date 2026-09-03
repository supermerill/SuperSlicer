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

/*
Parallel plugin jobs
====================

`parallel_for_storage_with_progress()` runs the independent indices in the
half-open range `[begin, end)` through the host's parallel executor while
keeping the C ABI callback details inside this header.

The worker must have this shape:

    void worker(uint32_t index, storage_handle *scratch_storage, Args... args);

The normal execution of one job is:

    check cancellation
        |
    call worker(index, scratch_storage, args...)
        |
    increment PluginProgress when the worker returns normally

Each job receives a fresh `scratch_storage`. It is destroyed immediately after
that worker returns, so handles allocated from it must be copied or moved into
longer-lived storage before the worker finishes. The extra arguments are
decayed and copied into one descriptor before the parallel loop starts. Pass
pointers or references to shared objects when copying would be expensive, but
protect every shared mutation with the appropriate synchronization.

Cancellation is checked before each job through `throw_if_cancelled()`. A
`PluginCancelled` exception stops normal work and is handled by the surrounding
plugin ABI wrapper. Other worker exceptions are not converted into warnings by
this helper; they propagate to the caller and must be invoked from a guarded
plugin callback such as `PluginBase::run_impl()`.

The call waits for the parallel executor to finish. Progress is incremented
only after a worker completes successfully, so a cancelled or throwing worker
does not report completed work.
*/
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
