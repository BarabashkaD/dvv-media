#include "dvv/media/frame_store.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/media/display.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "planner.h"
#include "store_state.h"

namespace dvv::media {

namespace {

// The device tier's budget in a store with a fake device domain and deviceBudget 0.
constexpr std::size_t TEST_DEVICE_BUDGET = std::size_t{1} << 30U;

[[nodiscard]] std::string_view apiName(Api api) noexcept {
    switch (api) {
        case Api::HostPlanes: return "HostPlanes";
        case Api::VkBuffer: return "VkBuffer";
        case Api::VkImage: return "VkImage";
    }
    return "?";
}

[[nodiscard]] std::string_view kindName(MemKind kind) noexcept {
    switch (kind) {
        case MemKind::Host: return "Host";
        case MemKind::HostVisible: return "HostVisible";
        case MemKind::DeviceLocal: return "DeviceLocal";
    }
    return "?";
}

[[nodiscard]] std::string_view layoutName(LayoutPolicy layout) noexcept {
    switch (layout) {
        case LayoutPolicy::Any: return "Any";
        case LayoutPolicy::Tight: return "Tight";
        case LayoutPolicy::RowAlign256: return "RowAlign256";
        case LayoutPolicy::VkOptimal: return "VkOptimal";
    }
    return "?";
}

// "gray@planes (HostPlanes, Host, Any)" for messages.
[[nodiscard]] std::string viewText(const ViewSpec& view) {
    std::string text =
        std::format("{} ({}, {}, {}", descriptor(view.format).name, apiName(view.api),
                    kindName(view.domain.kind), layoutName(view.layout));
    if (view.downscaleLog2 != 0) {
        text += std::format(", downscale log2 {}", static_cast<unsigned>(view.downscaleLog2));
    }
    text += ')';
    return text;
}

// The graph state of a view of the key's frame: nullopt for a format other than the original, its
// unpacked form and the display format, a colour other than the frame's (the display format: other
// than displayColour()), a downscale, or a domain the store has no tier for.
[[nodiscard]] std::optional<detail::StateKey> stateOf(const detail::Record& record,
                                                      std::span<const detail::Tier> tiers,
                                                      const ViewSpec& view) noexcept {
    const bool has_tier = std::ranges::any_of(
        tiers, [&](const detail::Tier& tier) { return tier.domain == view.domain; });
    if (isDisplay(view.format)) {
        // createWith interned the display colour, so displayColour() cannot throw here.
        if (view.colour != displayColour() || view.downscaleLog2 != 0 || !has_tier) {
            return std::nullopt;
        }
        return detail::StateKey{
            .unpacked = false, .api = view.api, .kind = view.domain.kind, .display = true};
    }
    bool unpacked = false;
    if (view.format == record.desc.format) {
        unpacked = false;
    } else if (view.format == record.unpacked) {
        unpacked = true;
    } else {
        return std::nullopt;
    }
    if (view.colour != record.desc.colour || view.downscaleLog2 != 0 || !has_tier) {
        return std::nullopt;
    }
    return detail::StateKey{.unpacked = unpacked, .api = view.api, .kind = view.domain.kind};
}

// Evicts unleased entries of every tier above its budget (§5.2). A tier's domain and budget are
// fixed after create, so they are read without the mutex; trim locks.
void evictOverBudget(FrameStore& store, const detail::StoreState& s) {
    for (const detail::Tier& tier : s.tiers) {
        store.trim(tier.domain, tier.budget);
    }
}

// Before the one retry after OutOfMemory (D10): evicts the tier down to half its budget.
void trimForRetry(FrameStore& store, const detail::StoreState& s, std::size_t tier) {
    store.trim(s.tiers[tier].domain, s.tiers[tier].budget / 2);
}

}  // namespace

// ---- ProduceContext ----

ProduceContext::ProduceContext(FrameStore& store, FrameKey key, FrameDesc desc,
                               IExecutor& io) noexcept
    : store_(&store), key_(key), desc_(desc), io_(&io) {}

Result<std::shared_ptr<HostFrame>> ProduceContext::allocateHost(const FrameDesc& desc,
                                                                std::size_t capacity,
                                                                std::size_t alignment,
                                                                std::size_t data_offset) {
    if (desc != desc_) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: allocateHost asked for another "
                                     "description than the source's",
                                     key_.index, key_.origin));
    }
    DVV_TRY(const FrameGeometry layout, geometry(desc.format, desc.width, desc.height));
    if (data_offset > capacity || layout.bytes > capacity - data_offset) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: {} bytes of frame data at offset {} "
                                     "do not fit into {} bytes",
                                     key_.index, key_.origin, layout.bytes, data_offset, capacity));
    }
    const detail::StoreState& s = *store_->state_;
    Result<platform::AlignedBuffer> allocated = s.hostAllocator(capacity, alignment);
    if (!allocated && allocated.error().code == Errc::OutOfMemory) {
        // D10: evict the host tier to half its budget, then allocate once more.
        trimForRetry(*store_, s, 0);
        allocated = s.hostAllocator(capacity, alignment);
    }
    DVV_TRY(platform::AlignedBuffer buffer, std::move(allocated));
    const ViewSpec view{desc.format,     desc.colour,          LayoutPolicy::Tight,
                        Api::HostPlanes, store_->hostDomain(), 0};
    auto frame = std::make_shared<HostFrame>(FrameStore::frameKey<HostFrame>(store_->objectId()),
                                             desc, view, std::move(buffer), capacity, data_offset);
    evictOverBudget(*store_, s);
    return frame;
}

Result<FrameRef> ProduceContext::publish(std::shared_ptr<IFrame> frame) {
    if (!frame) {
        return makeError(Errc::Internal, std::format("frame {} of origin {}: published no frame",
                                                     key_.index, key_.origin));
    }
    if (frame_) {
        return makeError(Errc::Internal, std::format("frame {} of origin {}: published twice",
                                                     key_.index, key_.origin));
    }
    if (frame->desc() != desc_) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: the published frame has another "
                                     "description than the source's",
                                     key_.index, key_.origin));
    }
    const ViewSpec original{desc_.format,    desc_.colour,         LayoutPolicy::Tight,
                            Api::HostPlanes, store_->hostDomain(), 0};
    const std::span<const ViewSpec> views = frame->nativeViews();
    if (std::ranges::find(views, original) == views.end()) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: the published frame lacks the "
                                     "original host view",
                                     key_.index, key_.origin));
    }
    FrameRef ref{key_, desc_, store_->shared_from_this()};
    frame_ = std::move(frame);
    return ref;
}

// ---- Leases ----

