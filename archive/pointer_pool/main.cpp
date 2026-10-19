#include "slab/slab_pool.h"
#include "benchmarks/benchmarks.h"
#include "benchmarks/parallel_executor.h"
#include "benchmarks/parallel_tasks.h"
#include "benchmarks/interactive_test.h"
#include "tests/tests.h"

#include <array>
#include <iostream>
#include <memory>
#include <new>

// 実行時引数は不要。使用する条件をここで切り替えて再ビルドする。
#define SLAB_RUN_TESTS
#define SLAB_RUN_BULK_BENCHMARK // 一括確保・ポインタコピー・並列走査を比較する。
// #define SLAB_TESTS_ONLY
// #define SLAB_RUN_BENCHMARKS
// #define SLAB_RUN_GROWTH_BENCHMARK    // 事前確保なしの追加と、再確保時の負荷も比較する。
// #define SLAB_RUN_PARALLEL_GROWTH_BENCHMARK // 1・10・14ワーカーの同時追加も比較する。
// #define SLAB_RUN_CHURN_BENCHMARK    // ランダム削除・再追加後の走査も測定する。
// #define SLAB_CHURN_GROWING       // 削除10%・追加20%で件数と容量を増やす。
// #define SLAB_RUN_INTERACTIVE_TEST
// #define SLAB_FULL_BENCHMARK         // 最大 100 万個で比較する。

namespace {




// 実体は普通の new/delete で管理する。Slab に入るのは、この実体を指すポインタだけ。
struct Enemy {
    static constexpr slab::SlabPool::PoolId Id{1};
    int health{};
    float position{};
    float speed{2.0f};
    std::size_t updateCount{};

    explicit Enemy(int hp) noexcept : health{hp} {}

    /** @brief 自分だけを更新する。同じ個体の同時更新は行わない。 */
    void Update(float deltaSeconds) noexcept {
        position += speed * deltaSeconds;
        ++updateCount;
    }
};

struct Bullet {
    static constexpr slab::SlabPool::PoolId Id{2};
    float speed{};
    float position{};
    std::size_t updateCount{};

    explicit Bullet(float value) noexcept : speed{value} {}

