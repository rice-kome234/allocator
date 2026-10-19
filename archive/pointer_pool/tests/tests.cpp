#include "tests.h"
#include "slab/slab_pool.h"
#include "../benchmarks/parallel_executor.h"
#include "../benchmarks/parallel_tasks.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tests {
namespace {

using Pool = slab::SlabPool;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "[テスト失敗] 行=" << line << " / 条件=" << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

/** @brief 配列の独立モデルと、全 Slab の連続範囲を照合する。 */
void Compare(const Pool& pool, Pool::PoolId id, const std::vector<void*>& expected) {
    CHECK(pool.Count(id) == expected.size());
    std::size_t first{};
    while (first < expected.size()) {
        const auto batch{pool.GetBatch(id, first, 11)};
        CHECK(!batch.empty() && batch.size() <= 11);
        for (std::size_t i{}; i < batch.size(); ++i) { CHECK(batch[i] == expected[first + i]); }
        first += batch.size();
    }
    CHECK(pool.GetBatch(id, first).empty());
    CHECK(pool.At(id, first) == nullptr);
}

void Registration() {
    Pool pool{};
    int value{};
    CHECK(pool.Count(1) == 0 && pool.At(1, 0) == nullptr);
    CHECK(pool.GetBatch(1, 0).empty());
    CHECK(pool.Add(1, &value) == Pool::InvalidIndex);
    CHECK(!pool.Reserve(1, 4));
    CHECK(pool.RemoveAt(1, 0).removed == nullptr);
    pool.Clear(1);
    const auto maximum{std::numeric_limits<std::size_t>::max()};
    CHECK(!pool.Register(0, Pool::Options{0, 0}));
    CHECK(!pool.Register(0, Pool::Options{maximum, 4 * sizeof(void*)}));
    CHECK(!pool.Register(0, Pool::Options{0, maximum}));
    CHECK(!pool.Register(0, Pool::Options{0, sizeof(void*) - 1}));
    CHECK(!pool.Register(0, Pool::Options{0, 64 + 1}));
    CHECK(!pool.Register(0, Pool::Options{0, maximum - maximum % sizeof(void*)}));
    CHECK(pool.GetStatistics().groups == 0);
    CHECK(pool.Register(0, Pool::Options{0, 4 * sizeof(void*)}));
    CHECK(pool.Register(0, Pool::Options{0, 4 * sizeof(void*)}));
    CHECK(!pool.Register(0, Pool::Options{0, 5 * sizeof(void*)}));
    CHECK(pool.Add(0, nullptr) == Pool::InvalidIndex);
    CHECK(pool.GetStatistics().slabs == 0);
    CHECK(pool.Add(0, &value) == 0);
    CHECK(pool.GetStatistics().capacity == 4);
    CHECK(pool.Register(std::numeric_limits<Pool::PoolId>::max()));
    CHECK(pool.GetStatistics().groups == 2);
}

/** @brief 初期個数を保持し、既定の 64 バイト拡張と全 Slab の先頭境界を確認する。 */
void AlignedGrowth() {
    Pool pool{};
    Pool::Options options{};
    options.initialCapacity = 100;
    CHECK(options.growthBytes == 64);
    CHECK(pool.Register(1, options));
    constexpr std::size_t growthSlots{64 / sizeof(void*)};
    constexpr std::size_t expandedCount{100 + growthSlots * 2 + 1};
    constexpr std::size_t expandedCapacity{100 + growthSlots * 3};
    std::array<int, 200> values{};
    CHECK(pool.Add(1, &values[0]) == 0);
    const auto* initialAddress{pool.GetBatch(1, 0).data()};
    for (std::size_t i{1}; i < expandedCount; ++i) {
        CHECK(pool.Add(1, &values[i]) == i);
    }
    CHECK(pool.GetBatch(1, 0).data() == initialAddress);
    CHECK(pool.GetBatch(1, 0).size() == 100);
    CHECK(pool.GetBatch(1, 100).size() == growthSlots);
    CHECK(pool.GetBatch(1, 100 + growthSlots).size() == growthSlots);
    CHECK(pool.GetBatch(1, 100 + growthSlots * 2).size() == 1);
    CHECK(pool.GetStatistics().slabs == 4);
    CHECK(pool.GetStatistics().capacity == expandedCapacity);
    CHECK(pool.GetStatistics().pointerBytes == 100 * sizeof(void*) + 64 * 3);

    // Reserve は不足分をまとめて確保する。先頭境界は保ち、要求個数は丸めない。
    CHECK(pool.Reserve(1, values.size()));
    CHECK(pool.GetStatistics().slabs == 5 && pool.GetStatistics().capacity == values.size());
    for (std::size_t i{expandedCount}; i < values.size(); ++i) {
        CHECK(pool.Add(1, &values[i]) == i);
    }
    CHECK(pool.GetBatch(1, expandedCapacity).size() == values.size() - expandedCapacity);
    CHECK(pool.GetBatch(1, 0).data() == initialAddress);
    std::size_t first{}, slabs{};
    while (first < values.size()) {
        const auto batch{pool.GetBatch(1, first)};
        CHECK(!batch.empty());
        CHECK(reinterpret_cast<std::uintptr_t>(batch.data()) % 64 == 0);
        for (std::size_t i{}; i < batch.size(); ++i) { CHECK(batch[i] == &values[first + i]); }
        first += batch.size();
        ++slabs;
    }
    CHECK(slabs == 5);
    pool.Clear(1);
    for (int& value : values) { CHECK(pool.Add(1, &value) != Pool::InvalidIndex); }
    CHECK(pool.GetStatistics().slabs == 5);
    CHECK(pool.GetBatch(1, 0).data() == initialAddress);

    // 初期 0 でも、最初の Add が 64 バイト分の Slab を確保する。
    options.initialCapacity = 0;
    CHECK(pool.Register(2, options));
    const auto previousCapacity{pool.GetStatistics().capacity};
    CHECK(pool.Add(2, &options) == 0);
    CHECK(pool.GetStatistics().capacity == previousCapacity + growthSlots);
    CHECK(reinterpret_cast<std::uintptr_t>(pool.GetBatch(2, 0).data()) % 64 == 0);
}
void Layout() {
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{100, 4 * sizeof(void*)}));
    CHECK(pool.Register(2, Pool::Options{1, sizeof(void*)}));
    std::array<int, 110> values{};
    for (std::size_t i{}; i < values.size(); ++i) { CHECK(pool.Add(1, &values[i]) == i); }
    CHECK(pool.GetBatch(1, 0).size() == 100);
    CHECK(pool.GetBatch(1, 99).size() == 1);
    CHECK(pool.GetBatch(1, 100).size() == 4);
    CHECK(pool.GetBatch(1, 104).size() == 4);
    CHECK(pool.GetBatch(1, 108).size() == 2);
    CHECK(pool.GetBatch(1, 50, 3).size() == 3);
    CHECK(pool.GetBatch(1, 0, 0).empty());
    CHECK(pool.GetBatch(1, Pool::InvalidIndex).empty());
    for (std::size_t i{}; i < values.size(); ++i) { CHECK(pool.At(1, i) == &values[i]); }
    int other{};
    CHECK(pool.Add(2, &other) == 0);
    CHECK(pool.At(2, 0) == &other && pool.Count(1) == 110);
    const auto stats{pool.GetStatistics()};
    CHECK(stats.count == 111 && stats.slabs == 5 && stats.capacity == 113);
    CHECK(stats.pointerBytes == 113 * sizeof(void*));
}

