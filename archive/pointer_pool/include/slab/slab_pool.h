/**
 * @file slab_pool.h
 * @brief 外部で生成した実体のポインタを、連続した Slab にまとめる非所有コンテナ。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace slab {

/**
 * @brief 数値 ID ごとに、走査用のポインタだけを密に保持する。
 * @details 実体は new や既存アロケータで生成する。Pool は実体を移動・破棄しない。
 * ポインタは Slab 内で連続し、領域の先頭を 64 バイト境界に揃える。異なる Slab の隣接は保証しない。
 * @note 一括登録は AddBatch、並列一括登録は BeginConcurrentAppend / AddBatchConcurrent /
 * EndConcurrentAppend を使う。登録と走査は利用側でフェーズを分ける。
 * 並列更新中は読み取り API だけを使用する。
 * GetBatch が返した範囲の利用中は追加・削除・予約・Clear・Pool 破棄を行わない。
 * @warning 同じ実体の重複登録は利用側で防ぐ。実体の寿命と Update のデータ競合も利用側の責任。
 */
class SlabPool {
    struct Block;
    struct Group;

public:
    /** @brief 型との対応は利用側で決める。0 も有効。 */
    using PoolId = std::uint32_t;

    /** @brief 密な配列の添字。RemoveAt による末尾交換で対象が変わるため、固定ハンドルではない。 */
    using Index = std::size_t;

    /** @brief Add の失敗を表す添字。 */
    static constexpr Index InvalidIndex{std::numeric_limits<Index>::max()};

    /** @brief 各 Slab のポインタ領域の先頭を揃える境界。対象 CPU の自動判定は行わない。 */
    static constexpr std::size_t SlabAlignment{64};

    /**
     * @brief 既存のポインタ列を T& として読む、非所有の型付き範囲。
     * @note begin / end / size / empty / operator[] を提供する。
     * 実装は slab_pool_impl.h。ポインタ配列の複製や追加確保は行わない。
     */
    template<class T>
    class Batch;

    /** @brief Slab 境界を越えて全件または指定区間を T& で走査する、確保不要のビュー。 */
    template<class T>
    class Range;


    /** @brief 初期容量はポインタ個数、追加容量はポインタ領域のバイト数で指定する。 */
    struct Options {
        /** @brief 登録時に一つの Slab として確保する個数。0 なら遅延確保する。 */
        std::size_t initialCapacity{64};

        /** @brief 追加 Slab のバイト数。既定は 64 バイト。sizeof(void*) の正の倍数を指定する。 */
        std::size_t growthBytes{64};
    };

    /** @brief 削除結果。実体は破棄しない。 */
    struct Removal {
        /** @brief 削除したポインタ。失敗時は nullptr。 */
        void* removed{};

        /** @brief 削除位置へ移った末尾のポインタ。交換がなければ nullptr。 */
        void* moved{};
    };

    /** @brief 実体のメモリを含まない、走査用コンテナの集計。 */
    struct Statistics {
        /** @brief 登録 ID 数。 */
        std::size_t groups{};

        /** @brief 保持する Slab 数。空の Slab も含む。 */
        std::size_t slabs{};

        /** @brief 登録されたポインタ数。 */
        std::size_t count{};

        /** @brief 未使用分を含むポインタ格納容量。 */
        std::size_t capacity{};

        /** @brief ポインタ配列に要求したバイト数。 */
        std::size_t pointerBytes{};

        /** @brief Pool・ID・Slab ヘッダー・索引に要求したバイト数。 */
        std::size_t managementBytes{};
    };

    SlabPool() noexcept;
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;
    SlabPool(SlabPool&&) = delete;
    SlabPool& operator=(SlabPool&&) = delete;

    /** @brief ポインタ格納領域だけを解放する。登録された実体は破棄しない。 */
    ~SlabPool() noexcept;

