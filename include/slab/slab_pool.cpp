/** @file slab_pool.cpp
 * @brief ポインタ用仮想メモリ、ID単位の排他取得、フレーム末尾の末尾入れ替え除去。
 */
#include "slab_pool.h"

#if defined(_WIN32)

#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <new>

namespace slab {
size_t SlabPool::GetPageBytes() noexcept {
    static const size_t bytes{[]() noexcept {
        SYSTEM_INFO information{};
        GetSystemInfo(&information);
        return static_cast<size_t>(information.dwPageSize);
    }()};
    return bytes;
}

bool SlabPool::RoundUp(size_t value, size_t unit, size_t& result) noexcept {
    const auto remainder{value % unit};
    result = value;
    if (remainder == 0) { return true; }
    const auto extra{unit - remainder};
    if (value > MaximumBytes - extra) { return false; }
    result += extra;
    return true;
}

/** @brief 一度に予約した仮想領域。コミットを増やしても先頭アドレスは不変。 */
struct SlabPool::Region {
    uintptr_t address{}; ///< VirtualAllocで予約した先頭アドレス。予約前は0。
    size_t bytes{}; ///< このRegion全体の予約バイト数。
    size_t committed{}; ///< 先頭から連続してコミット済みのバイト数。

    /** @brief 仮想領域をまだ予約していない管理ノードを作る。 */
    Region() noexcept = default;
    /** @brief 仮想領域の二重解放を防ぐためコピーは禁止する。 */
    Region(const Region&) = delete;
    /** @brief 仮想領域の二重解放を防ぐためコピー代入は禁止する。 */
    Region& operator=(const Region&) = delete;

    /** @brief listから破棄されるとき、所有する仮想領域を予約ごと解放する。 */
    ~Region() noexcept {
        if (address != 0) { (void)VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE); }
    }
};

/** @brief コミット後に寿命を開始したComponent*配列の管理情報。前後関係はlistが管理する。 */
struct SlabPool::Chunk {
    Component** data{}; ///< コミット済みのComponent*配列。メモリの所有者はRegion。
    size_t first{}; ///< Group全体で数えた、この区間の先頭添字。
    size_t capacity{}; ///< この区間に格納できるポインタ数。
};

SlabPool::Group::Group() noexcept = default;
SlabPool::Group::~Group() noexcept = default;

void SlabPool::Group::Acquire() noexcept {
    for (;;) {
        // CAS失敗時にはexpectedが書き換わるので、試行ごとにfalseへ戻す。
        bool expected{};
        if (inUse.compare_exchange_weak(expected, true, memory_order_acquire, memory_order_relaxed)) { return; }
        // 既に解放済みなら即座に戻る。待機後も、他の取得者と競合するためCASを再試行する。
        inUse.wait(true, memory_order_relaxed);
    }
}
void SlabPool::Group::Release() noexcept {
    inUse.store(false, memory_order_release);
    inUse.notify_one();
}

SlabPool::Group::Access::Access(Group* group) noexcept {
    if (group != nullptr) { group->Acquire(); owned = group; }
}
SlabPool::Group::Access::~Access() noexcept { if (owned != nullptr) { owned->Release(); } }
SlabPool::Group::Access::operator bool() const noexcept { return owned != nullptr; }

SlabPool::SlabPool() noexcept = default;

SlabPool& SlabPool::GetInstance() noexcept {
    static SlabPool instance{};
    return instance;
}

SlabPool::~SlabPool() noexcept {
    Clear();
}

SlabPool::Group* SlabPool::Find(PoolId id) const noexcept {
    const auto position{registry_.groups.find(id)};
    if (position == registry_.groups.end()) { return nullptr; }
    return &position->second;
}

bool SlabPool::Register(PoolId id, const Options& options) noexcept {
    const auto maximumCount{MaximumBytes / PointerBytes};
    if (Find(id) != nullptr || options.reservedCapacity == 0 || options.commitCount == 0 ||
        options.initialCapacity > options.reservedCapacity ||
        options.reservedCapacity > maximumCount || options.commitCount > maximumCount ||
        options.freeThreshold > maximumCount) { return false; }
    // 個数から必要バイト数へ変換し、ページの余りも格納に使える容量として確保する。
    const auto pageBytes{GetPageBytes()};
    size_t initialBytes{};
    size_t commitStep{};
    size_t reservationStep{};
    if (!RoundUp(options.initialCapacity * PointerBytes, pageBytes, initialBytes) ||
        !RoundUp(options.commitCount * PointerBytes, pageBytes, commitStep) ||
        !RoundUp(options.reservedCapacity * PointerBytes, pageBytes, reservationStep)) { return false; }
    auto& groups{registry_.groups};
    if (groups.empty()) { groups.reserve(InitialGroupCapacity); }
    // 登録済みIDは冒頭で除外済み。operator[]で空のGroupを作って設定する。
    auto& group{groups[id]};
    group.commitStep = initialBytes;
    group.reservationStep = reservationStep;
    group.freeThreshold = options.freeThreshold;
    group.preTouch = options.preTouch;
    group.allowAdditionalReserve = options.allowAdditionalReserve;
    bool prepared{ReserveGroup(group)};
    if (prepared && initialBytes != 0) {
        prepared = CommitGroup(group);
    }
    // 初期容量をまとめて準備した後は、通常の追加コミット量に戻す。
    group.commitStep = commitStep;
    if (!prepared) {
        // Groupのデストラクタが、途中まで準備した領域も解放する。
        groups.erase(id);
        return false;
    }
    return true;
}

