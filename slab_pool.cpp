/** @file slab_pool.cpp
 * @brief ポインタ用仮想メモリ、区間予約、フレーム末尾の末尾入れ替え除去。
 */
#include "slab_pool.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <new>

namespace slab {
namespace {
constexpr std::size_t MaximumBytes{static_cast<std::size_t>(PTRDIFF_MAX)};
constexpr std::size_t PointerBytes{sizeof(void*)};

[[nodiscard]]
bool RoundUp(std::size_t value, std::size_t unit, std::size_t& result) noexcept {
    const auto remainder{value % unit};
    result = value;
    if (remainder == 0) { return true; }
    const auto extra{unit - remainder};
    if (value > MaximumBytes - extra) { return false; }
    result += extra;
    return true;
}
}

/** @brief 一度に予約した仮想領域。コミットを増やしても先頭アドレスは不変。 */
struct SlabPool::Region {
    std::uintptr_t address{};
    std::size_t bytes{};
    std::size_t committed{};
    Region* next{};
};

/** @brief コミット後に寿命を開始したvoid*配列。前リンクは末尾からの除去で使う。 */
struct SlabPool::Chunk {
    void** data{};
    std::size_t first{};
    std::size_t capacity{};
    Chunk* previous{};
    Chunk* next{};
};

/** @brief ID単位の型・状態読み取り関数・格納領域。参照先は所有しない。 */
struct SlabPool::Group {
    PoolId id{};
    Options options{};
    const void* typeTag{};
    StateFunction active{};
    std::size_t pageBytes{};
    std::size_t commitStep{};
    std::size_t reservationStep{};
    std::size_t count{};
    std::size_t capacity{};
    std::size_t reservedBytes{};
    std::size_t appendLimit{};
    bool appending{};
    std::atomic<bool> mayContainInactive{};
    alignas(64) std::atomic<std::size_t> reservedEnd{};
    Region* regions{};
    Region* lastRegion{};
    Chunk* chunks{};
    Chunk* lastChunk{};
    Group* next{};
};

static_assert(std::atomic<std::size_t>::is_always_lock_free,
    "この実装はsize_tのロックフリー原子操作を必要とします。");

SlabPool::~SlabPool() noexcept {
    while (groups_ != nullptr) {
        auto* next{groups_->next};
        assert(!groups_->appending);
        ReleaseGroup(groups_);
        groups_ = next;
    }
}

SlabPool::Group* SlabPool::Find(PoolId id) const noexcept {
    for (auto* group{groups_}; group != nullptr; group = group->next) {
        if (group->id == id) { return group; }
    }
    return nullptr;
}

bool SlabPool::RegisterRaw(PoolId id, const Options& options, const void* tag, StateFunction active) noexcept {
    if (Find(id) != nullptr || options.reservedCapacity == 0 || options.commitBytes == 0 ||
        options.initialCapacity > options.reservedCapacity ||
        options.reservedCapacity > MaximumBytes / PointerBytes || options.commitBytes > MaximumBytes ||
        options.freeThreshold > MaximumBytes / PointerBytes) { return false; }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t pageBytes{system.dwPageSize};
    if (pageBytes % PointerBytes != 0) { return false; }
    std::size_t commitStep{};
    std::size_t reservationStep{};
    if (!RoundUp(options.commitBytes, pageBytes, commitStep) ||
        !RoundUp(options.reservedCapacity * PointerBytes, pageBytes, reservationStep)) { return false; }
    auto* group{new (std::nothrow) Group{}};
    if (group == nullptr) { return false; }
    group->id = id;
    group->options = options;
    group->typeTag = tag;
    group->active = active;
    group->pageBytes = pageBytes;
    group->commitStep = commitStep;
    group->reservationStep = reservationStep;
    if (!ReserveGroup(*group, options.reservedCapacity) || !CommitGroup(*group, options.initialCapacity)) {
        ReleaseGroup(group);
        return false;
    }
    group->next = groups_;
    groups_ = group;
    return true;
}

bool SlabPool::ReserveGroup(Group& group, std::size_t minimumCapacity) noexcept {
    if (minimumCapacity > MaximumBytes / PointerBytes) { return false; }
    const auto requestedBytes{minimumCapacity * PointerBytes};
    if (requestedBytes <= group.reservedBytes) { return true; }
    std::size_t bytes{};
    if (!RoundUp(std::max(requestedBytes - group.reservedBytes, group.reservationStep), group.pageBytes, bytes) ||
        bytes > MaximumBytes - group.reservedBytes) { return false; }
    auto* region{new (std::nothrow) Region{}};
    if (region == nullptr) { return false; }
    void* address{VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS)};
    if (address == nullptr) { delete region; return false; }
    region->address = reinterpret_cast<std::uintptr_t>(address);
    region->bytes = bytes;
    if (group.lastRegion != nullptr) { group.lastRegion->next = region; }
    else { group.regions = region; }
    group.lastRegion = region;
    group.reservedBytes += bytes;
    return true;
}