ReadLease::ReadLease(ReadLease&& other) noexcept : core_(std::exchange(other.core_, {})) {}

ReadLease& ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) {
        reset();
        core_ = std::exchange(other.core_, {});
    }
    return *this;
}

ReadLease::~ReadLease() {
    reset();
}

void ReadLease::reset() noexcept {
    if (core_.store) {
        core_.store->releaseLease(core_);
    }
    core_.binding = {};
    core_.entry.reset();  // may free the frame: after unlocking
    core_.store.reset();  // last: may destroy the store
}

const FrameKey& ReadLease::key() const noexcept {
    return core_.entry->key;
}
const FrameDesc& ReadLease::desc() const noexcept {
    return core_.entry->desc;
}
Fidelity ReadLease::fidelity() const noexcept {
    return core_.entry->fidelity;
}
SyncPoint ReadLease::ready() const noexcept {
    return core_.entry->ready;
}

PinLease::PinLease(PinLease&& other) noexcept : core_(std::exchange(other.core_, {})) {}

PinLease& PinLease::operator=(PinLease&& other) noexcept {
    if (this != &other) {
        reset();
        core_ = std::exchange(other.core_, {});
    }
    return *this;
}

PinLease::~PinLease() {
    reset();
}

void PinLease::reset() noexcept {
    if (core_.store) {
        core_.store->releaseLease(core_);
    }
    core_.binding = {};
    core_.entry.reset();  // may free the frame: after unlocking
    core_.store.reset();  // last: may destroy the store
}

const FrameKey& PinLease::key() const noexcept {
    return core_.entry->key;
}
const FrameDesc& PinLease::desc() const noexcept {
    return core_.entry->desc;
}
Fidelity PinLease::fidelity() const noexcept {
    return core_.entry->fidelity;
}
SyncPoint PinLease::ready() const noexcept {
    return core_.entry->ready;
}

// ---- Creation ----

FrameStore::FrameStore(CreationKey<FrameStore> key, FrameStoreOptions options) noexcept
    : Managed<FrameStore>(std::move(key)) {
    try {
        auto state = std::make_unique<detail::StoreState>();
        state->options = std::move(options);
        state_ = std::move(state);
    } catch (...) {
        state_.reset();  // createWith reports OutOfMemory
    }
}

FrameStore::~FrameStore() = default;

Result<std::shared_ptr<FrameStore>> FrameStore::create(FrameStoreOptions options,
                                                       std::source_location where) {
    std::vector<std::unique_ptr<detail::Converter>> converters;
    if (options.gpu && options.cpu && options.io) {
        // The unpack pipeline's setup error (Unsupported, R1) fails the creation.
        DVV_TRY(converters, detail::makeGpuConverters(options.gpu, options.cpu));
    } else {
        converters = detail::makeHostConverters();  // createWith reports missing executors
    }
    return createWith(std::move(options), std::move(converters), std::nullopt, std::nullopt, where);
}

Result<std::shared_ptr<FrameStore>> FrameStore::createWith(
    FrameStoreOptions options, std::vector<std::unique_ptr<detail::Converter>> converters,
    std::optional<Domain> staging, std::optional<Domain> device, std::source_location where) {
    if (!options.cpu || !options.io) {
        return makeError(Errc::Internal, "FrameStore::create needs a cpu and an io executor");
    }
    // Interned now, so the planner's state rule (stateOf, noexcept) only reads them later.
    static_cast<void>(displayFormat());
    static_cast<void>(displayColour());
    std::optional<detail::Tier> staging_tier;
    std::optional<detail::Tier> device_tier;
    if (options.gpu) {
        const gpu::ExecutionContext& context = *options.gpu;
        staging_tier.emplace();
        staging_tier->domain =
            Domain{MemKind::HostVisible, context.deviceId(), context.generation()};
        staging_tier->budget = options.stagingBudget;
        device_tier.emplace();
        device_tier->domain =
            Domain{MemKind::DeviceLocal, context.deviceId(), context.generation()};
        device_tier->budget =
            options.deviceBudget != 0
                ? options.deviceBudget
                : static_cast<std::size_t>(context.deviceInfo().deviceLocalMemoryBytes / 2);
    }
    if (staging) {
        staging_tier.emplace();
        staging_tier->domain = *staging;
        staging_tier->budget = options.stagingBudget;
    }
    if (device) {
        device_tier.emplace();
        device_tier->domain = *device;
        device_tier->budget = options.deviceBudget != 0 ? options.deviceBudget : TEST_DEVICE_BUDGET;
    }
    std::vector<detail::Tier> tiers(1);
    tiers[0].domain = Domain{};
    tiers[0].budget = options.hostBudget;
    if (staging_tier) {
        tiers.push_back(std::move(*staging_tier));
    }
    if (device_tier) {
        tiers.push_back(std::move(*device_tier));
    }

    // A converter from or to a memory kind without a tier is not registered (a host-only store
    // keeps only host-to-host converters).
    if (std::ranges::any_of(converters, [](const auto& converter) { return !converter; })) {
        return makeError(Errc::Internal, "FrameStore::create: a converter is null");
    }
    const auto has_tier = [&tiers](MemKind kind) {
        return std::ranges::any_of(
            tiers, [kind](const detail::Tier& tier) { return tier.domain.kind == kind; });
    };
    std::erase_if(converters, [&](const auto& converter) {
        return !has_tier(converter->from().kind) || !has_tier(converter->to().kind);
    });
    std::vector<std::unique_ptr<detail::ConverterSlot>> slots;
    slots.push_back(std::make_unique<detail::ConverterSlot>());
    slots[0]->name = "Produce";
    std::vector<std::size_t> converter_slot;
    converter_slot.reserve(converters.size());
    for (const auto& converter : converters) {
        std::size_t slot = 1;
        while (slot < slots.size() && slots[slot]->name != converter->name()) {
            ++slot;
        }
        if (slot == slots.size()) {
            slots.push_back(std::make_unique<detail::ConverterSlot>());
            slots.back()->name = std::string(converter->name());
        }
        converter_slot.push_back(slot);
    }

    auto store = std::make_shared<FrameStore>(
        CreationKey<FrameStore>(LifetimeRegistry::instance().issue<FrameStore>(NO_OWNER, where)),
        std::move(options));
    if (!store->state_) {
        return makeError(Errc::OutOfMemory, "FrameStore::create: out of memory");
    }
    detail::StoreState& s = *store->state_;
    s.converters = std::move(converters);
    s.converterSlot = std::move(converter_slot);
    s.slots = std::move(slots);
    s.tiers = std::move(tiers);
    return store;
}

