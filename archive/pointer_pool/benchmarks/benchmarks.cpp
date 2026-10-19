#include "benchmarks.h"
#include "slab/slab_pool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
using Pool = slab::SlabPool;
constexpr Pool::PoolId itemId{1};
std::uint64_t checksum{};

/** @brief 全方式が共有する実体。既存の双方向リンクを模し、毎回同じデータを更新する。 */
struct Item {
    Item* previous{};
    Item* next{};
    std::uint64_t id{};
    std::uint64_t x{};
    std::uint64_t y{};
    std::uint64_t z{};
    std::array<std::uint64_t, 2> padding{};

    explicit Item(std::uint64_t value) noexcept : id{value} { Reset(); }

    void Reset() noexcept {
        x = id * 3 + 1;
        y = id * 7 + 2;
        z = id * 11 + 3;
    }

    void Update() noexcept {
        x += y + 3;
        y ^= z + 5;
        z += x + id;
    }

    [[nodiscard]]
    std::uint64_t Digest() const noexcept { return x + y * 3 + z * 7 + id; }
};
static_assert(sizeof(Item) == 64);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[ベンチマーク検証失敗] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/** @brief 計測用の実体は一度だけ new し、同じ実体を三つの走査方法で共有する。 */
struct Fixture {
    std::vector<std::unique_ptr<Item>> owners{};
    std::vector<Item*> order{};

    explicit Fixture(std::size_t count, bool shuffled) {
        owners.reserve(count);
        order.reserve(count);
        for (std::size_t i{}; i < count; ++i) {
            std::unique_ptr<Item> item{new (std::nothrow) Item{i}};
            Require(item != nullptr, "実体の new に失敗しました。");
            order.push_back(item.get());
            owners.push_back(std::move(item));
        }
        if (shuffled) {
            std::mt19937 random{20261012};
            std::shuffle(order.begin(), order.end(), random);
        }
        for (std::size_t i{}; i < count; ++i) {
            if (i != 0) { order[i]->previous = order[i - 1]; }
            if (i + 1 != count) { order[i]->next = order[i + 1]; }
        }
    }

    void Reset() const noexcept {
        for (const auto& item : owners) { item->Reset(); }
    }

    void Validate(int updates) const {
        for (const auto& item : owners) {
            Item expected{item->id};
            for (int i{}; i < updates; ++i) { expected.Update(); }
            Require(item->x == expected.x && item->y == expected.y && item->z == expected.z,
                "更新漏れ・二重更新、または演算結果の不一致があります。");
            checksum += item->Digest();
        }
    }
};

enum class Mode : std::size_t { Linked, Slab, Vector };

/** @brief 生成・登録・検証を含めず、更新対象の列挙と Update だけを行う。 */
void Traverse(Mode mode, const Fixture& fixture, const Pool& pool, int passes) {
    for (int pass{}; pass < passes; ++pass) {
        switch (mode) {
        case Mode::Linked:
            for (Item* item{fixture.order.front()}; item != nullptr; item = item->next) { item->Update(); }
            break;
        case Mode::Slab: {
            const std::size_t count{pool.Count(itemId)};
            std::size_t first{};
            while (first < count) {
                const auto pointers{pool.GetBatch<Item>(itemId, first)};
                for (auto& item : pointers) { item.Update(); }
                first += pointers.size();
            }
            break;
        }
        case Mode::Vector:
            for (Item* item : fixture.order) { item->Update(); }
            break;
        }
    }
}

[[nodiscard]]
double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2]; // このベンチマークは奇数回で測定する。
}