    /** @brief 共有データに触れず、自分の位置を更新する。 */
    void Update(float deltaSeconds) noexcept {
        position += speed * deltaSeconds;
        ++updateCount;
    }
};

#ifndef SLAB_TESTS_ONLY
/** @brief 既存の実体をそのまま使い、そのポインタだけをID別に一括コピーする。 */
int ShowUsage() {
    std::array<std::unique_ptr<Enemy>, 6> enemies{};
    std::array<void*, 6> pointers{};
    for (std::size_t i{}; i < enemies.size(); ++i) {
        enemies[i].reset(new (std::nothrow) Enemy{100}); // 既存のアロケータへ置き換える場所。
        if (!enemies[i]) { return 1; }
        pointers[i] = enemies[i].get();
    }
    std::unique_ptr<Bullet> bullet{new (std::nothrow) Bullet{8.0f}};
    if (!bullet) { return 1; }

    slab::SlabPool pool{};
    // 初期0個。一括登録時に必要なポインタ数をまとめて確保する。
    if (!pool.Register(Enemy::Id, {0, 64}) || !pool.Register(Bullet::Id, {0, 64})) { return 1; }
    if (pool.AddBatch(Enemy::Id, pointers) == slab::SlabPool::InvalidIndex
        || pool.Add(Bullet::Id, bullet.get()) == slab::SlabPool::InvalidIndex) { return 1; }

    // 一つのforで同じIDの全件を走査する。Slabが複数あっても書き方は変わらない。
    for (auto& enemy : pool.GetRange<Enemy>(Enemy::Id)) { enemy.Update(0.25f); }
    for (auto& item : pool.GetRange<Bullet>(Bullet::Id)) { item.Update(0.25f); }
    std::cout << "\n=== ID別のポインタ一括登録と範囲for ===\n"
        << "Enemy=" << pool.Count(Enemy::Id) << "個 / Bullet=" << pool.Count(Bullet::Id)
        << "個 / 実体はコピーせず、元のEnemyの位置=" << enemies[0]->position << '\n';

    // 実体の所有権は利用側。解除してから元の確保方法で破棄する。
    pool.Clear(Enemy::Id);
    pool.Clear(Bullet::Id);
    for (auto& enemy : enemies) { enemy.reset(); }
    bullet.reset();
    return 0;
}

/** @brief スレッド管理を補助クラスに任せ、mainには登録と実体の処理を残す。 */
int ShowConcurrentUsage() {
    constexpr std::size_t objectCount{1000};
    std::array<std::unique_ptr<Enemy>, objectCount> enemies{};
    std::array<void*, objectCount> pointers{};
    for (std::size_t i{}; i < objectCount; ++i) {
        enemies[i].reset(new (std::nothrow) Enemy{100});
        if (!enemies[i]) { return 1; }
        pointers[i] = enemies[i].get();
    }
    slab::SlabPool pool{};
    if (!pool.Register(Enemy::Id, {0, 64})) { return 1; }
    benchmarks::ParallelTasks tasks{14};

    // 容量準備 → ポインタ区間の並列コピー → 完了待ちを一つの呼び出しで行う。
    if (!tasks.CopyPointers(pool, Enemy::Id, pointers, 256)) { return 1; }
    // 保存領域の担当は固定しない。呼び出すたびに現在の登録数を均等に分担する。
    tasks.ForEach(pool, Enemy::Id, &Enemy::Update, 0.25f);
    for (const auto& enemy : pool.GetRange<const Enemy>(Enemy::Id)) {
        if (enemy.updateCount != 1 || enemy.position != 0.5f) { return 1; }
    }
    std::cout << "並列使用例: 14ワーカー / Enemy=" << pool.Count(Enemy::Id)
        << "個 / ポインタ一括登録後、全件を1回Updateしました。\n";
    pool.Clear(Enemy::Id);
    for (auto& enemy : enemies) { enemy.reset(); }
    return 0;
}
#endif
#if defined(SLAB_RUN_INTERACTIVE_TEST) && !defined(SLAB_TESTS_ONLY)
/** @brief コンポーネント固有の操作だけを定義し、対話処理へ渡す。 */
int RunInteractiveExample() {
    slab::SlabPool pool{};
    slab::SlabPool::Options options{};
    options.initialCapacity = 64; // 初期確保は従来どおり個数で指定する。
    options.growthBytes = 64;     // 追加 Slab は 64 バイト（x64 でポインタ 8 個分）。
    if (!pool.Register(Enemy::Id, options) || !pool.Register(Bullet::Id, options)) {
        std::cerr << "[確保エラー] ポインタ用 Slab を準備できませんでした。\n";
        return 1;
    }

    // 1. 実体の確保・構築と、ポインタの登録。既存アロケータへ置き換える場所。
    const auto addEnemies{[&](std::size_t count) {
        for (std::size_t i{}; i < count; ++i) {
            Enemy* object{new (std::nothrow) Enemy{100}};
            if (object == nullptr || pool.Add(Enemy::Id, object) == slab::SlabPool::InvalidIndex) {
                delete object;
                std::cerr << "[確保エラー] Enemy の生成または登録に失敗しました。\n";
                break;
            }
        }
    }};
    const auto addBullets{[&](std::size_t count) {
        for (std::size_t i{}; i < count; ++i) {
            Bullet* object{new (std::nothrow) Bullet{8.0f}};
            if (object == nullptr || pool.Add(Bullet::Id, object) == slab::SlabPool::InvalidIndex) {
                delete object;
                std::cerr << "[確保エラー] Bullet の生成または登録に失敗しました。\n";
                break;
            }
        }
    }};

    // 2. 登録解除してから delete。RemoveAt は実体を破棄しない。
    const auto clearComponents{[&] {
        while (pool.Count(Enemy::Id) != 0) {
            delete static_cast<Enemy*>(pool.RemoveAt(Enemy::Id, pool.Count(Enemy::Id) - 1).removed);
        }
        while (pool.Count(Bullet::Id) != 0) {
            delete static_cast<Bullet*>(pool.RemoveAt(Bullet::Id, pool.Count(Bullet::Id) - 1).removed);
        }
    }};

    {
        benchmarks::ParallelExecutor executor{};
        benchmarks::InteractiveActions actions{};
        actions.addEnemies = addEnemies;
        actions.addBullets = addBullets;
        actions.removeEnemy = [&] { delete static_cast<Enemy*>(pool.RemoveAt(Enemy::Id, 0).removed); };
        actions.removeBullet = [&] { delete static_cast<Bullet*>(pool.RemoveAt(Bullet::Id, 0).removed); };
        actions.clear = clearComponents;

        // 3. 走査して呼び出す関数を渡す。各 ForEach は全対象の更新完了後に戻る。
        actions.update = [&](float deltaSeconds) {
            executor.ForEach(pool, Enemy::Id, &Enemy::Update, deltaSeconds);
            executor.ForEach(pool, Bullet::Id, &Bullet::Update, deltaSeconds);
        };

        // 4. 表示したい実体の値を返す。コンソールへの整形表示は benchmarks 側で行う。
        actions.inspectEnemy = [&] {
            benchmarks::ComponentState state{pool.Count(Enemy::Id)};
            if (const auto* enemy{static_cast<Enemy*>(pool.At(Enemy::Id, 0))}; enemy != nullptr) {
                state.position = enemy->position;
                state.updateCount = enemy->updateCount;
            }
            return state;
        };
        actions.inspectBullet = [&] {
            benchmarks::ComponentState state{pool.Count(Bullet::Id)};
            if (const auto* bullet{static_cast<Bullet*>(pool.At(Bullet::Id, 0))}; bullet != nullptr) {
                state.position = bullet->position;
                state.updateCount = bullet->updateCount;
            }
            return state;
        };
        benchmarks::RunInteractive(pool, actions);
    } // executor の破棄で全ワーカーを停止する。

    // 5. 終了時も、実体の破棄は main 側で行う。
    clearComponents();
    std::cout << "終了処理: 全ワーカー停止 / 登録数=" << pool.GetStatistics().count
        << " / 実体をすべて delete しました。\n";
    return 0;
}
#endif

}