// ---- Domains and tiers ----

Domain FrameStore::hostDomain() const noexcept {
    return Domain{};
}

std::optional<Domain> FrameStore::stagingDomain() const noexcept {
    const std::optional<std::size_t> tier = tierOfKind(MemKind::HostVisible);
    if (!tier) {
        return std::nullopt;
    }
    return state_->tiers[*tier].domain;  // fixed after create
}

std::optional<Domain> FrameStore::deviceDomain() const noexcept {
    const std::optional<std::size_t> tier = tierOfKind(MemKind::DeviceLocal);
    if (!tier) {
        return std::nullopt;
    }
    return state_->tiers[*tier].domain;
}

std::optional<std::size_t> FrameStore::tierOf(const Domain& domain) const noexcept {
    const std::vector<detail::Tier>& tiers = state_->tiers;  // the domains are fixed after create
    for (std::size_t index = 0; index < tiers.size(); ++index) {
        if (tiers[index].domain == domain) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> FrameStore::tierOfKind(MemKind kind) const noexcept {
    const std::vector<detail::Tier>& tiers = state_->tiers;
    for (std::size_t index = 0; index < tiers.size(); ++index) {
        if (tiers[index].domain.kind == kind) {
            return index;
        }
    }
    return std::nullopt;
}

std::size_t FrameStore::slotOf(const detail::Converter& converter) const noexcept {
    const detail::StoreState& s = *state_;
    for (std::size_t index = 0; index < s.converters.size(); ++index) {
        if (s.converters[index].get() == &converter) {
            return s.converterSlot[index];
        }
    }
    return 0;
}

// ---- Origins ----

Result<OriginId> FrameStore::attach(std::shared_ptr<ISource> source) {
    if (!source) {
        return makeError(Errc::Internal, "attach: no source");
    }
    const FrameDesc desc = source->info().desc;
    const FormatId unpacked = unpackedFormat(desc.format);
    detail::StoreState& s = *state_;
    // Declared before the lock: if emplace throws, the source is released after unlocking.
    detail::Origin origin{std::move(source), desc, unpacked};
    const std::scoped_lock lock(s.mutex);
    const OriginId id = s.nextOrigin;
    s.origins.emplace(id, std::move(origin));
    ++s.nextOrigin;
    return id;
}

void FrameStore::detach(OriginId origin) {
    detail::StoreState& s = *state_;
    // Released after unlocking.
    std::vector<std::shared_ptr<detail::Entry>> dropped;
    std::shared_ptr<ISource> source;
    const std::scoped_lock lock(s.mutex);
    std::size_t count = 0;
    for (const auto& [key, record] : s.records) {
        if (key.origin == origin) {
            count += record.entries.size();
        }
    }
    dropped.reserve(count);  // push_back below cannot throw
    if (const auto found = s.origins.find(origin); found != s.origins.end()) {
        source = std::move(found->second.source);
        s.origins.erase(found);
    }
    for (auto record = s.records.begin(); record != s.records.end();) {
        if (record->first.origin != origin) {
            ++record;
            continue;
        }
        for (std::shared_ptr<detail::Entry>& entry : record->second.entries) {
            detail::Tier& tier = s.tiers[entry->tier];
            if (entry->linked) {
                tier.lru.erase(entry->lruPosition);
                entry->linked = false;
            }
            if (entry->counted) {
                tier.bytes -= entry->bytes;
                entry->counted = false;
            }
            entry->inRecord = false;
            dropped.push_back(std::move(entry));
        }
        record = s.records.erase(record);
    }
}

// ---- Acquire ----

Task<Result<ReadLease>> FrameStore::acquire(FrameKey key, ViewSpec want, IExecutor& resume_on,
                                            std::source_location where) {
    const std::shared_ptr<FrameStore> self = shared_from_this();  // the store outlives the call
    Result<detail::LeaseCore> core =
        co_await acquireCore(key, want, LeaseKind::Read, resume_on, where);
    Result<ReadLease> result = core ? Result<ReadLease>(ReadLease(std::move(*core)))
                                    : Result<ReadLease>(std::unexpected(std::move(core).error()));
    co_await resumeOn(resume_on);  // D12: never inline, hits included
    co_return result;
}

Task<Result<PinLease>> FrameStore::pin(FrameKey key, ViewSpec view, IExecutor& resume_on,
                                       std::source_location where) {
    const std::shared_ptr<FrameStore> self = shared_from_this();  // the store outlives the call
    Result<detail::LeaseCore> core =
        co_await acquireCore(key, view, LeaseKind::Pin, resume_on, where);
    Result<PinLease> result = core ? Result<PinLease>(PinLease(std::move(*core)))
                                   : Result<PinLease>(std::unexpected(std::move(core).error()));
    co_await resumeOn(resume_on);  // D12: never inline, hits included
    co_return result;
}

Task<Result<OriginalRead>> FrameStore::acquireOriginal(FrameKey key, Api api, Domain domain,
                                                       IExecutor& resume_on, bool require_exact,
                                                       std::source_location where) {
    const std::shared_ptr<FrameStore> self = shared_from_this();  // the store outlives the call
    std::optional<FrameDesc> desc;
    {
        const detail::StoreState& s = *state_;
        const std::scoped_lock lock(s.mutex);
        if (const auto origin = s.origins.find(key.origin); origin != s.origins.end()) {
            desc = origin->second.desc;
        }
    }
    if (!desc) {
        co_await resumeOn(resume_on);  // D12: never inline
        co_return makeError(Errc::Internal, std::format("origin {} is not attached", key.origin));
    }
    if (isDisplay(desc->format)) {
        co_await resumeOn(resume_on);  // D12: never inline
        co_return makeError(Errc::Internal,
                            std::format("frame {} of origin {}: the display format is never an "
                                        "original",
                                        key.index, key.origin));
    }
    const ViewSpec want =
        require_exact ? originalView(*desc, api, domain) : planesView(*desc, api, domain);
    Result<ReadLease> lease =
        co_await acquire(key, want, resume_on, where);  // resumes on resume_on
    if (!lease) {
        co_return std::unexpected(std::move(lease).error());
    }
    co_return OriginalRead(std::move(*lease));
}

Task<Result<detail::LeaseCore>> FrameStore::acquireCore(FrameKey key, ViewSpec want, LeaseKind kind,
                                                        IExecutor& resume_on,
                                                        std::source_location where) {
    detail::StoreState& s = *state_;
    Result<detail::Prepared> prepared = makeError(Errc::Internal, "not prepared");
    {
        const std::scoped_lock lock(s.mutex);
        prepared = prepareLocked(key, want, kind);
    }
    if (!prepared) {
        co_return std::unexpected(std::move(prepared).error());
    }
    detail::Prepared& p = *prepared;
    if (p.kind == detail::Prepared::Kind::Join) {
        const Result<void> joined = co_await detail::JoinAwaiter(s, p.entry, resume_on);
        if (!joined) {
            co_return std::unexpected(joined.error());
        }
    } else if (p.kind == detail::Prepared::Kind::Plan) {
        const Result<void> ran = co_await runPlan(std::move(p.plan));
        // The plan published new entries. On success publishLocked reserved the requester's reader,
        // so its own entry is safe.
        evictOverBudget(*this, s);
        if (!ran) {
            co_return std::unexpected(ran.error());
        }
    }
    // One reader of p.entry is reserved for this requester now.
    co_return finishLease(std::move(p.entry), p.view, kind, where);
}

detail::LeaseCore FrameStore::finishLease(std::shared_ptr<detail::Entry> entry, ViewSpec view,
                                          LeaseKind kind, std::source_location where) {
    Binding binding;
    try {
        binding = entry->frame->bind(view);  // published: immutable, read without the mutex
    } catch (...) {
        const std::scoped_lock lock(state_->mutex);
        releaseReaderLocked(*entry);
        throw;
    }
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(state_->mutex);
        id = state_->nextLease;
        try {
            state_->leases.emplace(id, LeaseInfo{entry->key, view, kind, where});
        } catch (...) {
            releaseReaderLocked(*entry);
            throw;
        }
        ++state_->nextLease;
        if (kind == LeaseKind::Pin) {
            assert(entry->readers > 0 && "a pin lease without its reserved reader");
            --entry->readers;
            ++entry->pins;
        }
        touchLocked(*entry);
    }
    return {.store = shared_from_this(),
            .entry = std::move(entry),
            .view = view,
            .binding = std::move(binding),
            .id = id,
            .kind = kind};
}

Result<detail::Prepared> FrameStore::prepareLocked(const FrameKey& key, const ViewSpec& want,
                                                   LeaseKind /*kind*/) {
    detail::StoreState& s = *state_;
    auto record = s.records.find(key);
    if (record == s.records.end()) {
        const auto origin = s.origins.find(key.origin);
        if (origin == s.origins.end()) {
            return makeError(Errc::Internal, std::format("origin {} is not attached", key.origin));
        }
        record =
            s.records.emplace(key, detail::Record{origin->second.desc, origin->second.unpacked, {}})
                .first;
    }
    detail::Record& rec = record->second;
    auto [entry, view] = detail::selectEntry(rec.entries, want, rec.desc.format);
    if (entry) {
        detail::Prepared prepared;
        // The reader is reserved now, also for a pending entry: between this decision and the
        // join the entry may be published, and it must not be evicted before the joiner leases it.
        ++entry->readers;
        if (entry->state == detail::EntryState::Published) {
            touchLocked(*entry);
            prepared.kind = detail::Prepared::Kind::Hit;
        } else {
            prepared.kind = detail::Prepared::Kind::Join;
        }
        ++s.counters.hits;
        prepared.entry = std::move(entry);
        prepared.view = view;
        return prepared;
    }
    // The one colour-changing edge (M3 D6) leads to the display view, which has displayColour();
    // every other view keeps the frame's colour.
    const bool display = isDisplay(want.format);
    if (want.colour != (display ? displayColour() : rec.desc.colour)) {
        auto error =
            display
                ? makeError(Errc::Unsupported, std::format("the display view has colour {}, not {}",
                                                           displayColour(), want.colour))
                : makeError(Errc::Unsupported,
                            std::format("no colour conversion: frame {} of origin {} has "
                                        "colour {}, the request {}",
                                        key.index, key.origin, rec.desc.colour, want.colour));
        if (rec.entries.empty()) {
            s.records.erase(record);
        }
        return error;
    }
    Result<detail::Plan> plan = planLocked(key, rec, want);
    if (!plan) {
        if (rec.entries.empty()) {
            s.records.erase(record);
        }
        return std::unexpected(std::move(plan).error());
    }
    detail::Prepared prepared;
    prepared.kind = detail::Prepared::Kind::Plan;
    prepared.entry = plan->steps.back().output;
    prepared.view = prepared.entry->planned;
    prepared.plan = std::move(*plan);
    ++s.counters.misses;
    return prepared;
}

// The planner's rules (§5.2): the cheapest converter path to the wanted view, from one of the
// key's entries (cost 0) or from Produce (the frame's bytes) when the key has no original.
Result<detail::Plan> FrameStore::planLocked(const FrameKey& key, detail::Record& record,
                                            const ViewSpec& want) {
    detail::StoreState& s = *state_;
    const std::string_view original = descriptor(record.desc.format).name;
    const std::optional<detail::StateKey> target = stateOf(record, s.tiers, want);
    if (!target) {
        return makeError(Errc::Unsupported,
                         std::format("no converter path from {} to {}", original, viewText(want)));
    }
    const detail::NoPathKey no_path{
        .format = record.desc.format, .target = *target, .layout = want.layout};
    if (std::ranges::find(s.noPath, no_path) != s.noPath.end()) {
        return makeError(Errc::Unsupported,
                         std::format("no converter path from {} to {}", original, viewText(want)));
    }

    // Every view of every entry, then Produce (sources and starts are parallel).
    std::vector<detail::PlanSource> sources;
    std::vector<std::pair<std::shared_ptr<detail::Entry>, ViewSpec>> starts;
    bool has_root = false;
    bool adopted_root = false;
    for (const auto& entry : record.entries) {
        if (entry->state == detail::EntryState::Failed) {
            continue;
        }
        has_root = has_root || entry->root;
        adopted_root = adopted_root || (entry->root && entry->adopted);
        const bool published = entry->state == detail::EntryState::Published;
        const std::span<const ViewSpec> views =
            published ? std::span<const ViewSpec>(entry->views) : std::span(&entry->planned, 1);
        for (const ViewSpec& view : views) {
            if (const std::optional<detail::StateKey> state = stateOf(record, s.tiers, view)) {
                sources.push_back(detail::PlanSource{
                    .state = *state, .layout = view.layout, .fidelity = entry->fidelity});
                starts.emplace_back(entry, view);
            }
        }
    }
    std::shared_ptr<ISource> source;
    if (const auto origin = s.origins.find(key.origin); origin != s.origins.end()) {
        source = origin->second.source;
    }
    if (source && !has_root) {
        DVV_TRY(const FrameGeometry frame,
                geometry(record.desc.format, record.desc.width, record.desc.height));
        sources.push_back(detail::PlanSource{.state = detail::PRODUCED,
                                             .layout = LayoutPolicy::Tight,
                                             .fidelity = Fidelity::Exact,
                                             .cost = static_cast<double>(frame.bytes)});
        starts.emplace_back(nullptr, ViewSpec{});
    }
    std::vector<const detail::Converter*> converters;
    converters.reserve(s.converters.size());
    for (const auto& converter : s.converters) {
        converters.push_back(converter.get());
    }
    const std::optional<detail::FoundPath> path =
        detail::findPath(sources, converters, record.desc, *target, want.layout,
                         s.options.maxPlanDepth, want.format != record.desc.format);
    if (!path) {
        // With an attached source and no adopted original, every state of the key is reachable
        // from Produce: the answer depends only on the no-path key and holds for good.
        if (source && !adopted_root) {
            s.noPath.push_back(no_path);
        }
        return makeError(Errc::Unsupported,
                         std::format("no converter path from {} {}/{} to {}", original,
                                     apiName(detail::PRODUCED.api), kindName(detail::PRODUCED.kind),
                                     viewText(want)));
    }
    const auto& [start, start_view] = starts[path->source];
    if (start && path->steps.empty()) {
        // selectEntry would have chosen that entry: the rules disagree.
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: the planner chose an existing entry "
                                     "for {}",
                                     key.index, key.origin, viewText(want)));
    }

    // A pending output of `converter` (null: Produce) whose input has `input_fidelity`.
    const auto make_output = [&](const detail::Converter* converter, Fidelity input_fidelity) {
        const detail::StateKey state = converter != nullptr ? converter->to() : detail::PRODUCED;
        // createWith registers only converters whose memory kinds have a tier, and Host has one.
        const std::size_t tier = tierOfKind(state.kind).value_or(0);
        auto output = std::make_shared<detail::Entry>();
        output->key = key;
        output->desc = record.desc;
        output->planned =
            ViewSpec{state.display ? displayFormat()
                                   : (state.unpacked ? record.unpacked : record.desc.format),
                     state.display ? displayColour() : record.desc.colour,
                     converter != nullptr ? converter->outputLayout() : LayoutPolicy::Tight,
                     state.api,
                     s.tiers[tier].domain,
                     0};
        output->fidelity = converter != nullptr
                               ? detail::weaker(input_fidelity, converter->fidelity())
                               : Fidelity::Exact;
        output->root = converter == nullptr;
        output->tier = tier;
        return output;
    };

    detail::Plan plan;
    Fidelity fidelity = Fidelity::Exact;
    if (start) {
        plan.start = start;
        plan.startView = start_view;
        plan.startPending = start->state != detail::EntryState::Published;
        fidelity = start->fidelity;
    } else {
        plan.source = std::move(source);
        plan.steps.push_back({nullptr, make_output(nullptr, Fidelity::Exact)});
    }
    plan.steps.reserve(plan.steps.size() + path->steps.size());
    for (const detail::Converter* converter : path->steps) {
        std::shared_ptr<detail::Entry> output = make_output(converter, fidelity);
        fidelity = output->fidelity;
        plan.steps.push_back({converter, std::move(output)});
    }
    // Everything that can throw comes first: every pending entry gets its creator.
    record.entries.reserve(record.entries.size() + plan.steps.size());
    for (const detail::Step& step : plan.steps) {
        record.entries.push_back(step.output);
    }
    if (plan.start) {
        // The plan reads its input; reserved now even when it is pending (see prepareLocked).
        ++plan.start->readers;
    }
    return plan;
}