    /** @brief ID を既定容量で登録する。同じ設定の再登録は成功する。 */
    [[nodiscard]]
    bool Register(PoolId id) noexcept;

    /** @brief ID と容量を登録する。不正値・設定衝突・確保失敗は false。 */
    [[nodiscard]]
    bool Register(PoolId id, const Options& options) noexcept;

    /**
     * @brief 総容量を予約する。不足分を一つの Slab にまとめて追加する。
     * @note 新しい Slab は少なくとも growthBytes / sizeof(void*) 個分を持つ。
     */
    [[nodiscard]]
    bool Reserve(PoolId id, std::size_t capacity) noexcept;

    /**
     * @brief 非所有ポインタを末尾に追加して添字を返す。実体の構築は行わない。
     * @return 未登録・nullptr・確保失敗は InvalidIndex。
     * @pre 同じ実体を重複登録しないこと。重複検査は行わない。
     * @note ロックを取得しない。同じ Pool への同時追加には AddConcurrent を使う。
     */
    [[nodiscard]]
    Index Add(PoolId id, void* object) noexcept;

    /**
     * @brief 外部のポインタ列を一括コピーする。実体はコピー・移動しない。
     * @details 不足容量は一つの追加 Slab にまとめて確保し、既存のポインタ列は移動しない。
     * @return 最初の添字。未登録・nullptr 混入・確保失敗・並列登録中は InvalidIndex。
     * 空の入力は現在の末尾を返す。失敗時は登録数・既存要素を変更しない。
     * @pre 入力範囲は呼び出し終了まで不変で、コピー先と重ならないこと。同じ実体の重複登録は禁止。
     */
    [[nodiscard]]
    Index AddBatch(PoolId id, std::span<void* const> objects) noexcept;

    /**
     * @brief 追加上限分の容量を一括準備し、その ID の並列登録フェーズを開始する。
     * @details 必要ならここでメモリを確保する。この操作はロックフリーではない。
     * @pre 全ワーカー停止中に呼ぶ。全 ID の準備を完了してからワーカーを開始する。
     * @return 未登録・開始済み・桁あふれ・確保失敗は false。最大追加数 0 も有効。
     */
    [[nodiscard]]
    bool BeginConcurrentAppend(PoolId id, std::size_t maximumAdditionalCount) noexcept;

    /**
     * @brief 準備済み領域へ、ポインタ列を並列に一括コピーする。
     * @details CAS で区間を一度に予約し、互いに重ならない区間へコピーする。
     * メモリ確保・mutex・実体のコピーは行わない。Slab 境界を越える入力にも対応する。
     * 対象環境で size_t の atomic が常にロックフリーであることをコンパイル時に確認する。
     * @return 最初の添字。未開始・未登録・nullptr 混入・追加上限超過は InvalidIndex。
     * 失敗時は区間を消費しない。空入力は予約末尾を返す。呼び出し間の順序は不定。
     * @pre 全 ID の構成と入力を固定し、入力範囲とコピー先を重ねない。
     * 同じ実体を重複登録しないこと。並列実行中はこの API だけを呼ぶ。
     * 予約末尾はコピー完了を意味しない。全ワーカー完了の同期後に EndConcurrentAppend を呼ぶ。
     */
    [[nodiscard]]
    Index AddBatchConcurrent(PoolId id, std::span<void* const> objects) noexcept;

    /**
     * @brief 並列登録結果を走査可能にする。未登録・未開始は false。
     * @pre 全 AddBatchConcurrent の完了を join / barrier 等で同期してから呼ぶ。
     * この関数自身はワーカー完了を待たない。終了後から Count / GetRange 等を使用できる。
     */
    [[nodiscard]]
    bool EndConcurrentAppend(PoolId id) noexcept;