/** @brief 型付き範囲が実体そのものを参照し、const と Slab 境界を守ることを確認する。 */
void TypedBatches() {
    struct Object { int value{}; };
    static_assert(std::is_same_v<decltype(*std::declval<const Pool::Batch<Object>&>().begin()), Object&>);
    static_assert(std::is_same_v<decltype(*std::declval<const Pool::Batch<const Object>&>().begin()), const Object&>);
    static_assert(std::ranges::random_access_range<Pool::Batch<Object>>);

    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{2, 3 * sizeof(void*)}));
    std::array<Object, 8> objects{};
    for (std::size_t i{}; i < objects.size(); ++i) {
        objects[i].value = static_cast<int>(i);
        CHECK(pool.Add(1, &objects[i]) == i);
    }

    std::size_t first{};
    while (first < objects.size()) {
        const auto batch{pool.GetBatch<Object>(1, first)};
        CHECK(!batch.empty());
        CHECK(std::addressof(batch[0]) == &objects[first]);
        std::size_t offset{};
        for (auto& object : batch) {
            CHECK(std::addressof(object) == &objects[first + offset]);
            object.value += 10;
            ++offset;
        }
        CHECK(offset == batch.size());
        first += batch.size();
    }
    for (std::size_t i{}; i < objects.size(); ++i) {
        CHECK(objects[i].value == static_cast<int>(i) + 10);
    }

    const Pool& readPool{pool};
    const auto limited{readPool.GetBatch<const Object>(1, 2, 2)};
    CHECK(limited.size() == 2 && limited[0].value == 12 && limited[1].value == 13);
    CHECK(readPool.GetBatch<const Object>(1, 1).size() == 1); // 最初の Slab の残り。
    const auto absent{pool.GetBatch<Object>(99, 0)};
    CHECK(absent.empty() && absent.begin() == absent.end());
    CHECK(pool.GetBatch<Object>(1, 0, 0).empty());
    CHECK(pool.GetBatch<Object>(1, objects.size()).empty());
    CHECK(pool.GetBatch<Object>(1, Pool::InvalidIndex).empty());

    // 取得済み span を型付きの範囲に包んでも、実体はコピーされない。
    const Pool::Batch<Object> wrapped{pool.GetBatch(1, 2, 1)};
    CHECK(std::addressof(wrapped[0]) == &objects[2]);
    CHECK(pool.RemoveAt(1, 0).moved == &objects.back());
    CHECK(std::addressof(pool.GetBatch<Object>(1, 0)[0]) == &objects.back());
    pool.Clear(1);
    CHECK(pool.GetBatch<Object>(1, 0).empty());
}


