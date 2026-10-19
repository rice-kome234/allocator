/**
 * @file slab_pool.cpp
 * @brief 非所有ポインタの Slab 確保・末尾交換・連続範囲取得を実装する。
 */
#include "slab_pool.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>

namespace slab {
namespace {

// 配列サイズとポインタ差分を安全に表現できる上限。
constexpr std::size_t MaximumCapacity{
    static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(void*)};

static_assert(std::atomic<std::size_t>::is_always_lock_free,
    "並列一括登録にはロックフリーな size_t atomic が必要です。");

/** @brief 予約前に全入力を検査し、失敗による未初期化の穴を作らない。 */
[[nodiscard]]
bool ValidPointers(std::span<void* const> objects) noexcept {
    return std::all_of(objects.begin(), objects.end(), [](void* pointer) { return pointer != nullptr; });
}

}

/** @brief 一つの連続ポインタ配列。実体への所有権は持たない。 */
struct SlabPool::Block {
    /** @brief アラインメントを指定して確保したポインタ領域だけを解放する。 */
    struct PointerDeleter {
        void operator()(void** memory) const noexcept {
            ::operator delete(memory, std::align_val_t{SlabAlignment});
        }
    };
    std::size_t first{};
    std::size_t capacity{};
    std::unique_ptr<void*[], PointerDeleter> pointers{};
};

/** @brief ID ごとの密な登録列と、容量順の Slab 索引を保持する。 */
struct SlabPool::Group {
    PoolId id{};
    Options options{};
    std::size_t count{};
    std::size_t capacity{};
    std::size_t blockCount{};
    std::size_t indexCapacity{};
    std::unique_ptr<Block*[]> blocks{};
    Group* next{};
    /** @brief AddConcurrent 同士を ID ごとに同期する。他の API はロックしない。 */
    std::mutex appendMutex{};
    /** @brief 開始・終了は直列実行。コピー中は不変で、ワーカー開始の同期により公開する。 */
    bool appending{};
    std::size_t appendLimit{};
    /** @brief コピー済み件数ではなく、予約済み区間の末尾。読み取り用 count と分離する。 */
    alignas(SlabAlignment) std::atomic<std::size_t> reservedEnd{};

    ~Group() noexcept {
        for (std::size_t i{}; i < blockCount; ++i) { delete blocks[i]; }
    }

    /** @brief 添字を含む Slab を二分探索する。添字は総容量内であること。 */
    [[nodiscard]]
    std::size_t LocateIndex(std::size_t index) const noexcept {
        std::size_t lower{}, upper{blockCount};
        while (lower < upper) {
            const std::size_t middle{lower + (upper - lower) / 2};
            if (blocks[middle]->first <= index) { lower = middle + 1; }
            else { upper = middle; }
        }
        return lower - 1;
    }

    [[nodiscard]]
    Block* Locate(std::size_t index) const noexcept { return blocks[LocateIndex(index)]; }

    /** @brief 予約済み区間へコピーする。索引探索は最初の一度だけで、以後は隣の Slab へ進む。 */
    void CopyAt(std::size_t first, std::span<void* const> objects) noexcept {
        if (objects.empty()) { return; }
        std::size_t blockIndex{LocateIndex(first)};
        std::size_t copied{};
        while (copied < objects.size()) {
            Block& block{*blocks[blockIndex++]};
            const auto offset{first + copied - block.first};
            const auto length{std::min(objects.size() - copied, block.capacity - offset)};
            std::copy_n(objects.data() + copied, length, block.pointers.get() + offset);
            copied += length;
        }
    }

