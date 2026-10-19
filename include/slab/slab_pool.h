/** @file slab_pool.h
 * @brief 数値ID別に外部オブジェクトのポインタを保持するSlab Pool。
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <type_traits>
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
    Component() noexcept = default;
    /** @brief 基底ポインタ経由のdeleteを防ぎ、外部所有側での派生型の解放に限定する。 */
    ~Component() noexcept = default;
};

/**
 * @brief ポインタを連続配置する非所有プール。実体のコピー・移動・解放は行わない。
 * @details Componentのstateを直接読み、0の要素を走査から除外する。
 * 全利用者の完了後に RemoveInactive で末尾ポインタを移して穴を詰める。
 * @pre Register・Clear・破棄は全利用者の停止中だけ行う。PoolはRangeより長く生存すること。
 * 同じIDは同時に一つのRangeだけが取得でき、別IDは並列利用できる。
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

    /** @brief 個数はポインタ数、バイト数はポインタ格納領域のサイズ。 */
    struct Options {
        /** @brief 登録時にコミットする最低個数。0なら予約のみ。 */
        size_t initialCapacity{64};

        /** @brief 最初と不足時に予約する最低個数。 */
        size_t reservedCapacity{65536};

        /** @brief 追加コミット量。OSページ単位へ切り上げる。 */
        size_t commitBytes{64 * 1024};

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
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;
    SlabPool(SlabPool&&) = delete;
    SlabPool& operator=(SlabPool&&) = delete;

    /** @brief ポインタ格納領域だけを解放する。参照先は利用側が所有する。 */
    ~SlabPool() noexcept;

    /**
     * @brief Componentを継承した型を数値IDへ登録する。例: Register<Enemy>(1)。
     * @tparam T Componentをpublicかつ非virtualで一意に継承した非const/volatile型。
     * @details 共通基底のstateが0なら除去対象、非0なら利用可能。状態読み取り関数の指定は不要。
     * @note mapを含むID管理部の確保失敗は終了扱い。Slab領域・Slab管理情報の確保失敗はfalseを返す。
     */
    template<class T>
    [[nodiscard]]
    bool Register(PoolId id, const Options& options = {}) noexcept;

    /**
     * @brief ポインタを1件末尾に追加し、格納添字を返す。実体はコピーしない。
     * @pre IDにRegisterした型の実体を渡すこと。入力の実体は他の処理で使用しない。
     * 同じ実体を複数IDへ登録しない。派生型の照合と重複検査は行わない。
     * @details nullptr・未登録ID・容量不足・同じIDの使用中はInvalidIndex。
     * 別IDの追加は並列に呼べる。状態0のポインタも追加できるが走査では除外する。
     */
    [[nodiscard]]
    Index Add(PoolId id, Component* pointer) noexcept;

    /**
     * @brief ID全体の走査権を一度だけ試行して取得する。未登録・型違い・使用中なら無効な範囲。
     * @details bool変換で取得成功を確認する。要素0件でも取得成功ならtrue。
     * Rangeの破棄・Resetまで同じIDを排他し、release/acquireで次の利用者へ引き継ぐ。
     * 範囲forを最後まで回しても、名前付きRangeが残っていれば走査権は保持する。
     * @note スレッドは生成しない。待機や再試行も行わない。Rangeはコピー不可・移動可。
     */
    template<class T>
    [[nodiscard]]
    Range<T> GetRange(PoolId id) noexcept;

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
     * @pre 全IDの走査・状態更新・通知・取得済み参照の利用が完了していること。自動で待機しない。
     * @note 順序と添字は変わる。実体の位置は変わらず、deleteも行わない。
     */
    [[nodiscard]]
    size_t RemoveInactive() noexcept;

    /** @brief 全利用者の完了後、空きが閾値以下ならcommitBytesの1単位を追加。閾値0なら何もしない。 */
    [[nodiscard]]
    bool CommitAtFrameEnd(PoolId id) noexcept;

    /**
     * @brief 全IDの登録・ポインタ・管理情報を消し、予約とコミットを含む全格納領域を解放する。
     * @pre 他スレッドからのPool操作を停止してから呼ぶ。並列呼び出しとの同期は行わない。
     * @return 全解放できればtrue。使用中のIDがあれば何も解放せずfalse。
     * @details 空Poolにも繰り返し呼べる。再利用時はRegisterからやり直す。
     * @note 参照先のコンポーネント実体は解放しない。成功後に外側の所有者が解放する。
     */
    [[nodiscard]]
    bool Clear() noexcept;

private:
    /** @brief 一つのコミット区間にあるComponent*配列を参照する。 */
    struct Slice {
        Component** data{};
        size_t count{};
        const Chunk* next{};
    };
    struct View {
        Slice first{};
        size_t count{};
        Group* group{};
    };

    template<class T>
    [[nodiscard]]
    static const void* TypeTag() noexcept;
    [[nodiscard]]
    bool RegisterRaw(PoolId id, const Options& options, const void* tag) noexcept;
    [[nodiscard]]
    Group* Find(PoolId id) const noexcept;
    [[nodiscard]]
    static bool ReserveGroup(Group& group, size_t minimumCapacity) noexcept;
    [[nodiscard]]
    static bool CommitGroup(Group& group, size_t minimumCapacity) noexcept;
    [[nodiscard]]
    View AcquireView(PoolId id, const void* tag) const noexcept;
    static void ReleaseView(View& view) noexcept;
    [[nodiscard]]
    static Slice ReadChunk(const Chunk* chunk) noexcept;
    [[nodiscard]]
    static Slice Locate(const Group& group, Index index) noexcept;

    /** @brief ID管理mapを持つ。初回登録で生成し、Clearでバケット領域も含めて破棄する。 */
    unique_ptr<Registry> registry_{};
};