void Removal() {
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{3, 2 * sizeof(void*)}));
    std::array<int, 9> values{};
    std::vector<void*> model{};
    for (int& value : values) {
        CHECK(pool.Add(1, &value) == model.size());
        model.push_back(&value);
    }
    for (std::size_t index : {0u, 2u, 4u}) {
        const auto result{pool.RemoveAt(1, index)};
        CHECK(result.removed == model[index]);
        CHECK(result.moved == model.back());
        model[index] = model.back();
        model.pop_back();
        Compare(pool, 1, model);
    }
    while (!model.empty()) {
        const auto result{pool.RemoveAt(1, model.size() - 1)};
        CHECK(result.removed == model.back() && result.moved == nullptr);
        model.pop_back();
    }
    CHECK(pool.RemoveAt(1, 0).removed == nullptr);
    CHECK(pool.RemoveAt(1, Pool::InvalidIndex).removed == nullptr);
    CHECK(pool.GetStatistics().capacity == 9);
}

void NonOwning() {
    struct Object {
        int* destroyed{};
        ~Object() { ++*destroyed; }
    };
    int destroyed{};
    std::unique_ptr<Object> first{new (std::nothrow) Object{&destroyed}};
    std::unique_ptr<Object> second{new (std::nothrow) Object{&destroyed}};
    CHECK(first && second);
    {
        Pool pool{};
        CHECK(pool.Register(1));
        CHECK(pool.Add(1, first.get()) == 0);
        CHECK(pool.RemoveAt(1, 0).removed == first.get());
        CHECK(destroyed == 0);
        CHECK(pool.Add(1, first.get()) == 0);
        pool.Clear(1);
        CHECK(destroyed == 0 && pool.Count(1) == 0);
        CHECK(pool.Add(1, second.get()) == 0);
    }
    CHECK(destroyed == 0);
    first.reset();
    second.reset();
    CHECK(destroyed == 2);
}

