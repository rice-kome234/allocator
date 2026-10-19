/**
 * @file interactive_test.h
 * @brief キー入力と表示を受け持つ、対話テスト用の補助処理。
 */
#pragma once

#include <cstddef>
#include <functional>

namespace slab { class SlabPool; }

namespace benchmarks {

/** @brief main が返すコンポーネントの観察結果。 */
struct ComponentState {
    std::size_t count{};
    float position{};
    std::size_t updateCount{};
};

/**
 * @brief コンポーネント固有の処理を main から渡す。
 * @note すべてのコールバックを設定すること。呼び出しは主スレッドで直列に行う。
 * update は全対象の更新が終わってから戻ること。RunInteractive はコールバックを保持しない。
 */
struct InteractiveActions {
    std::function<void(std::size_t)> addEnemies{};
    std::function<void(std::size_t)> addBullets{};
    std::function<void()> removeEnemy{};
    std::function<void()> removeBullet{};
    std::function<void()> clear{};
    std::function<void(float)> update{};
    std::function<ComponentState()> inspectEnemy{};
    std::function<ComponentState()> inspectBullet{};
};

/** @brief 日本語コンソール出力と、Debug 時の CRT リーク検査を準備する。 */
void PrepareConsole();

/**
 * @brief 入力・停止切替・フレーム待機・表示を行う。
 * @note 戻った時点で更新は完了している。実体の終了時破棄は main が行う。
 */
void RunInteractive(const slab::SlabPool& pool, const InteractiveActions& actions);

}