/** @brief 各方式の順番を交代し、同じ実体・同じ順序で反復更新する。 */
void Compare(Fixture& fixture, std::size_t slots, int repetitions, int passes) {
    Pool pool{};
    Require(pool.Register(itemId, Pool::Options{slots, slots * sizeof(void*)}), "ID と容量の登録に失敗しました。");
    for (Item* item : fixture.order) {
        Require(pool.Add(itemId, item) != Pool::InvalidIndex, "ポインタの登録に失敗しました。");
    }
    // 初期値復元と一周のウォームアップは計測外。キャッシュを強制排出する測定ではない。
    std::array<std::vector<double>, 3> samples{};
    for (int repeat{}; repeat < repetitions; ++repeat) {
        for (int offset{}; offset < 3; ++offset) {
            const auto index{static_cast<std::size_t>((repeat + offset) % 3)};
            const auto mode{static_cast<Mode>(index)};
            fixture.Reset();
            Traverse(mode, fixture, pool, 1);
            const auto start{Clock::now()};
            Traverse(mode, fixture, pool, passes);
            const double elapsed{std::chrono::duration<double, std::milli>{Clock::now() - start}.count()};
            samples[index].push_back(elapsed);
            fixture.Validate(passes + 1);
        }
    }
    const double linked{Median(samples[0])};
    const double slabTime{Median(samples[1])};
    const double vectorTime{Median(samples[2])};
    double ratio{};
    if (slabTime > 0) { ratio = linked / slabTime; }
    const auto stats{pool.GetStatistics()};
    std::cout << "  Slab容量=" << slots << " ポインタ（" << slots * sizeof(void*) << " byte） / Slab数=" << stats.slabs
        << " / ポインタ配列=" << stats.pointerBytes << " byte / 管理領域=" << stats.managementBytes << " byte\n"
        << "    リンクリスト=" << linked << " ms / ポインタSlab=" << slabTime
        << " ms / std::vector参考=" << vectorTime << " ms\n"
        << "    リンクリスト時間 / Slab時間=" << ratio << " 倍";
    if (ratio > 1.0) { std::cout << "（この条件ではSlabが速い）\n"; }
    else { std::cout << "（この条件ではSlabは速くない）\n"; }
}

/** @brief 測定環境と比較に含まれる処理を表示する。 */
void PrintEnvironment() {
    std::cout << "\n=== new の実体: 双方向リンクリストとポインタSlabの走査比較 ===\n";
#ifdef _MSC_VER
    std::cout << "コンパイラ: MSVC=" << _MSC_FULL_VER << " / C++=" << _MSVC_LANG << '\n';
    int registers[4]{};
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<unsigned int>(registers[0]) >= 0x80000004u) {
        std::array<int, 12> brand{};
        for (unsigned int part{}; part < 3; ++part) {
            __cpuid(brand.data() + part * 4, static_cast<int>(0x80000002u + part));
        }
        std::array<char, 49> name{};
        std::memcpy(name.data(), brand.data(), 48);
        std::cout << "CPU=" << name.data() << '\n';
    }
#endif
#ifdef NDEBUG
    std::cout << "Release / 最適化あり\n";
#else
    std::cout << "Debug / 動作確認用です。性能の判断には Release を使ってください。\n";
#endif
    std::cout << "実体サイズ=" << sizeof(Item) << " byte / 測定は1スレッド\n"
        << "三方式は同じ実体・同じ走査順・同じUpdateを使用。生成・登録・破棄時間は含みません。\n"
        << "前ポインタ・次ポインタは全方式で実体内に残します。生成順は物理アドレス順を保証しません。\n"
        << "Slabの先頭は64バイト境界。64バイトの追加単位は、x64ではポインタ8個です。\n"
        << "各試行は値を復元して1周準備し、その後の反復走査を測定。測定順を交代して中央値を表示します。\n"
        << "ランダム順は同じ実体の順序だけを変更。キャッシュミス率そのものは計測していません。\n"
        << "速度比は1倍より大きいほどSlabが速く、1倍未満ならリンクリストが速い結果です。\n";
}

}

int Run(const Settings& settings) {
    PrintEnvironment();
    checksum = 0;
    std::cout << std::fixed << std::setprecision(4);
    std::vector<std::size_t> counts{10000, 100000};
    int repetitions{3};
    int passes{16};
    if (settings.full) {
        counts.push_back(1000000);
        repetitions = 5;
    }
    std::cout << "各測定の周回数=" << passes << " / 測定回数=" << repetitions << '\n';
    for (std::size_t count : counts) {
        for (bool shuffled : {false, true}) {
            const char* order{"生成順"};
            if (shuffled) { order = "ランダム順"; }
            std::cout << "\n実体数=" << count << " / 全方式の走査順=" << order << '\n';
            Fixture fixture{count, shuffled};
            for (std::size_t slots : {4u, 8u, 64u, 256u}) { Compare(fixture, slots, repetitions, passes); }
        }
    }
    std::cout << "\nすべての更新結果を検証しました。チェックサム=" << checksum << '\n';
    return 0;
}

}