// ---- Plan execution ----

// An exception in the completion path terminates: it would strand the waiters and poison the key.
// `noexcept` covers the frame allocation of this coroutine; the body's catch covers the rest
// (runStep frame allocation, error texts, publishLocked and failStepsLocked under the lock).
Task<Result<void>> FrameStore::runPlan(detail::Plan plan) noexcept {
    try {
        detail::StoreState& s = *state_;
        std::shared_ptr<detail::Entry> input = plan.start;
        ViewSpec input_view = plan.startView;
        if (input && plan.startPending) {
            // Another acquire makes the input: wait for it on the CPU executor.
            const Result<void> joined = co_await detail::JoinAwaiter(s, input, *s.options.cpu);
            if (!joined) {
                failSteps(plan, 0, joined.error());
                co_return std::unexpected(joined.error());
            }
        }
        for (std::size_t index = 0; index < plan.steps.size(); ++index) {
            detail::Step& step = plan.steps[index];
            Result<std::shared_ptr<IFrame>> made = co_await runStep(plan, step, input, input_view);
            if (step.converter != nullptr && !made && made.error().code == Errc::OutOfMemory) {
                // D10: evict the output's tier to half its budget, then run the step once more.
                // (Produce is not retried here: allocateHost already retried.)
                trimForRetry(*this, s, step.output->tier);
                made = co_await runStep(plan, step, input, input_view);
            }
            std::vector<detail::Waiter*> woken;
            std::optional<Error> error;
            {
                const std::scoped_lock lock(s.mutex);
                if (made) {
                    // On failure the frame stays in `made` and is released after unlocking.
                    if (Result<void> published = publishLocked(*step.output, *made, woken);
                        !published) {
                        error = std::move(published).error();
                    }
                } else {
                    error = made.error();
                }
                if (input) {
                    releaseReaderLocked(*input);  // the step is done with its input
                }
                if (error) {
                    failStepsLocked(plan, index, *error, woken);
                }
            }
            wake(woken);
            if (error) {
                co_return std::unexpected(std::move(*error));
            }
            input = step.output;  // publishLocked counted one reader for the plan
            input_view = step.output->planned;
        }
        co_return Result<void>{};
    } catch (...) {
        std::terminate();
    }
}