void ReservationAndReuse() {
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{0, 4 * sizeof(void*)}));
    CHECK(pool.Reserve(1, 100));
    CHECK(pool.GetStatistics().slabs == 1 && pool.GetStatistics().capacity == 100);
    CHECK(pool.Reserve(1, 101));
    CHECK(pool.GetStatistics().slabs == 2 && pool.GetStatistics().capacity == 104);
    const auto bytes{pool.GetStatistics().pointerBytes};
    CHECK(!pool.Reserve(1, std::numeric_limits<std::size_t>::max()));
    CHECK(pool.GetStatistics().pointerBytes == bytes);
    std::array<int, 104> values{};
    for (int& value : values) { CHECK(pool.Add(1, &value) != Pool::InvalidIndex); }
    pool.Clear(1);
    CHECK(pool.GetBatch(1, 0).empty() && pool.At(1, 0) == nullptr);
    CHECK(pool.GetStatistics().capacity == 104 && pool.Count(1) == 0);
    for (int& value : values) { CHECK(pool.Add(1, &value) != Pool::InvalidIndex); }
    CHECK(pool.GetStatistics().slabs == 2);
}

void RandomizedModel() {
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{5, 7 * sizeof(void*)}));
    std::array<int, 4000> values{};
    std::vector<void*> model{};
    std::mt19937 random{20261012};
    for (std::size_t step{}; step < values.size(); ++step) {
        if (!model.empty() && random() % 100 < 45) {
            const auto index{static_cast<std::size_t>(random()) % model.size()};
            const auto result{pool.RemoveAt(1, index)};
            CHECK(result.removed == model[index]);
            if (index + 1 != model.size()) { CHECK(result.moved == model.back()); }
            else { CHECK(result.moved == nullptr); }
            model[index] = model.back();
            model.pop_back();
        } else {
            CHECK(pool.Add(1, &values[step]) == model.size());
            model.push_back(&values[step]);
        }
        if (step % 41 == 0) { Compare(pool, 1, model); }
    }
    Compare(pool, 1, model);
}

/** @brief main と同じ実行クラスで、メンバ関数・引数・完了待ち・繰り返し実行を検証する。 */
void ParallelUpdate() {
    struct Object {
        std::size_t actual{}, expected{};
        float distance{}, expectedDistance{};
        std::thread::id worker{};

        void Update(float deltaSeconds) noexcept {
            ++actual;
            distance += deltaSeconds;
            worker = std::this_thread::get_id();
        }

        void UpdateTwice(float deltaSeconds) noexcept {
            actual += 2;
            distance += deltaSeconds * 2;
            worker = std::this_thread::get_id();
        }
    };
    struct Other {
        std::size_t updates{};
        float elapsed{};
        void Tick(float deltaSeconds) noexcept { ++updates; elapsed += deltaSeconds; }
    };

    std::array<Object, 530> objects{};
    std::array<Other, 3> others{};
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{100, 4 * sizeof(void*)}));
    CHECK(pool.Register(2, Pool::Options{1, 2 * sizeof(void*)}));
    benchmarks::ParallelExecutor executor{};
    executor.ForEach(pool, 999, &Object::Update, 0.25f);
    executor.ForEach(pool, 1, &Object::Update, 0.25f);
    CHECK(objects[0].actual == 0);

    // 0個・1個・ワーカー数未満でも、空の担当範囲が安全に完了する。
    executor.ForEach(pool, 2, &Other::Tick, 0.5f);
    CHECK(pool.Add(2, &others[0]) == 0);
    executor.ForEach(pool, 2, &Other::Tick, 0.5f);
    CHECK(others[0].updates == 1 && others[0].elapsed == 0.5f);
    CHECK(pool.Add(2, &others[1]) == 1);
    CHECK(pool.Add(2, &others[2]) == 2);
    executor.ForEach(pool, 2, &Other::Tick, 0.25f);
    CHECK(others[0].updates == 2 && others[0].elapsed == 0.75f);
    CHECK(others[1].updates == 1 && others[1].elapsed == 0.25f);
    CHECK(others[2].updates == 1 && others[2].elapsed == 0.25f);

    for (std::size_t i{}; i < 513; ++i) { CHECK(pool.Add(1, &objects[i]) == i); }
    for (std::size_t round{}; round < 20; ++round) {
        if (round < 17) {
            CHECK(pool.RemoveAt(1, 3).removed != nullptr);
            CHECK(pool.Add(1, &objects[513 + round]) != Pool::InvalidIndex);
        }
        std::size_t increment{1};
        float deltaSeconds{0.25f};
        if (round % 2 != 0) { increment = 2; deltaSeconds = 0.5f; }
        for (std::size_t i{}; i < pool.Count(1); ++i) {
            auto& object{*static_cast<Object*>(pool.At(1, i))};
            object.expected += increment;
            object.expectedDistance += deltaSeconds * static_cast<float>(increment);
        }
        if (round % 2 == 0) { executor.ForEach(pool, 1, &Object::Update, deltaSeconds); }
        else { executor.ForEach(pool, 1, &Object::UpdateTwice, deltaSeconds); }

        // ForEach が戻った直後に全結果を読めることを確認する。
        std::set<std::thread::id> workers{};
        for (const auto& object : objects) {
            CHECK(object.actual == object.expected && object.distance == object.expectedDistance);
            if (object.actual != 0) {
                CHECK(object.worker != std::this_thread::get_id());
                workers.insert(object.worker);
            }
        }
        CHECK(workers.size() == 4);
    }
    pool.Clear(1);
    executor.ForEach(pool, 1, &Object::Update, 1.0f);
    // ジョブを一度も実行しなかった実行器も、待機中のまま安全に終了する。
    benchmarks::ParallelExecutor unused{};
}


