/**
 * @file growth_benchmark.cpp
 * @brief 事前確保の有無と、容量拡張を伴う追加の時間を比較する。
 */
#include "benchmarks.h"
#include "growth_benchmark_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace benchmarks {
namespace {

using namespace growth_detail;

struct TotalSample {
    double preparationMs{};
    double appendMs{};
    std::size_t capacity{};
};

struct GrowthEvent {
    double milliseconds{};
    std::size_t appendNumber{};
    std::size_t beforeCapacity{};
    std::size_t afterCapacity{};
    std::size_t relocatedPointers{};
};

struct GrowthSample {
    std::size_t events{};
    std::size_t relocations{};
    std::size_t relocatedPointers{};
    std::size_t largestRelocation{};
    std::size_t capacity{};
    double sumMs{};
    GrowthEvent slowest{};
};

/** @brief 通常の追加ループをまとめて測る。個々の追加には時計・記録処理を入れない。 */
template<class Storage>
[[nodiscard]]
TotalSample MeasureTotal(const Configuration& configuration, const Source& source) {
    Storage storage{configuration};
    const auto start{Clock::now()};
    storage.Prepare(source.pointers.size());
    const auto prepared{Clock::now()};
    bool success{true};
    for (void* pointer : source.pointers) { success &= storage.Append(pointer); }
    const auto finished{Clock::now()};
    Require(success, "ポインタの追加に失敗しました。");
    storage.Validate(source.pointers);
    return {Milliseconds(prepared - start), Milliseconds(finished - prepared), storage.Capacity()};
}

/**
 * @brief 容量が尽きた追加だけを別走行で測る。
 * @details 記録・検証・容量取得は計測区間外。時間には確保・既存要素の再配置・旧領域解放が含まれる。
 * 再配置数はvectorの旧sizeから算出する論理的な要素数で、実際のメモリ転送量の測定ではない。
 */
template<class Storage>
[[nodiscard]]
GrowthSample MeasureGrowth(const Configuration& configuration, const Source& source) {
    Storage storage{configuration};
    storage.Prepare(source.pointers.size());
    std::size_t capacity{storage.Capacity()};
    GrowthSample sample{};
    for (std::size_t i{}; i < source.pointers.size(); ++i) {
        if (i != capacity) {
            Require(storage.Append(source.pointers[i]), "通常追加に失敗しました。");
            continue;
        }
        GrowthEvent event{};
        event.appendNumber = i + 1;
        event.beforeCapacity = capacity;
        const auto start{Clock::now()};
        const bool success{storage.Append(source.pointers[i])};
        const auto finished{Clock::now()};
        Require(success, "拡張を伴う追加に失敗しました。");
        event.milliseconds = Milliseconds(finished - start);
        capacity = storage.Capacity();
        event.afterCapacity = capacity;
        Require(capacity > event.beforeCapacity, "拡張後の容量が増えていません。");
        if (!configuration.slab) {
            event.relocatedPointers = i;
            sample.relocatedPointers += i;
            sample.largestRelocation = std::max(sample.largestRelocation, i);
            if (i != 0) { ++sample.relocations; }
        }
        ++sample.events;
        sample.sumMs += event.milliseconds;
        if (sample.events == 1 || event.milliseconds > sample.slowest.milliseconds) {
            sample.slowest = event;
        }
    }
    sample.capacity = capacity;
    storage.Validate(source.pointers);
    if (configuration.reserve) {
        Require(sample.events == 0 && sample.relocatedPointers == 0, "事前確保後に再確保が発生しました。");
    } else {
        Require(sample.events != 0, "事前確保なしの拡張が記録されていません。");
        if (configuration.slab) {
            const auto slots{configuration.growthBytes / sizeof(void*)};
            Require(sample.events == (source.pointers.size() + slots - 1) / slots,
                "Slabの拡張記録に漏れがあります。");
        } else {
            Require(sample.relocations + 1 == sample.events, "vectorの初回確保と再確保の数が不一致です。");
        }
    }
    return sample;
}

struct Samples {
    std::vector<TotalSample> totals{};
    std::vector<GrowthSample> growths{};
};

/** @brief どちらの走行も終了後に全ポインタを検証し、確保済みコンテナは使い回さない。 */
template<class Storage>
void Measure(const Configuration& configuration, const Source& source, Samples& samples) {
    samples.totals.push_back(MeasureTotal<Storage>(configuration, source));
    samples.growths.push_back(MeasureGrowth<Storage>(configuration, source));
}

void Print(const Configuration& configuration, const Samples& samples, int repetitions) {
    std::vector<double> preparations{}, appends{}, totals{}, growthSums{}, peaks{};
    GrowthEvent overallPeak{};
    const auto& reference{samples.growths.front()};
    for (std::size_t i{}; i < samples.totals.size(); ++i) {
        const auto& total{samples.totals[i]};
        const auto& growth{samples.growths[i]};
        Require(total.capacity == reference.capacity && growth.capacity == reference.capacity
            && growth.events == reference.events && growth.relocations == reference.relocations
            && growth.relocatedPointers == reference.relocatedPointers,
            "測定走行間で容量または拡張の内訳が一致しません。");
        preparations.push_back(total.preparationMs);
        appends.push_back(total.appendMs);
        totals.push_back(total.preparationMs + total.appendMs);
        growthSums.push_back(growth.sumMs);
        peaks.push_back(growth.slowest.milliseconds * 1000.0);
        if (growth.slowest.milliseconds > overallPeak.milliseconds) { overallPeak = growth.slowest; }
    }
    std::cout << "  " << configuration.name << '\n'
        << "    全体測定の中央値: 準備=" << Median(preparations) << " ms / 追加=" << Median(appends)
        << " ms / 準備+追加=" << Median(totals) << " ms / 最終容量=" << reference.capacity << " 個\n"
        << "    拡張回数=" << reference.events << " 回（初回領域確保を含む） / 登録ポインタの再配置="
        << reference.relocations << " 回 / 再配置対象の累計=" << reference.relocatedPointers
        << " 個（" << reference.relocatedPointers * sizeof(void*) << " バイト相当）\n"
        << "    最大1回の再配置対象=" << reference.largestRelocation << " 個（"
        << reference.largestRelocation * sizeof(void*) << " バイト相当）\n"
        << "    拡張診断: 拡張付き追加の合計中央値=" << Median(growthSums)
        << " ms / 各走行の最大値の中央値=" << Median(peaks) << " us / " << repetitions
        << " 走行での観測最大=" << overallPeak.milliseconds * 1000.0 << " us\n";
    if (reference.events != 0) {
        std::cout << "    観測最大の発生: " << overallPeak.appendNumber << " 個目 / 容量="
            << overallPeak.beforeCapacity << " -> " << overallPeak.afterCapacity
            << " 個 / 再配置対象=" << overallPeak.relocatedPointers << " 個\n";
    }
}

/** @brief 2回の時計読み取りの差を表示する。測定値からの減算は行わない。 */
[[nodiscard]]
double TimerBaselineNanoseconds() {
    std::vector<double> samples{};
    samples.reserve(257);
    for (int i{}; i < 257; ++i) {
        const auto start{Clock::now()};
        const auto finished{Clock::now()};
        samples.push_back(std::chrono::duration<double, std::nano>{finished - start}.count());
    }
    return Median(samples);
}

}