Task<Result<std::shared_ptr<IFrame>>> FrameStore::runStep(const detail::Plan& plan,
                                                          const detail::Step& step,
                                                          std::shared_ptr<detail::Entry> input,
                                                          ViewSpec input_view) {
    detail::StoreState& s = *state_;
    const std::size_t slot = step.converter == nullptr ? 0 : slotOf(*step.converter);
    const auto started = std::chrono::steady_clock::now();
    Result<std::shared_ptr<IFrame>> made = makeError(Errc::Internal, "not run");
    const std::string_view name =
        step.converter == nullptr ? std::string_view("Produce") : step.converter->name();
    try {
        if (step.converter == nullptr) {
            co_await resumeOn(*s.options.cpu);  // D12: the hop before every Produce
            ProduceContext context(*this, step.output->key, step.output->desc, *s.options.io);
            const Result<FrameRef> produced =
                co_await plan.source->produce(step.output->key.index, context);
            if (!produced) {
                made = std::unexpected(produced.error());
            } else if (!context.frame_) {
                made = makeError(Errc::Internal,
                                 std::format("source did not publish frame {} of origin {}",
                                             step.output->key.index, step.output->key.origin));
            } else {
                made = std::move(context.frame_);
            }
        } else {
            const Binding input_binding = input->frame->bind(input_view);
            made = co_await step.converter->run(
                detail::ConvertInput{input->key, input->desc, input_view, *input->frame,
                                     input_binding},
                detail::ConvertContext{*this, *s.options.cpu, s.options.gpu.get(),
                                       step.output->planned});
        }
    } catch (const std::exception& thrown) {
        made = makeError(Errc::Internal, std::format("{} threw: {}", name, thrown.what()));
    } catch (...) {
        made = makeError(Errc::Internal, std::format("{} threw", name));
    }
    detail::ConverterSlot& counters = *s.slots[slot];
    counters.runs.fetch_add(1, std::memory_order_relaxed);
    if (made && *made) {
        counters.bytes.fetch_add((*made)->bytes(), std::memory_order_relaxed);
    }
    counters.nanoseconds.fetch_add(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count()),
        std::memory_order_relaxed);
    co_return made;
}