    /** @brief Slab 一つを追加する。失敗時は既存の容量・登録内容を変更しない。 */
    [[nodiscard]]
    bool Append(std::size_t slots) noexcept {
        if (slots == 0 || slots > MaximumCapacity - capacity) { return false; }
        std::unique_ptr<Block> block{new (std::nothrow) Block{}};
        if (!block) { return false; }
        // C++20 の operator new で配列の格納領域を確保し、各ポインタを初期化する。
        block->pointers.reset(static_cast<void**>(::operator new(slots * sizeof(void*),
            std::align_val_t{SlabAlignment}, std::nothrow)));
        if (!block->pointers) { return false; }
        std::uninitialized_value_construct_n(block->pointers.get(), slots);

        if (blockCount == indexCapacity) {
            std::size_t replacementCapacity{8};
            if (indexCapacity != 0) {
                if (indexCapacity > MaximumCapacity / 2) { return false; }
                replacementCapacity = indexCapacity * 2;
            }
            std::unique_ptr<Block*[]> replacement{new (std::nothrow) Block*[replacementCapacity]{}};
            if (!replacement) { return false; }
            for (std::size_t i{}; i < blockCount; ++i) { replacement[i] = blocks[i]; }
            blocks = std::move(replacement);
            indexCapacity = replacementCapacity;
        }
        block->first = capacity;
        block->capacity = slots;
        blocks[blockCount++] = block.release();
        capacity += slots;
        return true;
    }
};

SlabPool::SlabPool() noexcept = default;

SlabPool::~SlabPool() noexcept {
    while (groups_ != nullptr) {
        Group* next{groups_->next};
        delete groups_;
        groups_ = next;
    }
}

SlabPool::Group* SlabPool::Find(PoolId id) const noexcept {
    for (Group* group{groups_}; group != nullptr; group = group->next) {
        if (group->id == id) { return group; }
    }
    return nullptr;
}

bool SlabPool::Register(PoolId id) noexcept {
    return Register(id, Options{});
}

bool SlabPool::Register(PoolId id, const Options& options) noexcept {
    if (options.growthBytes < sizeof(void*) || options.growthBytes % sizeof(void*) != 0
        || options.growthBytes / sizeof(void*) > MaximumCapacity
        || options.initialCapacity > MaximumCapacity) { return false; }
    if (const Group* existing{Find(id)}; existing != nullptr) {
        return existing->options.initialCapacity == options.initialCapacity
            && existing->options.growthBytes == options.growthBytes;
    }
    std::unique_ptr<Group> group{new (std::nothrow) Group{}};
    if (!group) { return false; }
    group->id = id;
    group->options = options;
    if (options.initialCapacity != 0 && !group->Append(options.initialCapacity)) { return false; }
    group->next = groups_;
    groups_ = group.release();
    return true;
}

bool SlabPool::Reserve(PoolId id, std::size_t capacity) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || group->appending || capacity > MaximumCapacity) { return false; }
    if (capacity <= group->capacity) { return true; }
    return group->Append(std::max(capacity - group->capacity, group->options.growthBytes / sizeof(void*)));
}

SlabPool::Index SlabPool::Add(PoolId id, void* object) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || group->appending || object == nullptr) { return InvalidIndex; }
    if (group->count == group->capacity && !group->Append(group->options.growthBytes / sizeof(void*))) {
        return InvalidIndex;
    }
    const Index index{group->count};
    Block* block{group->Locate(index)};
    block->pointers[index - block->first] = object;
    ++group->count;
    return index;
}

SlabPool::Index SlabPool::AddBatch(PoolId id, std::span<void* const> objects) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || group->appending || objects.size() > MaximumCapacity - group->count
        || !ValidPointers(objects)) { return InvalidIndex; }
    const Index first{group->count};
    if (!Reserve(id, first + objects.size())) { return InvalidIndex; }
    group->CopyAt(first, objects);
    group->count += objects.size();
    return first;
}

bool SlabPool::BeginConcurrentAppend(PoolId id, std::size_t maximumAdditionalCount) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || group->appending || maximumAdditionalCount > MaximumCapacity - group->count) {
        return false;
    }
    const auto limit{group->count + maximumAdditionalCount};
    if (!Reserve(id, limit)) { return false; }
    group->reservedEnd.store(group->count, std::memory_order_relaxed);
    group->appendLimit = limit;
    group->appending = true;
    return true;
}

