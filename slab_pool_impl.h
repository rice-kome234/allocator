/** @file slab_pool_impl.h
 * @brief 型付きポインタ登録、状態を確認するイテレータ、並列区間取得。
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <iterator>
#include <type_traits>

namespace slab {

template<class T>
const void* SlabPool::TypeTag() noexcept {
    static unsigned char tag{};
    return &tag;
}

template<class T, auto ReadState>
bool SlabPool::Register(PoolId id, const Options& options) noexcept {
    static_assert(std::is_class_v<T> && std::is_same_v<T, std::remove_cv_t<T>>,
        "const/volatileでないコンポーネント型を登録してください。");
    static_assert(std::is_nothrow_invocable_v<decltype(ReadState), const T&>,
        "状態読み取りはconst T&から呼べるnoexcept関数が必要です。");
    static_assert(std::is_integral_v<std::remove_cvref_t<std::invoke_result_t<decltype(ReadState), const T&>>>,
        "状態読み取り関数は整数を返してください。0が除去対象です。");
    return RegisterRaw(id, options, TypeTag<T>(), [](const void* pointer) noexcept {
        return std::invoke(ReadState, *static_cast<const T*>(pointer)) != 0;
    });
}

template<class T>
SlabPool::Index SlabPool::AddInput(PoolId id, std::span<T* const> pointers, bool concurrent) noexcept {
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>, "登録するポインタは非const型にしてください。");
    for (auto* pointer : pointers) {
        if (pointer == nullptr) { return InvalidIndex; }
    }
    return AddRaw(id, TypeTag<T>(), &pointers, pointers.size(),
        [](void** destination, const void* input, std::size_t first, std::size_t count) noexcept {
            const auto source{*static_cast<const std::span<T* const>*>(input)};
            for (std::size_t i{}; i < count; ++i) { destination[i] = source[first + i]; }
        }, concurrent);
}

template<class T>
SlabPool::Index SlabPool::Add(PoolId id, T* pointer) noexcept {
    return AddBatch<T>(id, {&pointer, 1});
}
template<class T>
SlabPool::Index SlabPool::AddBatch(PoolId id, std::span<T* const> pointers) noexcept {
    return AddInput<T>(id, pointers, false);
}
template<class T>
SlabPool::Index SlabPool::AddBatchConcurrent(PoolId id, std::span<T* const> pointers) noexcept {
    return AddInput<T>(id, pointers, true);
}

/**
 * @brief 自分専用のイテレータを持つ非所有ビュー。状態0を選択時に飛ばす。
 * @pre ビュー・取得した参照の利用中は追加・除去・解放をしない。状態のatomic更新は併用可。
 */
template<class T>
class SlabPool::Range {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        using value_type = std::remove_const_t<T>;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        Iterator() noexcept = default;
        [[nodiscard]]
        reference operator*() const noexcept { return *operator->(); }
        [[nodiscard]]
        pointer operator->() const noexcept { return static_cast<T*>(*slice_.data); }
        Iterator& operator++() noexcept { Advance(); SkipInactive(); return *this; }
        Iterator operator++(int) noexcept { auto previous{*this}; ++*this; return previous; }
        [[nodiscard]]
        friend bool operator==(const Iterator& left, const Iterator& right) noexcept {
            return left.slice_.data == right.slice_.data && left.remaining_ == right.remaining_;
        }
    private:
        friend class Range;
        Iterator(View view) noexcept : slice_{view.first}, remaining_{view.count}, active_{view.active} {
            SkipInactive();
        }
        void Advance() noexcept {
            --remaining_;
            if (remaining_ == 0) { slice_ = {}; }
            else {
                --slice_.count;
                if (slice_.count == 0) { slice_ = SlabPool::ReadChunk(slice_.next); }
                else { ++slice_.data; }
            }
        }
        void SkipInactive() noexcept {
            while (remaining_ != 0 && !active_(*slice_.data)) { Advance(); }
        }
        Slice slice_{};
        std::size_t remaining_{};
        StateFunction active_{};
    };

    Range() noexcept = default;
    [[nodiscard]]
    Iterator begin() const noexcept { return Iterator{view_}; }
    [[nodiscard]]
    Iterator end() const noexcept { return {}; }
    /** @brief 利用可能数を走査して数える。状態更新と競合する場合はスナップショットではない。 */
    [[nodiscard]]
    std::size_t size() const noexcept {
        std::size_t result{};
        for (auto iterator{begin()}; iterator != end(); ++iterator) { ++result; }
        return result;
    }
    [[nodiscard]]
    bool empty() const noexcept { return begin() == end(); }
