#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/gpu/device_id.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "packed_source.h"

namespace {

using dvv::Errc;
using dvv::syncWait;
using dvv::media::Api;
using dvv::media::Domain;
using dvv::media::Fidelity;
using dvv::media::FormatId;
using dvv::media::FrameDesc;
using dvv::media::FrameKey;
using dvv::media::FrameStore;
using dvv::media::HostPlanesB;
using dvv::media::LayoutPolicy;
using dvv::media::MemKind;
using dvv::media::OriginalRead;
using dvv::media::OriginId;
using dvv::media::ViewSpec;
using dvv::media::test::concatenated;
using dvv::media::test::packedOriginal;
using dvv::media::test::packedPlanes;
using dvv::media::test::PackedSource;
using dvv::media::test::runsOf;

constexpr std::uint64_t SEED = 7;

FormatId idOf(std::string_view name) {
    const auto id = dvv::media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(FormatId{0});
}

// A host-only store with a PackedSource of 3 frames (seed 7) attached.
struct HostStoreCase {
    explicit HostStoreCase(FrameDesc frame) : desc(frame) {
        pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        auto made = FrameStore::create({.cpu = pool, .io = pool});
        REQUIRE_MESSAGE(made.has_value(), made.error().describe());
        store = std::move(*made);
        auto attached = store->attach(std::make_shared<PackedSource>(desc, 3, SEED));
        REQUIRE_MESSAGE(attached.has_value(), attached.error().describe());
        origin = *attached;
    }

    [[nodiscard]] dvv::Result<OriginalRead> read(std::int64_t index, bool require_exact = false) {
        return syncWait(store->acquireOriginal({origin, index}, Api::HostPlanes,
                                               store->hostDomain(), *pool, require_exact));
    }

    FrameDesc desc;
    std::shared_ptr<dvv::ThreadPool> pool;
    std::shared_ptr<FrameStore> store;
    OriginId origin = 0;
};

// The two formats every case runs (as SUBCASEs): v210 34x17 and yuv420p10le 33x17.
std::vector<FrameDesc> descs() {
    return {FrameDesc{.format = idOf("v210"), .colour = 0, .width = 34, .height = 17},
            FrameDesc{.format = idOf("yuv420p10le"), .colour = 0, .width = 33, .height = 17}};
}

const HostPlanesB& planesOf(const OriginalRead& read) {
    const auto* planes = std::get_if<HostPlanesB>(&read.lease().binding());
    REQUIRE(planes != nullptr);
    return *planes;
}

}  // namespace

TEST_CASE("original read: view helpers") {
    for (const FrameDesc& base : descs()) {
        SUBCASE(dvv::media::descriptor(base.format).name.c_str()) {
            FrameDesc desc = base;
            desc.colour = dvv::media::internColour({.matrix = dvv::media::ColourMatrix::Bt709,
                                                    .range = dvv::media::ColourRange::Limited,
                                                    .transfer = dvv::media::TransferFn::Bt1886,
                                                    .primaries = dvv::media::Primaries::Bt709});
            const Domain host{};
            CHECK(dvv::media::originalView(desc, Api::HostPlanes, host) ==
                  ViewSpec{desc.format, desc.colour, LayoutPolicy::Any, Api::HostPlanes, host, 0});
            CHECK(dvv::media::planesView(desc, Api::HostPlanes, host) ==
                  ViewSpec{dvv::media::unpackedFormat(desc.format), desc.colour, LayoutPolicy::Any,
                           Api::HostPlanes, host, 0});
            const Domain device{MemKind::DeviceLocal, dvv::gpu::DeviceId{0}, 1};
            CHECK(dvv::media::planesView(desc, Api::VkImage, device) ==
                  ViewSpec{dvv::media::unpackedFormat(desc.format), desc.colour, LayoutPolicy::Any,
                           Api::VkImage, device, 0});
        }
    }
}

