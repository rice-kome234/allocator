/** @file parallel_tasks.h
 * @brief 任意個数のワーカーに添字範囲と処理を渡す、測定・使用例用の実行器。
 */
#pragma once

#include "slab/slab_pool.h"
#include <cstddef>
#include <functional>
#include <memory>

namespace benchmarks {

/**
 * @brief 指定本数のスレッドを再利用し、処理の完了まで待つ。
 * @note 同じ実行器の呼び出し元は一つ。コールバック内での再入・例外送出は不可。
 * スレッド生成などの資源不足は回復処理の対象外。
 */
class ParallelTasks {
    struct State;
public:
    /** @brief 引数は workerIndex, first, last。担当範囲は [first, last)。 */
    using Task = std::function<void(std::size_t, std::size_t, std::size_t)>;

    /** @brief workerCount は 1 以上。全スレッドの起動完了を待ってから戻る。 */
    explicit ParallelTasks(std::size_t workerCount);
    ~ParallelTasks();
    ParallelTasks(const ParallelTasks&) = delete;
    ParallelTasks& operator=(const ParallelTasks&) = delete;
    ParallelTasks(ParallelTasks&&) = delete;
    ParallelTasks& operator=(ParallelTasks&&) = delete;

    /** @brief 全件を均等に分担する。空の担当範囲も呼び出し、全処理完了後に戻る。 */
    void ForEachRange(std::size_t count, const Task& task);

    /**
     * @brief ポインタ列を ID ごとに一括準備し、区間単位で並列コピーして公開する。
     * @details 実体は複製しない。戻った時点で全コピー完了済み。batchSize は CAS 1回分の個数。
     * @pre 同じ Pool を他の処理で使用しない。入力・実体の寿命は呼び出し側が保持する。
     */
    [[nodiscard]]
    bool CopyPointers(slab::SlabPool& pool, slab::SlabPool::PoolId id,
        std::span<void* const> pointers, std::size_t batchSize = 256);

    /** @brief 現在の登録数を均等分割し、指定メンバ関数を全対象へ適用して完了を待つ。 */
    template<class T>
    void ForEach(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
        void (T::*function)(float) noexcept, float deltaSeconds);

    [[nodiscard]]
    std::size_t WorkerCount() const noexcept;

private:
    std::unique_ptr<State> state_{};
};

}

#include "parallel_tasks_impl.h"
