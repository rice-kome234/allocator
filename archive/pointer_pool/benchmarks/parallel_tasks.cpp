/** @file parallel_tasks.cpp
 * @brief バリアによる開始通知・完了待ちと、スレッドの所有を実装する。
 */
#include "parallel_tasks.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace benchmarks {

struct ParallelTasks::State {
    std::size_t workerCount{};
    std::barrier<> ready;
    std::barrier<> start;
    std::barrier<> finish;
    std::unique_ptr<std::jthread[]> workers{};
    const Task* task{};
    std::size_t count{};
    bool stopping{};

    explicit State(std::size_t value)
        : workerCount{value}, ready{static_cast<std::ptrdiff_t>(value + 1)},
          start{static_cast<std::ptrdiff_t>(value + 1)}, finish{static_cast<std::ptrdiff_t>(value + 1)} {
        workers = std::make_unique<std::jthread[]>(workerCount);
        for (std::size_t worker{}; worker < workerCount; ++worker) {
            workers[worker] = std::jthread{[this, worker] {
                ready.arrive_and_wait();
                for (;;) {
                    start.arrive_and_wait();
                    if (stopping) { return; }
                    const auto portion{count / workerCount};
                    const auto remainder{count % workerCount};
                    const auto first{portion * worker + std::min(worker, remainder)};
                    std::size_t last{first + portion};
                    if (worker < remainder) { ++last; }
                    (*task)(worker, first, last);
                    finish.arrive_and_wait();
                }
            }};
        }
        ready.arrive_and_wait();
    }

    ~State() {
        stopping = true;
        start.arrive_and_wait();
        for (std::size_t i{}; i < workerCount; ++i) { workers[i].join(); }
    }
};

ParallelTasks::ParallelTasks(std::size_t workerCount) {
    if (workerCount == 0 || workerCount >= static_cast<std::size_t>(std::barrier<>::max())) {
        std::cerr << "[並列実行エラー] ワーカー数が範囲外です。\n";
        std::exit(EXIT_FAILURE);
    }
    state_ = std::make_unique<State>(workerCount);
}

ParallelTasks::~ParallelTasks() = default;

void ParallelTasks::ForEachRange(std::size_t count, const Task& task) {
    state_->task = &task;
    state_->count = count;
    state_->start.arrive_and_wait();
    state_->finish.arrive_and_wait();
    state_->task = nullptr;
}

std::size_t ParallelTasks::WorkerCount() const noexcept { return state_->workerCount; }

bool ParallelTasks::CopyPointers(slab::SlabPool& pool, slab::SlabPool::PoolId id,
    std::span<void* const> pointers, std::size_t batchSize) {
    if (batchSize == 0 || std::any_of(pointers.begin(), pointers.end(),
        [](void* pointer) { return pointer == nullptr; })) { return false; }
    if (!pool.BeginConcurrentAppend(id, pointers.size())) { return false; }
    std::atomic<bool> succeeded{true};
    ForEachRange(pointers.size(), [&](std::size_t, std::size_t first, std::size_t last) {
        while (first < last) {
            const auto length{std::min(batchSize, last - first)};
            if (pool.AddBatchConcurrent(id, pointers.subspan(first, length)) == slab::SlabPool::InvalidIndex) {
                succeeded.store(false, std::memory_order_relaxed);
            }
            first += length;
        }
    }); // 完了バリア後なので、予約区間のすべてのコピーが呼び出し元へ公開されている。
    const bool ended{pool.EndConcurrentAppend(id)};
    return ended && succeeded.load(std::memory_order_relaxed);
}

}
