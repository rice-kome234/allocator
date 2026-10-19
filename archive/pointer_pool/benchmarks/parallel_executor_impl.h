/**
 * @file parallel_executor_impl.h
 * @brief 型付きメンバ関数を、型を知らない実行処理へ接続する。
 */
#pragma once

namespace benchmarks {

template<class T>
void ParallelExecutor::ForEach(const slab::SlabPool& pool, slab::SlabPool::PoolId id,
    void (T::*function)(float) noexcept, float deltaSeconds) {
    struct Context {
        void (T::*function)(float) noexcept{};
        float deltaSeconds{};
    };
    const Context context{function, deltaSeconds};
    Execute(pool, id, [](std::span<void* const> pointers, const void* data) noexcept {
        const auto& arguments{*static_cast<const Context*>(data)};
        for (auto& object : slab::SlabPool::Batch<T>{pointers}) {
            (object.*arguments.function)(arguments.deltaSeconds);
        }
    }, &context);
}

}
