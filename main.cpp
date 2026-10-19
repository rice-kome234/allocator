/** @file main.cpp
 * @brief 外部で生成したEnemyを登録し、走査・除去・解放する最小例。
 */
#include "slab/slab_pool.h"
#include <Windows.h>
#include <array>
#include <iostream>
#include <memory>
#include <new>

using namespace std;

/** @brief stateが0なら除去対象。実体の所有者はmain側。 */
struct Enemy : slab::Component {
    float x{};

    explicit Enemy(float position) noexcept : x{position} {}
    void Update(float delta) noexcept { x += delta; }
};

int main() {
    (void)SetConsoleOutputCP(CP_UTF8);
    constexpr slab::SlabPool::PoolId enemyId{1};
    auto& pool{slab::SlabPool::GetInstance()}; // 自分で生成せず、共有Poolへの参照を取得する。

    // 1. 数値IDに型を登録する。容量はポインタの個数、commitBytesはバイト数。
    if (!pool.Register<Enemy>(enemyId, {
        .initialCapacity = 4,
        .reservedCapacity = 1024,
        .commitBytes = 4096
    })) {
        cerr << "型の登録に失敗しました。\n";
        return 1;
    }

    // 2. 実体は外側で生成・所有し、Poolにはポインタだけを追加する。
    array<unique_ptr<Enemy>, 3> owners{};
    for (size_t i{}; i < owners.size(); ++i) {
        owners[i].reset(new (nothrow) Enemy{static_cast<float>(i)});
        slab::Component* component{owners[i].get()}; // 共通基底のポインタで渡せる。
        if (pool.Add(enemyId, component) == slab::SlabPool::InvalidIndex) {
            cerr << "Enemyの生成または追加に失敗しました。\n";
            return 1;
        }
    }

    // 3. ID全体の走査権を取得する。別IDは別スレッドで同時に処理できる。
    {
        auto enemies{pool.GetRange<Enemy>(enemyId)};
        if (!enemies) {
            cerr << "Enemyの走査権を取得できませんでした。\n";
            return 1;
        }
        size_t index{};
        for (auto& enemy : enemies) {
            enemy.Update(0.5f);
            cout << "Enemyの位置: " << enemy.x << '\n';
            // 4. 走査中に2個目を除去対象にする。以後の走査では状態0を飛ばす。
            if (index == 1) { enemy.state = 0; }
            ++index;
        }

        // 状態を変更したIDに通知する。
        if (!pool.NotifyStateChanged(enemyId)) {
            cerr << "状態変更の通知に失敗しました。\n";
            return 1;
        }
        cout << "利用可能なEnemy: " << enemies.size() << "個\n";
    } // Rangeの破棄で走査権を解放。取得した参照もここまでで使い終える。

    // 5. 全IDの処理が終わったフレーム末尾に、状態0のポインタだけを除去する。
    const auto removed{pool.RemoveInactive()};
    if (removed == slab::SlabPool::InvalidIndex) {
        cerr << "ポインタの除去に失敗しました。\n";
        return 1;
    }
    for (auto& owner : owners) {
        if (owner && owner->state == 0) { owner.reset(); } // 実体の解放は外側で行う。
    }
    cout << "除去: " << removed << "個\n";

    // 6. 全IDの登録とPoolの格納メモリを解放してから、残る実体を外側で解放する。
    if (!pool.Clear()) {
        cerr << "Poolのクリアに失敗しました。\n";
        return 1;
    }
    for (auto& owner : owners) { owner.reset(); }
    return 0;
}
