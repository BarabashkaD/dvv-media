#pragma once
// The frame store's shared state (M2 design §5.2). Private to dvv-media.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"

namespace dvv::media::detail {

enum class EntryState : std::uint8_t { Pending, Published, Failed };

// A coroutine waiting for a pending entry. Lives in the waiting coroutine's frame; the store
// never destroys it (no cancellation in M2; M3 removes nodes from Entry::waiters, D10 hook).
struct Waiter {
    std::coroutine_handle<> continuation;
    IExecutor* resumeOn = nullptr;
    // Set before resumption when the entry failed. The waiter's reader of the entry was reserved
    // when it chose to join (prepareLocked, planLocked).
    std::optional<Error> error;
};

struct Entry {
    // Fixed at insertion.
    FrameKey key;
    FrameDesc desc;
    ViewSpec planned{};  // the promised view (pending); views[0] when adopted
    Fidelity fidelity = Fidelity::Exact;
    bool root = false;  // the key's original: made by Produce or adopt
    bool adopted = false;
    std::size_t tier = 0;  // index into StoreState::tiers
    // Guarded by StoreState::mutex.
    EntryState state = EntryState::Pending;
    bool inRecord = true;  // false once evicted, detached or failed
    bool counted = false;  // `bytes` are included in tiers[tier].bytes
    bool linked = false;   // in tiers[tier].lru at lruPosition
    std::list<Entry*>::iterator lruPosition{};
    std::uint32_t readers = 0;  // read leases, plan steps reading it, results reserved for waiters
    std::uint32_t pins = 0;
    // An exception in the completion path terminates: it would strand the waiters and poison the
    // key.
    std::vector<Waiter*> waiters;
    std::optional<Error> failure;
    // Written under the mutex before `state` becomes Published, immutable afterwards: lease
    // holders and plan steps read them without the mutex.
    std::shared_ptr<IFrame> frame;
    std::vector<ViewSpec> views;  // frame->nativeViews()
    SyncPoint ready{};
    std::size_t bytes = 0;
};

struct Record {
    FrameDesc desc;
    FormatId unpacked = 0;                        // unpackedFormat(desc.format)
    std::vector<std::shared_ptr<Entry>> entries;  // published and pending
};

struct Origin {
    std::shared_ptr<ISource> source;
    FrameDesc desc;
    FormatId unpacked = 0;
};

struct Tier {
    Domain domain;
    std::size_t budget = 0;
    std::size_t bytes = 0;
    std::list<Entry*> lru;  // published in-index entries; front = least recently used
};

struct ConverterSlot {  // relaxed counters; Produce is slot 0
    std::string name;
    std::atomic<std::uint64_t> runs{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> nanoseconds{0};
};

struct Counters {
    std::uint64_t hits = 0, misses = 0, creations = 0, evictions = 0, failures = 0;
};

// A request the store's static graph cannot serve (§5.2). Cached only while every state of the
// key is reachable from Produce, so the answer depends on nothing else and is permanent.
struct NoPathKey {
    FormatId format = 0;  // the original format
    StateKey target;
    LayoutPolicy layout = LayoutPolicy::Any;
    bool operator==(const NoPathKey&) const = default;
};

using HostAllocator =
    std::function<Result<platform::AlignedBuffer>(std::size_t bytes, std::size_t alignment)>;

struct StoreState {
    FrameStoreOptions options;
    std::vector<std::unique_ptr<Converter>> converters;  // fixed after create
    std::vector<std::size_t> converterSlot;              // converters[i] -> slots index
    std::vector<std::unique_ptr<ConverterSlot>> slots;   // [0] = "Produce"
    // Allocates the host frames of Produce (test seam). Set before use: read without the mutex.
    HostAllocator hostAllocator = platform::allocateAligned;