// Publishes a step's output: checks the frame, records it, reserves one reader for the plan (its
// next step or the requester), and hands the waiters to the caller to wake after unlocking (each
// joiner reserved its own reader when it chose to join). On failure nothing changes and `frame`
// keeps the frame.
Result<void> FrameStore::publishLocked(detail::Entry& entry, std::shared_ptr<IFrame>& frame,
                                       std::vector<detail::Waiter*>& woken) {
    detail::StoreState& s = *state_;
    if (!frame || frame->desc() != entry.desc) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: the step made no frame or one with "
                                     "another description",
                                     entry.key.index, entry.key.origin));
    }
    const std::span<const ViewSpec> views = frame->nativeViews();
    if (std::ranges::find(views, entry.planned) == views.end()) {
        return makeError(Errc::Internal,
                         std::format("frame {} of origin {}: the made frame lacks the planned "
                                     "view",
                                     entry.key.index, entry.key.origin));
    }
    // Everything that can throw comes first.
    entry.views.assign(views.begin(), views.end());
    woken.reserve(woken.size() + entry.waiters.size());
    detail::Tier& tier = s.tiers[entry.tier];
    if (entry.inRecord) {
        tier.lru.push_back(&entry);
        entry.lruPosition = std::prev(tier.lru.end());
        entry.linked = true;
    }
    if (auto* host = dynamic_cast<HostFrame*>(frame.get())) {
        host->seal();
    }
    entry.frame = std::move(frame);
    entry.bytes = entry.frame->bytes();
    entry.state = detail::EntryState::Published;
    if (entry.inRecord) {
        tier.bytes += entry.bytes;
        entry.counted = true;
    }
    ++entry.readers;
    woken.insert(woken.end(), entry.waiters.begin(), entry.waiters.end());
    entry.waiters.clear();
    ++s.counters.creations;
    return {};
}

// Fails the pending outputs from step `from` on: their waiters get the error; the entries leave
// the index (failures are not cached).
void FrameStore::failStepsLocked(detail::Plan& plan, std::size_t from, const Error& error,
                                 std::vector<detail::Waiter*>& woken) {
    for (std::size_t index = from; index < plan.steps.size(); ++index) {
        detail::Entry& entry = *plan.steps[index].output;
        if (entry.state != detail::EntryState::Pending) {
            continue;
        }
        entry.state = detail::EntryState::Failed;
        entry.failure = error;
        for (detail::Waiter* waiter : entry.waiters) {
            waiter->error = error;
            woken.push_back(waiter);
        }
        entry.waiters.clear();
        static_cast<void>(removeFromIndexLocked(entry));  // the plan still holds the entry
        ++state_->counters.failures;
    }
}

void FrameStore::failSteps(detail::Plan& plan, std::size_t from, const Error& error) {
    std::vector<detail::Waiter*> woken;
    {
        const std::scoped_lock lock(state_->mutex);
        failStepsLocked(plan, from, error, woken);
    }
    wake(woken);
}

// Posts every waiter to its own executor (D12), after the mutex is released. A throwing post
// would strand a coroutine, so it terminates (noexcept).
void FrameStore::wake(std::span<detail::Waiter* const> waiters) noexcept {
    for (detail::Waiter* const waiter : waiters) {
        const std::coroutine_handle<> continuation = waiter->continuation;
        IExecutor* const executor = waiter->resumeOn;
        executor->post([continuation] { continuation.resume(); });
    }
}

// ---- Index ----

void FrameStore::releaseReaderLocked(detail::Entry& entry) noexcept {
    assert(entry.readers > 0 && "a reader released twice");
    --entry.readers;
    touchLocked(entry);
}

void FrameStore::touchLocked(detail::Entry& entry) noexcept {
    if (entry.linked) {
        std::list<detail::Entry*>& lru = state_->tiers[entry.tier].lru;
        lru.splice(lru.end(), lru, entry.lruPosition);
    }
}

// Takes the entry out of the index: LRU, tier bytes, record (erased when empty). Returns the
// record's reference to it, for release after unlocking (null when it was not in a record).
std::shared_ptr<detail::Entry> FrameStore::removeFromIndexLocked(detail::Entry& entry) noexcept {
    detail::StoreState& s = *state_;
    detail::Tier& tier = s.tiers[entry.tier];
    if (entry.linked) {
        tier.lru.erase(entry.lruPosition);
        entry.linked = false;
    }
    if (entry.counted) {
        tier.bytes -= entry.bytes;
        entry.counted = false;
    }
    std::shared_ptr<detail::Entry> owned;
    if (entry.inRecord) {
        entry.inRecord = false;
        const auto record = s.records.find(entry.key);
        if (record != s.records.end()) {
            auto& entries = record->second.entries;
            const auto found = std::ranges::find_if(
                entries, [&entry](const auto& candidate) { return candidate.get() == &entry; });
            if (found != entries.end()) {
                owned = std::move(*found);
                entries.erase(found);
            }
            if (entries.empty()) {
                s.records.erase(record);
            }
        }
    }
    return owned;
}

