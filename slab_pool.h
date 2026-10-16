/** @file slab_pool.h
 * @brief 数値ID別に外部オブジェクトのポインタを保持するSlab Pool。
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace slab {

/**
 * @brief ポインタを連続配置する非所有プール。実体のコピー・移動・解放は行わない。
 * @details コンポーネントの状態を読む関数を登録し、0の要素を走査から除外する。
 * 全利用者の完了後に RemoveInactive で末尾ポインタを移して穴を詰める。
 * @pre 登録・予約・コミット・追加・除去・Clear・破棄は走査と重ねない。
 * 並列追加は Begin/End の間だけ。並列更新は一つの ParallelRange を共有する。
 * 状態の読み書きは利用側でatomic等により同期し、参照先を除去前に解放しない。
 */
class SlabPool {
    struct Group;
    struct Region;
    struct Chunk;
public:
    using PoolId = std::uint32_t;
    using Index = std::size_t;
    static constexpr Index InvalidIndex{std::numeric_limits<Index>::max()};

    /** @brief 個数はポインタ数、バイト数はポインタ格納領域のサイズ。 */
    struct Options {
        /** @brief 登録時にコミットする最低個数。0なら予約のみ。 */
        std::size_t initialCapacity{64};

        /** @brief 最初と不足時に予約する最低個数。 */
        std::size_t reservedCapacity{65536};

        /** @brief 追加コミット量。OSページ単位へ切り上げる。 */
        std::size_t commitBytes{64 * 1024};

        /** @brief コミット時に新しいページへ書き込む。 */
        bool preTouch{true};

        /** @brief 0なら追加時に自動コミット。1以上ならフレーム末尾にこの空き数以下で補充。 */
        std::size_t freeThreshold{0};
    };

    /** @brief 件数は状態0の未除去ポインタも含む。メモリ値は実体を含まない。 */
    struct Statistics {
        std::size_t count{};
        std::size_t capacity{};
        std::size_t reservedCapacity{};
        std::size_t reservedBytes{};
        std::size_t committedBytes{};
        std::size_t regions{};
        std::size_t chunks{};
        std::size_t pointerSize{};
        std::size_t commitQuantum{};
        bool mayContainInactive{}; ///< 状態変化の通知があり、フレーム末尾の削除走査が必要。
    };

    /** @brief 状態0を飛ばし、参照先をT&として返す専用イテレータの範囲。 */
    template<class T>
    class Range;

    /** @brief 複数ワーカーへ重複しない区間を配る。一つのインスタンスを共有する。 */
    template<class T>
    class ParallelRange;

    SlabPool() noexcept = default;
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;
    SlabPool(SlabPool&&) = delete;
    SlabPool& operator=(SlabPool&&) = delete;

    /** @brief ポインタ格納領域だけを解放する。参照先は利用側が所有する。 */
    ~SlabPool() noexcept;

    /**
     * @brief 型と状態読み取り関数を数値IDへ登録する。
     * @tparam ReadState const T&から整数を返すnoexcept関数、またはconstメンバ関数。
     * @details 0は除去対象、非0は利用可能。例: Register<Enemy, &Enemy::GetState>(1)。
     * 状態読み取り関数は並列呼び出しに対応すること。型自体はコピー不可でも登録できる。
     */
    template<class T, auto ReadState>
    [[nodiscard]]
    bool Register(PoolId id, const Options& options = {}) noexcept;

    /** @brief ポインタ格納用の仮想アドレスを予約する。 */
    [[nodiscard]]
    bool Reserve(PoolId id, std::size_t minimumCapacity) noexcept;

    /** @brief 指定数以上を格納できるようコミットする。予約不足なら別領域を追加。 */
    [[nodiscard]]
    bool Commit(PoolId id, std::size_t minimumCapacity) noexcept;

    /** @brief 全利用者の完了後、空きが閾値以下ならcommitBytesの1単位を追加。閾値0なら何もしない。 */
    [[nodiscard]]
    bool CommitAtFrameEnd(PoolId id) noexcept;

    /** @brief ポインタを末尾に追加する。nullptr・型違い・容量不足はInvalidIndex。 */
    template<class T>
    [[nodiscard]]
    Index Add(PoolId id, T* pointer) noexcept;

    /**
     * @brief ポインタ値を一括追加する。実体はコピーしない。戻り値は先頭添字。
     * @pre 同じ実体を更新用の範囲へ重複登録しない。Poolは重複検査を行わない。
     * @note 入力にnullptrがあれば全体を拒否。状態0のポインタは登録できるが走査では除外する。
     */
    template<class T>
    [[nodiscard]]
    Index AddBatch(PoolId id, std::span<T* const> pointers) noexcept;

    /** @brief 単一呼び出し元で容量を準備して並列追加を開始する。 */
    [[nodiscard]]
    bool BeginConcurrentAppend(PoolId id, std::size_t maximumAdditionalCount) noexcept;

    /** @brief Begin後、準備済み領域へ重複しない区間を確保してポインタを追加する。 */
    template<class T>
    [[nodiscard]]
    Index AddBatchConcurrent(PoolId id, std::span<T* const> pointers) noexcept;

