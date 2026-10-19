/** @file slab_pool.cpp
 * @brief ポインタ用仮想メモリ、ID単位の排他取得、フレーム末尾の末尾入れ替え除去。
 */
#include "slab_pool.h"
#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <new>

namespace slab {
namespace {
constexpr size_t MaximumBytes{static_cast<size_t>(PTRDIFF_MAX)};
/** @brief 対象環境の通常ページサイズを数値で指定する。OSの実際のページサイズに合わせる。 */
constexpr size_t PageBytes{4096};
/** @brief 格納するポインタのサイズを数値で指定する。現在のx64構成では8バイト。 */
constexpr size_t PointerBytes{8};
/** @brief 初回登録時にmapのバケットを事前確保する想定ID数。Groupの実体は登録時に作る。 */
constexpr size_t InitialGroupCapacity{64};

static_assert(PointerBytes == sizeof(Component*),
    "設定したポインタサイズが実際のComponent*と一致していません。");
static_assert(PointerBytes != 0 && PageBytes != 0 && PageBytes <= MaximumBytes && PageBytes % PointerBytes == 0,
    "ページサイズには、ポインタサイズの倍数となる正のバイト数を指定してください。");

[[nodiscard]]
bool RoundUp(size_t value, size_t unit, size_t& result) noexcept {
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
    uintptr_t address{};
    size_t bytes{};
    size_t committed{};

    Region() noexcept = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    /** @brief listから破棄されるとき、所有する仮想領域を予約ごと解放する。 */
    ~Region() noexcept {
        if (address != 0) { (void)VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE); }
    }
};

/** @brief コミット後に寿命を開始したComponent*配列の管理情報。前後関係はlistが管理する。 */
struct SlabPool::Chunk {
    Component** data{};
    size_t first{};
    size_t capacity{};
};

SlabPool::Group::Group() noexcept = default;
SlabPool::Group::~Group() noexcept = default;

bool SlabPool::Group::TryAcquire() noexcept {
    bool expected{};
    return inUse.compare_exchange_strong(expected, true, memory_order_acquire, memory_order_relaxed);
}
void SlabPool::Group::Release() noexcept { inUse.store(false, memory_order_release); }

SlabPool::Group::Access::Access(Group* group) noexcept {
    if (group != nullptr && group->TryAcquire()) { owned = group; }
}
SlabPool::Group::Access::~Access() noexcept { if (owned != nullptr) { owned->Release(); } }
SlabPool::Group::Access::operator bool() const noexcept { return owned != nullptr; }

SlabPool::SlabPool() noexcept = default;

SlabPool& SlabPool::GetInstance() noexcept {
    static SlabPool instance{};
    return instance;
}

SlabPool::~SlabPool() noexcept {
    const bool cleared{Clear()};
    assert(cleared);
    (void)cleared;
}

SlabPool::Group* SlabPool::Find(PoolId id) const noexcept {
    const auto position{registry_.groups.find(id)};
    if (position == registry_.groups.end()) { return nullptr; }
    return &position->second;
}

bool SlabPool::RegisterRaw(PoolId id, const Options& options, const void* tag) noexcept {
    if (Find(id) != nullptr || options.reservedCapacity == 0 || options.commitBytes == 0 ||
        options.initialCapacity > options.reservedCapacity ||
        options.reservedCapacity > MaximumBytes / PointerBytes || options.commitBytes > MaximumBytes ||
        options.freeThreshold > MaximumBytes / PointerBytes) { return false; }
    size_t commitStep{};
    size_t reservationStep{};
    if (!RoundUp(options.commitBytes, PageBytes, commitStep) ||
        !RoundUp(options.reservedCapacity * PointerBytes, PageBytes, reservationStep)) { return false; }
    auto& groups{registry_.groups};
    if (groups.empty()) { groups.reserve(InitialGroupCapacity); }
    const auto [position, inserted]{groups.try_emplace(id)};
    if (!inserted) { return false; }
    auto& group{position->second};
    group.options = options;
    group.typeTag = tag;
    group.commitStep = commitStep;
    group.reservationStep = reservationStep;
    if (!ReserveGroup(group, options.reservedCapacity) || !CommitGroup(group, options.initialCapacity)) {
        // Groupのデストラクタが、途中まで準備した領域も解放する。
        groups.erase(position);
        return false;
    }
    return true;
}

bool SlabPool::ReserveGroup(Group& group, size_t minimumCapacity) noexcept {
    if (minimumCapacity > MaximumBytes / PointerBytes) { return false; }
    const auto requestedBytes{minimumCapacity * PointerBytes};
    if (requestedBytes <= group.reservedBytes) { return true; }
    size_t bytes{};
    if (!RoundUp(max(requestedBytes - group.reservedBytes, group.reservationStep), PageBytes, bytes) ||
        bytes > MaximumBytes - group.reservedBytes) { return false; }
    auto& region{group.regions.emplace_back()};
    void* address{VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS)};
    if (address == nullptr) { group.regions.pop_back(); return false; }
    region.address = reinterpret_cast<uintptr_t>(address);
    region.bytes = bytes;
    group.reservedBytes += bytes;
    return true;
}