/** @brief 同一ID・異なるIDへの同時追加と、戻り値の添字・拡張・再利用を検証する。 */
void ConcurrentAddition() {
    for (const std::size_t workerCount : {1u, 10u, 14u}) {
        benchmarks::ParallelTasks executor{workerCount};
        CHECK(executor.WorkerCount() == workerCount);
        for (const std::size_t count : {0u, 1u, 3u, 10003u}) {
            std::vector<std::size_t> values(count), indices(count);
            for (std::size_t i{}; i < count; ++i) { values[i] = i; }
            for (const bool splitIds : {false, true}) {
                Pool pool{};
                CHECK(pool.Register(1, Pool::Options{0, 64}));
                CHECK(pool.Register(2, Pool::Options{3, 64}));
                for (int round{}; round < 2; ++round) {
                    std::vector<std::thread::id> workers(workerCount);
                    executor.ForEachRange(count, [&](std::size_t worker, std::size_t first, std::size_t last) {
                        workers[worker] = std::this_thread::get_id();
                        CHECK(pool.AddConcurrent(999, nullptr) == Pool::InvalidIndex);
                        CHECK(pool.AddConcurrent(999, &values) == Pool::InvalidIndex);
                        CHECK(pool.AddConcurrent(1, nullptr) == Pool::InvalidIndex);
                        for (std::size_t i{first}; i < last; ++i) {
                            Pool::PoolId id{1};
                            if (splitIds) { id += static_cast<Pool::PoolId>(i % 2); }
                            indices[i] = pool.AddConcurrent(id, &values[i]);
                        }
                    });
                    const std::set<std::thread::id> uniqueWorkers{workers.begin(), workers.end()};
                    CHECK(uniqueWorkers.size() == workerCount);
                    CHECK(!uniqueWorkers.contains(std::this_thread::get_id()));
                    CHECK(pool.GetStatistics().count == count);
                    std::array<std::vector<void*>, 2> expected{};
                    expected[0].resize(pool.Count(1));
                    expected[1].resize(pool.Count(2));
                    for (std::size_t i{}; i < count; ++i) {
                        std::size_t group{};
                        if (splitIds) { group = i % 2; }
                        CHECK(indices[i] < expected[group].size());
                        CHECK(expected[group][indices[i]] == nullptr);
                        expected[group][indices[i]] = &values[i];
                        CHECK(values[i] == i);
                    }
                    Compare(pool, 1, expected[0]);
                    Compare(pool, 2, expected[1]);
                    for (const auto id : {1u, 2u}) {
                        if (pool.Count(id) != 0) { CHECK(pool.RemoveAt(id, 0).removed != nullptr); }
                        pool.Clear(id);
                        CHECK(pool.Count(id) == 0);
                    }
                }
                CHECK(pool.Add(1, &values) == 0);
                CHECK(pool.RemoveAt(1, 0).removed == &values);
            }
        }
    }
}
/** @brief ポインタの格納配列が増えても、既存実体のアドレス・内容は変わらない。 */
void Expansion() {
    Pool pool{};
    CHECK(pool.Register(1, Pool::Options{1, sizeof(void*)}));
    std::array<std::uint64_t, 300> values{};
    for (std::size_t i{}; i < values.size(); ++i) {
        values[i] = i + 1;
        CHECK(pool.Add(1, &values[i]) == i);
    }
    CHECK(pool.GetStatistics().slabs == 300);
    for (std::size_t i{}; i < values.size(); ++i) {
        CHECK(pool.At(1, i) == &values[i] && values[i] == i + 1);
    }
}