// Evicts unleased, unpinned entries of a tier until it holds at most `target` bytes: Derived
// entries first, then Lossless, then Exact, each pass least recently used first (§5.2).
void FrameStore::evictLocked(std::size_t tier_index, std::size_t target,
                             std::vector<std::shared_ptr<detail::Entry>>& dropped) {
    detail::Tier& tier = state_->tiers[tier_index];
    if (tier.bytes <= target) {
        return;
    }
    dropped.reserve(dropped.size() + tier.lru.size());  // push_back below cannot throw
    for (const Fidelity pass : {Fidelity::Derived, Fidelity::Lossless, Fidelity::Exact}) {
        for (auto position = tier.lru.begin(); position != tier.lru.end() && tier.bytes > target;) {
            detail::Entry& entry = **position;
            ++position;  // removal invalidates only the removed entry's iterator
            if (entry.fidelity == pass && entry.readers == 0 && entry.pins == 0) {
                dropped.push_back(removeFromIndexLocked(entry));
                ++state_->counters.evictions;
            }
        }
    }
}

void FrameStore::releaseLease(detail::LeaseCore& core) noexcept {
    const std::scoped_lock lock(state_->mutex);
    detail::Entry& entry = *core.entry;
    if (core.kind == LeaseKind::Pin) {
        assert(entry.pins > 0 && "a pin released twice");
        --entry.pins;
    } else {
        assert(entry.readers > 0 && "a reader released twice");
        --entry.readers;
    }
    state_->leases.erase(core.id);
    touchLocked(entry);
}

// ---- Adopt, trim, statistics ----

Result<ReadLease> FrameStore::adopt(const FrameKey& key, std::shared_ptr<IFrame> frame,
                                    SyncPoint ready, std::source_location where) {
    detail::StoreState& s = *state_;
    if (!frame) {
        return makeError(Errc::Internal, std::format("adopt: no frame for frame {} of origin {}",
                                                     key.index, key.origin));
    }
    const FrameDesc desc = frame->desc();
    const std::span<const ViewSpec> views = frame->nativeViews();
    const bool views_fit = !views.empty() && std::ranges::all_of(views, [&](const ViewSpec& view) {
        return view.format == desc.format && view.colour == desc.colour &&
               tierOf(view.domain).has_value();
    });
    if (!views_fit) {
        return makeError(Errc::Internal,
                         std::format("adopt: frame {} of origin {} needs native views of its own "
                                     "format and colour in the store's domains",
                                     key.index, key.origin));
    }
    const ViewSpec view = views[0];
    auto entry = std::make_shared<detail::Entry>();
    entry->key = key;
    entry->desc = desc;
    entry->planned = view;
    entry->fidelity = Fidelity::Exact;
    entry->root = true;
    entry->adopted = true;
    entry->tier = tierOf(view.domain).value_or(0);
    entry->views.assign(views.begin(), views.end());
    entry->ready = ready;
    entry->bytes = frame->bytes();
    Binding binding = frame->bind(view);
    const FormatId unpacked = unpackedFormat(desc.format);
    std::shared_ptr<FrameStore> self = shared_from_this();
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(s.mutex);
        auto record = s.records.find(key);
        bool created = false;
        if (record == s.records.end()) {
            // The description comes from the attached source when there is one.
            detail::Record fresh{desc, unpacked, {}};
            if (const auto origin = s.origins.find(key.origin); origin != s.origins.end()) {
                fresh.desc = origin->second.desc;
                fresh.unpacked = origin->second.unpacked;
            }
            if (fresh.desc != desc) {
                return makeError(Errc::Internal,
                                 std::format("adopt: frame {} of origin {} has another "
                                             "description than its source",
                                             key.index, key.origin));
            }
            record = s.records.emplace(key, std::move(fresh)).first;
            created = true;
        } else {
            const auto& entries = record->second.entries;
            if (std::ranges::any_of(entries, [](const auto& candidate) {
                    return candidate->root && candidate->state != detail::EntryState::Failed;
                })) {
                return makeError(Errc::Internal,
                                 std::format("frame {} of origin {} already has an original",
                                             key.index, key.origin));
            }
            if (record->second.desc != desc) {
                return makeError(Errc::Internal,
                                 std::format("adopt: frame {} of origin {} has another "
                                             "description than its record",
                                             key.index, key.origin));
            }
        }
        detail::Record& rec = record->second;
        detail::Tier& tier = s.tiers[entry->tier];
        id = s.nextLease;
        try {
            rec.entries.reserve(rec.entries.size() + 1);
            s.leases.emplace(id, LeaseInfo{key, view, LeaseKind::Read, where});
            try {
                tier.lru.push_back(entry.get());
            } catch (...) {
                s.leases.erase(id);
                throw;
            }
        } catch (...) {
            if (created) {
                s.records.erase(record);
            }
            throw;
        }
        // Nothing below throws.
        ++s.nextLease;
        if (auto* host = dynamic_cast<HostFrame*>(frame.get())) {
            host->seal();
        }
        entry->frame = std::move(frame);
        entry->state = detail::EntryState::Published;
        entry->lruPosition = std::prev(tier.lru.end());
        entry->linked = true;
        tier.bytes += entry->bytes;
        entry->counted = true;
        entry->readers = 1;  // the returned lease
        rec.entries.push_back(entry);
    }
    evictOverBudget(*this, s);  // the returned lease keeps the adopted entry
    return ReadLease(detail::LeaseCore{.store = std::move(self),
                                       .entry = std::move(entry),
                                       .view = view,
                                       .binding = std::move(binding),
                                       .id = id,
                                       .kind = LeaseKind::Read});
}

void FrameStore::trim(Domain domain, std::size_t target_bytes) {
    const std::optional<std::size_t> tier = tierOf(domain);
    if (!tier) {
        return;
    }
    std::vector<std::shared_ptr<detail::Entry>> dropped;  // released after unlocking
    const std::scoped_lock lock(state_->mutex);
    evictLocked(*tier, target_bytes, dropped);
}

StoreStats FrameStore::stats() const {
    const detail::StoreState& s = *state_;
    StoreStats stats;
    stats.converters.reserve(s.slots.size());
    for (const auto& slot : s.slots) {  // names are fixed after create; counters are relaxed
        stats.converters.push_back(
            ConverterStats{.name = slot->name,
                           .runs = slot->runs.load(std::memory_order_relaxed),
                           .bytes = slot->bytes.load(std::memory_order_relaxed),
                           .nanoseconds = slot->nanoseconds.load(std::memory_order_relaxed)});
    }
    stats.tiers.reserve(s.tiers.size());
    const std::scoped_lock lock(s.mutex);
    for (const detail::Tier& tier : s.tiers) {
        stats.tiers.push_back(TierStats{.domain = tier.domain,
                                        .bytes = tier.bytes,
                                        .budget = tier.budget,
                                        .views = tier.lru.size()});
        stats.frames += tier.lru.size();  // linked = published and in the index
    }
    for (const auto& [id, info] : s.leases) {
        if (info.kind == LeaseKind::Pin) {
            ++stats.pins;
        } else {
            ++stats.leases;
        }
    }
    stats.hits = s.counters.hits;
    stats.misses = s.counters.misses;
    stats.creations = s.counters.creations;
    stats.evictions = s.counters.evictions;
    stats.failures = s.counters.failures;
    return stats;
}

