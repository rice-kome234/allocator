/**
 * @file growth_benchmark_internal.h
 * @brief 単一・複数スレッドの追加測定で共有する実体とコンテナ接続。
 */
#pragma once

#include "slab/slab_pool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <vector>

namespace benchmarks::growth_detail {

using Clock = std::chrono::steady_clock;
using Pool = slab::SlabPool;
constexpr Pool::PoolId objectId{1};

inline void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[追加測定の検証失敗] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]]
inline double Milliseconds(Clock::duration elapsed) noexcept {
    return std::chrono::duration<double, std::milli>{elapsed}.count();
}

[[nodiscard]]
inline double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/** @brief 確保元を全方式で統一する。実体の生成はどの時間にも含めない。 */
struct Source {
    struct Object {
        std::uint64_t id{};
        std::array<std::uint64_t, 7> padding{};
    };
    static_assert(sizeof(Object) == 64);
    std::vector<std::unique_ptr<Object>> owners{};
    std::vector<void*> pointers{};

    explicit Source(std::size_t count) {
        owners.reserve(count);
        pointers.reserve(count);
        for (std::size_t i{}; i < count; ++i) {
            std::unique_ptr<Object> object{new (std::nothrow) Object{i, {}}};
            Require(object != nullptr, "比較用の実体を生成できませんでした。");
            pointers.push_back(object.get());
            owners.push_back(std::move(object));
        }
    }

    void ValidateObjects() const {
        for (std::size_t i{}; i < owners.size(); ++i) {
            Require(owners[i].get() == pointers[i] && owners[i]->id == i,
                "コンポーネント本体のアドレスまたは値が変化しました。");
        }
    }
};

struct Configuration {
    const char* name{};
    bool slab{};
    bool reserve{};
    std::size_t growthBytes{};
};

/** @brief std::vector の既定アロケータと拡張方針をそのまま測る。 */
class VectorStorage {
public:
    explicit VectorStorage(const Configuration& configuration) : reserve_{configuration.reserve} {}

    void Prepare(std::size_t count) {
        if (reserve_) { pointers_.reserve(count); }
    }

    [[nodiscard]]
    bool Append(void* pointer) {
        pointers_.push_back(pointer);
        return true;
    }

    [[nodiscard]]
    std::size_t Capacity() const noexcept { return pointers_.capacity(); }

    /** @brief vector も、共有時は1回の追加ごとに mutex を取得する。 */
    [[nodiscard]]
    bool AppendConcurrent(void* pointer) {
        const std::lock_guard lock{mutex_};
        return Append(pointer);
    }

    void Validate(std::span<void* const> expected) const {
        Require(pointers_.size() == expected.size(), "vectorの登録数が不一致です。");
        Require(std::equal(pointers_.begin(), pointers_.end(), expected.begin()),
            "vector内のポインタの値または順序が不一致です。");
    }

    /** @brief 計測終了後の集合検証用にポインタ列を複製する。 */
    [[nodiscard]]
    std::vector<void*> CopyPointers() const { return pointers_; }
private:
    bool reserve_{};
    std::vector<void*> pointers_{};
    std::mutex mutex_{};
};

/** @brief 初期容量0の本物のSlabPoolを使う。内部索引の拡張もAdd時間に含まれる。 */
class SlabStorage {
public:
    explicit SlabStorage(const Configuration& configuration) : growthBytes_{configuration.growthBytes} {}

    void Prepare(std::size_t) {
        Require(pool_.Register(objectId, Pool::Options{0, growthBytes_}), "SlabのID登録に失敗しました。");
    }

    [[nodiscard]]
    bool Append(void* pointer) noexcept { return pool_.Add(objectId, pointer) != Pool::InvalidIndex; }

    [[nodiscard]]
    bool AppendConcurrent(void* pointer) noexcept {
        return pool_.AddConcurrent(objectId, pointer) != Pool::InvalidIndex;
    }

    [[nodiscard]]
    std::size_t Capacity() const noexcept { return pool_.GetStatistics().capacity; }

    void Validate(std::span<void* const> expected) const {
        Require(pool_.Count(objectId) == expected.size(), "Slabの登録数が不一致です。");
        std::size_t first{};
        while (first < expected.size()) {
            const auto batch{pool_.GetBatch(objectId, first)};
            Require(!batch.empty() && batch.size() <= expected.size() - first, "Slabの範囲が不正です。");
            Require(std::equal(batch.begin(), batch.end(), expected.begin() + first),
                "Slab内のポインタの値または順序が不一致です。");
            first += batch.size();
        }
        const auto slots{growthBytes_ / sizeof(void*)};
        const auto expectedSlabs{(expected.size() + slots - 1) / slots};
        const auto statistics{pool_.GetStatistics()};
        Require(statistics.slabs == expectedSlabs && statistics.capacity == expectedSlabs * slots,
            "Slabの拡張回数または最終容量が不一致です。");
    }

    /** @brief 計測終了後の集合検証用に、全Slabからポインタ列を複製する。 */
    [[nodiscard]]
    std::vector<void*> CopyPointers() const {
        std::vector<void*> result{};
        const auto count{pool_.Count(objectId)};
        result.reserve(count);
        for (std::size_t first{}; first < count;) {
            const auto batch{pool_.GetBatch(objectId, first)};
            Require(!batch.empty(), "Slabのポインタ列を取り出せませんでした。");
            result.insert(result.end(), batch.begin(), batch.end());
            first += batch.size();
        }
        return result;
    }
private:
    std::size_t growthBytes_{};
    Pool pool_{};
};

}
