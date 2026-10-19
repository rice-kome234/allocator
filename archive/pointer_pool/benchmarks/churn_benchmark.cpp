/**
 * @file churn_benchmark.cpp
 * @brief ランダムな実体の削除・生成・末尾追加と、その後の並列走査を比較する。
 */
#include "benchmarks.h"
#include "growth_benchmark_internal.h"
#include "parallel_tasks.h"

#include <iomanip>
#include <numeric>
#include <random>

namespace benchmarks {
namespace {

using growth_detail::Clock;
using growth_detail::Median;
using growth_detail::Milliseconds;
using growth_detail::Require;
using Pool = slab::SlabPool;
constexpr Pool::PoolId itemId{1};
constexpr std::size_t cycleCount{8};
std::uint64_t churnChecksum{};

/** @brief 比較対象共通の64バイト実体。リンク・添字を全方式で同じサイズだけ保持する。 */
struct Item {
    Item* previous{};
    Item* next{};
    std::size_t id{};
    Pool::Index index{Pool::InvalidIndex};
    std::uint64_t x{};
    std::uint64_t y{};
    std::uint64_t updates{};
    std::uint64_t padding{};

    explicit Item(std::size_t value) noexcept : id{value} { Reset(); }
    void Reset() noexcept { x = id * 3 + 1; y = id * 7 + 2; updates = 0; }
    void Update() noexcept { x += y + 3; y ^= x + id; ++updates; }
};
static_assert(sizeof(Item) == 64);

enum class Kind { Linked, Vector, Slab };
struct Configuration {
    const char* name{};
    Kind kind{};
    bool reserve{};
    std::size_t growthBytes{};
};

/** @brief 乱数生成を計測外に置き、登録順が異なっても同じIDの実体を削除する。 */
struct Plan {
    struct Cycle {
        std::vector<std::size_t> removed{};
        std::vector<std::size_t> added{};
        std::size_t aliveCount{};
    };
    std::size_t initialCount{};
    std::size_t peakCount{};
    std::vector<std::size_t> born{};
    std::array<Cycle, cycleCount> cycles{};

    Plan(std::size_t count, bool growing) : initialCount{count}, peakCount{count}, born(count, 0) {
        std::vector<std::size_t> alive(count);
        std::iota(alive.begin(), alive.end(), std::size_t{});
        std::mt19937 random{20261013};
        std::size_t changes{count / 10};
        if (count != 0) { changes = std::max(changes, std::size_t{1}); }
        for (std::size_t cycle{}; cycle < cycleCount; ++cycle) {
            auto& step{cycles[cycle]};
            for (std::size_t i{}; i < changes; ++i) {
                std::uniform_int_distribution<std::size_t> choose{0, alive.size() - 1};
                const auto index{choose(random)};
                step.removed.push_back(alive[index]);
                alive[index] = alive.back();
                alive.pop_back();
            }
            std::size_t addedCount{changes};
            if (growing) { addedCount *= 2; }
            for (std::size_t i{}; i < addedCount; ++i) {
                const auto id{born.size()};
                step.added.push_back(id);
                alive.push_back(id);
                born.push_back(cycle);
            }
            step.aliveCount = alive.size();
            peakCount = std::max(peakCount, alive.size());
        }
    }
};

/** @brief 既存構成を模した侵入型の双方向リスト。対象ポインタから直接取り外す。 */
class LinkedStorage {
public:
    LinkedStorage(const Configuration&, std::size_t, std::size_t workers) : starts_(workers) {}

    [[nodiscard]]
    bool Append(Item* item) noexcept {
        item->previous = tail_;
        item->next = nullptr;
        if (tail_ != nullptr) { tail_->next = item; }
        else { head_ = item; }
        tail_ = item;
        ++count_;
        return true;
    }

    [[nodiscard]]
    bool AppendConcurrent(Item* item) {
        const std::lock_guard lock{mutex_};
        return Append(item);
    }

    [[nodiscard]]
    bool Remove(Item* item) noexcept {
        if (item->previous != nullptr) { item->previous->next = item->next; }
        else { head_ = item->next; }
        if (item->next != nullptr) { item->next->previous = item->previous; }
        else { tail_ = item->previous; }
        --count_;
        return true;
    }

