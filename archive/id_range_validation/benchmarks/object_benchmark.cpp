/** @file object_benchmark.cpp
 * @brief IDごとの全件走査と、同じ分担の固定長ポインタ配列を比較する。
 */
#include "benchmarks.h"
#include "parallel_tasks.h"
#include "slab/slab_pool.h"
#include <Windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <thread>

namespace benchmarks {
namespace {
using Pool = slab::SlabPool;
using Clock = std::chrono::steady_clock;
constexpr std::size_t Repetitions{7};
constexpr std::size_t Passes{32};
constexpr std::size_t Warmup{2};

struct Component : slab::Component {
    float x{};
    std::size_t id{};
    std::size_t updates{};
    float speed{2.0f};
    std::array<float, 9> values{};
    explicit Component(std::size_t value) noexcept : id{value} {}
    void Update(float delta) noexcept {
        x += speed * delta;
        ++updates;
        for (auto& value : values) { value += delta; }
    }
    void Reset() noexcept { updates = 0; x = 0.0f; values.fill(0.0f); }
    [[nodiscard]]
    bool Verify() const noexcept {
        std::size_t expected{};
        if (state != 0) { expected = Passes + Warmup; }
        if (updates != expected || x != static_cast<float>(expected) || speed != 2.0f) { return false; }
        for (auto value : values) {
            if (value != static_cast<float>(expected) * 0.5f) { return false; }
        }
        return true;
    }
};
static_assert(sizeof(Component) == 64);

[[nodiscard]]
double Milliseconds(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>{Clock::now() - start}.count();
}
[[nodiscard]]
double Median(std::array<double, Repetitions> samples) noexcept {
    std::sort(samples.begin(), samples.end());
    return samples[Repetitions / 2];
}
/** @brief 同じIDの全件を同じワーカーが処理するよう、比較用の入力も同じ個数に分ける。 */
[[nodiscard]]
std::size_t First(std::size_t id, std::size_t count, std::size_t idCount) noexcept {
    return count / idCount * id + std::min(id, count % idCount);
}

[[nodiscard]]
double TimeScan(ParallelTasks& workers, Pool& pool, std::span<Component* const> source, std::size_t method) {
    const auto idCount{workers.WorkerCount()};
    std::atomic<bool> valid{true};
    const ParallelTasks::Task task{[&](std::size_t, std::size_t firstId, std::size_t lastId) {
        for (auto id{firstId}; id < lastId; ++id) {
            if (method == 0) {
                auto range{pool.GetRange<Component>(static_cast<Pool::PoolId>(id))};
                if (!range) { valid = false; continue; }
                for (auto& object : range) { object.Update(0.5f); }
            } else {
                const auto last{First(id + 1, source.size(), idCount)};
                for (auto i{First(id, source.size(), idCount)}; i < last; ++i) {
                    if (source[i]->state != 0) { source[i]->Update(0.5f); }
                }
            }
        }
    }};
    const auto execute{[&] {
        if (idCount == 1) { task(0, 0, idCount); }
        else { workers.ForEachRange(idCount, task); }
    }};
    for (std::size_t pass{}; pass < Warmup; ++pass) { execute(); }
    const auto start{Clock::now()};
    for (std::size_t pass{}; pass < Passes; ++pass) { execute(); }
    const auto elapsed{Milliseconds(start) / static_cast<double>(Passes)};
    if (!valid) { return -1.0; }
    return elapsed;
}

[[nodiscard]]
bool Measure(std::size_t count, bool shuffled, bool includeInactive) {
    auto owners{std::make_unique<std::unique_ptr<Component>[]>(count)};
    auto pointers{std::make_unique<Component*[]>(count)};
    std::size_t inactive{};
    for (std::size_t i{}; i < count; ++i) {
        owners[i].reset(new (std::nothrow) Component{i});
        if (!owners[i]) { return false; }
        if (includeInactive && i % 4 == 0) { owners[i]->state = 0; ++inactive; }
        pointers[i] = owners[i].get();
    }
    if (shuffled) {
        std::mt19937 random{20261017};
        std::shuffle(pointers.get(), pointers.get() + count, random);
    }
    const std::span<Component* const> source{pointers.get(), count};
    std::cout << "\n全IDの合計=" << count << "、状態0=" << inactive << "、実体=" << sizeof(Component)
              << "バイト、アクセス順=";
    if (shuffled) { std::cout << "シャッフル"; }
    else { std::cout << "newした順"; }
    std::cout << "\nスレッド/ID数 | ID単位のPool走査 | 通常のポインタ配列\n";
    for (auto threads : {1U, 10U, 14U}) {
        Pool pool{};
        for (unsigned id{}; id < threads; ++id) {
            const auto first{First(id, count, threads)};
            const auto length{First(id + 1, count, threads) - first};
            if (!pool.Register<Component>(id, {.initialCapacity = 0, .reservedCapacity = length,
                .commitBytes = 64 * 1024}) || !pool.Commit(id, length) ||
                pool.AddBatch<Component>(id, source.subspan(first, length)) != 0) { return false; }
            auto range{pool.GetRange<Component>(id)};
            if (!range || range.StoredCount() != length) { return false; }
            for (std::size_t i{}; i < length; ++i) {
                auto* expected{source[first + i]};
                if (expected->state == 0) { expected = nullptr; }
                if (range.At(i) != expected) { return false; }
            }
        }
        ParallelTasks workers{threads};
        std::array<std::array<double, Repetitions>, 2> samples{};
        for (std::size_t round{}; round <= Repetitions; ++round) {
            for (std::size_t position{}; position < 2; ++position) {
                const auto method{(position + round) % 2};
                for (std::size_t i{}; i < count; ++i) { owners[i]->Reset(); }
                const auto elapsed{TimeScan(workers, pool, source, method)};
                if (elapsed < 0.0) { return false; }
                for (std::size_t i{}; i < count; ++i) {
                    if (owners[i]->id != i || !owners[i]->Verify()) { return false; }
                }
                if (round != 0) { samples[method][round - 1] = elapsed; }
            }
        }
        std::cout << std::setw(12) << threads << " | " << std::setw(17) << Median(samples[0])
                  << " | " << std::setw(20) << Median(samples[1]) << '\n';
        if (pool.RemoveInactive() != inactive || pool.RemoveInactive() != 0) { return false; }
        std::size_t remaining{};
        for (unsigned id{}; id < threads; ++id) {
            auto range{pool.GetRange<Component>(id)};
            if (!range) { return false; }
            remaining += range.StoredCount();
            for (const auto& object : range) {
                if (!object.Verify() || &object != owners[object.id].get()) { return false; }
            }
        }
        if (remaining != count - inactive) { return false; }
    }
    return true;
}
}

void PrepareConsole() noexcept {
    (void)SetConsoleOutputCP(CP_UTF8);
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
}
bool RunPointers(bool full) {
    std::cout << "\n--- IDごとの並列走査比較（全IDを1回処理するms・7回の中央値）---\n"
              << "MSVC=" << _MSC_FULL_VER << "、C++=" << __cplusplus
              << "、論理プロセッサ=" << std::thread::hardware_concurrency() << '\n'
              << "同じ合計件数を1/10/14個のIDへ分割し、1IDにつき1ワーカーで全件を走査します。\n"
              << "両方式とも同じ実体と普通のstateを読み、同じID単位の分担でUpdateします。\n"
              << "2回慣らした後の32回平均。初回走行を除外し、方式の測定順を交代します。\n"
              << "登録・生成・検証は計測外。IDの走査権取得と解放・ワーカー完了待ちは計測内です。\n";
    std::cout << std::fixed << std::setprecision(5);
    for (auto count : {std::size_t{10000}, std::size_t{100000}}) {
        for (bool shuffled : {false, true}) {
            for (bool inactive : {false, true}) {
                if (!Measure(count, shuffled, inactive)) {
                    std::cerr << "[測定エラー] IDの取得・更新回数・状態0の除外・末尾除去の検証に失敗しました。\n";
                    return false;
                }
            }
        }
    }
    if (full) {
        for (bool shuffled : {false, true}) {
            for (bool inactive : {false, true}) {
                if (!Measure(1000000, shuffled, inactive)) { return false; }
            }
        }
    }
    std::cout << "全条件でID単位の走査・更新回数・状態0の除去後の参照先を確認しました。\n";
    return true;
}
}
