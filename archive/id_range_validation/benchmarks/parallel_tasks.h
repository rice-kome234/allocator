/** @file parallel_tasks.h
 * @brief 使用例と計測用の汎用実行器。SlabPool本体から独立してスレッドを管理する。
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace benchmarks {

/**
 * @brief ワーカーを再利用し、全処理の完了まで待つ。
 * @note 呼び出し元は一つ。再入・コールバックの例外送出は不可。
 * スレッド生成など標準ライブラリの資源不足は回復処理の対象外。
 */
class ParallelTasks {
    struct State;
public:
    /** @brief workerIndex と担当する [first, last) を受け取る。 */
    using Task = std::function<void(std::size_t, std::size_t, std::size_t)>;

    explicit ParallelTasks(std::size_t workerCount);
    ~ParallelTasks();
    ParallelTasks(const ParallelTasks&) = delete;
    ParallelTasks& operator=(const ParallelTasks&) = delete;
    ParallelTasks(ParallelTasks&&) = delete;
    ParallelTasks& operator=(ParallelTasks&&) = delete;

    /** @brief 添字を均等分担し、全ワーカーの完了後に戻る。 */
    void ForEachRange(std::size_t count, const Task& task);

    [[nodiscard]]
    std::size_t WorkerCount() const noexcept;

private:
    std::unique_ptr<State> state_{};
};

}