    /** @brief 添字アクセスを持たないので、各担当範囲の先頭をリスト走査で求める。 */
    void PrepareTraversal(std::size_t workers, std::size_t count) noexcept {
        Item* cursor{head_};
        for (std::size_t worker{}; worker < workers; ++worker) {
            starts_[worker] = cursor;
            if (worker + 1 == workers) { break; }
            std::size_t length{count / workers};
            if (worker < count % workers) { ++length; }
            for (std::size_t i{}; i < length; ++i) { cursor = cursor->next; }
        }
    }

    void Traverse(std::size_t worker, std::size_t first, std::size_t last) const noexcept {
        Item* item{starts_[worker]};
        for (std::size_t i{first}; i < last; ++i) { item->Update(); item = item->next; }
    }

    [[nodiscard]]
    std::vector<Item*> Snapshot() const {
        std::vector<Item*> result{};
        result.reserve(count_);
        Item* previous{};
        for (Item* item{head_}; item != nullptr; item = item->next) {
            Require(result.size() < count_ && item->previous == previous, "リンクリストの循環または前リンク不一致です。");
            result.push_back(item);
            previous = item;
        }
        Require(result.size() == count_ && previous == tail_, "リンクリストの件数または末尾が不一致です。");
        return result;
    }

private:
    Item* head_{};
    Item* tail_{};
    std::size_t count_{};
    std::vector<Item*> starts_{};
    std::mutex mutex_{};
};

/** @brief vectorもSlabと同じ末尾交換削除にし、移った実体の添字を更新する。 */
class VectorStorage {
public:
    VectorStorage(const Configuration& configuration, std::size_t peakCount, std::size_t) {
        if (configuration.reserve) { pointers_.reserve(peakCount); }
    }

    [[nodiscard]]
    bool Append(Item* item) {
        item->index = pointers_.size();
        pointers_.push_back(item);
        return true;
    }

    [[nodiscard]]
    bool AppendConcurrent(Item* item) {
        const std::lock_guard lock{mutex_};
        return Append(item);
    }

    [[nodiscard]]
    bool Remove(Item* item) noexcept {
        const auto index{item->index};
        if (index >= pointers_.size() || pointers_[index] != item) { return false; }
        pointers_[index] = pointers_.back();
        pointers_[index]->index = index;
        pointers_.pop_back();
        return true;
    }

    void PrepareTraversal(std::size_t, std::size_t) const noexcept {}
    void Traverse(std::size_t, std::size_t first, std::size_t last) const noexcept {
        for (std::size_t i{first}; i < last; ++i) { pointers_[i]->Update(); }
    }

    [[nodiscard]]
    std::vector<Item*> Snapshot() const {
        for (std::size_t i{}; i < pointers_.size(); ++i) {
            Require(pointers_[i]->index == i, "vectorの末尾交換後の添字が不一致です。");
        }
        return pointers_;
    }

private:
    std::vector<Item*> pointers_{};
    std::mutex mutex_{};
};

class SlabStorage {
public:
    SlabStorage(const Configuration& configuration, std::size_t, std::size_t) {
        Require(pool_.Register(itemId, Pool::Options{0, configuration.growthBytes}), "変更比較のID登録に失敗しました。");
    }

    [[nodiscard]]
    bool Append(Item* item) noexcept {
        item->index = pool_.Add(itemId, item);
        return item->index != Pool::InvalidIndex;
    }

    [[nodiscard]]
    bool AppendConcurrent(Item* item) noexcept {
        item->index = pool_.AddConcurrent(itemId, item);
        return item->index != Pool::InvalidIndex;
    }

    [[nodiscard]]
    bool Remove(Item* item) noexcept {
        const auto result{pool_.RemoveAt(itemId, item->index)};
        if (result.moved != nullptr) { static_cast<Item*>(result.moved)->index = item->index; }
        return result.removed == item;
    }

    void PrepareTraversal(std::size_t, std::size_t) const noexcept {}
    void Traverse(std::size_t, std::size_t first, std::size_t last) const noexcept {
        while (first < last) {
            const auto batch{pool_.GetBatch<Item>(itemId, first, last - first)};
            for (auto& item : batch) { item.Update(); }
            first += batch.size();
        }
    }