bool SlabPool::CommitGroup(Group& group, std::size_t minimumCapacity) noexcept {
    if (minimumCapacity <= group.capacity) { return true; }
    if (!ReserveGroup(group, minimumCapacity)) { return false; }
    std::size_t additionalBytes{};
    if (!RoundUp((minimumCapacity - group.capacity) * PointerBytes, group.commitStep, additionalBytes)) { return false; }
    additionalBytes = std::min(additionalBytes, group.reservedBytes - group.capacity * PointerBytes);
    const auto targetCapacity{group.capacity + additionalBytes / PointerBytes};
    for (auto* region{group.regions}; group.capacity < targetCapacity; region = region->next) {
        assert(region != nullptr);
        const auto available{region->bytes - region->committed};
        if (available == 0) { continue; }
        const auto bytes{std::min((targetCapacity - group.capacity) * PointerBytes, available)};
        auto* chunk{new (std::nothrow) Chunk{}};
        if (chunk == nullptr) { return false; }
        void* address{reinterpret_cast<void*>(region->address + region->committed)};
        void* committed{VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE)};
        if (committed == nullptr) { delete chunk; return false; }
        if (group.options.preTouch) {
            for (std::size_t offset{}; offset < bytes; offset += group.pageBytes) {
                *(static_cast<volatile std::byte*>(committed) + offset) = std::byte{};
            }
        }
        // 未コミット領域には配列を作らない。値の書き込み前にポインタ配列の寿命だけを開始する。
        chunk->capacity = bytes / PointerBytes;
        chunk->data = ::new (committed) void*[chunk->capacity];
        chunk->first = group.capacity;
        chunk->previous = group.lastChunk;
        if (group.lastChunk != nullptr) { group.lastChunk->next = chunk; }
        else { group.chunks = chunk; }
        group.lastChunk = chunk;
        region->committed += bytes;
        group.capacity += chunk->capacity;
    }
    return true;
}

bool SlabPool::Reserve(PoolId id, std::size_t minimumCapacity) noexcept {
    auto* group{Find(id)};
    return group != nullptr && !group->appending && ReserveGroup(*group, minimumCapacity);
}
bool SlabPool::Commit(PoolId id, std::size_t minimumCapacity) noexcept {
    auto* group{Find(id)};
    return group != nullptr && !group->appending && CommitGroup(*group, minimumCapacity);
}
bool SlabPool::CommitAtFrameEnd(PoolId id) noexcept {
    auto* group{Find(id)};
    if (group == nullptr || group->appending) { return false; }
    if (group->options.freeThreshold == 0 || group->capacity - group->count > group->options.freeThreshold) { return true; }
    const auto additionalCount{group->commitStep / PointerBytes};
    if (additionalCount > MaximumBytes / PointerBytes - group->capacity) { return false; }
    return CommitGroup(*group, group->capacity + additionalCount);
}

SlabPool::Slice SlabPool::ReadChunk(const Chunk* chunk) noexcept {
    if (chunk == nullptr) { return {}; }
    return {chunk->data, chunk->capacity, chunk->next};
}
SlabPool::Slice SlabPool::Locate(const Group& group, Index index) noexcept {
    for (auto* chunk{group.chunks}; chunk != nullptr; chunk = chunk->next) {
        if (index >= chunk->first && index - chunk->first < chunk->capacity) {
            const auto offset{index - chunk->first};
            return {chunk->data + offset, chunk->capacity - offset, chunk->next};
        }
    }
    return {};
}

SlabPool::Index SlabPool::AddRaw(PoolId id, const void* tag, const void* source,
    std::size_t count, StoreFunction store, bool concurrent) noexcept {
    auto* group{Find(id)};
    if (group == nullptr || group->typeTag != tag || group->appending != concurrent) { return InvalidIndex; }
    std::size_t first{};
    if (concurrent) {
        first = group->reservedEnd.load(std::memory_order_relaxed);
        do {
            if (count > group->appendLimit - first) { return InvalidIndex; }
        } while (!group->reservedEnd.compare_exchange_weak(first, first + count, std::memory_order_relaxed));
    } else {
        first = group->count;
        if (count > InvalidIndex - first) { return InvalidIndex; }
        if (first + count > group->capacity &&
            (group->options.freeThreshold != 0 || !CommitGroup(*group, first + count))) { return InvalidIndex; }
    }
    auto slice{Locate(*group, first)};
    std::size_t stored{};
    bool containsInactive{};
    while (stored < count) {
        const auto length{std::min(count - stored, slice.count)};
        assert(length != 0);
        store(slice.data, source, stored, length);
        for (std::size_t i{}; i < length; ++i) {
            if (!group->active(slice.data[i])) { containsInactive = true; }
        }
        stored += length;
        slice = ReadChunk(slice.next);
    }
    if (!concurrent) { group->count += count; }
    // 登録時から0の実体も、通知漏れなしで次のフレーム末尾に除去する。
    if (containsInactive) { group->mayContainInactive.store(true, std::memory_order_release); }
    return first;
}

