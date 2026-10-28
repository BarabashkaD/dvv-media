#pragma once

// The frame store (M2 design §5.1, §5.2): one index of frames per key, single-flight creation,
// read and pin leases, adoption, trimming and statistics.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/frame.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"

namespace dvv::gpu {
class ExecutionContext;
}  // namespace dvv::gpu

namespace dvv::media {

class FrameStore;
enum class LeaseKind : std::uint8_t { Read, Pin };

namespace detail {
struct Entry;
struct StoreState;
struct StoreAccess;
struct Waiter;
struct Step;
struct Plan;
struct Prepared;
struct Record;
class Converter;
// What ReadLease and PinLease hold (§5.2: the store and the entry).
struct LeaseCore {
    std::shared_ptr<FrameStore> store;
    std::shared_ptr<Entry> entry;
    ViewSpec view{};
    Binding binding{};
    std::uint64_t id = 0;
    LeaseKind kind = LeaseKind::Read;
};
}  // namespace detail

// Move-only handle; keeps the view alive.
class ReadLease {
public:
    ReadLease() = default;
    ReadLease(ReadLease&& other) noexcept;
    ReadLease& operator=(ReadLease&& other) noexcept;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ~ReadLease();  // under the store mutex: readers-- and LRU touch (§5.2)
    [[nodiscard]] explicit operator bool() const noexcept { return core_.entry != nullptr; }
    // Valid leases only:
    [[nodiscard]] const FrameKey& key() const noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept;
    [[nodiscard]] const ViewSpec& view() const noexcept { return core_.view; }
    [[nodiscard]] Fidelity fidelity() const noexcept;
    [[nodiscard]] const Binding& binding() const noexcept { return core_.binding; }
    [[nodiscard]] SyncPoint ready() const noexcept;  // M2: always reached (D10)

private:
    friend class FrameStore;
    explicit ReadLease(detail::LeaseCore core) noexcept : core_(std::move(core)) {}
    void reset() noexcept;
    detail::LeaseCore core_;
};

// The same shape and accessors as ReadLease; counts as a pin, not a reader.
class PinLease {
public:
    PinLease() = default;
    PinLease(PinLease&& other) noexcept;
    PinLease& operator=(PinLease&& other) noexcept;
    PinLease(const PinLease&) = delete;
    PinLease& operator=(const PinLease&) = delete;
    ~PinLease();  // under the store mutex: pins-- and LRU touch (§5.2)
    [[nodiscard]] explicit operator bool() const noexcept { return core_.entry != nullptr; }
    // Valid leases only:
    [[nodiscard]] const FrameKey& key() const noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept;
    [[nodiscard]] const ViewSpec& view() const noexcept { return core_.view; }
    [[nodiscard]] Fidelity fidelity() const noexcept;
    [[nodiscard]] const Binding& binding() const noexcept { return core_.binding; }
    [[nodiscard]] SyncPoint ready() const noexcept;  // M2: always reached (D10)

private:
    friend class FrameStore;
    explicit PinLease(detail::LeaseCore core) noexcept : core_(std::move(core)) {}
    void reset() noexcept;
    detail::LeaseCore core_;
};

// A read of a key's original (§6.6): only FrameStore::acquireOriginal makes one.
class OriginalRead {
public:
    [[nodiscard]] const ReadLease& lease() const noexcept { return lease_; }  // Exact or Lossless

private:
    friend class FrameStore;
    explicit OriginalRead(ReadLease lease) noexcept : lease_(std::move(lease)) {}
    ReadLease lease_;
};

struct TierStats {
    Domain domain;
    std::size_t bytes = 0, budget = 0, views = 0;
};
struct ConverterStats {
    std::string name;
    std::uint64_t runs = 0, bytes = 0, nanoseconds = 0;
};
struct StoreStats {
    std::vector<TierStats> tiers;        // host first, then staging and device when present
    std::uint64_t frames = 0;            // published entries in the index
    std::uint64_t leases = 0, pins = 0;  // live ReadLeases / PinLeases
    std::uint64_t hits = 0;       // acquires served by an existing (published or pending) entry
    std::uint64_t misses = 0;     // acquires that planned
    std::uint64_t creations = 0;  // published entries made by Produce or a converter
    std::uint64_t evictions = 0, failures = 0;
    std::vector<ConverterStats> converters;  // "Produce" first, then each converter name
};
struct LeaseInfo {
    FrameKey key;
    ViewSpec view;
    LeaseKind kind = LeaseKind::Read;
    std::source_location where;
};

struct FrameStoreOptions {
    std::shared_ptr<IExecutor> cpu;                // converters, the hop before Produce (required)
    std::shared_ptr<IExecutor> io;                 // handed to sources (required)
    std::shared_ptr<gpu::ExecutionContext> gpu{};  // null: host-only store
    std::size_t hostBudget = 2ULL << 30U;
    std::size_t stagingBudget = 1ULL << 30U;
    std::size_t deviceBudget = 0;  // 0: half of deviceLocalMemoryBytes
    // M3 D6: the display plan is Produce + HostToStaging + Upload + GpuUnpack + ToDisplay.
    std::size_t maxPlanDepth = 6;
};

class FrameStore final : public Managed<FrameStore>,
                         public std::enable_shared_from_this<FrameStore> {
public:
    // Own authority. Errc::Internal for a missing cpu or io executor.
    [[nodiscard]] static Result<std::shared_ptr<FrameStore>> create(
        FrameStoreOptions options, std::source_location where = std::source_location::current());
    FrameStore(CreationKey<FrameStore> key, FrameStoreOptions options) noexcept;
    ~FrameStore();  // leases hold the store, so no entry can be leased when the store dies

    [[nodiscard]] Domain hostDomain() const noexcept;                    // {Host}
    [[nodiscard]] std::optional<Domain> stagingDomain() const noexcept;  // {HostVisible, d, gen}
    [[nodiscard]] std::optional<Domain> deviceDomain() const noexcept;   // {DeviceLocal, d, gen}

    [[nodiscard]] Result<OriginId> attach(std::shared_ptr<ISource> source);
    void detach(OriginId origin);

    // `key` and `want` are taken by value: the tasks are lazy, and a reference to a temporary
    // would dangle when the task is awaited later.
    [[nodiscard]] Task<Result<ReadLease>> acquire(
        FrameKey key, ViewSpec want, IExecutor& resume_on,
        std::source_location where = std::source_location::current());
    [[nodiscard]] Task<Result<PinLease>> pin(
        FrameKey key, ViewSpec view, IExecutor& resume_on,
        std::source_location where = std::source_location::current());
    // The key's original in `api` and `domain`, colour forced to the frame's own: with
    // require_exact the original format (Exact), otherwise the unpacked format (Lossless). Plans
    // nothing itself: an acquire of originalView() or planesView(). Errc::Internal for a detached
    // or unknown origin.
    [[nodiscard]] Task<Result<OriginalRead>> acquireOriginal(
        FrameKey key, Api api, Domain domain, IExecutor& resume_on, bool require_exact = false,
        std::source_location where = std::source_location::current());
    [[nodiscard]] Result<ReadLease> adopt(
        const FrameKey& key, std::shared_ptr<IFrame> frame, SyncPoint ready = {},
        std::source_location where = std::source_location::current());
    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] std::vector<LeaseInfo> outstandingLeases() const;
    void trim(Domain domain, std::size_t target_bytes);

private:
    friend class ReadLease;
    friend class PinLease;
    friend class ProduceContext;
    friend struct detail::StoreAccess;