bool SlabPool::CommitGroup(Group& group, size_t minimumCapacity) noexcept {
    if (minimumCapacity <= group.capacity) { return true; }
    if (!ReserveGroup(group, minimumCapacity)) { return false; }
    size_t additionalBytes{};
    if (!RoundUp((minimumCapacity - group.capacity) * PointerBytes, group.commitStep, additionalBytes)) { return false; }
    additionalBytes = min(additionalBytes, group.reservedBytes - group.capacity * PointerBytes);
    const auto targetCapacity{group.capacity + additionalBytes / PointerBytes};
    for (auto& region : group.regions) {
        if (group.capacity >= targetCapacity) { break; }
        const auto available{region.bytes - region.committed};
        if (available == 0) { continue; }
        const auto bytes{min((targetCapacity - group.capacity) * PointerBytes, available)};
        auto& chunk{group.chunks.emplace_back()};
        void* address{reinterpret_cast<void*>(region.address + region.committed)};
        void* committed{VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE)};
        if (committed == nullptr) { group.chunks.pop_back(); return false; }
        if (group.options.preTouch) {
            for (size_t offset{}; offset < bytes; offset += PageBytes) {
                *(static_cast<volatile byte*>(committed) + offset) = byte{};
            }
        }
        // 未コミット領域には配列を作らない。値の書き込み前にポインタ配列の寿命だけを開始する。
        chunk.capacity = bytes / PointerBytes;
        chunk.data = ::new (committed) Component*[chunk.capacity];
        chunk.first = group.capacity;
        region.committed += bytes;
        group.capacity += chunk.capacity;
    }
    return true;
}

bool SlabPool::CommitAtFrameEnd(PoolId id) noexcept {
    auto* group{Find(id)};
    Group::Access access{group};
    if (!access) { return false; }
    if (group->options.freeThreshold == 0 || group->capacity - group->count > group->options.freeThreshold) { return true; }
    const auto additionalCount{group->commitStep / PointerBytes};
    if (additionalCount > MaximumBytes / PointerBytes - group->capacity) { return false; }
    return CommitGroup(*group, group->capacity + additionalCount);
}

SlabPool::Slice SlabPool::ReadChunk(ChunkIterator chunk, ChunkIterator end) noexcept {
    if (chunk == end) { return {}; }
    return {chunk->data, chunk->capacity, next(chunk), end};
}
SlabPool::Slice SlabPool::Locate(const Group& group, Index index) noexcept {
    const auto end{group.chunks.cend()};
    for (auto chunk{group.chunks.cbegin()}; chunk != end; ++chunk) {
        if (index >= chunk->first && index - chunk->first < chunk->capacity) {
            const auto offset{index - chunk->first};
            return {chunk->data + offset, chunk->capacity - offset, next(chunk), end};
        }
    }
    return {};
}

SlabPool::Index SlabPool::Add(PoolId id, Component* pointer) noexcept {
    if (pointer == nullptr) { return InvalidIndex; }
    auto* group{Find(id)};
    Group::Access access{group};
    if (!access) { return InvalidIndex; }
    const auto index{group->count};
    if (index == group->capacity &&
        (group->options.freeThreshold != 0 || !CommitGroup(*group, index + 1))) { return InvalidIndex; }
    const auto slice{Locate(*group, index)};
    *slice.data = pointer;
    ++group->count;
    // 登録時から0の実体も、通知漏れなしで次のフレーム末尾に除去する。
    if (pointer->state == 0) { group->mayContainInactive.store(true, memory_order_release); }
    return index;
}

bool SlabPool::NotifyStateChanged(PoolId id) noexcept {
    auto* group{Find(id)};
    if (group == nullptr) { return false; }
    group->mayContainInactive.store(true, memory_order_release);
    return true;
}

size_t SlabPool::RemoveInactive() noexcept {
    auto& groups{registry_.groups};
    // 先に全IDを取得し、一つでも使用中なら取得済みの権利だけを戻す。内容は変更しない。
    for (auto position{groups.begin()}; position != groups.end(); ++position) {
        if (!position->second.TryAcquire()) {
            for (auto acquired{groups.begin()}; acquired != position; ++acquired) { acquired->second.Release(); }
            return InvalidIndex;
        }
    }
    size_t removed{};
    for (auto& entry : groups) {
        auto& group{entry.second};
        if (!group.mayContainInactive.exchange(false, memory_order_acq_rel)) { continue; }
        if (group.count == 0) { continue; }
        const auto originalCount{group.count};
        auto front{group.chunks.begin()};
        auto tail{prev(group.chunks.end())};
        while (tail->first >= group.count) { --tail; }
        size_t index{};
        while (index < group.count) {
            while (index >= front->first + front->capacity) { ++front; }
            auto& current{front->data[index - front->first]};
            if (current->state != 0) { ++index; continue; }
            auto& last{tail->data[group.count - 1 - tail->first]};
            current = last;
            last = nullptr;
            --group.count;
            // 移した末尾も状態0の場合があるので、同じindexを再検査する。
            while (tail != group.chunks.begin() && tail->first >= group.count) { --tail; }
        }
        removed += originalCount - group.count;
    }
    for (auto& entry : groups) { entry.second.Release(); }
    return removed;
}

bool SlabPool::Clear() noexcept {
    auto& groups{registry_.groups};
    // 全IDを先に確認し、使用中なら取得した権利だけを戻す。部分的には解放しない。
    for (auto position{groups.begin()}; position != groups.end(); ++position) {
        if (!position->second.TryAcquire()) {
            for (auto acquired{groups.begin()}; acquired != position; ++acquired) { acquired->second.Release(); }
            return false;
        }
    }
    // 空のmapと交換し、登録に伴うノード・バケットと全Slabを解放する。
    decltype(registry_.groups) empty{};
    groups.swap(empty);
    return true;
}

SlabPool::View SlabPool::AcquireView(PoolId id, const void* tag) const noexcept {
    auto* group{Find(id)};
    if (group == nullptr || group->typeTag != tag || !group->TryAcquire()) {
        return {};
    }
    return {ReadChunk(group->chunks.cbegin(), group->chunks.cend()), group->count, group};
}
void SlabPool::ReleaseView(View& view) noexcept {
    auto* group{view.group};
    view = {};
    if (group != nullptr) { group->Release(); }
}
}