std::vector<LeaseInfo> FrameStore::outstandingLeases() const {
    const detail::StoreState& s = *state_;
    std::vector<LeaseInfo> leases;
    const std::scoped_lock lock(s.mutex);
    leases.reserve(s.leases.size());
    for (const auto& [id, info] : s.leases) {
        leases.push_back(info);
    }
    return leases;
}

ViewSpec originalView(const FrameDesc& desc, Api api, Domain domain) {
    return ViewSpec{desc.format, desc.colour, LayoutPolicy::Any, api, domain, 0};
}

ViewSpec planesView(const FrameDesc& desc, Api api, Domain domain) {
    return ViewSpec{unpackedFormat(desc.format), desc.colour, LayoutPolicy::Any, api, domain, 0};
}

}  // namespace dvv::media

namespace dvv::media::detail {

Result<std::shared_ptr<FrameStore>> StoreAccess::create(
    FrameStoreOptions options, std::vector<std::unique_ptr<Converter>> converters,
    TestDomains domains) {
    return FrameStore::createWith(std::move(options), std::move(converters), domains.staging,
                                  domains.device, std::source_location::current());
}

std::size_t StoreAccess::waiterCount(const FrameStore& store, const FrameKey& key) {
    const StoreState& s = *store.state_;
    const std::scoped_lock lock(s.mutex);
    const auto record = s.records.find(key);
    if (record == s.records.end()) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : record->second.entries) {
        if (entry->state == EntryState::Pending) {
            count += entry->waiters.size();
        }
    }
    return count;
}

Result<void> StoreAccess::checkInvariants(const FrameStore& store) {
    const StoreState& s = *store.state_;
    const std::scoped_lock lock(s.mutex);
    std::vector<std::size_t> sums(s.tiers.size(), 0);
    std::vector<std::size_t> linked(s.tiers.size(), 0);
    for (const auto& [key, record] : s.records) {
        const std::string where = std::format("frame {} of origin {}", key.index, key.origin);
        if (record.entries.empty()) {
            return makeError(Errc::Internal, where + ": empty record");
        }
        // Single flight: one original per key, and no view made twice at the same fidelity.
        const auto& entries = record.entries;
        if (std::ranges::count_if(entries, [](const auto& entry) {
                return entry->root && entry->state != EntryState::Failed;
            }) > 1) {
            return makeError(Errc::Internal, where + ": more than one original");
        }
        for (std::size_t a = 0; a < entries.size(); ++a) {
            for (std::size_t b = a + 1; b < entries.size(); ++b) {
                if (entries[a]->state != EntryState::Failed &&
                    entries[b]->state != EntryState::Failed &&
                    entries[a]->planned == entries[b]->planned &&
                    entries[a]->fidelity == entries[b]->fidelity) {
                    return makeError(Errc::Internal,
                                     where + ": two entries with the same view and fidelity");
                }
            }
        }
        for (const auto& entry : record.entries) {
            if (!entry->inRecord || entry->tier >= s.tiers.size() || !(entry->key == key)) {
                return makeError(Errc::Internal, where + ": an entry is not marked as indexed");
            }
            if (entry->counted) {
                sums[entry->tier] += entry->bytes;
            }
            const bool published = entry->state == EntryState::Published;
            if (entry->linked) {
                ++linked[entry->tier];
                if (!published || !entry->counted) {
                    return makeError(Errc::Internal,
                                     where + ": a linked entry is not published and counted");
                }
            } else if (published) {
                return makeError(Errc::Internal, where + ": a published entry is not linked");
            }
            if (!published) {
                continue;
            }
            std::uint32_t reads = 0;
            std::uint32_t pins = 0;
            for (const auto& [id, info] : s.leases) {
                if (info.key == key &&
                    std::ranges::find(entry->views, info.view) != entry->views.end()) {
                    ++(info.kind == LeaseKind::Pin ? pins : reads);
                }
            }
            if (reads > entry->readers) {
                return makeError(Errc::Internal, std::format("{}: {} read leases but {} readers",
                                                             where, reads, entry->readers));
            }
            if (pins != entry->pins) {
                return makeError(Errc::Internal, std::format("{}: {} pin leases but {} pins", where,
                                                             pins, entry->pins));
            }
        }
    }
    for (std::size_t index = 0; index < s.tiers.size(); ++index) {
        const Tier& tier = s.tiers[index];
        if (sums[index] != tier.bytes) {
            return makeError(Errc::Internal,
                             std::format("tier {}: {} bytes counted but entries of {} bytes", index,
                                         tier.bytes, sums[index]));
        }
        if (linked[index] != tier.lru.size()) {
            return makeError(Errc::Internal,
                             std::format("tier {}: {} LRU nodes but {} linked entries", index,
                                         tier.lru.size(), linked[index]));
        }
        for (const Entry* entry : tier.lru) {
            if (!entry->linked || entry->tier != index || !entry->inRecord) {
                return makeError(Errc::Internal,
                                 std::format("tier {}: an LRU node is not a linked entry", index));
            }
        }
    }
    return {};
}

StoreState& StoreAccess::state(FrameStore& store) noexcept {
    return *store.state_;
}

std::size_t StoreAccess::noPathCount(const FrameStore& store) {
    const StoreState& s = *store.state_;
    const std::scoped_lock lock(s.mutex);
    return s.noPath.size();
}

void StoreAccess::setHostAllocator(FrameStore& store, HostAllocator allocator) {
    store.state_->hostAllocator = std::move(allocator);
}

std::vector<std::pair<ViewSpec, Fidelity>> StoreAccess::views(const FrameStore& store,
                                                              const FrameKey& key) {
    const StoreState& s = *store.state_;
    std::vector<std::pair<ViewSpec, Fidelity>> views;
    const std::scoped_lock lock(s.mutex);
    const auto record = s.records.find(key);
    if (record == s.records.end()) {
        return views;
    }
    for (const auto& entry : record->second.entries) {
        if (entry->state == EntryState::Published) {
            views.emplace_back(entry->planned, entry->fidelity);
        }
    }
    return views;
}

}  // namespace dvv::media::detail
