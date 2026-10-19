/**
 * @file slab_pool_impl.h
 * @brief ポインタ格納形式を変更せず、型付き参照を返すテンプレート実装。
 */
#pragma once

#include <ranges>
#include <algorithm>
#include <iterator>
#include <type_traits>

namespace slab {

/**
 * @brief void* を一つずつ元の型へ戻す、連続ポインタ列へのビュー。
 * @details void** を T** に読み替えず、個々の void* を static_cast する。
 * イテレータの利用中は、この Batch 自体と参照元の Slab を有効に保つこと。
 */
template<class T>
class SlabPool::Batch {
    static_assert(std::is_object_v<T>, "Batch の型にはオブジェクト型を指定してください。");

    /** @brief ポインタの型変換はここに集約する。実体のコピーは行わない。 */
    struct Dereference {
        [[nodiscard]]
        T& operator()(void* pointer) const noexcept {
            return *static_cast<T*>(pointer);
        }
    };

    using View = std::ranges::transform_view<std::span<void* const>, Dereference>;

public:
    Batch() noexcept = default;

    /** @brief 並列実行側などで、取得済みのポインタ範囲にも型を付けられる。 */
    explicit Batch(std::span<void* const> pointers) noexcept : view_{pointers, Dereference{}} {}

    [[nodiscard]]
    auto begin() const noexcept { return view_.begin(); }

    [[nodiscard]]
    auto end() const noexcept { return view_.end(); }

    [[nodiscard]]
    std::size_t size() const noexcept { return view_.size(); }

    [[nodiscard]]
    bool empty() const noexcept { return view_.empty(); }

    /** @brief 範囲内の実体を参照する。index は size() 未満であること。 */
    [[nodiscard]]
    T& operator[](std::size_t index) const noexcept {
        return view_[static_cast<std::ptrdiff_t>(index)];
    }

private:
    View view_{};
};

template<class T>
SlabPool::Batch<T> SlabPool::GetBatch(PoolId id, Index first, std::size_t maximumCount) const noexcept {
    return Batch<T>{GetBatch(id, first, maximumCount)};
}

/** @brief Slab 内は連続アクセスし、境界だけで次の Slab を取得する範囲。 */
template<class T>
class SlabPool::Range {
    static_assert(std::is_object_v<T>, "Range の型にはオブジェクト型を指定してください。");
public:
    /** @brief ポインタを T& に戻す前方イテレータ。実体やポインタ列は複製しない。 */
    class Iterator {
    public:
        using value_type = std::remove_cv_t<T>;
        using difference_type = std::ptrdiff_t;
        using reference = T&;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        Iterator() noexcept = default;
        Iterator(const SlabPool* pool, PoolId id, Index index, Index last) noexcept
            : pool_{pool}, id_{id}, index_{index}, last_{last} { Load(); }

        [[nodiscard]]
        T& operator*() const noexcept { return *static_cast<T*>(batch_[offset_]); }

        [[nodiscard]]
        T* operator->() const noexcept { return static_cast<T*>(batch_[offset_]); }

        Iterator& operator++() noexcept {
            ++index_;
            ++offset_;
            if (index_ < last_ && offset_ == batch_.size()) { Load(); }
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator previous{*this};
            ++*this;
            return previous;
        }

        [[nodiscard]]
        bool operator==(const Iterator& other) const noexcept {
            return pool_ == other.pool_ && id_ == other.id_ && index_ == other.index_
                && last_ == other.last_;
        }
    private:
        void Load() noexcept {
            offset_ = 0;
            if (index_ < last_) { batch_ = pool_->GetBatch(id_, index_, last_ - index_); }
        }

        const SlabPool* pool_{};
        PoolId id_{};
        Index index_{}, last_{};
        std::span<void* const> batch_{};
        std::size_t offset_{};
    };

    Range() noexcept = default;
    Range(const SlabPool* pool, PoolId id, Index first, std::size_t count) noexcept
        : pool_{pool}, id_{id}, first_{first}, count_{count} {}

    [[nodiscard]]
    Iterator begin() const noexcept { return {pool_, id_, first_, first_ + count_}; }

    [[nodiscard]]
    Iterator end() const noexcept { return {pool_, id_, first_ + count_, first_ + count_}; }

    [[nodiscard]]
    std::size_t size() const noexcept { return count_; }

    [[nodiscard]]
    bool empty() const noexcept { return count_ == 0; }
private:
    const SlabPool* pool_{};
    PoolId id_{};
    Index first_{};
    std::size_t count_{};
};

template<class T>
SlabPool::Range<T> SlabPool::GetRange(PoolId id, Index first, std::size_t maximumCount) const noexcept {
    const auto count{Count(id)};
    if (first >= count) { return {}; }
    return {this, id, first, std::min(maximumCount, count - first)};
}

}