    [[nodiscard]] static Result<std::shared_ptr<FrameStore>> createWith(
        FrameStoreOptions options, std::vector<std::unique_ptr<detail::Converter>> converters,
        std::optional<Domain> staging, std::optional<Domain> device, std::source_location where);
    template <class T>
    [[nodiscard]] static CreationKey<FrameStore> frameKey(ObjectId owner) {
        return CreationKey<FrameStore>(LifetimeRegistry::instance().issue<T>(owner));
    }

    [[nodiscard]] Task<Result<detail::LeaseCore>> acquireCore(FrameKey key, ViewSpec want,
                                                              LeaseKind kind, IExecutor& resume_on,
                                                              std::source_location where);
    [[nodiscard]] Result<detail::Prepared> prepareLocked(const FrameKey& key, const ViewSpec& want,
                                                         LeaseKind kind);
    [[nodiscard]] Result<detail::Plan> planLocked(const FrameKey& key, detail::Record& record,
                                                  const ViewSpec& want);
    [[nodiscard]] detail::LeaseCore finishLease(std::shared_ptr<detail::Entry> entry, ViewSpec view,
                                                LeaseKind kind, std::source_location where);
    // An exception in the completion path terminates: it would strand the waiters and poison the
    // key.
    [[nodiscard]] Task<Result<void>> runPlan(detail::Plan plan) noexcept;
    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> runStep(
        const detail::Plan& plan, const detail::Step& step, std::shared_ptr<detail::Entry> input,
        ViewSpec input_view);
    [[nodiscard]] Result<void> publishLocked(detail::Entry& entry, std::shared_ptr<IFrame>& frame,
                                             std::vector<detail::Waiter*>& woken);
    void failStepsLocked(detail::Plan& plan, std::size_t from, const Error& error,
                         std::vector<detail::Waiter*>& woken);
    void failSteps(detail::Plan& plan, std::size_t from, const Error& error);
    static void wake(std::span<detail::Waiter* const> waiters) noexcept;
    void releaseReaderLocked(detail::Entry& entry) noexcept;
    void touchLocked(detail::Entry& entry) noexcept;
    [[nodiscard]] std::shared_ptr<detail::Entry> removeFromIndexLocked(
        detail::Entry& entry) noexcept;
    void evictLocked(std::size_t tier_index, std::size_t target,
                     std::vector<std::shared_ptr<detail::Entry>>& dropped);
    void releaseLease(detail::LeaseCore& core) noexcept;
    [[nodiscard]] std::size_t slotOf(const detail::Converter& converter) const noexcept;
    [[nodiscard]] std::optional<std::size_t> tierOf(const Domain& domain) const noexcept;
    [[nodiscard]] std::optional<std::size_t> tierOfKind(MemKind kind) const noexcept;

    std::unique_ptr<detail::StoreState> state_;
};

// {desc.format, desc.colour, Any, api, domain, 0}
[[nodiscard]] ViewSpec originalView(const FrameDesc& desc, Api api, Domain domain);
// {unpackedFormat(desc.format), desc.colour, Any, api, domain, 0}
[[nodiscard]] ViewSpec planesView(const FrameDesc& desc, Api api, Domain domain);

}  // namespace dvv::media