/** @brief 実体をコピーせず、一括登録・Slab境界を越えるfor・上限付き走査を検証する。 */
void BulkRegistrationAndRange() {
    struct Object {
        std::size_t updates{};
        Object() noexcept = default;
        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;
        void Update(float) noexcept { ++updates; }
    };
    static_assert(std::ranges::forward_range<Pool::Range<Object>>);
    static_assert(std::is_same_v<decltype(*std::declval<Pool::Range<const Object>>().begin()), const Object&>);
    Pool pool{};
    std::array<Object, 600> objects{};
    std::array<void*, 600> pointers{};
    for (std::size_t i{}; i < objects.size(); ++i) { pointers[i] = &objects[i]; }
    CHECK(pool.AddBatch(99, pointers) == Pool::InvalidIndex);
    CHECK(pool.Register(1, {3, 64}));
    CHECK(pool.Reserve(1, 11) && pool.Reserve(1, 29));
    CHECK(pool.AddBatch(1, {}) == 0);
    std::array<void*, 2> invalid{&objects[0], nullptr};
    CHECK(pool.AddBatch(1, invalid) == Pool::InvalidIndex && pool.Count(1) == 0);
    CHECK(pool.AddBatch(1, std::span{pointers}.first(25)) == 0);
    CHECK(pool.GetBatch(1, 0).size() == 3 && pool.GetBatch(1, 3).size() == 8);
    const auto* firstAddress{pool.GetBatch(1, 0).data()};
    const auto range{pool.GetRange<Object>(1)};
    CHECK(range.size() == 25 && !range.empty());
    auto iterator{range.begin()};
    auto copied{iterator++};
    CHECK(&*copied == &objects[0] && &*iterator == &objects[1]);
    CHECK(iterator->updates == 0);
    std::size_t index{};
    for (auto& object : range) { CHECK(&object == &objects[index++]); }
    CHECK(index == 25);
    CHECK(pool.GetRange<Object>(99).empty());
    CHECK(pool.GetRange<Object>(1, Pool::InvalidIndex).empty());
    CHECK(pool.GetRange<Object>(1, 25).empty());
    const auto empty{pool.GetRange<Object>(1, 3, 0)};
    CHECK(empty.empty() && empty.begin() == empty.end());
    index = 2;
    for (auto& object : pool.GetRange<Object>(1, 2, 13)) { CHECK(&object == &objects[index++]); }
    CHECK(index == 15);
    CHECK(pool.AddBatch(1, std::span{pointers}.subspan(25)) == 25);
    CHECK(pool.GetBatch(1, 0).data() == firstAddress && pool.GetStatistics().slabs == 4);
    pointers[0] = nullptr; // 元のポインタ表を変えても登録済みのポインタ値は変わらない。
    CHECK(pool.At(1, 0) == &objects[0]);
    benchmarks::ParallelTasks tasks{14};
    tasks.ForEach(pool, 1, &Object::Update, 0.0f);
    for (const auto& object : objects) { CHECK(object.updates == 1); }
    CHECK(pool.RemoveAt(1, 2).moved == &objects.back());
    CHECK(&*pool.GetRange<Object>(1, 2, 1).begin() == &objects.back());
    pool.Clear(1);
    CHECK(pool.GetRange<Object>(1).empty());
    pointers[0] = &objects[0];
    CHECK(tasks.CopyPointers(pool, 1, pointers, 19));
    CHECK(pool.GetBatch(1, 0).data() == firstAddress && pool.Count(1) == pointers.size());
    CHECK(!tasks.CopyPointers(pool, 1, invalid));
    CHECK(!tasks.CopyPointers(pool, 1, {}, 0));
    CHECK(pool.Count(1) == pointers.size());
}