SlabPool::Index SlabPool::AddBatchConcurrent(PoolId id, std::span<void* const> objects) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || !group->appending || !ValidPointers(objects)) { return InvalidIndex; }
    Index first{group->reservedEnd.load(std::memory_order_relaxed)};
    for (;;) {
        if (objects.size() > group->appendLimit - first) { return InvalidIndex; }
        if (objects.empty()) { return first; }
        // CAS は区間の重複だけを防ぐ。ポインタの公開は呼び出し側の完了バリアが担う。
        if (group->reservedEnd.compare_exchange_weak(first, first + objects.size(),
            std::memory_order_relaxed, std::memory_order_relaxed)) { break; }
    }
    group->CopyAt(first, objects);
    return first;
}

bool SlabPool::EndConcurrentAppend(PoolId id) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || !group->appending) { return false; }
    // 全コピー完了を利用側で同期済みであること。予約だけを完了として扱ってはいけない。
    group->count = group->reservedEnd.load(std::memory_order_relaxed);
    group->appending = false;
    return true;
}

SlabPool::Index SlabPool::AddConcurrent(PoolId id, void* object) noexcept {
    // 登録リストは同時追加の開始前に確定するため、Find の読み取りは競合しない。
    Group* group{Find(id)};
    if (group == nullptr || object == nullptr) { return InvalidIndex; }
    const std::lock_guard lock{group->appendMutex};
    return Add(id, object);
}
SlabPool::Removal SlabPool::RemoveAt(PoolId id, Index index) noexcept {
    Group* group{Find(id)};
    if (group == nullptr || group->appending || index >= group->count) { return {}; }
    Block* destination{group->Locate(index)};
    void*& entry{destination->pointers[index - destination->first]};
    Removal result{entry, nullptr};
    const Index last{group->count - 1};
    Block* source{group->Locate(last)};
    void*& tail{source->pointers[last - source->first]};
    if (index != last) {
        result.moved = tail;
        entry = tail;
    }
    tail = nullptr;
    --group->count;
    return result;
}

void SlabPool::Clear(PoolId id) noexcept {
    if (Group* group{Find(id)}; group != nullptr && !group->appending) {
        // count 外は参照しない。次回 Add が上書きするため全配列の初期化は不要。
        group->count = 0;
    }
}

std::size_t SlabPool::Count(PoolId id) const noexcept {
    const Group* group{Find(id)};
    if (group == nullptr || group->appending) { return 0; }
    return group->count;
}

void* SlabPool::At(PoolId id, Index index) const noexcept {
    const auto batch{GetBatch(id, index, 1)};
    if (batch.empty()) { return nullptr; }
    return batch.front();
}

std::span<void* const> SlabPool::GetBatch(PoolId id, Index first, std::size_t maximumCount) const noexcept {
    const Group* group{Find(id)};
    if (group == nullptr || group->appending || first >= group->count || maximumCount == 0) { return {}; }
    const Block* block{group->Locate(first)};
    const std::size_t offset{first - block->first};
    const std::size_t length{std::min({group->count - first, block->capacity - offset, maximumCount})};
    return {block->pointers.get() + offset, length};
}

SlabPool::Statistics SlabPool::GetStatistics() const noexcept {
    Statistics result{};
    result.managementBytes = sizeof(SlabPool);
    for (const Group* group{groups_}; group != nullptr; group = group->next) {
        ++result.groups;
        result.slabs += group->blockCount;
        result.count += group->count;
        result.capacity += group->capacity;
        result.pointerBytes += group->capacity * sizeof(void*);
        result.managementBytes += sizeof(Group) + group->indexCapacity * sizeof(Block*)
            + group->blockCount * sizeof(Block);
    }
    return result;
}

}
