/** @file tests.cpp
 * @brief ポインタ非所有・状態判定・変更通知・末尾除去・並列区間取得を検証する。
 */
#include "tests.h"
#include "slab/slab_pool.h"
#include "../benchmarks/parallel_tasks.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <random>
#include <type_traits>
#include <utility>
#include <thread>

namespace tests {
namespace {
using Pool = slab::SlabPool;
#define REQUIRE(...) do { if (!(__VA_ARGS__)) { \
    std::cerr << "  [失敗] 行 " << __LINE__ << "：" << #__VA_ARGS__ << '\n'; return false; } } while (false)

struct Record : slab::Component {
    Record() noexcept = default;
    std::size_t id{};
    std::atomic<std::size_t> updates{};
    void Update(float) noexcept { updates.fetch_add(1, std::memory_order_relaxed); }
};
struct Other : slab::Component {
    Other() noexcept = default;
};
static_assert(!std::is_destructible_v<slab::Component>);
static_assert(!std::is_polymorphic_v<slab::Component>);
static_assert(!std::is_copy_constructible_v<Record>);
static_assert(std::forward_iterator<Pool::Range<Record>::Iterator>);
static_assert(std::is_same_v<decltype(*std::declval<const Pool&>().GetRange<Record>(0).begin()), const Record&>);

/** @brief テストの実体を外部所有し、固定長ポインタ配列を用意する。 */
struct Objects {
    std::size_t count{};
    std::unique_ptr<Record[]> records{};
    std::unique_ptr<Record*[]> pointers{};
    explicit Objects(std::size_t length)
        : count{length}, records{new (std::nothrow) Record[length]}, pointers{new (std::nothrow) Record*[length]} {
        if (!records || !pointers) { return; }
        for (std::size_t i{}; i < count; ++i) { records[i].id = i; pointers[i] = &records[i]; }
    }
    [[nodiscard]]
    bool Valid() const noexcept { return records != nullptr && pointers != nullptr; }
    [[nodiscard]]
    std::span<Record* const> Span() const noexcept { return {pointers.get(), count}; }
};

bool Registration() {
    Pool pool{};
    REQUIRE(!pool.Register<Record>(1, {.initialCapacity = 2, .reservedCapacity = 1}));
    REQUIRE(!pool.Register<Record>(1, {.initialCapacity = 0, .reservedCapacity = 0}));
    REQUIRE(!pool.Register<Record>(1, {.commitBytes = 0}));
    REQUIRE(!pool.Register<Record>(1, {.initialCapacity = 0, .reservedCapacity = Pool::InvalidIndex}));
    REQUIRE(!pool.Register<Record>(1, {.freeThreshold = Pool::InvalidIndex}));
    REQUIRE(pool.Register<Record>(0));
    REQUIRE(!pool.Register<Other>(0));
    REQUIRE(pool.Register<Other>(2));
    Record object{};
    REQUIRE(pool.Add(0, &object) == 0 && pool.GetRange<Record>(0).At(0) == &object);
    REQUIRE(pool.Add(2, &object) == Pool::InvalidIndex);
    REQUIRE(pool.Add<Record>(0, nullptr) == Pool::InvalidIndex);
    std::array<Record*, 2> bad{&object, nullptr};
    REQUIRE(pool.AddBatch<Record>(0, bad) == Pool::InvalidIndex && pool.Count(0) == 1);
    REQUIRE(pool.AddBatch<Record>(0, {}) == 1);
    REQUIRE(pool.GetRange<Other>(0).empty() && pool.GetRange<Other>(0).At(0) == nullptr);
    REQUIRE(pool.Count(99) == 0 && pool.GetRange<Record>(99).At(0) == nullptr);
    REQUIRE(!pool.Reserve(99, 1) && !pool.Commit(99, 1) && !pool.Clear(99));
    REQUIRE(!pool.NotifyStateChanged(99) && pool.RemoveInactive() == 0);
    return true;
}

struct Lifetime : slab::Component {
    static inline int destroyed{};
    Lifetime() noexcept = default;
    Lifetime(const Lifetime&) = delete;
    ~Lifetime() noexcept { ++destroyed; }
};

bool NonOwnership() {
    Lifetime::destroyed = 0;
    auto first{std::unique_ptr<Lifetime>{new (std::nothrow) Lifetime{}}};
    auto second{std::unique_ptr<Lifetime>{new (std::nothrow) Lifetime{}}};
    REQUIRE(first && second);
    {
        Pool pool{};
        REQUIRE(pool.Register<Lifetime>(1));
        REQUIRE(pool.Add(1, first.get()) == 0 && pool.Add(1, second.get()) == 1);
        first->state = 0;
        REQUIRE(pool.NotifyStateChanged(1) && pool.RemoveInactive() == 1);
        REQUIRE(Lifetime::destroyed == 0 && pool.GetRange<Lifetime>(1).At(0) == second.get());
        first.reset();
        REQUIRE(Lifetime::destroyed == 1);
        REQUIRE(pool.Clear(1) && Lifetime::destroyed == 1);
        REQUIRE(pool.Add(1, second.get()) == 0);
    }
    REQUIRE(Lifetime::destroyed == 1);
    second.reset();
    REQUIRE(Lifetime::destroyed == 2);
    return true;
}

bool ReserveAndCommit() {
    Pool pool{};
    Objects objects{1700};
    REQUIRE(objects.Valid());
    REQUIRE(pool.Register<Record>(1,
        {.initialCapacity = 0, .reservedCapacity = 512, .commitBytes = 4096, .preTouch = false}));
    REQUIRE(pool.GetStatistics(1).committedBytes == 0);
    REQUIRE(pool.Commit(1, 1) && pool.Count(1) == 0);
    REQUIRE(pool.AddBatch<Record>(1, objects.Span()) == 0);
    const auto stats{pool.GetStatistics(1)};
    REQUIRE(stats.count == 1700 && stats.capacity >= 1700 && stats.pointerSize == sizeof(void*));
    REQUIRE(stats.committedBytes == stats.capacity * sizeof(void*) && stats.regions >= 2 && stats.chunks >= 2);
    REQUIRE(pool.GetRange<Record>(1).At(0) == &objects.records[0]);
    REQUIRE(pool.Reserve(1, stats.reservedCapacity + 512));
    REQUIRE(pool.GetStatistics(1).committedBytes == stats.committedBytes);
    REQUIRE(pool.Clear(1) && pool.GetStatistics(1).capacity == stats.capacity);
    REQUIRE(pool.Add(1, &objects.records[1699]) == 0);
    REQUIRE(pool.GetRange<Record>(1).At(0) == &objects.records[1699]);
    return true;
}

bool FrameCommit() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t page{system.dwPageSize};
    const auto perPage{page / sizeof(void*)};
    Objects objects{perPage * 4};
    REQUIRE(objects.Valid());
    Pool pool{};
    REQUIRE(pool.Register<Record>(1, {.initialCapacity = 1, .reservedCapacity = perPage * 3,
        .commitBytes = page * 2, .preTouch = true, .freeThreshold = perPage * 2}));
    REQUIRE(pool.Add(1, objects.pointers[0]) == 0);
    const auto before{pool.GetStatistics(1)};
    REQUIRE(before.committedBytes == page * 2);
    REQUIRE(pool.CommitAtFrameEnd(1));
    const auto after{pool.GetStatistics(1)};
    REQUIRE(after.committedBytes - before.committedBytes == page * 2 && after.regions == 2);
    REQUIRE(pool.AddBatch<Record>(1, objects.Span().subspan(1)) == 1);
    Record extra{};
    REQUIRE(pool.Add(1, &extra) == Pool::InvalidIndex);
    REQUIRE(pool.CommitAtFrameEnd(1) && pool.Add(1, &extra) == objects.count);
    REQUIRE(pool.GetRange<Record>(1).At(0) == objects.pointers[0]);
    REQUIRE(pool.Clear(1));
    REQUIRE(pool.Register<Record>(2, {.initialCapacity = 0, .reservedCapacity = 1, .commitBytes = page}));
    REQUIRE(pool.AddBatch<Record>(2, objects.Span()) == 0);
    const auto autoBytes{pool.GetStatistics(2).committedBytes};
    REQUIRE(pool.CommitAtFrameEnd(2) && pool.GetStatistics(2).committedBytes == autoBytes);
    return true;
}

bool RangeBoundaries() {
    Objects objects{1543};
    REQUIRE(objects.Valid());
    Pool pool{};
    REQUIRE(pool.Register<Record>(1, {.initialCapacity = 0, .reservedCapacity = 512, .commitBytes = 4096}));
    for (std::size_t first{}; first < objects.count; first += 300) {
        REQUIRE(pool.AddBatch<Record>(1, objects.Span().subspan(first, std::min(std::size_t{300}, objects.count - first))) == first);
    }
    const std::array<std::size_t, 6> inactive{0, 511, 512, 1023, 1024, 1542};
    for (auto i : inactive) { objects.records[i].state = 0; }
    REQUIRE(pool.NotifyStateChanged(1));
    REQUIRE(pool.GetRange<Record>(1).size() == objects.count - inactive.size());
    std::size_t visited{};
    for (auto& item : pool.GetRange<Record>(1)) {
        REQUIRE(&item == objects.pointers[item.id] && item.state != 0);
        ++visited;
    }
    REQUIRE(visited == objects.count - inactive.size());
    REQUIRE(pool.RemoveInactive() == inactive.size());
    REQUIRE(pool.GetRange<Record>(1).size() == objects.count - inactive.size());
    return true;
}

bool SwapRemoval() {
    Objects objects{6};
    REQUIRE(objects.Valid());
    Pool pool{};
    REQUIRE(pool.Register<Record>(1));
    REQUIRE(pool.AddBatch<Record>(1, objects.Span()) == 0);
    objects.records[1].state = 0;
    objects.records[4].state = 0;
    objects.records[5].state = 0;
    REQUIRE(pool.NotifyStateChanged(1) && pool.RemoveInactive() == 3);
    REQUIRE(pool.GetRange<Record>(1).At(0) == objects.pointers[0]);
    REQUIRE(pool.GetRange<Record>(1).At(1) == objects.pointers[3]);
    REQUIRE(pool.GetRange<Record>(1).At(2) == objects.pointers[2]);
    REQUIRE(pool.Count(1) == 3 && pool.RemoveInactive() == 0);
    REQUIRE(pool.Add(1, objects.pointers[4]) == 3);
    REQUIRE(pool.GetStatistics(1).mayContainInactive);
    REQUIRE(pool.RemoveInactive() == 1);
    for (auto& item : pool.GetRange<Record>(1)) { item.state = 0; }
    REQUIRE(pool.NotifyStateChanged(1) && pool.RemoveInactive() == 3);
    REQUIRE(pool.Count(1) == 0 && pool.RemoveInactive() == 0);
    return true;
}

bool RandomChurn() {
    Objects objects{3500};
    REQUIRE(objects.Valid());
    Pool pool{};
    REQUIRE(pool.Register<Record>(1, {.initialCapacity = 0, .reservedCapacity = 512, .commitBytes = 4096}));
    std::array<bool, 3500> present{};
    std::mt19937 random{20261016};
    std::size_t introduced{};
    for (std::size_t round{}; round < 7; ++round) {
        const auto first{pool.Count(1)};
        REQUIRE(pool.AddBatch<Record>(1, objects.Span().subspan(introduced, 500)) == first);
        for (std::size_t i{introduced}; i < introduced + 500; ++i) { present[i] = true; }
        introduced += 500;
        std::size_t expectedRemoved{};
        for (std::size_t i{}; i < introduced; ++i) {
            if (present[i] && random() % 3 == 0) {
                objects.records[i].state = 0;
                present[i] = false;
                ++expectedRemoved;
            }
        }
        REQUIRE(pool.NotifyStateChanged(1) && pool.RemoveInactive() == expectedRemoved);
        std::array<bool, 3500> seen{};
        std::size_t visited{};
        for (auto& item : pool.GetRange<Record>(1)) {
            REQUIRE(item.id < introduced && present[item.id] && !seen[item.id] && &item == objects.pointers[item.id]);
            seen[item.id] = true;
            ++visited;
        }
        REQUIRE(visited == pool.Count(1));
        for (std::size_t i{}; i < introduced; ++i) { REQUIRE(seen[i] == present[i]); }
    }
    return true;
}

bool ScopedAccess() {
    static_assert(!std::is_copy_constructible_v<Pool::Range<Record>>);
    static_assert(std::is_nothrow_move_constructible_v<Pool::Range<Record>>);
    static_assert(std::is_same_v<decltype(slab::Component::state), int>);
    static_assert(std::is_same_v<decltype(std::declval<const Pool&>().GetRange<Record>(1).At(0)), const Record*>);
    Pool pool{};
    Record object{};
    REQUIRE(pool.Register<Record>(1) && pool.Register<Record>(2));
    REQUIRE(pool.Add(1, &object) == 0);
    REQUIRE(!pool.GetRange<Other>(1) && !pool.GetRange<Record>(99));
    {
        auto first{pool.GetRange<Record>(1)};
        REQUIRE(first && first.StoredCount() == 1 && first.At(0) == &object);
        REQUIRE(!pool.GetRange<Record>(1) && !std::as_const(pool).GetRange<Record>(1));
        auto second{pool.GetRange<Record>(2)};
        REQUIRE(second && second.empty()); // 空IDでも走査権を保持する。
        REQUIRE(!pool.GetRange<Record>(2));
        second = std::move(first); // 元のID2を解放し、ID1を引き継ぐ。
        REQUIRE(!first && first.empty() && first.At(0) == nullptr && second.At(0) == &object);
        REQUIRE(pool.GetRange<Record>(2) && !pool.GetRange<Record>(1));
        auto third{std::move(second)};
        REQUIRE(!second && third);
        third = std::move(third);
        REQUIRE(third && !pool.GetRange<Record>(1));
        for (auto& item : third) {
            item.state = 2;
            if (item.id == 0) { break; }
        }
        REQUIRE(!pool.GetRange<Record>(1)); // breakや末尾到達だけでは解放しない。
        third.Reset();
        third.Reset();
        REQUIRE(!third && third.empty() && third.StoredCount() == 0);
    }
    auto constRange{std::as_const(pool).GetRange<Record>(1)};
    REQUIRE(constRange && constRange.At(0)->state == 2 && constRange.At(Pool::InvalidIndex) == nullptr);
    REQUIRE(!pool.GetRange<Record>(1));
    constRange.Reset();
    const auto earlyReturn{[&] { auto range{pool.GetRange<Record>(1)}; return static_cast<bool>(range); }};
    REQUIRE(earlyReturn() && pool.GetRange<Record>(1));
    return true;
}

bool BusyOperations() {
    Pool pool{};
    Record object{};
    Record extra{};
    REQUIRE(pool.Register<Record>(1) && pool.Register<Record>(2));
    REQUIRE(pool.Add(1, &object) == 0);
    auto range{pool.GetRange<Record>(1)};
    REQUIRE(range && !pool.Clear(1) && !pool.Reserve(1, 1) && !pool.Commit(1, 1) && !pool.CommitAtFrameEnd(1));
    REQUIRE(pool.Add(1, &extra) == Pool::InvalidIndex && pool.AddBatch<Record>(1, {}) == Pool::InvalidIndex);
    REQUIRE(pool.Count(1) == 0 && !pool.GetStatistics(1).available);
    REQUIRE(pool.Add(2, &extra) == 0 && pool.GetStatistics(2).available);
    range.At(0)->state = 0;
    REQUIRE(pool.NotifyStateChanged(1) && pool.RemoveInactive() == Pool::InvalidIndex);
    range.Reset();
    REQUIRE(pool.GetStatistics(1).mayContainInactive && pool.RemoveInactive() == 1);
    REQUIRE(pool.Count(1) == 0 && pool.Count(2) == 1);
    return true;
}

bool SameIdContention() {
    for (auto threads : {10U, 14U}) {
        Record object{};
        Pool pool{};
        REQUIRE(pool.Register<Record>(1) && pool.Add(1, &object) == 0);
        benchmarks::ParallelTasks workers{threads};
        std::barrier rendezvous{static_cast<std::ptrdiff_t>(threads)};
        std::atomic<std::size_t> winners{};
        workers.ForEachRange(threads, [&](std::size_t, std::size_t, std::size_t) {
            rendezvous.arrive_and_wait();
            auto range{pool.GetRange<Record>(1)};
            if (range) { ++winners; range.At(0)->state = 2; }
            // 全員が取得を試みるまで成功者が走査権を保持する。
            rendezvous.arrive_and_wait();
        });
        REQUIRE(winners == 1 && pool.GetRange<Record>(1).At(0)->state == 2);
    }
    return true;
}

bool DifferentIds() {
    for (auto threads : {10U, 14U}) {
        Objects objects{threads * 73};
        REQUIRE(objects.Valid());
        Pool pool{};
        for (unsigned id{}; id < threads; ++id) {
            REQUIRE(pool.Register<Record>(id, {.initialCapacity = 0, .reservedCapacity = 512, .commitBytes = 4096}));
        }
        benchmarks::ParallelTasks workers{threads};
        std::atomic<bool> valid{true};
        workers.ForEachRange(threads, [&](std::size_t, std::size_t first, std::size_t last) {
            for (auto i{first}; i < last; ++i) {
                if (pool.AddBatch<Record>(static_cast<Pool::PoolId>(i), objects.Span().subspan(i * 73, 73)) != 0) { valid = false; }
            }
        });
        REQUIRE(valid);
        std::barrier rendezvous{static_cast<std::ptrdiff_t>(threads)};
        workers.ForEachRange(threads, [&](std::size_t worker, std::size_t, std::size_t) {
            auto range{pool.GetRange<Record>(static_cast<Pool::PoolId>(worker))};
            if (!range) { valid = false; }
            rendezvous.arrive_and_wait(); // 全IDを同時に取得できることを確認。
            if (range) {
                for (auto& item : range) { item.Update(1.0f); item.state = 2; }
            }
        });
        REQUIRE(valid);
        // 次の走査では別ワーカーへIDを割り当てる。
        workers.ForEachRange(threads, [&](std::size_t worker, std::size_t, std::size_t) {
            auto range{pool.GetRange<Record>(static_cast<Pool::PoolId>((worker + 1) % threads))};
            if (!range) { valid = false; return; }
            for (auto& item : range) {
                if (item.state != 2) { valid = false; }
                item.Update(1.0f);
            }
        });
        REQUIRE(valid);
        for (std::size_t i{}; i < objects.count; ++i) { REQUIRE(objects.records[i].updates == 2); }
    }
    return true;
}

struct Plain : slab::Component {
    Plain() noexcept = default;
    std::size_t value{};
};

bool ThreadHandoff() {
    Pool pool{};
    Plain object{};
    REQUIRE(pool.Register<Plain>(1) && pool.Add(1, &object) == 0);
    benchmarks::ParallelTasks workers{2};
    std::atomic<int> stage{};
    std::atomic<bool> valid{true};
    workers.ForEachRange(2, [&](std::size_t worker, std::size_t, std::size_t) {
        if (worker == 0) {
            auto range{pool.GetRange<Plain>(1)};
            if (!range) { valid = false; stage.store(-1, std::memory_order_relaxed); return; }
            range.At(0)->state = 7;
            range.At(0)->value = 987654;
            // この合図はrelaxed。実体の公開はRangeのrelease/acquireが担当する。
            stage.store(1, std::memory_order_relaxed);
        } else {
            int observed{};
            while ((observed = stage.load(std::memory_order_relaxed)) == 0) { std::this_thread::yield(); }
            if (observed < 0) { return; }
            Pool::Range<Plain> range{};
            while (!(range = pool.GetRange<Plain>(1))) { std::this_thread::yield(); }
            if (range.At(0)->state != 7 || range.At(0)->value != 987654) { valid = false; }
            range.At(0)->state = 0;
            if (!pool.NotifyStateChanged(1)) { valid = false; }
        }
    });
    REQUIRE(valid && pool.RemoveInactive() == 1);
    return true;
}

bool DirtyAndRemoveAll() {
    Pool pool{};
    Plain first{};
    Other second{};
    Plain clean{};
    REQUIRE(pool.RemoveInactive() == 0);
    REQUIRE(pool.Register<Plain>(1) && pool.Register<Other>(2) && pool.Register<Plain>(3));
    REQUIRE(pool.Register<Plain>(4, {.initialCapacity = 0}));
    REQUIRE(pool.Add(1, &first) == 0 && pool.Add(2, &second) == 0 && pool.Add(3, &clean) == 0);
    // 通知なしでは除去しないことを検証するため、停止中に意図的に通知を省く。
    first.state = 0;
    clean.state = 0;
    for (int i{}; i < 20; ++i) { REQUIRE(pool.RemoveInactive() == 0); }
    REQUIRE(pool.Count(1) == 1 && pool.Count(3) == 1);
    second.state = 0;
    REQUIRE(pool.NotifyStateChanged(1) && pool.NotifyStateChanged(2) && pool.NotifyStateChanged(4));
    REQUIRE(pool.RemoveInactive() == 2 && pool.Count(3) == 1);
    for (auto id : {1U, 2U, 3U, 4U}) { REQUIRE(!pool.GetStatistics(id).mayContainInactive); }
    REQUIRE(pool.NotifyStateChanged(3) && pool.RemoveInactive() == 1);
    REQUIRE(pool.Add(1, &first) == 0 && pool.GetStatistics(1).mayContainInactive);
    REQUIRE(pool.RemoveInactive() == 1 && pool.RemoveInactive() == 0);
    first.state = 2;
    REQUIRE(pool.Add(1, &first) == 0 && pool.NotifyStateChanged(1) && pool.RemoveInactive() == 0);
    REQUIRE(!pool.GetStatistics(1).mayContainInactive);
    REQUIRE(pool.NotifyStateChanged(1) && pool.Clear(1) && !pool.GetStatistics(1).mayContainInactive);
    return true;
}

bool RemoveAllBusy() {
    Pool pool{};
    Record busy{};
    Other inactive{};
    REQUIRE(pool.Register<Record>(1) && pool.Register<Other>(2));
    REQUIRE(pool.Add(1, &busy) == 0 && pool.Add(2, &inactive) == 0);
    inactive.state = 0;
    REQUIRE(pool.NotifyStateChanged(2));
    auto range{pool.GetRange<Record>(1)};
    REQUIRE(range && pool.RemoveInactive() == Pool::InvalidIndex);
    // 先に取得したID2も解放され、変更フラグと格納数は維持される。
    REQUIRE(pool.GetStatistics(2).mayContainInactive && pool.Count(2) == 1);
    REQUIRE(pool.GetRange<Other>(2));
    range.Reset();
    REQUIRE(pool.RemoveInactive() == 1 && pool.Count(1) == 1 && pool.Count(2) == 0);
    return true;
}

/** @brief 基底が先頭にない配置を作る。 */
struct Prefix {
    virtual ~Prefix() noexcept = default;
    std::size_t marker{12345};
};
struct OffsetComponent : Prefix, slab::Component {
    OffsetComponent() noexcept = default;
    std::size_t id{};
};

bool BasePointerConversion() {
    constexpr std::size_t count{1031};
    auto objects{std::unique_ptr<OffsetComponent[]>{new (std::nothrow) OffsetComponent[count]}};
    auto pointers{std::unique_ptr<OffsetComponent*[]>{new (std::nothrow) OffsetComponent*[count]}};
    REQUIRE(objects && pointers);
    REQUIRE(static_cast<void*>(static_cast<slab::Component*>(&objects[0])) != static_cast<void*>(&objects[0]));
    for (std::size_t i{}; i < count; ++i) { objects[i].id = i; pointers[i] = &objects[i]; }
    Pool pool{};
    REQUIRE(pool.Register<OffsetComponent>(1, {.initialCapacity = 0, .reservedCapacity = 512, .commitBytes = 4096}));
    REQUIRE(pool.AddBatch<OffsetComponent>(1, {pointers.get(), count}) == 0);
    std::size_t inactive{};
    {
        auto range{pool.GetRange<OffsetComponent>(1)};
        REQUIRE(range);
        for (std::size_t i{}; i < count; ++i) {
            REQUIRE(range.At(i) == &objects[i] && range.At(i)->marker == 12345);
            if (i % 5 == 0) { range.At(i)->state = 0; ++inactive; }
        }
        REQUIRE(pool.NotifyStateChanged(1));
    }
    REQUIRE(pool.RemoveInactive() == inactive);
    auto range{std::as_const(pool).GetRange<OffsetComponent>(1)};
    REQUIRE(range && range.StoredCount() == count - inactive);
    for (const auto& item : range) {
        REQUIRE(item.id < count && &item == &objects[item.id] && item.id % 5 != 0 && item.marker == 12345);
    }
    return true;
}

bool DirtyConcurrent() {
    for (auto threads : {10U, 14U}) {
        Objects objects{threads};
        REQUIRE(objects.Valid());
        Pool pool{};
        for (unsigned id{}; id < threads; ++id) {
            REQUIRE(pool.Register<Record>(id) && pool.Add(id, objects.pointers[id]) == 0);
        }
        benchmarks::ParallelTasks workers{threads};
        std::atomic<bool> valid{true};
        workers.ForEachRange(threads, [&](std::size_t worker, std::size_t, std::size_t) {
            const auto id{static_cast<Pool::PoolId>(worker)};
            auto range{pool.GetRange<Record>(id)};
            if (!range) { valid = false; return; }
            range.At(0)->state = 0;
            if (!pool.NotifyStateChanged(id) || !pool.NotifyStateChanged(id)) { valid = false; }
        });
        REQUIRE(valid && pool.RemoveInactive() == threads && pool.RemoveInactive() == 0);
    }
    return true;
}
}