bool SlabPool::BeginConcurrentAppend(PoolId id, std::size_t maximumAdditionalCount) noexcept {
    auto* group{Find(id)};
    if (group == nullptr || group->appending || maximumAdditionalCount > InvalidIndex - group->count) { return false; }
    if (group->count + maximumAdditionalCount > group->capacity &&
        (group->options.freeThreshold != 0 || !CommitGroup(*group, group->count + maximumAdditionalCount))) { return false; }
    group->appendLimit = group->count + maximumAdditionalCount;
    group->reservedEnd.store(group->count, std::memory_order_relaxed);
    group->appending = true;
    return true;
}
bool SlabPool::EndConcurrentAppend(PoolId id) noexcept {
    auto* group{Find(id)};
    if (group == nullptr || !group->appending) { return false; }
    group->count = group->reservedEnd.load(std::memory_order_relaxed);
    group->appending = false;
    return true;
}

bool SlabPool::NotifyStateChanged(PoolId id) noexcept {
    auto* group{Find(id)};
    if (group == nullptr) { return false; }
    group->mayContainInactive.store(true, std::memory_order_release);
    return true;
}

std::size_t SlabPool::RemoveInactive() noexcept {
    // 一部のIDだけ除去してから失敗しないよう、先に全IDの追加フェーズを確認する。
    for (auto* group{groups_}; group != nullptr; group = group->next) {
        if (group->appending) { return InvalidIndex; }
    }
    std::size_t removed{};
    for (auto* group{groups_}; group != nullptr; group = group->next) {
        if (!group->mayContainInactive.exchange(false, std::memory_order_acq_rel)) { continue; }
        if (group->count == 0) { continue; }
        const auto originalCount{group->count};
        auto* front{group->chunks};
        auto* tail{group->lastChunk};
        while (tail->first >= group->count) { tail = tail->previous; }
        std::size_t index{};
        while (index < group->count) {
            while (index >= front->first + front->capacity) { front = front->next; }
            auto& current{front->data[index - front->first]};
            if (group->active(current)) { ++index; continue; }
            auto& last{tail->data[group->count - 1 - tail->first]};
            current = last;
            last = nullptr;
            --group->count;
            // 移した末尾も状態0の場合があるので、同じindexを再検査する。
            while (tail->previous != nullptr && tail->first >= group->count) { tail = tail->previous; }
        }
        removed += originalCount - group->count;
    }
    return removed;
}

bool SlabPool::Clear(PoolId id) noexcept {
    auto* group{Find(id)};
    if (group == nullptr || group->appending) { return false; }
    group->count = 0;
    group->mayContainInactive.store(false, std::memory_order_relaxed);
    return true;
}

void SlabPool::ReleaseGroup(Group* group) noexcept {
    while (group->chunks != nullptr) {
        auto* next{group->chunks->next};
        delete group->chunks;
        group->chunks = next;
    }
    while (group->regions != nullptr) {
        auto* next{group->regions->next};
        (void)VirtualFree(reinterpret_cast<void*>(group->regions->address), 0, MEM_RELEASE);
        delete group->regions;
        group->regions = next;
    }
    delete group;
}

std::size_t SlabPool::Count(PoolId id) const noexcept {
    const auto* group{Find(id)};
    if (group == nullptr || group->appending) { return 0; }
    return group->count;
}
SlabPool::Statistics SlabPool::GetStatistics(PoolId id) const noexcept {
    const auto* group{Find(id)};
    if (group == nullptr || group->appending) { return {}; }
    Statistics result{};
    result.count = group->count;
    result.capacity = group->capacity;
    result.reservedCapacity = group->reservedBytes / PointerBytes;
    result.reservedBytes = group->reservedBytes;
    result.committedBytes = group->capacity * PointerBytes;
    result.pointerSize = PointerBytes;
    result.commitQuantum = group->pageBytes;
    result.mayContainInactive = group->mayContainInactive.load(std::memory_order_acquire);
    for (auto* region{group->regions}; region != nullptr; region = region->next) { ++result.regions; }
    for (auto* chunk{group->chunks}; chunk != nullptr; chunk = chunk->next) { ++result.chunks; }
    return result;
}
SlabPool::View SlabPool::GetView(PoolId id, const void* tag, Index first, std::size_t maximumCount) const noexcept {
    const auto* group{Find(id)};
    if (group == nullptr || group->appending || group->typeTag != tag || first >= group->count || maximumCount == 0) {
        return {};
    }
    return {Locate(*group, first), std::min(maximumCount, group->count - first), group->active, group};
}
}