/** @brief CAS予約の一意性、バッチ内順序、境界跨ぎ、ID分離、フェーズ再開を検証する。 */
void ConcurrentBatches() {
    for (const std::size_t workers : {1u, 10u, 14u}) {
        benchmarks::ParallelTasks tasks{workers};
        for (const std::size_t count : {0u, 1u, 3u, 10003u}) {
            auto values{std::make_unique<std::size_t[]>(count)};
            auto pointers{std::make_unique<void*[]>(count)};
            auto otherValues{std::make_unique<std::size_t[]>(count)};
            auto otherPointers{std::make_unique<void*[]>(count)};
            auto indices{std::make_unique<std::size_t[]>(count)};
            auto seen{std::make_unique<unsigned char[]>(count + 1)};
            for (std::size_t i{}; i < count; ++i) {
                values[i] = i;
                pointers[i] = &values[i];
                otherValues[i] = i;
                otherPointers[i] = &otherValues[i];
            }
            Pool pool{};
            CHECK(pool.Register(1, {3, 64}) && pool.Register(2, {0, 64}));
            CHECK(pool.Reserve(1, count / 4) && pool.Reserve(1, count / 2));
            CHECK(!pool.EndConcurrentAppend(1));
            CHECK(!pool.BeginConcurrentAppend(99, 1));
            CHECK(!pool.BeginConcurrentAppend(1, Pool::InvalidIndex));
            CHECK(pool.AddBatchConcurrent(1, {}) == Pool::InvalidIndex);
            for (int round{}; round < 3; ++round) {
                pool.Clear(1);
                pool.Clear(2);
                std::fill_n(seen.get(), count + 1, static_cast<unsigned char>(0));
                int prefix{};
                CHECK(pool.Add(1, &prefix) == 0);
                CHECK(pool.BeginConcurrentAppend(1, count));
                CHECK(pool.BeginConcurrentAppend(2, count));
                CHECK(!pool.BeginConcurrentAppend(1, 1));
                // ワーカー開始前でも、登録中の件数・ビューは公開しない。
                CHECK(pool.Count(1) == 0 && pool.GetRange<int>(1).empty());
                CHECK(pool.GetBatch(1, 0).empty());
                CHECK(!pool.Reserve(1, count + 100));
                CHECK(pool.Add(1, &prefix) == Pool::InvalidIndex);
                CHECK(pool.AddBatch(1, {}) == Pool::InvalidIndex);
                CHECK(pool.RemoveAt(1, 0).removed == nullptr);
                pool.Clear(1); // フェーズ中は登録内容を変更しない。
                tasks.ForEachRange(count, [&](std::size_t worker, std::size_t first, std::size_t last) {
                    const std::array<void*, 2> invalid{&prefix, nullptr};
                    CHECK(pool.AddBatchConcurrent(1, invalid) == Pool::InvalidIndex);
                    CHECK(pool.AddBatchConcurrent(99, {}) == Pool::InvalidIndex);
                    const auto chunk{1 + worker * 17};
                    while (first < last) {
                        const auto length{std::min(chunk, last - first)};
                        const std::span<void* const> batch{pointers.get() + first, length};
                        const auto start{pool.AddBatchConcurrent(1, batch)};
                        CHECK(start != Pool::InvalidIndex);
                        CHECK(pool.AddBatchConcurrent(2, {otherPointers.get() + first, length}) != Pool::InvalidIndex);
                        for (std::size_t i{}; i < length; ++i) { indices[first + i] = start + i; }
                        first += length;
                    }
                });
                CHECK(pool.Count(1) == 0); // 完了していてもEnd前は公開しない。
                CHECK(pool.EndConcurrentAppend(1) && pool.EndConcurrentAppend(2));
                CHECK(!pool.EndConcurrentAppend(1));
                CHECK(pool.Count(1) == count + 1 && pool.At(1, 0) == &prefix);
                CHECK(pool.Count(2) == count);
                for (std::size_t i{}; i < count; ++i) {
                    CHECK(indices[i] > 0 && indices[i] <= count && seen[indices[i]] == 0);
                    seen[indices[i]] = 1;
                    CHECK(pool.At(1, indices[i]) == &values[i] && values[i] == i);
                }
                std::fill_n(seen.get(), count + 1, static_cast<unsigned char>(0));
                for (const auto& value : pool.GetRange<const std::size_t>(2)) {
                    CHECK(value < count && &value == &otherValues[value] && seen[value] == 0);
                    seen[value] = 1;
                }
                CHECK(pool.RemoveAt(1, 0).removed == &prefix);
                CHECK(pool.Count(1) == count);
            }
        }
    }
}