    /** @brief 全追加ワーカーの終了を外部で待ってから件数を公開する。この関数自体は待たない。 */
    [[nodiscard]]
    bool EndConcurrentAppend(PoolId id) noexcept;

    /**
     * @brief 状態変化をID単位で通知する。状態を0にした後に呼ぶ。複数スレッドから通知可。
     * @note フラグを立てるだけで、実体もポインタ配列も変更しない。重複通知は問題ない。
     * コンポーネントの数値を直接変更する場合も、この通知を忘れないこと。
     */
    [[nodiscard]]
    bool NotifyStateChanged(PoolId id) noexcept;

    /**
     * @brief 全IDのうち変更フラグがあるものだけ、状態0を末尾との入れ替えで除去する。
     * @return 全IDの除去数の合計。未登録・全フラグなしなら0。
     * いずれかのIDが並列追加中なら、全IDを変更せずInvalidIndexを返す。
     * @details フラグがないIDのポインタ配列は走査しない。処理したIDのフラグは解除する。
     * @pre 全IDの走査・状態更新・通知・取得済み参照の利用が完了していること。自動で待機しない。
     * @note 順序と添字は変わる。実体の位置は変わらず、deleteも行わない。
     */
    [[nodiscard]]
    std::size_t RemoveInactive() noexcept;

    /** @brief 全ポインタを除去して容量を保持する。参照先を解放しない。 */
    [[nodiscard]]
    bool Clear(PoolId id) noexcept;

    /** @brief 格納数。状態0もフレーム末尾の除去までは数える。未登録・並列追加中は0。 */
    [[nodiscard]]
    std::size_t Count(PoolId id) const noexcept;

    [[nodiscard]]
    Statistics GetStatistics(PoolId id) const noexcept;

    /**
     * @brief 状態0を飛ばす範囲。first/maximumCountは未除去ポインタも含む添字と個数。
     * @note 複数スレッドから取得可。ただし同じ範囲を取得すれば同じ実体も返る。
     * 取得・選択済みの実体の処理は、状態0になっても中断しない。
     */
    template<class T>
    [[nodiscard]]
    Range<T> GetRange(PoolId id, Index first = 0, std::size_t maximumCount = InvalidIndex) noexcept;

    template<class T>
    [[nodiscard]]
    Range<const T> GetRange(PoolId id, Index first = 0, std::size_t maximumCount = InvalidIndex) const noexcept;

    /** @brief 共有する取得器を作る。TryAcquireで各ワーカーに一度だけ区間を配る。batchSizeは1以上。 */
    template<class T>
    [[nodiscard]]
    ParallelRange<T> GetParallelRange(PoolId id, std::size_t batchSize = 256) noexcept;

    template<class T>
    [[nodiscard]]
    ParallelRange<const T> GetParallelRange(PoolId id, std::size_t batchSize = 256) const noexcept;

    /** @brief 利用可能な参照先。状態0・型違い・範囲外等はnullptr。除去後の添字は保持しないこと。 */
    template<class T>
    [[nodiscard]]
    T* At(PoolId id, Index index) noexcept;

    template<class T>
    [[nodiscard]]
    const T* At(PoolId id, Index index) const noexcept;

private:
    using StateFunction = bool (*)(const void*) noexcept;
    using StoreFunction = void (*)(void**, const void*, std::size_t, std::size_t) noexcept;

    /** @brief 一つのコミット区間にあるvoid*配列を参照する。 */
    struct Slice {
        void** data{};
        std::size_t count{};
        const Chunk* next{};
    };
    struct View {
        Slice first{};
        std::size_t count{};
        StateFunction active{};
        const Group* group{};
    };

    template<class T>
    [[nodiscard]]
    static const void* TypeTag() noexcept;
    template<class T>
    [[nodiscard]]
    Index AddInput(PoolId id, std::span<T* const> pointers, bool concurrent) noexcept;
    [[nodiscard]]
    bool RegisterRaw(PoolId id, const Options& options, const void* tag, StateFunction active) noexcept;
    [[nodiscard]]
    Index AddRaw(PoolId id, const void* tag, const void* source, std::size_t count,
        StoreFunction store, bool concurrent) noexcept;
    [[nodiscard]]
    Group* Find(PoolId id) const noexcept;
    [[nodiscard]]
    static bool ReserveGroup(Group& group, std::size_t minimumCapacity) noexcept;
    [[nodiscard]]
    static bool CommitGroup(Group& group, std::size_t minimumCapacity) noexcept;
    static void ReleaseGroup(Group* group) noexcept;
    [[nodiscard]]
    View GetView(PoolId id, const void* tag, Index first, std::size_t maximumCount) const noexcept;
    [[nodiscard]]
    static Slice ReadChunk(const Chunk* chunk) noexcept;
    [[nodiscard]]
    static Slice Locate(const Group& group, Index index) noexcept;

    Group* groups_{};
};
}
#include "slab_pool_impl.h"
