#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/lifetime.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"

namespace media = dvv::media;

namespace {

// Tasks 10-12 extend this list; it catches a provider dropped at link time (R12).
constexpr std::array<std::string_view, 3> EXPECTED_PROVIDERS{"raw", "synthetic", "y4m"};

}  // namespace

TEST_CASE("providers: conformance") {
    auto& lifetimes = dvv::LifetimeRegistry::instance();
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const auto statuses = registry->list(dvv::Kind::Source);

    std::vector<std::string_view> ids;
    ids.reserve(statuses.size());
    for (const auto& status : statuses) {
        ids.push_back(status.info.id);
    }
    std::vector<std::string_view> expected(EXPECTED_PROVIDERS.begin(), EXPECTED_PROVIDERS.end());
    std::ranges::sort(expected);
    CHECK(ids == expected);

    for (const auto& status : statuses) {
        INFO(std::string(status.info.id));
        // Neither schema() nor probe() threw, and nothing is needed beyond no capabilities.
        CHECK(status.available());
        REQUIRE(status.schema != nullptr);
        CHECK(status.schema->resolve({}).has_value());

        const std::size_t live_before = lifetimes.liveCount();
        {
            auto provider = registry->createComponent<media::ISourceProvider>(status.info.id);
            REQUIRE(provider.has_value());
            REQUIRE(*provider != nullptr);
            const auto* managed = dynamic_cast<const dvv::ManagedObject*>(provider->get());
            REQUIRE(managed != nullptr);
            CHECK(managed->ownerId() == registry->objectId());
        }
        CHECK(lifetimes.liveCount() == live_before);
    }
}
