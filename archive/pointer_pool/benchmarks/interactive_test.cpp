/**
 * @file interactive_test.cpp
 * @brief 対話テストの入力、時間管理、状態表示を実装する。
 */
#include "interactive_test.h"
#include "slab/slab_pool.h"

#include <chrono>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include <conio.h>
#endif
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace benchmarks {
namespace {

/** @brief 入力リダイレクトでは1文字ずつ処理し、EOFで終了する。 */
[[nodiscard]]
int ReadCommand(bool keyboardInput) {
#ifdef _WIN32
    if (keyboardInput) {
        if (_kbhit() == 0) { return 0; }
        const int key{_getch()};
        if (key == 0 || key == 224) { (void)_getch(); return 0; }
        return key;
    }
#else
    (void)keyboardInput;
#endif
    const int key{std::cin.get()};
    if (key == std::char_traits<char>::eof()) { return 'q'; }
    return key;
}

/** @brief コンポーネントの実装を知らず、main が返した値だけを表示する。 */
void PrintStatus(const slab::SlabPool& pool, const InteractiveActions& actions,
    std::size_t frame, bool paused) {
    const char* state{"更新中"};
    if (paused) { state = "一時停止"; }
    const auto stats{pool.GetStatistics()};
    const auto enemy{actions.inspectEnemy()};
    const auto bullet{actions.inspectBullet()};
    std::cout << '[' << state << "] フレーム=" << frame
        << " / Enemy=" << enemy.count << " / Bullet=" << bullet.count
        << " / Slab=" << stats.slabs << " / 登録=" << stats.count << " / 容量=" << stats.capacity;
    if (enemy.count != 0) {
        std::cout << " / 先頭Enemy位置=" << enemy.position << " (更新=" << enemy.updateCount << ')';
    }
    if (bullet.count != 0) {
        std::cout << " / 先頭Bullet位置=" << bullet.position << " (更新=" << bullet.updateCount << ')';
    }
    std::cout << '\n' << std::flush;
}

}

void PrepareConsole() {
#ifdef _WIN32
    (void)SetConsoleOutputCP(CP_UTF8);
#endif
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
}

void RunInteractive(const slab::SlabPool& pool, const InteractiveActions& actions) {
    bool keyboardInput{};
#ifdef _WIN32
    DWORD inputMode{};
    keyboardInput = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &inputMode) != 0;
#endif
    std::cout << "\n=== new の実体 + ポインタ Slab の並列 Update ===\n"
        << "実体は new/delete / ポインタ用 Pool=1 個 / 更新方法はmainから指定\n"
        << "E: Enemy +1   B: Bullet +1   1: Enemy +64   2: Bullet +64\n"
        << "D: 先頭Enemyを削除   F: 先頭Bulletを削除（末尾ポインタで穴を埋める）\n"
        << "C: 全削除   Space: 一時停止・再開   S: 状態表示   Q / Esc: 終了\n"
        << "Enter は不要です。追加・削除は全 Update の終了後に行います。\n";
    if (!keyboardInput) {
        std::cout << "入力リダイレクト: 1 文字で 1 フレーム。'.' は操作なし、EOF で終了します。\n";
    }

    using Clock = std::chrono::steady_clock;
    auto lastDisplay{Clock::now()};
    std::size_t frame{};
    bool paused{}, running{true};
    PrintStatus(pool, actions, frame, paused);
    while (running) {
        const auto frameStart{Clock::now()};
        bool changed{true};
        switch (ReadCommand(keyboardInput)) {
        case 'e': case 'E': actions.addEnemies(1); break;
        case 'b': case 'B': actions.addBullets(1); break;
        case '1': actions.addEnemies(64); break;
        case '2': actions.addBullets(64); break;
        case 'd': case 'D': actions.removeEnemy(); break;
        case 'f': case 'F': actions.removeBullet(); break;
        case 'c': case 'C': actions.clear(); break;
        case ' ': paused = !paused; break;
        case 's': case 'S': break;
        case 'q': case 'Q': case 27: running = false; break;
        default: changed = false; break;
        }
        if (!running) { break; }
        if (!paused) {
            actions.update(1.0f / 60.0f);
            ++frame;
        }
        if (changed || Clock::now() - lastDisplay >= std::chrono::seconds{1}) {
            PrintStatus(pool, actions, frame, paused);
            lastDisplay = Clock::now();
        }
        if (keyboardInput) { std::this_thread::sleep_until(frameStart + std::chrono::microseconds{16667}); }
    }
    PrintStatus(pool, actions, frame, paused);
}

}