    [[nodiscard]]
    std::vector<Item*> Snapshot() const {
        std::vector<Item*> result{};
        const auto count{pool_.Count(itemId)};
        result.reserve(count);
        for (std::size_t first{}; first < count;) {
            const auto batch{pool_.GetBatch<Item>(itemId, first)};
            Require(!batch.empty(), "Slab走査が進んでいません。");
            for (auto& item : batch) {
                Require(item.index == result.size(), "Slabの末尾交換後の添字が不一致です。");
                result.push_back(&item);
            }
            first += batch.size();
        }
        return result;
    }

private:
    Pool pool_{};
};

struct ScanTime { double splitMs{}, totalMs{}; };

/** @brief 範囲分割の準備と、開始通知から全ワーカー完了までを走査時間に含める。 */
template<class Storage>
[[nodiscard]]
ScanTime Scan(Storage& storage, ParallelTasks& executor, std::size_t count) {
    const ParallelTasks::Task task{[&](std::size_t worker, std::size_t first, std::size_t last) {
        storage.Traverse(worker, first, last);
    }};
    const auto start{Clock::now()};
    storage.PrepareTraversal(executor.WorkerCount(), count);
    const auto prepared{Clock::now()};
    executor.ForEachRange(count, task);
    const auto finish{Clock::now()};
    return {Milliseconds(prepared - start), Milliseconds(finish - start)};
}

/** @brief 各周期の計測後に全実体・重複・削除漏れ・更新回数と計算結果を確認する。 */
template<class Storage>
void Validate(const Storage& storage, const std::vector<std::unique_ptr<Item>>& owners,
    const Plan& plan, std::size_t cycle) {
    const auto actual{storage.Snapshot()};
    Require(actual.size() == plan.cycles[cycle].aliveCount, "変更後の登録件数が不一致です。");
    std::vector<unsigned char> seen(owners.size(), 0);
    for (const Item* item : actual) {
        Require(item != nullptr && item->id < owners.size(), "不正な実体が登録されています。");
        const auto id{item->id};
        Require(owners[id].get() == item && seen[id] == 0, "実体の不一致または重複登録です。");
        seen[id] = 1;
        Item expected{id};
        const auto updates{cycle + 1 - plan.born[id]};
        for (std::size_t i{}; i < updates; ++i) { expected.Update(); }
        Require(item->x == expected.x && item->y == expected.y && item->updates == expected.updates,
            "走査の更新漏れ・二重更新・計算結果の不一致です。");
        churnChecksum += item->x + item->y + item->updates;
    }
    for (std::size_t id{}; id < owners.size(); ++id) {
        Require(static_cast<bool>(owners[id]) == (seen[id] != 0), "削除漏れまたは登録漏れがあります。");
    }
}

struct Sample {
    double initialScanMs{};
    double removeMs{}, appendMs{}, scanMs{}, splitMs{}, totalMs{}, maximumCycleMs{};
};

/** @brief 1走行ごとに実体とコンテナを作り直し、削除→追加→走査を8周期繰り返す。 */
template<class Storage>
[[nodiscard]]
Sample Measure(const Configuration& configuration, const Plan& plan, ParallelTasks& executor, bool concurrent) {
    // 実体の所有権スロットは固定。各ワーカーは異なるスロットだけを書き換える。
    std::vector<std::unique_ptr<Item>> owners(plan.born.size());
    Storage storage{configuration, plan.peakCount, executor.WorkerCount()};
    for (std::size_t id{}; id < plan.initialCount; ++id) {
        owners[id].reset(new (std::nothrow) Item{id});
        Require(owners[id] != nullptr && storage.Append(owners[id].get()), "初期状態の生成・登録に失敗しました。");
    }
    // 変更前の走査を一度慣らし、値を復元してから参考の初期走査を測る。
    (void)Scan(storage, executor, plan.initialCount);
    for (const auto& item : owners) { if (item) { item->Reset(); } }
    Sample sample{};
    sample.initialScanMs = Scan(storage, executor, plan.initialCount).totalMs;
    for (const auto& item : owners) { if (item) { item->Reset(); } }
    std::vector<unsigned char> succeeded(executor.WorkerCount(), 1);
    for (std::size_t cycle{}; cycle < cycleCount; ++cycle) {
        const auto& step{plan.cycles[cycle]};
        bool removed{true};
        const auto removing{Clock::now()};
        for (const auto id : step.removed) {
            removed &= storage.Remove(owners[id].get());
            owners[id].reset(); // 登録解除後に実体をdeleteする。deleteも計測に含む。
        }
        const auto removedAt{Clock::now()};
        Require(removed, "ランダム削除に失敗しました。");
        const ParallelTasks::Task append{[&](std::size_t worker, std::size_t first, std::size_t last) {
            bool success{true};
            for (std::size_t i{first}; i < last; ++i) {
                const auto id{step.added[i]};
                owners[id].reset(new (std::nothrow) Item{id});
                Item* item{owners[id].get()};
                if (item == nullptr) { success = false; continue; }
                if (concurrent) { success &= storage.AppendConcurrent(item); }
                else { success &= storage.Append(item); }
            }
            succeeded[worker] = static_cast<unsigned char>(success);
        }};
        const auto adding{Clock::now()};
        if (concurrent) { executor.ForEachRange(step.added.size(), append); }
        else { append(0, 0, step.added.size()); }
        const auto addedAt{Clock::now()};
        Require(std::all_of(succeeded.begin(), succeeded.end(), [](auto value) { return value != 0; }),
            "実体生成または末尾追加に失敗しました。");
        const auto scan{Scan(storage, executor, step.aliveCount)};
        const auto removeMs{Milliseconds(removedAt - removing)};
        const auto appendMs{Milliseconds(addedAt - adding)};
        const auto total{removeMs + appendMs + scan.totalMs};
        sample.removeMs += removeMs;
        sample.appendMs += appendMs;
        sample.scanMs += scan.totalMs;
        sample.splitMs += scan.splitMs;
        sample.totalMs += total;
        sample.maximumCycleMs = std::max(sample.maximumCycleMs, total);
        Validate(storage, owners, plan, cycle);
    }
    return sample;
}

[[nodiscard]]
Sample Dispatch(const Configuration& configuration, const Plan& plan, ParallelTasks& executor, bool concurrent) {
    switch (configuration.kind) {
    case Kind::Linked: return Measure<LinkedStorage>(configuration, plan, executor, concurrent);
    case Kind::Vector: return Measure<VectorStorage>(configuration, plan, executor, concurrent);
    case Kind::Slab: return Measure<SlabStorage>(configuration, plan, executor, concurrent);
    }
    Require(false, "比較方式が不正です。");
    return {};
}

void Print(const Configuration& configuration, const std::vector<Sample>& samples) {
    std::vector<double> initial{}, removals{}, appends{}, scans{}, splits{}, totals{}, peaks{};
    double peak{};
    for (const auto& sample : samples) {
        initial.push_back(sample.initialScanMs);
        removals.push_back(sample.removeMs / cycleCount);
        appends.push_back(sample.appendMs / cycleCount);
        scans.push_back(sample.scanMs / cycleCount);
        splits.push_back(sample.splitMs / cycleCount);
        totals.push_back(sample.totalMs / cycleCount);
        peaks.push_back(sample.maximumCycleMs);
        peak = std::max(peak, sample.maximumCycleMs);
    }
    std::cout << "  " << configuration.name << '\n'
        << "    1周期平均の中央値: 削除+delete=" << Median(removals) << " ms / new+末尾追加="
        << Median(appends) << " ms / 走査+Update=" << Median(scans) << " ms / 合計=" << Median(totals) << " ms\n"
        << "    走査に含む分割準備=" << Median(splits) << " ms / 変更前の初期走査=" << Median(initial) << " ms\n"
        << "    周期合計の最大: 各走行の最大値の中央値=" << Median(peaks)
        << " ms / 全走行の観測最大=" << peak << " ms\n";
}

}

int RunChurn(const Settings& settings) {
    std::cout << std::fixed << std::setprecision(4)
        << "\n=== ランダム削除・末尾追加・走査の繰り返し比較 ===\n"
        << "毎周期、初期個数の10%を重複なしでランダム削除。固定seed=20261013、全方式で同じ実体IDを選びます。\n"
        << "削除・追加・走査は完了待ちを挟んで分離します。削除は主スレッド、走査は1・10・14ワーカーです。\n"
        << "追加は主スレッドの通常Addと、同じコンテナへのAddConcurrentを比較します。\n"
        << "リンクリストとvectorの同時追加も1回ごとのmutex。vectorはSlabと同じ末尾交換削除です。\n"
        << "実体は64バイト。削除は登録解除とdelete、追加はnewと末尾登録、走査は全件のUpdateを含みます。\n"
        << "初期構築・乱数生成・スレッド起動・検証・終了時の破棄は計測外。通知と完了待ちは計測内です。\n"
        << "リストの並列分割に必要な先頭探索は走査時間に含め、内訳にも表示します。\n"
        << "8周期の平均を走行ごとに求め、その中央値を表示。合計は削除・追加・走査の3区間の和です。\n"
        << "各周期後に全件検証するためキャッシュが温まる条件です。物理アドレス・列挙順は方式や実行で異なります。\n"
        << "通常のリンクリストは残存順を保ち、vector/Slabは末尾交換。同時追加の登録順は不定です。\n"
        << "初期vectorのreserve有無も表示します。容量を縮めないため同数補充では追加時に容量を再利用します。\n"
        << "最大周期時間はOSやヒープ状態を含む観測値です。ゲームのフレーム時間や将来の上限ではありません。\n";
    if (settings.churnGrowing) { std::cout << "増加条件: 初期個数の10%削除・20%追加。8周期後は初期個数の1.8倍。\n"; }
    else { std::cout << "同数補充条件: 削除した数だけ追加し、各周期後の実体数は一定。\n"; }
    const std::array configurations{
        Configuration{"双方向リンクリスト", Kind::Linked, false, 0},
        Configuration{"vector・初期reserveなし", Kind::Vector, false, 0},
        Configuration{"vector・最大件数reserveあり", Kind::Vector, true, 0},
        Configuration{"Slab・64バイト", Kind::Slab, false, 64},
        Configuration{"Slab・512バイト", Kind::Slab, false, 512},
        Configuration{"Slab・2048バイト", Kind::Slab, false, 2048},
    };
    churnChecksum = 0;
    // 空・全件削除・ワーカーより小さい件数でも、末尾交換と再追加が成立することを先に確認。
    {
        ParallelTasks executor{14};
        for (const auto count : {0u, 1u, 3u, 17u}) {
            for (const bool growing : {false, true}) {
                const Plan plan{count, growing};
                for (const bool concurrent : {false, true}) {
                    for (const auto& configuration : configurations) { (void)Dispatch(configuration, plan, executor, concurrent); }
                }
            }
        }
    }
    std::cout << "境界検証成功: 空・全件削除・再追加・余りのある並列分割。\n";
    std::vector<std::size_t> counts{10000, 100000};
    int repetitions{3};
    if (settings.full) { counts.push_back(1000000); repetitions = 5; }
    std::cout << "各条件を1走行慣らした後、方式順を交代して" << repetitions << "走行を計測。\n";
    for (const auto count : counts) {
        const Plan plan{count, settings.churnGrowing};
        for (const auto workers : {1u, 10u, 14u}) {
            ParallelTasks executor{workers};
            for (const bool concurrent : {false, true}) {
                std::array<std::vector<Sample>, configurations.size()> samples{};
                for (const auto& configuration : configurations) { (void)Dispatch(configuration, plan, executor, concurrent); }
                for (int repeat{}; repeat < repetitions; ++repeat) {
                    for (std::size_t offset{}; offset < configurations.size(); ++offset) {
                        const auto index{(static_cast<std::size_t>(repeat) + offset) % configurations.size()};
                        samples[index].push_back(Dispatch(configurations[index], plan, executor, concurrent));
                    }
                }
                const char* mode{"通常Add・主スレッドで追加"};
                if (concurrent) { mode = "AddConcurrent・ワーカーから同時追加"; }
                std::cout << "\n初期実体=" << count << " / 削除/周期=" << plan.cycles[0].removed.size()
                    << " / 追加/周期=" << plan.cycles[0].added.size() << " / 周期数=" << cycleCount
                    << " / ワーカー=" << workers << " / " << mode << '\n';
                for (std::size_t i{}; i < configurations.size(); ++i) { Print(configurations[i], samples[i]); }
            }
        }
    }
    std::cout << "全周期の実体・添字・削除/登録漏れ・更新結果を検証しました。チェックサム=" << churnChecksum << '\n';
    return 0;
}

}