TEST_CASE("original read: a host-only store unpacks on the CPU") {
    for (const FrameDesc& desc : descs()) {
        SUBCASE(dvv::media::descriptor(desc.format).name.c_str()) {
            HostStoreCase test(desc);
            const auto read = test.read(1);
            REQUIRE_MESSAGE(read.has_value(), read.error().describe());
            CHECK(read->lease().fidelity() == Fidelity::Lossless);
            CHECK(read->lease().view().format == dvv::media::unpackedFormat(desc.format));
            const auto expected = packedPlanes(desc.format, desc.width, desc.height, SEED, 1);
            REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
            CHECK(concatenated(planesOf(*read)) == *expected);
            const auto stats = test.store->stats();
            CHECK(runsOf(stats, "Produce") == 1);
            CHECK(runsOf(stats, "CpuUnpack") == 1);
            std::vector<std::string> names;
            names.reserve(stats.converters.size());
            for (const auto& converter : stats.converters) {
                names.push_back(converter.name);
            }
            std::ranges::sort(names);
            CHECK(names == std::vector<std::string>{"CpuUnpack", "Produce"});
        }
    }
}

TEST_CASE("original read: requireExact returns the original bytes") {
    for (const FrameDesc& desc : descs()) {
        SUBCASE(dvv::media::descriptor(desc.format).name.c_str()) {
            HostStoreCase test(desc);
            const auto read = test.read(1, true);
            REQUIRE_MESSAGE(read.has_value(), read.error().describe());
            CHECK(read->lease().fidelity() == Fidelity::Exact);
            CHECK(read->lease().view().format == desc.format);
            const auto expected = packedOriginal(desc.format, desc.width, desc.height, SEED, 1);
            REQUIRE_MESSAGE(expected.has_value(), expected.error().describe());
            CHECK(concatenated(planesOf(*read)) == *expected);
            CHECK(runsOf(test.store->stats(), "CpuUnpack") == 0);
        }
    }
}

TEST_CASE("original read: a second read is a hit") {
    for (const FrameDesc& desc : descs()) {
        SUBCASE(dvv::media::descriptor(desc.format).name.c_str()) {
            HostStoreCase test(desc);
            const auto first = test.read(1);
            REQUIRE_MESSAGE(first.has_value(), first.error().describe());
            const auto before = test.store->stats();
            const auto second = test.read(1);
            REQUIRE_MESSAGE(second.has_value(), second.error().describe());
            const auto after = test.store->stats();
            CHECK(after.hits == before.hits + 1);
            CHECK(runsOf(after, "CpuUnpack") == 1);
            CHECK(concatenated(planesOf(*second)) == concatenated(planesOf(*first)));
        }
    }
}

TEST_CASE("original read: a host-only store has no staging domain") {
    for (const FrameDesc& desc : descs()) {
        SUBCASE(dvv::media::descriptor(desc.format).name.c_str()) {
            HostStoreCase test(desc);
            CHECK(test.store->stagingDomain() == std::nullopt);
            CHECK(test.store->deviceDomain() == std::nullopt);
            const auto read = syncWait(test.store->acquireOriginal(
                {test.origin, 1}, Api::HostPlanes,
                Domain{MemKind::HostVisible, dvv::gpu::DeviceId{0}, 1}, *test.pool));
            REQUIRE_FALSE(read.has_value());
            CHECK(read.error().code == Errc::Unsupported);
        }
    }
}

TEST_CASE("original read: a detached origin is Internal") {
    for (const FrameDesc& desc : descs()) {
        SUBCASE(dvv::media::descriptor(desc.format).name.c_str()) {
            HostStoreCase test(desc);
            test.store->detach(test.origin);
            const auto read = test.read(1);
            REQUIRE_FALSE(read.has_value());
            CHECK(read.error().code == Errc::Internal);
            CHECK(read.error().message.find("not attached") != std::string::npos);
        }
    }
}
