/** @file parallel_tasks_impl.h
 * @brief 型付き範囲へ main から渡されたメンバ関数を適用する。
 */
#pragma once

namespace benchmarks {

template<class T>
void ParallelTasks::ForEach(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
    void (T::*function)(float) noexcept, float deltaSeconds) {
    ForEachRange(pool.Count(id), [&](std::size_t, std::size_t first, std::size_t last) {
        for (auto& object : pool.GetRange<T>(id, first, last - first)) {
            (object.*function)(deltaSeconds);
        }
    });
}

}