    /**
     * @brief 複数スレッドから安全に末尾へ追加する。実体は利用側で構築してから渡す。
     * @details 同じ ID への追加・Slab 拡張を ID ごとの mutex で直列化する。
     * 異なる ID は別の mutex を使う。通常の Add はロックを取得しない。
     * @pre 全 ID の Register・Reserve を先に済ませ、同時実行中は AddConcurrent だけを使用する。
     * Count・At・GetBatch・GetStatistics・Add・削除・Clear・破棄も、全追加の完了後に行う。
     * @return 未登録・nullptr・確保失敗は InvalidIndex。それ以外は、この追加に割り当てた添字。
     * @note 登録順はロックの取得順で決まり、スレッド間の順序は不定。ロックフリーではない。
     * 重複ポインタの検査や実体へのアクセスの同期は行わない。
     */
    [[nodiscard]]
    Index AddConcurrent(PoolId id, void* object) noexcept;

    /**
     * @brief 指定位置を削除し、末尾のポインタで穴を埋める。順序は維持しない。
     * @note 添字を実体に記録する場合、moved の添字を削除位置に更新する。
     * @return 不正な ID・添字は空の結果。検索は Slab 索引の二分探索。
     */
    [[nodiscard]]
    Removal RemoveAt(PoolId id, Index index) noexcept;

    /** @brief 登録だけをすべて解除する。実体と予約済み Slab は保持する。 */
    void Clear(PoolId id) noexcept;

    /** @brief 登録数。未登録 ID は 0。 */
    [[nodiscard]]
    std::size_t Count(PoolId id) const noexcept;

    /** @brief 添字の実体を取得する。範囲外・未登録は nullptr。 */
    [[nodiscard]]
    void* At(PoolId id, Index index) const noexcept;

    /**
     * @brief first から同じ Slab 内で連続する登録済みポインタ列を取得する。
     * @param maximumCount 返す最大個数。ワーカーの担当区間を越えないように制限できる。
     * @return Slab 境界までの span。範囲外・未登録・最大個数 0 は空。
     * @note span のポインタ値は書き換え不可。実体への書き込みは利用側で同期する。
     * 各 span を走査し、first に span.size() を加算して次へ進む。
     */
    [[nodiscard]]
    std::span<void* const> GetBatch(PoolId id, Index first,
        std::size_t maximumCount = std::numeric_limits<std::size_t>::max()) const noexcept;

    /**
     * @brief 同じ Slab 内の実体を T& で列挙する。auto& で直接メンバにアクセスできる。
     * @pre id の各ポインタを、登録時の型に合う T* へ戻せること。実行時の型検査は行わない。
     * @note 読み取り専用の実体は GetBatch<const T> で取得する。
     * 範囲の寿命・Slab 境界・最大個数の契約は、型なしの GetBatch と同じ。
     */
    template<class T>
    [[nodiscard]]
    Batch<T> GetBatch(PoolId id, Index first,
        std::size_t maximumCount = std::numeric_limits<std::size_t>::max()) const noexcept;

    /**
     * @brief Slab をまたいで T& を列挙する。範囲 for で全件を処理できる。
     * @details first と maximumCount で並列処理の担当範囲を指定できる。
     * Slab 境界だけで次の範囲を取得し、要素ごとの索引検索・追加確保は行わない。
     * @pre ID と型の対応は利用側で保証する。走査中は追加・削除・Clear・実体の破棄を行わない。
     * @return 未登録・範囲外・並列登録フェーズ中は空。const T による読み取りにも対応。
     */
    template<class T>
    [[nodiscard]]
    Range<T> GetRange(PoolId id, Index first = 0,
        std::size_t maximumCount = std::numeric_limits<std::size_t>::max()) const noexcept;


    /** @brief 全 ID の集計。実体・CRT/OS の管理領域は含まない。 */
    [[nodiscard]]
    Statistics GetStatistics() const noexcept;

private:
    [[nodiscard]]
    Group* Find(PoolId id) const noexcept;

    Group* groups_{};
};

}

#include "slab_pool_impl.h"
