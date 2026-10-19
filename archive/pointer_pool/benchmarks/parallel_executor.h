/**
 * @file parallel_executor.h
 * @brief Slab 内のポインタを、常駐ワーカーで並列処理するテスト用の補助クラス。
 */
#pragma once

#include "slab/slab_pool.h"

#include <memory>

namespace benchmarks {

/**
 * @brief main から渡されたメンバ関数を、4 本の常駐ワーカーで実行する。
 * @note ForEach は同期呼び出し。戻った時点で全対象の処理が完了している。
 * 呼び出し元は一つのスレッドに限定し、実行中は Pool と実体の寿命を変更しない。
 * 各対象の更新は独立していること。同じ実体を重複登録しないこと。
 * スレッド生成などの資源不足は回復処理の対象外。
 */
class ParallelExecutor {
    struct State;

public:
    /** @brief ワーカーは生成時に一度だけ起動する。 */
    ParallelExecutor();

    /** @brief 待機中のワーカーを停止し、全スレッドの終了を待つ。 */
    ~ParallelExecutor();

    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(const ParallelExecutor&) = delete;
    ParallelExecutor(ParallelExecutor&&) = delete;
    ParallelExecutor& operator=(ParallelExecutor&&) = delete;

    /**
     * @brief 登録された全 T に、渡されたメンバ関数と時間刻みで処理を行う。
     * @pre id に登録されているポインタは、すべて T* に戻せること。
     * @note 未登録 ID または空の ID は何もしない。メンバ関数は nullptr 不可。
     * 実装は parallel_executor_impl.h、同期処理は parallel_executor.cpp に置く。
     */
    template<class T>
    void ForEach(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
        void (T::*function)(float) noexcept, float deltaSeconds);

private:
    using BatchFunction = void (*)(std::span<void* const>, const void*) noexcept;

    /** @brief コールバックと引数は完了バリアまでだけ参照し、保持しない。 */
    void Execute(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
        BatchFunction function, const void* context);

    std::unique_ptr<State> state_{};
};

}

#include "parallel_executor_impl.h"
