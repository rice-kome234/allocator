/** @file benchmarks.h
 * @brief コンソール設定とポインタ走査の時間比較。
 */
#pragma once
namespace benchmarks {
void PrepareConsole() noexcept;
[[nodiscard]]
bool RunPointers(bool full = false);
}