bool SlabPool::ReserveGroup(Group& group) noexcept {
    if (!group.allowAdditionalReserve && group.reservedBytes != 0) { return false; }
    const auto bytes{group.reservationStep};
    if (bytes > MaximumBytes - group.reservedBytes) { return false; }
    auto& region{group.regions.emplace_back()};
    void* address{VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS)};
    if (address == nullptr) { group.regions.pop_back(); return false; }
    region.address = reinterpret_cast<uintptr_t>(address);
    region.bytes = bytes;
    group.reservedBytes += bytes;
    return true;
}

bool SlabPool::CommitGroup(Group& group) noexcept {
    const auto committedBytes{group.capacity * PointerBytes};
    auto additionalBytes{group.commitStep};
    if (!group.allowAdditionalReserve) {
        additionalBytes = min(additionalBytes, group.reservedBytes - committedBytes);
    }
    if (additionalBytes == 0 || additionalBytes > MaximumBytes - committedBytes) { return false; }
    const auto targetBytes{committedBytes + additionalBytes};
    while (group.reservedBytes < targetBytes) {
        if (!ReserveGroup(group)) { return false; }
    }
    const auto targetCapacity{targetBytes / PointerBytes};
    for (auto& region : group.regions) {
        if (group.capacity >= targetCapacity) { break; }
        const auto available{region.bytes - region.committed};
        if (available == 0) { continue; }
        const auto bytes{min((targetCapacity - group.capacity) * PointerBytes, available)};
        auto& chunk{group.chunks.emplace_back()};
        void* address{reinterpret_cast<void*>(region.address + region.committed)};
        void* committed{VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE)};
        if (committed == nullptr) { group.chunks.pop_back(); return false; }
        if (group.preTouch) {
            const auto pageBytes{GetPageBytes()};
            for (size_t offset{}; offset < bytes; offset += pageBytes) {
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

bool SlabPool::CommitAtFrameEnd() noexcept {
    bool completed{true};
    for (auto& entry : registry_.groups) {
        auto& group{entry.second};
        // 閾値は登録後に変わらない。自動補充のIDは走査権も取得せずに除外する。
        if (group.freeThreshold == 0) { continue; }
        Group::Access access{&group};
        if (group.capacity - group.count > group.freeThreshold) { continue; }
        // 一つのIDで失敗しても、残りのIDの補充は続ける。
        if (!CommitGroup(group)) { completed = false; }
    }
    return completed;
}

SlabPool::Slice SlabPool::ReadChunk(ChunkIterator chunk) noexcept {
    return {chunk->data, chunk->capacity, next(chunk)};
}
SlabPool::Slice SlabPool::Locate(const Group& group, Index index) noexcept {
    const auto end{group.chunks.cend()};
    for (auto chunk{group.chunks.cbegin()}; chunk != end; ++chunk) {
        if (index >= chunk->first && index - chunk->first < chunk->capacity) {
            const auto offset{index - chunk->first};
            return {chunk->data + offset, chunk->capacity - offset, next(chunk)};
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
        (group->freeThreshold != 0 || !CommitGroup(*group))) { return InvalidIndex; }
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
    size_t removed{};
    for (auto& entry : registry_.groups) {
        auto& group{entry.second};
        // このIDだけを取得する。continueや反復の終了時にも走査権を解放する。
        Group::Access access{&group};
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
    return removed;
}

void SlabPool::Clear() noexcept {
    auto& groups{registry_.groups};
    // 新規操作の停止と待機中の操作の完了は呼び出し元が保証する。この検査は同期処理ではない。
    for ([[maybe_unused]] const auto& entry : groups) {
        assert(!entry.second.inUse.load(memory_order_relaxed));
    }
    // 空のmapと交換し、登録に伴うノード・バケットと全Slabを解放する。
    decltype(registry_.groups) empty{};
    groups.swap(empty);
}

SlabPool::View SlabPool::AcquireView(PoolId id) const noexcept {
    auto* group{Find(id)};
    if (group == nullptr) { return {}; }
    group->Acquire();
    // 空のIDではChunkを参照しない。走査権は保持し、有効な空Rangeとして返す。
    if (group->count == 0) { return {{}, 0, group}; }
    return {ReadChunk(group->chunks.cbegin()), group->count, group};
}
void SlabPool::ReleaseView(View& view) noexcept {
    auto* group{view.group};
    view = {};
    if (group != nullptr) { group->Release(); }
}
}

#else

namespace slab {
// Windows以外はAPIの呼び出しを正常扱いで受け流す。ポインタや実体は保持しない。
SlabPool::SlabPool() noexcept = default;
SlabPool::~SlabPool() noexcept = default;

SlabPool& SlabPool::GetInstance() noexcept {
    static SlabPool instance{};
    return instance;
}

bool SlabPool::Register(PoolId, const Options&) noexcept { return true; }
SlabPool::Index SlabPool::Add(PoolId, Component*) noexcept { return 0; }
bool SlabPool::NotifyStateChanged(PoolId) noexcept { return true; }
size_t SlabPool::RemoveInactive() noexcept { return 0; }
bool SlabPool::CommitAtFrameEnd() noexcept { return true; }
void SlabPool::Clear() noexcept {}
}

#endif // defined(_WIN32)

namespace slab {
const SlabPool::Options& SlabPool::DefaultOptions() noexcept {
    static constexpr Options options{};
    return options;
}
}