int RunGrowth(const Settings& settings) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n=== 事前確保なし: ポインタ追加と容量拡張の比較 ===\n"
        << "実体は計測前にnew。比較対象はstd::vector<void*>とSlabPoolで、実体の移動はありません。\n"
        << "全体測定と拡張診断は別走行。各走行は空のコンテナから開始し、全ポインタを終了後に検証します。\n"
        << "準備にはvectorのreserveまたはSlabのID登録を含めます。終了時のコンテナ破棄は計測外です。\n"
        << "拡張診断は容量が尽きた1回の追加を計測し、確保・再配置・旧領域解放をまとめて含めます。\n"
        << "Slabは初期容量0、Reserveなし。登録ポインタの再配置は0でも、索引の再確保・コピー時間は含みます。\n"
        << "再配置バイト数は対象要素数からの算出値です。コピー処理だけの時間や実際の転送量ではありません。\n"
        << "時計の空測定中央値=" << TimerBaselineNanoseconds() << " ns（減算なし）\n"
        << "観測最大にはOSの割り込みや他処理の影響も含まれ、将来の最大待ち時間を保証しません。\n";
    const std::array configurations{
        Configuration{"vector・reserveなし", false, false, 0},
        Configuration{"vector・全件reserveあり", false, true, 0},
        Configuration{"Slab・初期0・追加64バイト", true, false, 64},
        Configuration{"Slab・初期0・追加512バイト", true, false, 512},
        Configuration{"Slab・初期0・追加2048バイト", true, false, 2048},
    };
    std::vector<std::size_t> counts{10000, 100000};
    int repetitions{3};
    if (settings.full) { counts.push_back(1000000); repetitions = 5; }
    std::cout << "1走行で指定個数を1回ずつ追加 / 測定回数=" << repetitions << '\n';
    for (std::size_t count : counts) {
        Source source{count};
        std::array<Samples, configurations.size()> samples{};
        // 各方式を一度実行してから、方式の順序を交代して測定する。
        for (const auto& configuration : configurations) {
            if (configuration.slab) { (void)MeasureTotal<SlabStorage>(configuration, source); }
            else { (void)MeasureTotal<VectorStorage>(configuration, source); }
        }
        for (int repeat{}; repeat < repetitions; ++repeat) {
            for (std::size_t offset{}; offset < configurations.size(); ++offset) {
                const auto index{(static_cast<std::size_t>(repeat) + offset) % configurations.size()};
                const auto& configuration{configurations[index]};
                if (configuration.slab) { Measure<SlabStorage>(configuration, source, samples[index]); }
                else { Measure<VectorStorage>(configuration, source, samples[index]); }
            }
        }
        std::cout << "\n追加数=" << count << " 個 / 実体は生成順\n";
        for (std::size_t i{}; i < configurations.size(); ++i) { Print(configurations[i], samples[i], repetitions); }
        source.ValidateObjects();
    }
    std::cout << "すべての追加・拡張結果と実体アドレスを検証しました。\n";
    return 0;
}

}