// -----------------------------------------------------------------------------
// テンプレート実装（Range・イテレータを含む）
// -----------------------------------------------------------------------------

template<class T>
const void* SlabPool::TypeTag() noexcept {
    static_assert(is_same_v<T, remove_cv_t<T>>,
        "登録する型は非const/volatileにしてください。");
    static_assert(is_convertible_v<T*, Component*> &&
        requires(Component* pointer) { static_cast<T*>(pointer); },
        "Componentをpublicかつ非virtualで一意に継承してください。");
    static unsigned char tag{};
    return &tag;
}

template<class T>
bool SlabPool::Register(PoolId id, const Options& options) noexcept {
    return RegisterRaw(id, options, TypeTag<T>());
}

/**
 * @brief ID全体の走査権を所有する移動専用の範囲。状態0を選択時に飛ばす。
 * @tparam T IDに登録したコンポーネント型。const付きなら読み取り専用の参照を返す。
 * @pre 同じRange自体を複数スレッドから同時に使わない。参照はReset・破棄前に使い終える。
 * @details constの範囲も同じIDを排他する。コピー不可で、移動時に走査権を引き継ぐ。
 * 範囲forの終了やbreakでは解放せず、このRangeのReset・破棄まで走査権を保持する。
 * @note 実体は所有しない。boolは取得成功、emptyは利用可能要素の有無を表す。
 */
template<class T>
class SlabPool::Range {
public:
    /**
     * @brief ポインタ配列を順にたどり、stateが0の実体を飛ばす前方イテレータ。
     * @details コピーするのは走査位置だけで、IDの走査権は所有しない。
     * 生成元RangeのReset・破棄後は、このイテレータも取得済み参照も利用できない。
     * @note 状態の判定は走査開始時と位置を進める時に行う。
     */
    class Iterator {
    public:
        using iterator_category = forward_iterator_tag; ///< 標準アルゴリズム向けの分類。
        using iterator_concept = forward_iterator_tag; ///< C++20のイテレータ分類。
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
        /** @brief 状態を判定せず1枠進む。コミット区間の末尾なら次の区間へ移る。 */
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
    return Range<T>{AcquireView(id, TypeTag<remove_const_t<T>>())};
}

template<class T>
SlabPool::Range<const T> SlabPool::GetRange(PoolId id) const noexcept {
    return Range<const T>{AcquireView(id, TypeTag<remove_const_t<T>>())};
}
}
