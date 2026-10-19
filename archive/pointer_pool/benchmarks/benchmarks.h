#pragma once

namespace benchmarks {

/** @brief 実行条件は main.cpp で指定する。実行時引数は使わない。 */
struct Settings {
    /** @brief true は最大 100 万個、false は 1 万個・10 万個。 */
    bool full{};

    /** @brief true は変更比較で10%削除・20%追加。false は10%ずつの同数補充。 */
    bool churnGrowing{};
};

/** @brief 同じ new の実体を、同じ順序・同じ演算で走査して日本語で比較する。 */
int Run(const Settings& settings);

/** @brief 事前確保の有無・追加全体・拡張を伴う追加の時間と再配置対象数を比較する。 */
int RunGrowth(const Settings& settings);

/** @brief 1・10・14ワーカーで通常追加・同時追加・共有時の競合を比較する。 */
int RunParallelGrowth(const Settings& settings);

/** @brief ランダム削除・末尾追加・走査を、1・10・14ワーカーで比較する。 */
int RunChurn(const Settings& settings);

/** @brief 固定Slabへの一括コピー・mutex追加・CAS追加と、1/10/14ワーカーの走査を測る。 */
int RunBulk(const Settings& settings);

}
