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
#include "dvv/gpu/gpu_fwd.h"
#include "dvv/media/frame.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"

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
// What a Lease holds (§5.2: the store and the entry).
struct LeaseCore {
    std::shared_ptr<FrameStore> store;
    std::shared_ptr<Entry> entry;
    ViewSpec view{};
    Binding binding{};
    std::uint64_t id = 0;
    LeaseKind kind = LeaseKind::Read;
};
}  // namespace detail

// A move-only handle that keeps one view of one store entry alive. Read leases count as readers,
// pin leases as pins; the destructor releases under the store mutex (binding, entry, store last).
template <LeaseKind K>
class Lease {
public:
    static constexpr LeaseKind KIND = K;
    Lease() = default;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease();
    [[nodiscard]] explicit operator bool() const noexcept { return core_.entry != nullptr; }
    // Valid leases only:
    [[nodiscard]] const FrameKey& key() const noexcept;
    [[nodiscard]] const FrameDesc& desc() const noexcept;
    [[nodiscard]] const ViewSpec& view() const noexcept { return core_.view; }
    [[nodiscard]] Fidelity fidelity() const noexcept;
    [[nodiscard]] const Binding& binding() const noexcept { return core_.binding; }
    // The host planes of a HostPlanes binding; null for an empty lease or another binding. Valid
    // while this lease (or a share of it) lives.
    [[nodiscard]] const HostPlanesB* planes() const noexcept;
    // When the content is ready: the sync point given to adopt, or {} (always reached) for a frame
    // the store made.
    [[nodiscard]] SyncPoint ready() const noexcept;
    // A second lease on the same entry and view, counted like any lease (one more reader for Read,
    // one more pin for Pin) and listed in outstandingLeases() with `where`. Takes the store mutex.
    // An empty lease gives an empty lease (no lock). Valid also after the origin was detached: the
    // entry lives while any lease holds it.
    [[nodiscard]] Lease share(std::source_location where = std::source_location::current()) const;

private:
    friend class FrameStore;
    explicit Lease(detail::LeaseCore core) noexcept : core_(std::move(core)) {}
    void reset() noexcept;
    detail::LeaseCore core_;
};
using ReadLease = Lease<LeaseKind::Read>;
using PinLease = Lease<LeaseKind::Pin>;
extern template class Lease<LeaseKind::Read>;
extern template class Lease<LeaseKind::Pin>;

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
    std::size_t pending = 0;  // allocated for frames being made, not yet published
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

// Lossless: the unpacked planes of the original; Exact: the original format.
enum class OriginalFidelity : std::uint8_t { Lossless, Exact };
struct OriginalRequest {
    Api api = Api::HostPlanes;
    Domain domain{};
    OriginalFidelity fidelity = OriginalFidelity::Lossless;
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
    // Cancels the origin's queued and running produce and conversion steps: their acquires end
    // with Errc::Cancelled. Detaching an unknown origin does nothing.
    void detach(OriginId origin);

    // `key` and `want` are taken by value: the tasks are lazy, and a reference to a temporary
    // would dangle when the task is awaited later.
    [[nodiscard]] Task<Result<ReadLease>> acquire(
        FrameKey key, ViewSpec want, IExecutor& resume_on,
        std::source_location where = std::source_location::current());
    [[nodiscard]] Task<Result<PinLease>> pin(
        FrameKey key, ViewSpec view, IExecutor& resume_on,
        std::source_location where = std::source_location::current());
    // The key's original as `request` asks, colour forced to the frame's own. Plans nothing
    // itself: an acquire of originalView() (Exact) or planesView() (Lossless). Errc::Internal for
    // a detached or unknown origin. `request` is taken by value: the task is lazy.
    [[nodiscard]] Task<Result<OriginalRead>> acquireOriginal(
        FrameKey key, OriginalRequest request, IExecutor& resume_on,
        std::source_location where = std::source_location::current());
    // Deprecated (removed in 1.0.0): acquireOriginal(key, {api, domain, fidelity}, resume_on).
    [[nodiscard]] Task<Result<OriginalRead>> acquireOriginal(
        FrameKey key, Api api, Domain domain, IExecutor& resume_on, bool require_exact = false,
        std::source_location where = std::source_location::current());
    [[nodiscard]] Result<ReadLease> adopt(
        const FrameKey& key, std::shared_ptr<IFrame> frame, SyncPoint ready = {},
        std::source_location where = std::source_location::current());
    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] std::vector<LeaseInfo> outstandingLeases() const;
    // Evicts unleased, unpinned entries until bytes + pending <= target_bytes.
    void trim(Domain domain, std::size_t target_bytes);

private:
    template <LeaseKind>
    friend class Lease;
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
    [[nodiscard]] detail::LeaseCore shareLease(const detail::LeaseCore& core,
                                               std::source_location where);
    // An exception in the completion path terminates: it would strand the waiters and poison the
    // key.
    [[nodiscard]] Task<Result<void>> runPlan(detail::Plan plan) noexcept;
    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> runStep(
        const detail::Plan& plan, const detail::Step& step, std::shared_ptr<detail::Entry> input,
        ViewSpec input_view);
    [[nodiscard]] Result<void> publishLocked(detail::Entry& entry, std::shared_ptr<IFrame>& frame,
                                             std::vector<detail::Waiter*>& woken);
    // `cancelled`: the plan's origin was detached; the steps do not count in failures.
    void failStepsLocked(detail::Plan& plan, std::size_t from, const Error& error,
                         std::vector<detail::Waiter*>& woken, bool cancelled);
    void failSteps(detail::Plan& plan, std::size_t from, const Error& error, bool cancelled);
    static void wake(std::span<detail::Waiter* const> waiters) noexcept;
    void releaseReaderLocked(detail::Entry& entry) noexcept;
    void touchLocked(detail::Entry& entry) noexcept;
    [[nodiscard]] std::shared_ptr<detail::Entry> removeFromIndexLocked(
        detail::Entry& entry) noexcept;
    void evictLocked(std::size_t tier_index, std::size_t target,
                     std::vector<std::shared_ptr<detail::Entry>>& dropped);
    // A frame being made counts in its tier from allocation to publish (TierStats::pending).
    void reserveLocked(detail::Entry& output, std::size_t bytes) noexcept;  // tier.pending += bytes
    void settleLocked(detail::Entry& output) noexcept;  // tier.pending -= reserved; reserved = 0
    // Settles `bytes` of the entry's reservation: an allocation that failed.
    void settleLocked(detail::Entry& output, std::size_t bytes) noexcept;
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