int RunAll() {
    struct Test { const char* name; bool (*run)(); };
    const Test cases[]{
        {"数値ID・型・null入力・無効設定", Registration},
        {"非所有・コピー不可型・実体を解放しない", NonOwnership},
        {"予約・コミット・領域追加・容量再利用", ReserveAndCommit},
        {"フレーム末尾コミット・自動/明示方式", FrameCommit},
        {"状態0の除外・複数コミット区間の境界", RangeBoundaries},
        {"末尾との入れ替え・全件除去", SwapRemoval},
        {"ランダムな除去と追加・実体アドレス維持", RandomChurn},
        {"走査権のRAII・移動・const・空ID・早期return", ScopedAccess},
        {"使用中IDへの追加・除去・容量操作を拒否", BusyOperations},
        {"10・14スレッドの同じIDへの競合で成功者は1人", SameIdContention},
        {"10・14個のIDを並列追加・全件走査・担当交代", DifferentIds},
        {"release/acquireによる普通の整数の引き継ぎ", ThreadHandoff},
        {"変更フラグ・全IDの除去・通知省略・空ID", DirtyAndRemoveAll},
        {"一括除去の失敗時に全IDの内容と権利を維持", RemoveAllBusy},
        {"多重継承の基底位置補正・const取得・末尾除去", BasePointerConversion},
        {"10・14個のIDからの変更通知と一括除去", DirtyConcurrent}
    };
    std::size_t passed{};
    for (const auto& test : cases) {
        if (!test.run()) { std::cerr << "[テスト失敗] " << test.name << '\n'; return 1; }
        ++passed;
        std::cout << "[成功] " << test.name << '\n';
    }
    std::cout << "テスト結果：" << passed << '/' << std::size(cases) << " 成功\n";
    return 0;
}
}