int main() {
    benchmarks::PrepareConsole();
#if defined(SLAB_RUN_TESTS) || defined(SLAB_TESTS_ONLY)
    if (const int result{tests::RunAll()}; result != 0) { return result; }
#endif
#ifdef SLAB_TESTS_ONLY
    return 0;
#else
    if (ShowUsage() != 0) {
        std::cerr << "[使用例エラー] 生成・登録・解除に失敗しました。\n";
        return 1;
    }
    if (ShowConcurrentUsage() != 0) {
        std::cerr << "[使用例エラー] 同時追加または結果確認に失敗しました。\n";
        return 1;
    }
#if defined(SLAB_RUN_BULK_BENCHMARK) || defined(SLAB_RUN_BENCHMARKS) || defined(SLAB_RUN_GROWTH_BENCHMARK) || defined(SLAB_RUN_PARALLEL_GROWTH_BENCHMARK) || defined(SLAB_RUN_CHURN_BENCHMARK)
    benchmarks::Settings settings{};
#ifdef SLAB_CHURN_GROWING
    settings.churnGrowing = true;
#endif
#ifdef SLAB_FULL_BENCHMARK
    settings.full = true;
#endif
#ifdef SLAB_RUN_BULK_BENCHMARK
    if (const int result{benchmarks::RunBulk(settings)}; result != 0) { return result; }
#endif
#ifdef SLAB_RUN_BENCHMARKS
    if (const int result{benchmarks::Run(settings)}; result != 0) { return result; }
#endif
#ifdef SLAB_RUN_GROWTH_BENCHMARK
    if (const int result{benchmarks::RunGrowth(settings)}; result != 0) { return result; }
#endif
#ifdef SLAB_RUN_PARALLEL_GROWTH_BENCHMARK
    if (const int result{benchmarks::RunParallelGrowth(settings)}; result != 0) { return result; }
#endif
#ifdef SLAB_RUN_CHURN_BENCHMARK
    if (const int result{benchmarks::RunChurn(settings)}; result != 0) { return result; }
#endif
#endif
#ifdef SLAB_RUN_INTERACTIVE_TEST
    return RunInteractiveExample();
#else
    return 0;
#endif
#endif
}
