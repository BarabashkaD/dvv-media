#pragma once

// The shared fixture of the source-provider tests (Tasks 10-12): opens URIs through a SourceFactory
// built from every linked component and reads originals through a host-only FrameStore.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/platform/file_reader.h"
#include "dvv/registry/component_registry.h"

// A file read mode a test runs in and the SourceInfo::reader it gives here (M3.1 U-IO2): Buffered,
// Auto and, on Windows, Unbuffered. On Windows the temporary directory is on NTFS, so Auto and
// Unbuffered read unbuffered; elsewhere Auto reads buffered.
struct FileReadCase {
    dvv::platform::FileReadMode mode;
    std::string_view reader;
};
[[nodiscard]] std::vector<FileReadCase> fileReadCases();

// A registry with every linked component, cpu and io pools (2 threads each), a SourceFactory and
// a host-only FrameStore. Destroys the store, the factory, the registry and then the pools.
class SourceHarness {
public:
    SourceHarness();

    // Opens a URI; fails the test (REQUIRE) on an error or Ambiguous. file_reads: how file
    // sources read (SourceRequest::fileReads); hint: SourceRequest::hint.
    [[nodiscard]] std::shared_ptr<dvv::media::ISource> open(
        std::string_view uri,
        dvv::platform::FileReadMode file_reads = dvv::platform::FileReadMode::Auto,
        std::optional<dvv::media::FormatHint> hint = std::nullopt);
    // The raw result, for error and ambiguity tests. A URI that does not parse gives its error.
    [[nodiscard]] dvv::Result<
        std::variant<std::unique_ptr<dvv::media::ISource>, dvv::media::Ambiguous>>
    tryOpen(std::string_view uri,
            dvv::platform::FileReadMode file_reads = dvv::platform::FileReadMode::Auto,
            std::optional<dvv::media::FormatHint> hint = std::nullopt);
    // attach + acquireOriginal(requireExact, HostPlanes, host) + copy of the contiguous bytes; the
    // source is detached again before this returns.
    [[nodiscard]] dvv::Result<std::vector<std::byte>> originalBytes(
        const std::shared_ptr<dvv::media::ISource>& source, std::int64_t index);
    [[nodiscard]] dvv::media::FrameStore& store();

private:
    // Declared in creation order, so destruction runs the other way round.
    std::shared_ptr<dvv::ThreadPool> cpu_;
    std::shared_ptr<dvv::ThreadPool> io_;
    std::unique_ptr<dvv::ComponentRegistry> registry_;
    std::unique_ptr<dvv::media::SourceFactory> factory_;
    std::shared_ptr<dvv::media::FrameStore> store_;
};