/** @brief 残容量を競合して奪い合っても、失敗したバッチが穴や上限超過を作らない。 */
void ConcurrentBatchLimit() {
    constexpr std::size_t workers{14}, perWorker{16}, limit{23};
    std::array<int, workers * perWorker> values{};
    std::array<void*, values.size()> pointers{};
    std::array<Pool::Index, values.size()> indices{};
    for (std::size_t i{}; i < values.size(); ++i) { pointers[i] = &values[i]; }
    benchmarks::ParallelTasks tasks{workers};
    Pool pool{};
    CHECK(pool.Register(1, {100, 64})); // 物理容量が余っていても、フェーズの追加上限を守る。
    for (int round{}; round < 20; ++round) {
        pool.Clear(1);
        indices.fill(Pool::InvalidIndex);
        CHECK(pool.BeginConcurrentAppend(1, limit));
        tasks.ForEachRange(values.size(), [&](std::size_t worker, std::size_t first, std::size_t last) {
            while (first < last) {
                const auto length{std::min(1 + worker % 5, last - first)};
                const auto start{pool.AddBatchConcurrent(1, std::span{pointers}.subspan(first, length))};
                if (start != Pool::InvalidIndex) {
                    for (std::size_t i{}; i < length; ++i) { indices[first + i] = start + i; }
                }
                first += length;
            }
        });
        CHECK(pool.EndConcurrentAppend(1));
        std::array<bool, limit> seen{};
        std::size_t successes{};
        for (std::size_t i{}; i < indices.size(); ++i) {
            if (indices[i] != Pool::InvalidIndex) {
                CHECK(indices[i] < limit && !seen[indices[i]]);
                seen[indices[i]] = true;
                CHECK(pool.At(1, indices[i]) == pointers[i]);
                ++successes;
            }
        }
        CHECK(pool.Count(1) == successes && successes <= limit && successes != 0);
        for (std::size_t i{}; i < successes; ++i) { CHECK(seen[i] && pool.At(1, i) != nullptr); }
        CHECK(pool.BeginConcurrentAppend(1, 0));
        CHECK(pool.AddBatchConcurrent(1, std::span{pointers}.first(1)) == Pool::InvalidIndex);
        CHECK(pool.AddBatchConcurrent(1, {}) == successes);
        CHECK(pool.EndConcurrentAppend(1) && pool.Count(1) == successes);
    }
}

#undef CHECK
}

int RunAll() {
    struct Test { const char* name; void (*run)(); };
    const std::array tests{
        Test{"ID・容量設定・不正入力", Registration},
        Test{"64バイト拡張・先頭アラインメント・初期容量保持", AlignedGrowth},
        Test{"可変容量・連続範囲・境界・ID分離", Layout},
        Test{"型付き参照・const・Slab境界・空の範囲", TypedBatches},
        Test{"末尾交換・削除結果・空の範囲", Removal},
        Test{"実体の非所有と破棄責任", NonOwning},
        Test{"一括予約・桁あふれ拒否・Clear後の再利用", ReservationAndReuse},
        Test{"ランダム追加削除と独立モデルの一致", RandomizedModel},
        Test{"並列実行クラス・関数と引数・完了待ち", ParallelUpdate},
        Test{"1・10・14スレッドの同時追加・ID分離・添字・再利用", ConcurrentAddition},
        Test{"Slab索引の拡張・実体アドレスの保持", Expansion},
        Test{"ポインタ一括コピー・実体非コピー・全Slabの範囲for", BulkRegistrationAndRange},
        Test{"CAS一括登録・1/10/14ワーカー・境界・ID分離・公開", ConcurrentBatches},
        Test{"CAS容量競合・バッチ失敗時の原子性・上限保持", ConcurrentBatchLimit},
    };
    std::cout << "=== ポインタ Slab の自動テスト ===\n";
    for (const Test& test : tests) {
        test.run();
        std::cout << "[成功] " << test.name << '\n';
    }
    std::cout << tests.size() << '/' << tests.size() << " テスト成功\n";
    return 0;
}

}
