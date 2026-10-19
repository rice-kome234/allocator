/** @file bulk_benchmark.cpp
 * @brief 同じ固定Slabにポインタをコピーし、登録方式と並列走査を比較する。
 */
#include "benchmarks.h"
#include "parallel_tasks.h"
#include "slab/slab_pool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <thread>

namespace benchmarks {
namespace {

using Pool = slab::SlabPool;
using Clock = std::chrono::steady_clock;
constexpr std::size_t Passes{16};
constexpr std::size_t Repetitions{5};

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[一括登録の検証失敗] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/** @brief コピーできない外部実体。変更するのは元の実体の値だけ。 */
struct Item {
    static constexpr Pool::PoolId Id{1};
    std::size_t index{};
    std::uint64_t updates{}, value{};
    std::array<std::uint64_t, 5> padding{};

    explicit Item(std::size_t id) noexcept : index{id}, value{id + 1} {}
    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;
    void Update(float) noexcept {
        value = value * 1664525u + 1013904223u;
        ++updates;
    }
};
static_assert(sizeof(Item) == 64);

/** @brief 全方式で同じ実体・入力配列を使う。生成と破棄は測定外。 */
struct Source {
    std::size_t count{};
    std::unique_ptr<std::unique_ptr<Item>[]> owners{};
    std::unique_ptr<void*[]> pointers{};
    std::unique_ptr<unsigned char[]> seen{};

    explicit Source(std::size_t size) : count{size},
        owners{new (std::nothrow) std::unique_ptr<Item>[size]},
        pointers{new (std::nothrow) void*[size]}, seen{new (std::nothrow) unsigned char[size]} {
        Require(owners && pointers && seen, "入力配列の確保に失敗しました。");
        for (std::size_t i{}; i < count; ++i) {
            owners[i].reset(new (std::nothrow) Item{i});
            Require(owners[i] != nullptr, "外部実体の生成に失敗しました。");
            pointers[i] = owners[i].get();
        }
    }

