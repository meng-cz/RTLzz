#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <thread>
#include <vector>

namespace pred {

// Fixed contiguous ranges keep output ordering and exception selection stable.
// The caller participates, so threads is the total concurrency, not extra workers.
template<class Function>
void parallelRanges(std::size_t count, std::size_t threads, std::size_t grain,
                    Function function) {
    if (!count) return;
    const auto workers = std::min(std::max(std::size_t{1}, threads),
                                  std::max(std::size_t{1}, count / grain));
    if (workers == 1) { function(0, count); return; }
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread> pool;
    auto run = [&](std::size_t worker) {
        try { function(count * worker / workers, count * (worker + 1) / workers); }
        catch (...) { errors[worker] = std::current_exception(); }
    };
    try {
        for (std::size_t i = 1; i < workers; ++i) pool.emplace_back(run, i);
        run(0);
    } catch (...) {
        for (auto& thread : pool) thread.join();
        throw;
    }
    for (auto& thread : pool) thread.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
}

template<class Function>
void parallelFor(std::size_t count, std::size_t threads, Function function,
                 std::size_t grain = 128) {
    parallelRanges(count, threads, grain, [&](std::size_t begin, std::size_t end) {
        for (auto i = begin; i < end; ++i) function(i);
    });
}

} // namespace pred
