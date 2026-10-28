#include "source_harness.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"

namespace media = dvv::media;

std::vector<FileReadCase> fileReadCases() {
    using dvv::platform::FileReadMode;
#if defined(_WIN32)
    return {{.mode = FileReadMode::Buffered, .reader = "buffered"},
            {.mode = FileReadMode::Unbuffered, .reader = "unbuffered"},
            {.mode = FileReadMode::Auto, .reader = "unbuffered"}};
#else
    return {{.mode = FileReadMode::Buffered, .reader = "buffered"},
            {.mode = FileReadMode::Auto, .reader = "buffered"}};
#endif
}

SourceHarness::SourceHarness() {
    auto cpu = dvv::ExecutorFactory::makeThreadPool(2);
    REQUIRE_MESSAGE(cpu.has_value(), cpu.error().describe());
    cpu_ = std::move(*cpu);
    auto io = dvv::ExecutorFactory::makeThreadPool(2);
    REQUIRE_MESSAGE(io.has_value(), io.error().describe());
    io_ = std::move(*io);
    registry_ = dvv::ComponentRegistry::create(dvv::Capabilities{});
    REQUIRE(registry_ != nullptr);
    auto factory = media::SourceFactory::create(*registry_, io_);
    REQUIRE_MESSAGE(factory.has_value(), factory.error().describe());
    factory_ = std::move(*factory);
    auto store = media::FrameStore::create({.cpu = cpu_, .io = io_});
    REQUIRE_MESSAGE(store.has_value(), store.error().describe());
    store_ = std::move(*store);
}

std::shared_ptr<media::ISource> SourceHarness::open(std::string_view uri,
                                                    dvv::platform::FileReadMode file_reads,
                                                    std::optional<media::FormatHint> hint) {
    INFO(std::string(uri));
    auto opened = tryOpen(uri, file_reads, hint);
    REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
    auto* source = std::get_if<std::unique_ptr<media::ISource>>(&*opened);
    REQUIRE_MESSAGE(source != nullptr, "the open was ambiguous");
    REQUIRE(*source != nullptr);
    return std::shared_ptr<media::ISource>(std::move(*source));
}

dvv::Result<std::variant<std::unique_ptr<media::ISource>, media::Ambiguous>> SourceHarness::tryOpen(
    std::string_view uri, dvv::platform::FileReadMode file_reads,
    std::optional<media::FormatHint> hint) {
    DVV_TRY(dvv::Uri parsed, dvv::parseUri(uri));
    const media::SourceRequest request{
        .uri = std::move(parsed), .hint = hint, .fileReads = file_reads};
    return dvv::syncWait(factory_->open(request));
}

dvv::Result<std::vector<std::byte>> SourceHarness::originalBytes(
    const std::shared_ptr<media::ISource>& source, std::int64_t index) {
    DVV_TRY(const media::OriginId origin, store_->attach(source));
    dvv::Result<std::vector<std::byte>> bytes;
    {
        auto read = dvv::syncWait(store_->acquireOriginal({origin, index}, media::Api::HostPlanes,
                                                          store_->hostDomain(), *cpu_, true));
        if (read) {
            const auto* planes = std::get_if<media::HostPlanesB>(&read->lease().binding());
            REQUIRE(planes != nullptr);
            std::vector<std::byte> copy;
            for (std::size_t p = 0; p < planes->planeCount; ++p) {
                copy.insert(copy.end(), planes->planes[p].begin(), planes->planes[p].end());
            }
            bytes = std::move(copy);
        } else {
            bytes = std::unexpected(std::move(read).error());
        }
    }
    // The lease is gone, so the origin can go too.
    store_->detach(origin);
    return bytes;
}

media::FrameStore& SourceHarness::store() {
    return *store_;
}