    void Reset() noexcept {
        for (std::size_t i{}; i < count; ++i) {
            owners[i]->updates = 0;
            owners[i]->value = i + 1;
            seen[i] = 0;
        }
    }
};

enum class Method { SerialBatch, MutexSingle, CasSingle, CasBatch };
struct Configuration { const char* name{}; Method method{}; };
constexpr std::array Configurations{
    Configuration{"主スレッドで一括コピー", Method::SerialBatch},
    Configuration{"並列・mutexで1個ずつ", Method::MutexSingle},
    Configuration{"並列・CASで1個ずつ", Method::CasSingle},
    Configuration{"並列・CASで256個ずつ", Method::CasBatch},
};

struct Sample { double prepare{}, append{}, scan{}; };

[[nodiscard]]
double Milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

/** @brief 登録数・重複・実体のアドレス・Update回数と値を測定外で検証する。 */
void Validate(const Pool& pool, Source& source, bool ordered) {
    Require(pool.Count(Item::Id) == source.count, "登録数が一致しません。");
    std::size_t position{};
    for (const auto& item : pool.GetRange<const Item>(Item::Id)) {
        Require(item.index < source.count && source.owners[item.index].get() == &item,
            "元の実体とアドレスが一致しません。");
        Require(source.seen[item.index] == 0, "重複したポインタがあります。");
        source.seen[item.index] = 1;
        Require(item.updates == Passes, "Update回数が一致しません。");
        std::uint64_t expected{item.index + 1};
        for (std::size_t pass{}; pass < Passes; ++pass) { expected = expected * 1664525u + 1013904223u; }
        Require(item.value == expected, "Update結果が一致しません。");
        if (ordered) { Require(item.index == position, "一括コピーの入力順序が変わりました。"); }
        ++position;
    }
    Require(position == source.count, "全件走査できませんでした。");
}

[[nodiscard]]
Sample Measure(Source& source, ParallelTasks& tasks, Method method) {
    source.Reset();
    Pool pool{};
    Require(pool.Register(Item::Id, {0, 64}), "ID登録に失敗しました。");
    const auto preparing{Clock::now()};
    Require(pool.Reserve(Item::Id, source.count), "一括確保に失敗しました。");
    const auto prepared{Clock::now()};
    const std::span<void* const> pointers{source.pointers.get(), source.count};
    if (method == Method::SerialBatch) {
        Require(pool.AddBatch(Item::Id, pointers) == 0, "一括コピーに失敗しました。");
    } else if (method == Method::MutexSingle) {
        tasks.ForEachRange(source.count, [&](std::size_t, std::size_t first, std::size_t last) {
            for (std::size_t i{first}; i < last; ++i) {
                Require(pool.AddConcurrent(Item::Id, pointers[i]) != Pool::InvalidIndex,
                    "mutex追加に失敗しました。");
            }
        });
    } else {
        std::size_t batchSize{256};
        if (method == Method::CasSingle) { batchSize = 1; }
        Require(tasks.CopyPointers(pool, Item::Id, pointers, batchSize), "並列コピーに失敗しました。");
    }
    const auto appended{Clock::now()};
    for (std::size_t pass{}; pass < Passes; ++pass) { tasks.ForEach(pool, Item::Id, &Item::Update, 0.0f); }
    const auto scanned{Clock::now()};
    Validate(pool, source, method == Method::SerialBatch);
    Require(pool.GetStatistics().slabs == 1, "一括確保したSlabが分割されています。");
    return {Milliseconds(prepared - preparing), Milliseconds(appended - prepared),
        Milliseconds(scanned - appended) / static_cast<double>(Passes)};
}

[[nodiscard]]
double Median(std::array<double, Repetitions> values) noexcept {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}

int RunBulk(const Settings& settings) {
    std::cout << "\n=== 固定Slabへのポインタ一括コピーと並列走査 ===\n"
        << "実体は外部new、コピーするのはポインタ値のみ。コンテナにvectorは使用しません。\n"
        << "全方式で同じ容量を1 Slabに事前確保。ワーカーの起動・実体の生成/破棄・検証は測定外。\n"
        << "確保と登録は各5回の中央値、走査は16回の平均を取ってから5回の中央値。\n"
        << "並列時間は開始通知・完了待ちを含みます。登録順は並列方式では不定です。\n"
        << "CASの対象は区間予約のみ。OS確保・完了バリアを含む処理全体はロックフリーではありません。\n";
    std::cout << "論理プロセッサ=" << std::thread::hardware_concurrency()
        << " / ポインタ=" << sizeof(void*) << "バイト / C++=" << __cplusplus;
#ifdef _MSC_VER
    std::cout << " / MSVC=" << _MSC_FULL_VER;
#endif
#ifdef NDEBUG
    std::cout << " / Release\n";
#else
    std::cout << " / Debug（性能判断にはReleaseを使用）\n";
#endif
    std::array<std::size_t, 3> counts{10000, 100000, 1000000};
    std::size_t countCases{2};
    if (settings.full) { countCases = counts.size(); }
    for (std::size_t sizeCase{}; sizeCase < countCases; ++sizeCase) {
        Source source{counts[sizeCase]};
        for (const std::size_t workers : {1u, 10u, 14u}) {
            ParallelTasks tasks{workers};
            std::array<std::array<Sample, Repetitions>, Configurations.size()> samples{};
            for (const auto& config : Configurations) { (void)Measure(source, tasks, config.method); }
            for (std::size_t repeat{}; repeat < Repetitions; ++repeat) {
                for (std::size_t offset{}; offset < Configurations.size(); ++offset) {
                    const auto index{(repeat + offset) % Configurations.size()};
                    samples[index][repeat] = Measure(source, tasks, Configurations[index].method);
                }
            }
            std::cout << "\n件数=" << source.count << " / 走査ワーカー=" << workers << " / 単位: ms\n"
                << "方式 | 一括確保 | 登録 | 1回の走査+Update | 登録の観測最大\n";
            for (std::size_t method{}; method < Configurations.size(); ++method) {
                std::array<double, Repetitions> prepare{}, append{}, scan{};
                for (std::size_t repeat{}; repeat < Repetitions; ++repeat) {
                    prepare[repeat] = samples[method][repeat].prepare;
                    append[repeat] = samples[method][repeat].append;
                    scan[repeat] = samples[method][repeat].scan;
                }
                std::cout << Configurations[method].name << " | " << std::fixed << std::setprecision(4)
                    << Median(prepare) << " | " << Median(append) << " | " << Median(scan)
                    << " | " << *std::max_element(append.begin(), append.end()) << '\n';
            }
        }
    }
    std::cout << "[成功] 全方式でポインタ集合・元の実体・全件のUpdate結果が一致しました。\n";
    return 0;
}

}