private:
    friend class SlabPool;
    template<class> friend class ParallelRange;
    explicit Range(View view) noexcept : view_{view} {}
    View view_{};
};

/**
 * @brief 一つの取得器を共有して、並列ワーカーへ未取得の区間を配布する。
 * @details 区間だけをCASで予約し、各区間はRangeのイテレータで状態0を飛ばす。
 * ワーカー間の実体更新完了は外部のバリア/joinで同期する。取得器の使い回し・リセットは不可。
 */
template<class T>
class SlabPool::ParallelRange {
public:
    ParallelRange() noexcept = default;
    ParallelRange(const ParallelRange&) = delete;
    ParallelRange& operator=(const ParallelRange&) = delete;

    /**
     * @brief 未取得の区間を返す。全区間を配り終えたらfalseにして出力も空にする。
     * @note trueでも状態0だけの空Rangeになり得る。empty()を取得終了の判定に使わない。
     * 出力Rangeは各ワーカーが個別に持つ。並列取得可能だが取得器自体の寿命は外部で守る。
     */
    [[nodiscard]]
    bool TryAcquire(Range<T>& range) noexcept {
        range = {};
        auto first{next_.load(std::memory_order_relaxed)};
        std::size_t length{};
        do {
            if (first >= view_.count || batchSize_ == 0) { return false; }
            length = std::min(batchSize_, view_.count - first);
        } while (!next_.compare_exchange_weak(first, first + length, std::memory_order_relaxed));
        range = Range<T>{View{SlabPool::Locate(*view_.group, first), length, view_.active, view_.group}};
        return true;
    }
private:
    friend class SlabPool;
    ParallelRange(View view, std::size_t batchSize) noexcept : view_{view}, batchSize_{batchSize} {}
    View view_{};
    std::size_t batchSize_{};
    alignas(64) std::atomic<std::size_t> next_{};
};

template<class T>
SlabPool::Range<T> SlabPool::GetRange(PoolId id, Index first, std::size_t maximumCount) noexcept {
    return Range<T>{GetView(id, TypeTag<std::remove_const_t<T>>(), first, maximumCount)};
}
template<class T>
SlabPool::Range<const T> SlabPool::GetRange(PoolId id, Index first, std::size_t maximumCount) const noexcept {
    return Range<const T>{GetView(id, TypeTag<std::remove_const_t<T>>(), first, maximumCount)};
}
template<class T>
SlabPool::ParallelRange<T> SlabPool::GetParallelRange(PoolId id, std::size_t batchSize) noexcept {
    return ParallelRange<T>{GetView(id, TypeTag<std::remove_const_t<T>>(), 0, InvalidIndex), batchSize};
}
template<class T>
SlabPool::ParallelRange<const T> SlabPool::GetParallelRange(PoolId id, std::size_t batchSize) const noexcept {
    return ParallelRange<const T>{GetView(id, TypeTag<std::remove_const_t<T>>(), 0, InvalidIndex), batchSize};
}
template<class T>
T* SlabPool::At(PoolId id, Index index) noexcept {
    const auto view{GetView(id, TypeTag<std::remove_const_t<T>>(), index, 1)};
    if (view.count == 0 || !view.active(*view.first.data)) { return nullptr; }
    return static_cast<T*>(*view.first.data);
}
template<class T>
const T* SlabPool::At(PoolId id, Index index) const noexcept {
    const auto view{GetView(id, TypeTag<std::remove_const_t<T>>(), index, 1)};
    if (view.count == 0 || !view.active(*view.first.data)) { return nullptr; }
    return static_cast<const T*>(*view.first.data);
}
}