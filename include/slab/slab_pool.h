/** @file slab_pool.h
 * @brief 数値ID別に外部オブジェクトのポインタを保持するSlab Pool。
 */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace slab {
// 標準ライブラリの名前はslab名前空間内でまとめて扱う。
using namespace std;
using std::byte; // Windows側のbyteと区別する。

/**
 * @brief Poolが状態を直接読むための共通基底。実体の所有権は利用側にある。
 * @details 派生クラスはpublicかつ非virtualで一意に継承し、stateを再定義しない。
 * 仮想関数は持たない。実体の解放には、生成した派生型のポインタを使う。
 */
struct Component {
    /**
     * @brief 0は除去対象、非0は利用可能。0にした後はNotifyStateChangedも呼ぶ。
     * @note 並列処理中はIDの走査権を持つ利用者だけが読み書きする。
     */
    int state{1};

protected:
    /** @brief 利用可能な状態で基底部分を初期化する。生成は派生クラスから行う。 */
    Component() noexcept = default;
    /** @brief 基底ポインタ経由のdeleteを防ぎ、外部所有側での派生型の解放に限定する。 */
    ~Component() noexcept = default;
};

/**
 * @brief ポインタを連続配置する非所有プール。実体のコピー・移動・解放は行わない。
 * @details Componentのstateを直接読み、0の要素を走査から除外する。
 * 全利用者の完了後に RemoveInactive で末尾ポインタを移して穴を詰める。
 * @pre Register・Clear・破棄は待機者を含む全利用者の停止中だけ行う。PoolはRangeより長く生存すること。
 * 同じIDは同時に一つのRangeだけが取得でき、使用中なら解放を待つ。別IDは並列利用できる。
 * Rangeを保持したまま同じIDのGetRange・Add・CommitAtFrameEndを呼ばないこと。
 * 複数IDを同時に保持する場合は全スレッドで取得順を統一し、循環待ちを作らないこと。
 * 実体を複数IDで共有したり、担当外の実体へ並列アクセスしたりしない。
 * 取得した参照・ポインタ・イテレータはRangeの解放前に使い終えること。
 */
class SlabPool {
    struct Group;
    struct Region;
    struct Chunk;
    struct Registry;
public:
    using PoolId = uint32_t;
    using Index = size_t;
    static constexpr Index InvalidIndex{numeric_limits<Index>::max()};

    /** @brief 容量と閾値はコンポーネント数で指定する。1個につきComponent*を1枠保持し、実際の確保量はOSページ単位へ切り上げる。 */
    struct Options {
        /** @brief 登録時にコミットする最低個数。追加コミット単位へ切り上げ、予約量を上限とする。0なら予約のみ。 */
        size_t initialCapacity{64};

        /** @brief 最初と不足時に予約する最低個数。1以上かつinitialCapacity以上を指定する。 */
        size_t reservedCapacity{65536};

        /** @brief 追加コミットの基準個数。1以上を指定し、ポインタのバイト数へ変換してページ単位に切り上げる。自動補充時は予約済みの残量が上限。 */
        size_t commitCount{512};

        /** @brief コミット時に新しいページへ書き込む。 */
        bool preTouch{true};

        /** @brief 0なら追加時に自動コミット。1以上ならフレーム末尾にこの空き数以下で補充。 */
        size_t freeThreshold{0};
    };

    /** @brief ID全体の走査権を持つ範囲。状態0を飛ばし、破棄時に走査権を解放する。 */
    template<class T>
    class Range;

    /**
     * @brief 共有インスタンスを取得する。利用側でインスタンスを生成する必要はない。
     * @details 初回呼び出し時に一度だけ生成し、プログラム終了時に破棄する。
     * 初期化はスレッドセーフ。各操作の並列利用条件は通常のインスタンスと同じ。
     */
    [[nodiscard]]
    static SlabPool& GetInstance() noexcept;

    /** @brief 独立したPoolも必要に応じて生成できる。共有インスタンスとは別の領域を持つ。 */
    SlabPool() noexcept;
    /** @brief 格納領域と走査権の二重所有を防ぐためコピーは禁止する。 */
    SlabPool(const SlabPool&) = delete;
    /** @brief 格納領域と走査権の二重所有を防ぐためコピー代入は禁止する。 */
    SlabPool& operator=(const SlabPool&) = delete;
    /** @brief 利用中のRangeとの寿命関係を固定するためPoolの移動は禁止する。 */
    SlabPool(SlabPool&&) = delete;
    /** @brief 利用中のRangeとの寿命関係を固定するためPoolの移動代入は禁止する。 */
    SlabPool& operator=(SlabPool&&) = delete;

    /** @brief ポインタ格納領域だけを解放する。参照先は利用側が所有する。 */
    ~SlabPool() noexcept;

    /**
     * @brief 数値IDのポインタ格納領域を登録する。例: Register(1)。
     * @pre IDとコンポーネント型の対応は利用側で決め、同じIDに異なる型を混在させない。
     * @details 型情報は保持しない。登録済みIDへの再登録は内容を変更せずfalseを返す。
     * @note map・listなど標準ライブラリの確保失敗は終了扱い。Slab用の仮想メモリの確保失敗はfalseを返す。
     */
    [[nodiscard]]
    bool Register(PoolId id, const Options& options = {}) noexcept;

    /**
     * @brief ポインタを1件末尾に追加し、格納添字を返す。実体はコピーしない。
     * @pre IDに対応する型の実体を渡すこと。入力の実体は他の処理で使用しない。
     * 同じ実体を複数IDへ登録しない。派生型の照合と重複検査は行わない。
     * 呼び出し側が同じIDのRangeを保持していないこと。
     * @details 同じIDが使用中なら解放を待ってから追加する。先着順は保証しない。
     * nullptr・未登録ID・容量不足や確保失敗はInvalidIndex。
     * 別IDの追加は並列に呼べる。状態0のポインタも追加できるが走査では除外する。
     */
    [[nodiscard]]
    Index Add(PoolId id, Component* pointer) noexcept;

    /**
     * @brief ID全体の走査権を取得する。使用中なら解放を待ち、未登録なら無効な範囲を返す。
     * @tparam T Componentをpublicかつ非virtualで一意に継承した型。constは可、volatileは不可。
     * @pre TはIDに対応する実体の型を指定すること。実行時の型照合は行わない。
     * 呼び出し側が同じIDのRangeを保持していないこと。
     * @details bool変換で取得成功を確認する。要素0件でも取得成功ならtrue。
     * Rangeの破棄・Resetまで同じIDを排他し、release/acquireで次の利用者へ引き継ぐ。
     * 範囲forを最後まで回しても、名前付きRangeが残っていれば走査権は保持する。
     * @note atomic::waitで待機して取得を再試行する。先着順や取得までの時間は保証しない。
     * スレッドは生成しない。Rangeはコピー不可・移動可。
     */
    template<class T>
    [[nodiscard]]
    Range<T> GetRange(PoolId id) noexcept;

    /**
     * @brief 読み取り専用の参照を返す範囲を取得する。取得条件と走査権の寿命は非const版と同じ。
     * @pre TはIDに対応するコンポーネント型であること。実行時の型照合は行わない。
     * @note const版も同じIDの走査権を取得し、他のRangeや追加処理を排他する。
     */
    template<class T>
    [[nodiscard]]
    Range<const T> GetRange(PoolId id) const noexcept;

    /**
     * @brief 状態変化をID単位で通知する。状態を0にした後に呼ぶ。複数スレッドから通知可。
     * @note フラグを立てるだけで、実体もポインタ配列も変更しない。重複通知は問題ない。
     * コンポーネントの数値を直接変更する場合も、この通知を忘れないこと。
     * この通知はstateの排他制御を行わない。並列処理中のstate変更は取得済みRange内で行う。
     */
    [[nodiscard]]
    bool NotifyStateChanged(PoolId id) noexcept;

    /**
     * @brief 全IDのうち変更フラグがあるものだけ、状態0を末尾との入れ替えで除去する。
     * @return 全IDの除去数の合計。未登録・全フラグなしなら0。
     * いずれかのIDが使用中なら、全IDの内容と変更フラグを維持してInvalidIndexを返す。
     * @details フラグがないIDのポインタ配列は走査しない。処理したIDのフラグは解除する。
     * @pre 待機中の操作も含め、全IDの走査・状態更新・通知・取得済み参照の利用が完了していること。
     * 自動で待機しない。
     * @note 順序と添字は変わる。実体の位置は変わらず、deleteも行わない。
     */
    [[nodiscard]]
    size_t RemoveInactive() noexcept;

    /**
     * @brief フレーム末尾で空きが閾値以下ならcommitCountをページ単位に切り上げた容量を追加。閾値0なら何もしない。
     * @details 同じIDが使用中なら解放を待つ。未登録ID・サイズ超過・確保失敗はfalse。
     * @pre 呼び出し側が同じIDのRangeを保持していないこと。
     */
    [[nodiscard]]
    bool CommitAtFrameEnd(PoolId id) noexcept;

    /**
     * @brief 全IDの登録・ポインタ・管理情報を消し、予約とコミットを含む全格納領域を解放する。
     * @pre 待機中の操作も含め、他スレッドからのPool操作を停止してから呼ぶ。
     * 並列呼び出しとの同期は行わない。
     * @return 全解放できればtrue。使用中のIDがあれば何も解放せずfalse。
     * @details 空Poolにも繰り返し呼べる。再利用時はRegisterからやり直す。
     * Registry自体はメンバーとして残し、mapを空の初期状態へ戻す。
     * @note 参照先のコンポーネント実体は解放しない。成功後に外側の所有者が解放する。
     */
    [[nodiscard]]
    bool Clear() noexcept;

private:
    /** @brief サイズ計算とポインタ差分で扱うバイト数の上限。 */
    static constexpr size_t MaximumBytes{static_cast<size_t>(PTRDIFF_MAX)};
    /** @brief 格納するポインタのサイズ。対象構成に合わせてコンパイル時に決まる。 */
    static constexpr size_t PointerBytes{sizeof(Component*)};
    /** @brief 初回登録時にmapのバケットを事前確保する想定ID数。 */
    static constexpr size_t InitialGroupCapacity{64};

    /** @brief OSの通常ページサイズを初回だけ取得し、全Poolで共有する。走査時は呼ばない。 */
    [[nodiscard]]
    static size_t GetPageBytes() noexcept;

    /**
     * @brief valueをunitの倍数へ切り上げる。既に倍数なら値を変えない。
     * @pre unitは正数で、valueとunitはMaximumBytes以下であること。
     * @param result 成功時の切り上げ結果。falseの場合は利用しないこと。
     * @return 切り上げ結果がMaximumBytes以下ならtrue。上限を超える場合はfalse。
     */
    [[nodiscard]]
    static bool RoundUp(size_t value, size_t unit, size_t& result) noexcept;

    /** @brief cpp側で定義するChunkのlistをたどる。Rangeは走査権の寿命中だけ参照する。 */
    using ChunkIterator = list<Chunk>::const_iterator;

    /** @brief 一つのコミット区間にあるComponent*配列を参照する。走査の終端は残り格納数で判断する。 */
    struct Slice {
        Component** data{}; ///< 参照開始位置。空のSliceではnullptr。
        size_t count{}; ///< 参照開始位置から区間末尾までの枠数。未使用枠も含む。
        ChunkIterator next{}; ///< 現在の区間を使い切った後に参照するChunk。
    };
    /**
     * @brief 取得済みの走査権と格納領域をRangeへ渡す内部情報。
     * @note View自体の破棄では走査権を解放しない。受け取ったRangeがReleaseViewを呼ぶ。
     */
    struct View {
        Slice first{}; ///< 最初のコミット区間。実際の走査数はcountで制限する。
        size_t count{}; ///< 取得時点の格納数。状態0の要素も含む。
        Group* group{}; ///< 走査権を取得したGroup。nullptrなら取得失敗。
    };

    /**
     * @brief 数値IDから登録済みGroupを検索する。未登録ならnullptr。
     * @note 検索だけを行い、Groupの作成や走査権の取得は行わない。
     */
    [[nodiscard]]
    Group* Find(PoolId id) const noexcept;

    /**
     * @brief Group全体で最低個数を格納できるよう仮想アドレス領域を予約する。
     * @param minimumCapacity 既存の予約分を含む、必要なポインタの総数。
     * @pre Groupの初期化中、または呼び出し元が走査権を保持していること。
     * @return 予約済み容量で足りるか、追加予約に成功すればtrue。サイズ超過・予約失敗はfalse。
     * @note コミットは行わない。既存領域を移動せず、必要ならRegionを追加する。
     */
    [[nodiscard]]
    static bool ReserveGroup(Group& group, size_t minimumCapacity) noexcept;

    /**
     * @brief Group全体で最低個数を書き込めるよう、不足分をコミットする。
     * @param minimumCapacity 既存のコミット分を含む、必要なポインタの総数。
     * @pre Groupの初期化中、または呼び出し元が走査権を保持していること。
     * @return 必要容量を準備できればtrue。サイズ超過・予約やコミットの失敗はfalse。
     * @details 必要なら予約も追加する。ポインタ配列の寿命を開始し、preTouchもここで行う。
     * @note 途中で失敗しても、それまで成功した予約・コミットはGroupに保持する。
     */
    [[nodiscard]]
    static bool CommitGroup(Group& group, size_t minimumCapacity) noexcept;

    /**
     * @brief ID全体の走査権を待機して取得し、Range用の参照情報を返す。
     * @return 成功時はGroup・先頭区間・状態0を含む格納数。未登録なら空のView。
     * @pre 呼び出し側が同じIDのRangeを保持していないこと。
     * @details 使用中ならAcquireで待機する。要素0件でも取得に成功する。型照合は行わない。
     * @note constでもGroupの走査権は更新する。取得後はRangeを通じてReleaseViewで解放する。
     */
    [[nodiscard]]
    View AcquireView(PoolId id) const noexcept;

    /**
     * @brief Viewを空にしてから、そのIDの走査権を解放する。空のViewにも呼べる。
     * @pre このViewから得た参照・ポインタ・イテレータの利用が完了していること。
     * @note 格納領域とコンポーネント実体は解放しない。
     */
    static void ReleaseView(View& view) noexcept;

    /**
     * @brief 有効なChunkのポインタ配列と次の区間をSliceとして返す。
     * @pre chunkは終端ではない有効なイテレータであり、走査権を保持していること。
     * @note 終端判定は呼び出し側が格納数で行う。枠数には未使用枠も含むため、残り格納数を上限に走査する。
     */
    [[nodiscard]]
    static Slice ReadChunk(ChunkIterator chunk) noexcept;

    /**
     * @brief Group内の通算添字を含むChunkを検索し、その枠から区間末尾までを返す。
     * @pre 呼び出し元がGroupの走査権を保持していること。
     * @return 対象枠を先頭とするSlice。コミット済み容量の範囲外なら空のSlice。
     * @note 追加先の未使用枠も検索対象。実体の状態は調べず、Chunk一覧を順に検索する。
     */
    [[nodiscard]]
    static Slice Locate(const Group& group, Index index) noexcept;

    // 内部型の定義。直接メンバーとして保持するため、必要な型情報をここへまとめる。
    /** @brief ID単位の格納領域と走査権。型情報は保持せず、コンポーネント実体は所有しない。 */
    struct Group {
        size_t commitStep{}; ///< commitCount個分のポインタ領域をページ単位に切り上げたバイト数。
        size_t reservationStep{}; ///< reservedCapacity個分のポインタ領域をページ単位に切り上げたバイト数。
        size_t freeThreshold{}; ///< フレーム末尾で補充する空きポインタ数。0なら追加時に自動補充。
        bool preTouch{}; ///< コミット時に新しいページへ書き込むか。
        size_t count{}; ///< 状態0を含む格納済みポインタ数。
        size_t capacity{}; ///< コミット済み領域に格納できるポインタ数。
        size_t reservedBytes{}; ///< 全Regionの予約バイト数。未コミット部分も含む。
        atomic<bool> mayContainInactive{}; ///< 状態0が含まれる可能性を表す変更通知フラグ。
        atomic<bool> inUse{}; ///< RangeまたはPool操作がこのIDを使用している間はtrue。待機と通知にも使う。
        list<Region> regions{}; ///< 予約領域を所有し、各Regionの破棄でVirtualFreeする。
        list<Chunk> chunks{}; ///< コミット区間の管理情報を所有する。

        /** @brief Region・Chunkの定義があるcpp側で空の管理領域を初期化する。 */
        Group() noexcept;
        /** @brief 管理情報と仮想領域を解放する。実体の解放は外部の所有者が行う。 */
        ~Group() noexcept;

        /** @brief Clear・RemoveInactive用に走査権を一度だけ取得する。使用中なら待たずに失敗する。 */
        [[nodiscard]]
        bool TryAcquire() noexcept;

        /**
         * @brief IDの走査権を取得する。使用中ならatomic::waitで待機して再試行する。
         * @pre 同じIDの走査権を自分が保持していないこと。待機中もGroupが生存すること。
         * @note 先着順・取得までの時間は保証しない。所有者の解放を待つ排他処理であり、ロックフリーではない。
         */
        void Acquire() noexcept;
        /** @brief 走査権を解放し、更新内容を次の利用者へ引き継いで待機者へ通知する。 */
        void Release() noexcept;

        /** @brief Add・CommitAtFrameEnd用の待機式排他ガード。破棄時に走査権を解放する。 */
        struct Access {
            Group* owned{}; ///< 走査権を取得したGroup。未取得ならnullptr。
            /** @brief 走査権を待機して取得する。nullptrの場合だけ未取得のままにする。 */
            explicit Access(Group* group) noexcept;
            /** @brief 走査権の二重解放を防ぐためコピーは禁止する。 */
            Access(const Access&) = delete;
            /** @brief 走査権の二重解放を防ぐためコピー代入は禁止する。 */
            Access& operator=(const Access&) = delete;
            /** @brief 取得に成功した場合だけ走査権を解放する。 */
            ~Access() noexcept;
            /** @brief 走査権を取得できていればtrue。 */
            [[nodiscard]]
            explicit operator bool() const noexcept;
        };
    };

    /** @brief 数値IDからGroupを検索する管理部。SlabPoolと同じ寿命で保持する。 */
    struct Registry {
        unordered_map<PoolId, Group> groups{}; ///< 登録済みIDと、その格納領域・走査権を所有する。
    };

    /** @brief constのGetRangeでも走査権を更新するためmutable。登録情報の変更は非const操作だけが行う。 */
    mutable Registry registry_{};
};

// -----------------------------------------------------------------------------
// テンプレート実装（Range・イテレータを含む）
// -----------------------------------------------------------------------------

/**
 * @brief ID全体の走査権を所有する移動専用の範囲。状態0を選択時に飛ばす。
 * @tparam T IDに対応するコンポーネント型。const付きなら読み取り専用の参照を返す。
 * @pre 同じRange自体を複数スレッドから同時に使わない。参照はReset・破棄前に使い終える。
 * @details constの範囲も同じIDを排他する。コピー不可で、移動時に走査権を引き継ぐ。
 * 範囲forの終了やbreakでは解放せず、このRangeのReset・破棄まで走査権を保持する。
 * @note 実体は所有しない。boolは取得成功、emptyは利用可能要素の有無を表す。
 */
template<class T>
class SlabPool::Range {
    // 継承とconst指定の検査はコンパイル時だけ行い、実行時の型情報は持たない。
    static_assert(!is_volatile_v<T>, "走査する型にvolatileは指定できません。");
    static_assert(is_convertible_v<T*, const Component*> &&
        requires(Component* pointer) { static_cast<T*>(pointer); },
        "Componentをpublicかつ非virtualで一意に継承してください。");
public:
    /**
     * @brief ポインタ配列を順にたどり、stateが0の実体を飛ばす前方イテレータ。
     * @details コピーするのは走査位置だけで、IDの走査権は所有しない。
     * 生成元RangeのReset・破棄後は、このイテレータも取得済み参照も利用できない。
     * @note 状態の判定は走査開始時と位置を進める時に行う。
     * 複数のコピーで同じ列をたどる間は、stateの0/非0を変更しないこと。
     * 分類タグはコンパイル時の型情報であり、操作の実装や実行時データは追加しない。
     */
    class Iterator {
    public:
        using iterator_category = forward_iterator_tag; ///< iterator_traitsを使う従来の標準アルゴリズム向けの分類。
        using iterator_concept = forward_iterator_tag; ///< C++20のiterator concept判定・ranges向けに優先される分類。
        using value_type = remove_const_t<T>; ///< 参照先の値型。
        using difference_type = ptrdiff_t; ///< 差分を表す符号付き型。
        using pointer = T*; ///< const指定を維持したポインタ型。
        using reference = T&; ///< const指定を維持した参照型。

        /** @brief 終端を表すイテレータを作る。 */
        Iterator() noexcept = default;
        /** @brief 現在選択している実体への参照を返す。@pre 終端ではないこと。 */
        [[nodiscard]]
        reference operator*() const noexcept { return *operator->(); }
        /** @brief 現在選択している実体へのポインタを返す。@pre 終端ではないこと。 */
        [[nodiscard]]
        pointer operator->() const noexcept { return static_cast<T*>(*slice_.data); }
        /** @brief 次の利用可能な実体、または終端へ進む。@pre 終端ではないこと。 */
        Iterator& operator++() noexcept { Advance(); SkipInactive(); return *this; }
        /** @brief 進む前の位置を返して、次の利用可能な実体へ進む。@pre 終端ではないこと。 */
        Iterator operator++(int) noexcept { auto previous{*this}; ++*this; return previous; }
        /** @brief 同じRange内の位置、または終端との一致を判定する。 */
        [[nodiscard]]
        friend bool operator==(const Iterator& left, const Iterator& right) noexcept {
            return left.slice_.data == right.slice_.data && left.remaining_ == right.remaining_;
        }
    private:
        friend class Range;
        /** @brief Rangeの格納領域を借り、最初の利用可能な実体を選択する。 */
        Iterator(View view) noexcept : slice_{view.first}, remaining_{view.count} {
            if (remaining_ == 0) { slice_ = {}; }
            SkipInactive();
        }
        /** @brief 状態を判定せず1枠進む。格納数を使い切れば終了し、残っている場合だけ次の区間へ移る。 */
        void Advance() noexcept {
            --remaining_;
            if (remaining_ == 0) { slice_ = {}; }
            else {
                --slice_.count;
                if (slice_.count == 0) { slice_ = SlabPool::ReadChunk(slice_.next); }
                else { ++slice_.data; }
            }
        }
        /** @brief 状態0の枠を飛ばし、利用可能な実体か終端まで進む。 */
        void SkipInactive() noexcept {
            while (remaining_ != 0 && (*slice_.data)->state == 0) { Advance(); }
        }
        Slice slice_{}; ///< 現在位置からコミット区間の末尾までの借用範囲。
        size_t remaining_{}; ///< 現在位置を含む未走査の格納数。状態0も含む。
    };

    /** @brief 走査権を持たない無効なRangeを作る。 */
    Range() noexcept = default;
    /** @brief 走査権の二重所有を防ぐためコピーは禁止する。 */
    Range(const Range&) = delete;
    /** @brief 走査権の二重所有を防ぐためコピー代入は禁止する。 */
    Range& operator=(const Range&) = delete;
    /** @brief 走査権を引き継ぎ、移動元を無効にする。 */
    Range(Range&& other) noexcept : view_{exchange(other.view_, {})} {}
    /** @brief 自身の走査権を解放してから引き継ぐ。自己代入なら何もしない。 */
    Range& operator=(Range&& other) noexcept {
        if (this != &other) { Reset(); view_ = exchange(other.view_, {}); }
        return *this;
    }
    /** @brief 走査権を解放する。コンポーネント実体は解放しない。 */
    ~Range() noexcept { Reset(); }

    /** @brief 取得成功ならtrue。空のIDでも成功し、走査権を保持する。 */
    [[nodiscard]]
    explicit operator bool() const noexcept { return view_.group != nullptr; }
    /**
     * @brief 走査権を明示解放して無効にする。繰り返し呼んでもよい。
     * @pre このRangeの参照・ポインタ・イテレータを使い終えていること。
     */
    void Reset() noexcept { SlabPool::ReleaseView(view_); }
    /** @brief 最初の利用可能な実体を指すイテレータを返す。存在しない場合はendと一致する。 */
    [[nodiscard]]
    Iterator begin() const noexcept { return Iterator{view_}; }
    /** @brief 終端を返す。終端の参照取得やインクリメントはできない。 */
    [[nodiscard]]
    Iterator end() const noexcept { return {}; }
    /** @brief 状態0を含む格納数を走査せず返す。無効なRangeなら0。 */
    [[nodiscard]]
    size_t StoredCount() const noexcept { return view_.count; }
    /** @brief 状態0を除いた個数を全件走査して数える。無効なRangeなら0。 */
    [[nodiscard]]
    size_t size() const noexcept {
        size_t result{};
        for (auto iterator{begin()}; iterator != end(); ++iterator) { ++result; }
        return result;
    }
    /** @brief 利用可能な実体がなければtrue。取得の成否はbool変換で別に確認する。 */
    [[nodiscard]]
    bool empty() const noexcept { return begin() == end(); }
private:
    friend class SlabPool;
    /** @brief Poolから取得済みの走査権と格納領域を受け取る。 */
    explicit Range(View view) noexcept : view_{view} {}
    View view_{}; ///< 所有する走査権と、その寿命中に参照できる格納領域。
};

template<class T>
SlabPool::Range<T> SlabPool::GetRange(PoolId id) noexcept {
    return Range<T>{AcquireView(id)};
}

template<class T>
SlabPool::Range<const T> SlabPool::GetRange(PoolId id) const noexcept {
    return Range<const T>{AcquireView(id)};
}
}
