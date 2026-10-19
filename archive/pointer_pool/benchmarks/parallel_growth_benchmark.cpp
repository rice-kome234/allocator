/**
 * @file parallel_growth_benchmark.cpp
 * @brief 通常追加・排他付き追加を、独立コンテナと共有コンテナで比較する。
 */
#include "benchmarks.h"
#include "growth_benchmark_internal.h"
#include "parallel_tasks.h"

#include <functional>
#include <iomanip>
#include <thread>

namespace benchmarks {
namespace {

using namespace growth_detail;

enum class Mode { Private, PrivateConcurrent, SharedConcurrent };

[[nodiscard]]
const char* ModeName(Mode mode) noexcept {
    switch (mode) {
    case Mode::Private: return "独立コンテナ・通常Add（ロックなし）";
    case Mode::PrivateConcurrent: return "独立コンテナ・AddConcurrent（競合なし）";
    case Mode::SharedConcurrent: return "共有コンテナ・AddConcurrent（同じIDへ追加）";
    }
    return "不正な条件";
}

/** @brief ワーカーごとの成否・最大値の記録。別のワーカーと書き込み先を分ける。 */
struct alignas(64) WorkerResult {
    bool success{true};
    double maximumUs{};
    std::size_t sourceIndex{};
    std::size_t completed{};
};

struct Sample {
    double prepareMs{};
    double appendMs{};
    double maximumUs{};
    std::size_t sourceIndex{};
    std::size_t worker{};
    std::size_t capacity{};
};

/** @brief 共有時は登録順が不定なので、全ポインタの集合を重複数も含めて照合する。 */
template<class Storage>
void ValidateShared(const Storage& storage, const Source& source) {
    auto actual{storage.CopyPointers()};
    storage.Validate(actual);
    auto expected{source.pointers};
    std::sort(actual.begin(), actual.end(), std::less<void*>{});
    std::sort(expected.begin(), expected.end(), std::less<void*>{});
    Require(actual == expected, "同時追加に欠落・重複・異なるポインタがあります。");
}

/**
 * @brief 通常測定は全体を、診断走行は個々の呼び出し全体を計測する。
 * @details 診断は AddConcurrent 内のロック待ち・追加・拡張・ロック解放を含む。
 * API の外側から計測し、内部容量の同時読み取りや別の排他ロックは追加しない。
 */
template<class Storage, bool Concurrent, bool Diagnostic>
void AppendRange(Storage& storage, std::span<void* const> pointers,
    std::size_t first, std::size_t last, WorkerResult& result) {
    for (std::size_t i{first}; i < last; ++i) {
        Clock::time_point start{};
        if constexpr (Diagnostic) { start = Clock::now(); }
        bool success{};
        if constexpr (Concurrent) { success = storage.AppendConcurrent(pointers[i]); }
        else { success = storage.Append(pointers[i]); }
        if constexpr (Diagnostic) {
            const auto finish{Clock::now()};
            const auto elapsed{Milliseconds(finish - start) * 1000.0};
            if (elapsed > result.maximumUs) {
                result.maximumUs = elapsed;
                result.sourceIndex = i;
            }
        }
        result.success &= success;
    }
    result.completed = last - first;
}

/** @brief 全走行で新しいコンテナを使用する。事前確保は主スレッドで行い、別に計時する。 */
template<class Storage, bool Diagnostic>
[[nodiscard]]
Sample Measure(ParallelTasks& executor, Mode mode, const Configuration& configuration, const Source& source) {
    std::size_t containerCount{executor.WorkerCount()};
    if (mode == Mode::SharedConcurrent) { containerCount = 1; }
    std::vector<std::unique_ptr<Storage>> containers{};
    for (std::size_t i{}; i < containerCount; ++i) {
        containers.push_back(std::make_unique<Storage>(configuration));
    }
    std::vector<WorkerResult> results(executor.WorkerCount());
    const auto count{source.pointers.size()};
    const auto portion{count / executor.WorkerCount()};
    const auto remainder{count % executor.WorkerCount()};
    const auto preparing{Clock::now()};
    for (std::size_t i{}; i < containerCount; ++i) {
        std::size_t target{portion};
        if (i < remainder) { ++target; }
        if (mode == Mode::SharedConcurrent) { target = count; }
        containers[i]->Prepare(target);
    }
    const auto prepared{Clock::now()};
    const ParallelTasks::Task task{[&](std::size_t worker, std::size_t first, std::size_t last) {
        if (mode == Mode::SharedConcurrent) {
            AppendRange<Storage, true, Diagnostic>(*containers[0], source.pointers, first, last, results[worker]);
        } else if (mode == Mode::PrivateConcurrent) {
            AppendRange<Storage, true, Diagnostic>(*containers[worker], source.pointers, first, last, results[worker]);
        } else {
            AppendRange<Storage, false, Diagnostic>(*containers[worker], source.pointers, first, last, results[worker]);
        }
    }};
    const auto start{Clock::now()};
    executor.ForEachRange(count, task);
    const auto finished{Clock::now()};
    Sample sample{Milliseconds(prepared - preparing), Milliseconds(finished - start)};
    std::size_t completed{};
    for (std::size_t i{}; i < results.size(); ++i) {
        Require(results[i].success, "並列実行中のポインタ追加に失敗しました。");
        completed += results[i].completed;
        if (results[i].maximumUs > sample.maximumUs) {
            sample.maximumUs = results[i].maximumUs;
            sample.sourceIndex = results[i].sourceIndex;
            sample.worker = i;
        }
    }
    Require(completed == count, "ワーカーの担当件数が合計件数と一致しません。");
    if (mode == Mode::SharedConcurrent) {
        ValidateShared(*containers[0], source);
    } else {
        for (std::size_t i{}; i < containerCount; ++i) {
            const auto first{portion * i + std::min(i, remainder)};
            std::size_t length{portion};
            if (i < remainder) { ++length; }
            containers[i]->Validate(std::span<void* const>{source.pointers}.subspan(first, length));
        }
    }
    for (const auto& storage : containers) { sample.capacity += storage->Capacity(); }
    return sample;
}

struct Samples {
    std::vector<Sample> totals{};
    std::vector<Sample> diagnostics{};
};

void Print(const Configuration& configuration, const Samples& samples) {
    std::vector<double> prepares{}, appends{}, totals{}, peaks{};
    Sample peak{};
    const auto capacity{samples.totals.front().capacity};
    for (std::size_t i{}; i < samples.totals.size(); ++i) {
        const auto& total{samples.totals[i]};
        const auto& diagnostic{samples.diagnostics[i]};
        Require(total.capacity == capacity && diagnostic.capacity == capacity, "走行間で最終容量が異なります。");
        prepares.push_back(total.prepareMs);
        appends.push_back(total.appendMs);
        totals.push_back(total.prepareMs + total.appendMs);
        peaks.push_back(diagnostic.maximumUs);
        if (diagnostic.maximumUs > peak.maximumUs) { peak = diagnostic; }
    }
    std::cout << "  " << configuration.name << '\n'
        << "    全体の中央値: 準備=" << Median(prepares) << " ms / 追加=" << Median(appends)
        << " ms / 準備+追加=" << Median(totals) << " ms / 合計容量=" << capacity << " 個\n"
        << "    別走行の1回追加診断: 各走行の最大値の中央値=" << Median(peaks)
        << " us / 全走行の観測最大=" << peak.maximumUs << " us（ワーカー=" << peak.worker
        << " / 入力の添字=" << peak.sourceIndex << "）\n";
}

}

int RunParallelGrowth(const Settings& settings) {
    std::cout << std::fixed << std::setprecision(4)
        << "\n=== 1・10・14スレッド: 通常追加と同時追加の比較 ===\n"
        << "論理プロセッサ数=" << std::thread::hardware_concurrency() << " / スレッド数はワーカー本数。\n"
        << "全スレッド合計で1万・10万個。実体は計測前にnewし、追加中は実体を書き換えません。\n"
        << "通常Addは独立プール限定。AddConcurrentは実際のAPIを使い、同じIDへの追加をmutexで同期します。\n"
        << "vectorの同時追加にも同じく1追加ごとのmutexを使います。\n"
        << "独立条件はワーカーごとに別コンテナ。共有条件は全ワーカーから一つのコンテナへ追加します。\n"
        << "準備は主スレッドでreserveまたはID登録。Slabは初期0・Reserveなし。\n"
        << "追加時間は開始通知から全ワーカー完了までの実時間で、通知・待機とOSスケジューリングを含みます。\n"
        << "スレッド起動・実体生成・コンテナ生成/破棄・結果検証は計測外。CPU固定は行いません。\n"
        << "個々の追加時間は別走行で診断。AddConcurrentではロック待ちと解放も含みます。\n"
        << "診断には時計の負担があり、総時間に加算しません。共有時の最大値を再確保だけの時間とは扱いません。\n"
        << "登録数・全ポインタの一致を毎回検証します。共有時の順序は不定です。\n";
    const std::array configurations{
        Configuration{"vector・reserveなし", false, false, 0},
        Configuration{"vector・全件reserveあり", false, true, 0},
        Configuration{"Slab・初期0・追加64バイト", true, false, 64},
        Configuration{"Slab・初期0・追加512バイト", true, false, 512},
        Configuration{"Slab・初期0・追加2048バイト", true, false, 2048},
    };
    const std::array modes{Mode::Private, Mode::PrivateConcurrent, Mode::SharedConcurrent};
    std::vector<std::size_t> counts{10000, 100000};
    int repetitions{3};
    if (settings.full) { counts.push_back(1000000); repetitions = 5; }
    std::cout << "慣らし各1回 / 計測各" << repetitions << "回・方式順を交代 / 中央値を表示\n";
    for (std::size_t count : counts) {
        Source source{count};
        for (const std::size_t workerCount : {1u, 10u, 14u}) {
            ParallelTasks executor{workerCount};
            for (const auto mode : modes) {
                std::array<Samples, configurations.size()> samples{};
                for (const auto& configuration : configurations) {
                    if (configuration.slab) { (void)Measure<SlabStorage, false>(executor, mode, configuration, source); }
                    else { (void)Measure<VectorStorage, false>(executor, mode, configuration, source); }
                }
                for (int repeat{}; repeat < repetitions; ++repeat) {
                    for (std::size_t offset{}; offset < configurations.size(); ++offset) {
                        const auto index{(static_cast<std::size_t>(repeat) + offset) % configurations.size()};
                        const auto& configuration{configurations[index]};
                        if (configuration.slab) {
                            samples[index].totals.push_back(Measure<SlabStorage, false>(executor, mode, configuration, source));
                            samples[index].diagnostics.push_back(Measure<SlabStorage, true>(executor, mode, configuration, source));
                        } else {
                            samples[index].totals.push_back(Measure<VectorStorage, false>(executor, mode, configuration, source));
                            samples[index].diagnostics.push_back(Measure<VectorStorage, true>(executor, mode, configuration, source));
                        }
                    }
                }
                std::cout << "\n追加数=" << count << " 個 / ワーカー=" << workerCount << " / " << ModeName(mode) << '\n';
                for (std::size_t i{}; i < configurations.size(); ++i) { Print(configurations[i], samples[i]); }
            }
        }
        source.ValidateObjects();
    }
    std::cout << "すべての並列追加結果と実体アドレスを検証しました。\n";
    return 0;
}

}