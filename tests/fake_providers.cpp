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
#include "dvv/registry/parameters.h"

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
    FakeProvider(CreationKey<ComponentRegistry> key, const ParameterValues& /*values*/) noexcept
        : Managed<FakeProvider<N>>(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = IDS[N], .name = IDS[N], .kind = Kind::Source, .version = 1};
    }
    static const ParameterSchema& schema() {
        static const ParameterSchema SCHEMA;
        return SCHEMA;
    }
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

}  // namespace

DVV_REGISTER(ISourceProvider, FakeProviderA);
DVV_REGISTER(ISourceProvider, FakeProviderB);
DVV_REGISTER(ISourceProvider, FakeProviderC);

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
}

}  // namespace dvv::media::fakes