    mutable std::mutex mutex;       // guards everything below
    std::vector<NoPathKey> noPath;  // never shrinks; at most formats x states x layouts
    std::unordered_map<OriginId, Origin> origins;
    OriginId nextOrigin = 1;
    std::unordered_map<FrameKey, Record, FrameKeyHash> records;
    std::vector<Tier> tiers;  // [0] host, then staging, device
    std::unordered_map<std::uint64_t, LeaseInfo> leases;
    std::uint64_t nextLease = 1;
    Counters counters;
};

struct Step {
    const Converter* converter = nullptr;  // null: Produce
    std::shared_ptr<Entry> output;         // pending, created by this plan
};
struct Plan {
    std::shared_ptr<Entry> start;  // input of steps[0]; null when steps[0] is Produce
    ViewSpec startView{};
    bool startPending = false;        // join `start` first; either way the plan holds one reader
    std::shared_ptr<ISource> source;  // Produce only (R11)
    std::vector<Step> steps;
};
struct Prepared {
    enum class Kind : std::uint8_t { Hit, Join, Plan };
    Kind kind = Kind::Hit;
    std::shared_ptr<Entry> entry;  // Hit/Join: the chosen entry; Plan: the last output
    ViewSpec view{};               // the view the lease will have
    Plan plan;
};

// co_await JoinAwaiter(state, entry, executor): waits until a pending entry is published or failed
// (its error). The caller reserved its reader of the entry under the same lock as its decision to
// join, so the entry cannot be evicted before the caller leases it, even when it is published
// before the caller registers. Resumed by the completer through a post to `resume_on`; continues
// inline when the entry completed before the caller could register.
class JoinAwaiter {
public:
    JoinAwaiter(StoreState& state, std::shared_ptr<Entry> entry, IExecutor& resume_on) noexcept
        : state_(&state), entry_(std::move(entry)) {
        waiter_.resumeOn = &resume_on;
    }
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> continuation) {
        waiter_.continuation = continuation;
        const std::scoped_lock lock(state_->mutex);
        switch (entry_->state) {
            case EntryState::Pending:
                entry_->waiters.push_back(&waiter_);  // nothing of *this is touched after unlock
                return true;
            case EntryState::Published: return false;
            case EntryState::Failed: waiter_.error = entry_->failure; return false;
        }
        return false;
    }
    [[nodiscard]] Result<void> await_resume() {
        if (waiter_.error) {
            return std::unexpected(std::move(*waiter_.error));
        }
        return {};
    }

private:
    StoreState* state_;
    std::shared_ptr<Entry> entry_;
    Waiter waiter_;
};

// Test and converter hooks.
struct StoreAccess {
    struct TestDomains {
        std::optional<Domain> staging;
        std::optional<Domain> device;
    };
    // A store with these converters (instead of the defaults) and optional fake domains.
    [[nodiscard]] static Result<std::shared_ptr<FrameStore>> create(
        FrameStoreOptions options, std::vector<std::unique_ptr<Converter>> converters,
        TestDomains domains = {});
    // A frame owned by the store (CreationKey<FrameStore>), for converters.
    template <class T, class... Args>
    [[nodiscard]] static std::shared_ptr<T> makeFrame(FrameStore& store, Args&&... args) {
        return std::make_shared<T>(FrameStore::frameKey<T>(store.objectId()),
                                   std::forward<Args>(args)...);
    }
    // Waiters registered on the pending entries of a key.
    [[nodiscard]] static std::size_t waiterCount(const FrameStore& store, const FrameKey& key);
    // Internal with the first violated rule: per tier, bytes == the sum of its counted entries
    // and every linked entry is published, in the index and counted; no empty record; per record,
    // at most one non-failed original and no two non-failed entries with the same planned view
    // and fidelity; per entry, read leases <= readers and pin leases == pins. Every rule holds
    // whenever the mutex is free, so it may also run while other threads acquire, release and trim.
    // A lease is attributed to the in-index entry of its key that offers its view, so call it while
    // no lease of an evicted or detached entry is held.
    [[nodiscard]] static Result<void> checkInvariants(const FrameStore& store);
    [[nodiscard]] static StoreState& state(FrameStore& store) noexcept;
    // Entries of the no-path cache.
    [[nodiscard]] static std::size_t noPathCount(const FrameStore& store);
    // Replaces the host allocator of Produce; call before any acquire.
    static void setHostAllocator(FrameStore& store, HostAllocator allocator);
    // The planned view and fidelity of every published in-index entry of a key, in insertion
    // order.
    [[nodiscard]] static std::vector<std::pair<ViewSpec, Fidelity>> views(const FrameStore& store,
                                                                          const FrameKey& key);
};

}  // namespace dvv::media::detail
