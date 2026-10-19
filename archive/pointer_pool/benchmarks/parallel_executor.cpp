/**
 * @file parallel_executor.cpp
 * @brief ワーカーの起動、担当範囲の分割、開始・完了の同期を実装する。
 */
#include "parallel_executor.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <thread>

namespace benchmarks {

/** @brief public ヘッダーからスレッドと同期の詳細を隠す。 */
struct ParallelExecutor::State {
    static constexpr std::size_t WorkerCount{4};

    std::barrier<> start{static_cast<std::ptrdiff_t>(WorkerCount + 1)};
    std::barrier<> finish{static_cast<std::ptrdiff_t>(WorkerCount + 1)};
    bool stopped{};
    const slab::SlabPool* pool{};
    slab::SlabPool::PoolId id{};
    std::size_t count{};
    BatchFunction function{};
    const void* context{};
    std::array<std::jthread, WorkerCount> workers{};

    State() {
        for (std::size_t worker{}; worker < WorkerCount; ++worker) {
            workers[worker] = std::jthread{[this, worker] { Work(worker); }};
        }
    }

    ~State() {
        stopped = true;
        start.arrive_and_wait();
        for (auto& worker : workers) { worker.join(); }
    }

    /** @brief 一つのジョブを終えたら、次の開始バリアまで待機する。 */
    void Work(std::size_t worker) {
        while (true) {
            start.arrive_and_wait();
            if (stopped) { break; }

            const std::size_t portion{count / WorkerCount};
            const std::size_t remainder{count % WorkerCount};
            std::size_t first{portion * worker + std::min(worker, remainder)};
            const std::size_t last{portion * (worker + 1) + std::min(worker + 1, remainder)};
            while (first < last) {
                const auto batch{pool->GetBatch(id, first, last - first)};
                function(batch, context);
                first += batch.size();
            }
            finish.arrive_and_wait();
        }
    }
};

ParallelExecutor::ParallelExecutor() : state_{std::make_unique<State>()} {}
ParallelExecutor::~ParallelExecutor() = default;

void ParallelExecutor::Execute(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
    BatchFunction function, const void* context) {
    const auto count{pool.Count(id)};
    if (count == 0) { return; }
    state_->pool = &pool;
    state_->id = id;
    state_->count = count;
    state_->function = function;
    state_->context = context;
    // 開始バリアでジョブを公開し、完了バリアで全対象の更新を受け取る。
    state_->start.arrive_and_wait();
    state_->finish.arrive_and_wait();
    state_->pool = nullptr;
    state_->function = nullptr;
    state_->context = nullptr;
}

}
