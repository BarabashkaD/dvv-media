#include "fake_providers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

namespace dvv::media::fakes {

namespace {

constexpr std::array<std::string_view, 3> IDS{"fake_a", "fake_b", "fake_c"};
constexpr std::array<std::string_view, 2> SCHEMES{"fake", "file"};

std::array<FakeScript, 3>& scripts() {
    static std::array<FakeScript, 3> instance;
    return instance;
}

// A source that only reports who opened it.
class FakeSource final : public ISource, public Managed<FakeSource> {
public:
    FakeSource(CreationKey<SourceFactory> key, SourceInfo info) noexcept
        : Managed(std::move(key)), info_(std::move(info)) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t /*index*/,
                                                 ProduceContext& /*ctx*/) override {
        co_return makeError(Errc::Internal, "fake source: produce is not supported");
    }

private:
    SourceInfo info_;
};

template <std::size_t N>
class FakeProvider final : public ISourceProvider, public Managed<FakeProvider<N>> {
public:
    explicit FakeProvider(CreationKey<ComponentRegistry> key) noexcept
        : Managed<FakeProvider<N>>(std::move(key)) {}

    static ComponentInfo info() noexcept { return {.id = IDS[N], .name = IDS[N], .version = 1}; }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {"fake"});
    }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return SCHEMES;
    }

    [[nodiscard]] Match match(const SourceRequest& request,
                              std::span<const std::byte> head) const override {
        FakeScript& own = scripts()[N];
        ++own.matchCalls;
        own.lastHead.assign(head.begin(), head.end());
        own.lastSidecar = request.sidecar;
        if (own.throwInMatch) {
            throw std::runtime_error(std::string(IDS[N]) + " match failure");
        }
        return Match{.confidence = own.confidence, .guess = {}, .why = own.why};
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(
        OpenTicket ticket, const SourceRequest& /*request*/, const FormatGuess& /*guess*/,
        const OpenContext& /*context*/) override {
        FakeScript& own = scripts()[N];
        ++own.openCalls;
        if (own.throwInOpen) {
            throw std::runtime_error(std::string(IDS[N]) + " open failure");
        }
        if (own.refuseInOpen) {
            co_return makeError(Errc::Unsupported, std::string(IDS[N]) + " refuses the content");
        }
        SourceInfo info;
        info.provider = std::string(IDS[N]);
        std::unique_ptr<ISource> source = std::make_unique<FakeSource>(
            SourceFactory::issueKey<FakeSource>(std::move(ticket)), std::move(info));
        co_return std::move(source);
    }
};

using FakeProviderA = FakeProvider<0>;
using FakeProviderB = FakeProvider<1>;
using FakeProviderC = FakeProvider<2>;

constexpr std::array<std::string_view, 1> GATED_SCHEMES{"gated"};

// A source that counts its destructions.
class GatedSource final : public ISource, public Managed<GatedSource> {
public:
    GatedSource(CreationKey<SourceFactory> key, SourceInfo info) noexcept
        : Managed(std::move(key)), info_(std::move(info)) {}
    GatedSource(const GatedSource&) = delete;
    GatedSource& operator=(const GatedSource&) = delete;
    GatedSource(GatedSource&&) = delete;
    GatedSource& operator=(GatedSource&&) = delete;
    ~GatedSource() override { ++gated().destroyed; }

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t /*index*/,
                                                 ProduceContext& /*ctx*/) override {
        co_return makeError(Errc::Internal, "gated source: produce is not supported");
    }

private:
    SourceInfo info_;
};

class GatedProvider final : public ISourceProvider, public Managed<GatedProvider> {
public:
    explicit GatedProvider(CreationKey<ComponentRegistry> key) noexcept
        : Managed<GatedProvider>(std::move(key)) {}

    static ComponentInfo info() noexcept { return {.id = "gated", .name = "gated", .version = 1}; }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {"fake"});
    }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return GATED_SCHEMES;
    }

    [[nodiscard]] Match match(const SourceRequest& /*request*/,
                              std::span<const std::byte> /*head*/) const override {
        return Match{.confidence = 95, .guess = {}, .why = "gated"};
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(OpenTicket ticket,
                                                              const SourceRequest& /*request*/,
                                                              const FormatGuess& /*guess*/,
                                                              const OpenContext& context) override {
        GatedState& state = gated();
        ++state.opens;
        co_await state.wait(context.io);
        state.sawStop = context.stop.stop_requested();
        SourceInfo info;
        info.provider = "gated";
        std::unique_ptr<ISource> source = std::make_unique<GatedSource>(
            SourceFactory::issueKey<GatedSource>(std::move(ticket)), std::move(info));
        co_return std::move(source);
    }
};

constexpr std::array<std::string_view, 1> FAKE2_SCHEMES{"fake2"};

// A provider whose probe needs a capability the test registries leave out, so it is unavailable.
class Fake2Provider final : public ISourceProvider, public Managed<Fake2Provider> {
public:
    explicit Fake2Provider(CreationKey<ComponentRegistry> key) noexcept
        : Managed<Fake2Provider>(std::move(key)) {}

    static ComponentInfo info() noexcept { return {.id = "fake2", .name = "fake2", .version = 1}; }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {"fake2"});
    }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return FAKE2_SCHEMES;
    }

    [[nodiscard]] Match match(const SourceRequest& /*request*/,
                              std::span<const std::byte> /*head*/) const override {
        return Match{.confidence = 0, .guess = {}, .why = "fake2"};
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(
        OpenTicket /*ticket*/, const SourceRequest& /*request*/, const FormatGuess& /*guess*/,
        const OpenContext& /*context*/) override {
        co_return makeError(Errc::Internal, "fake2 cannot open");
    }
};

}  // namespace

DVV_REGISTER(ISourceProvider, FakeProviderA);
DVV_REGISTER(ISourceProvider, FakeProviderB);
DVV_REGISTER(ISourceProvider, FakeProviderC);
DVV_REGISTER(ISourceProvider, GatedProvider);
DVV_REGISTER(ISourceProvider, Fake2Provider);

void GatedState::open() {
    std::vector<std::pair<std::coroutine_handle<>, IExecutor*>> waiters;
    {
        const std::scoped_lock lock(mutex_);
        open_ = true;
        waiters.swap(waiters_);
    }
    for (auto& [handle, executor] : waiters) {
        executor->post([handle] { handle.resume(); });
    }
}

void GatedState::reset() {
    const std::scoped_lock lock(mutex_);
    open_ = false;
    waiters_.clear();
    sawStop = false;
    opens = 0;
}

std::size_t GatedState::waiting() const {
    const std::scoped_lock lock(mutex_);
    return waiters_.size();
}

GatedState& gated() {
    static GatedState instance;
    return instance;
}

FakeScript& script(std::string_view id) {
    for (std::size_t i = 0; i < IDS.size(); ++i) {
        if (IDS[i] == id) {
            return scripts()[i];
        }
    }
    throw std::invalid_argument("no fake provider " + std::string(id));
}

void resetScripts() {
    for (FakeScript& own : scripts()) {
        own = FakeScript{};
    }
    gated().reset();
}

}  // namespace dvv::media::fakes
