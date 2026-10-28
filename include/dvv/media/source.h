#pragma once

// Frame sources (M2 design §4.5, spec §7.1): what a source tells about itself, the context the
// store hands to one produce() call, and the source interface.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/task.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/frame.h"
#include "dvv/media/types.h"

namespace dvv::media {

enum class SourceAccess : std::uint8_t { RandomCheap, RandomCostly, Sequential };

struct SourceInfo {
    FrameDesc desc;
    Rational fps;
    std::optional<std::int64_t> frameCount;  // nullopt = unbounded generator
    SourceAccess access = SourceAccess::RandomCheap;
    bool deterministic = true;
    std::string resolvedUri;            // includes the format: reopens without guessing
    std::string provider;               // component id
    std::vector<std::string> warnings;  // e.g. "N trailing bytes ignored"
    // File sources: FileReader::modeText(), "unbuffered", "buffered" or "buffered (fallback:
    // <reason>)" (M3.1 U-IO2); empty otherwise (D18 bench).
    std::string reader;
    // Info-level lines for the log, not the infobar (D5 design §3.5), e.g. "clip.yuv.json
    // describes clip.yuv (it wins over the file name)".
    std::vector<std::string> notes{};
    // The colour hint the provider derived from the file tags (D5R F1): `bt601` names the BT.601
    // PAL/NTSC that the matrix code points to (the provider also writes it into desc.colour, which
    // the render path needs), and `nonStandard` says that the file signalled a code that fits no
    // field. Pass it to resolveColour()/standardOf() when labelling the stream. Sources without
    // file tags (raw, y4m, synthetic) leave it default.
    FileHint colourHint{};
    // The UTF-8 path of the clip description used (raw: clip=@<file> as given, or the sidecar
    // <file>.<ext>.json); empty when none.
    std::string clipFile{};
};

// Made by the store for one produce() call.
class ProduceContext {
public:
    ProduceContext(const ProduceContext&) = delete;
    ProduceContext& operator=(const ProduceContext&) = delete;
    ProduceContext(ProduceContext&&) = delete;
    ProduceContext& operator=(ProduceContext&&) = delete;
    ~ProduceContext() = default;

    [[nodiscard]] const FrameKey& key() const noexcept { return key_; }
    [[nodiscard]] IExecutor& io() const noexcept { return *io_; }
    // A host frame for this key: `capacity` bytes aligned to `alignment`, frame data at
    // `data_offset`. Errc::Internal when desc differs from the source's, the alignment is not a
    // power of two, or data_offset + geometry().bytes > capacity; Errc::OutOfMemory (Task 7b: after
    // one eviction retry).
    [[nodiscard]] Result<std::shared_ptr<HostFrame>> allocateHost(const FrameDesc& desc,
                                                                  std::size_t capacity,
                                                                  std::size_t alignment,
                                                                  std::size_t data_offset);
    // Hands the frame to the store, which publishes it as the key's pending original when
    // produce() returns. Does not call adopt. Errc::Internal for a null frame, a second publish,
    // a desc that differs, or a frame without the original host view
    // {desc.format, desc.colour, Tight, HostPlanes, hostDomain()}.
    [[nodiscard]] Result<FrameRef> publish(std::shared_ptr<IFrame> frame);

private:
    friend class FrameStore;
    ProduceContext(FrameStore& store, FrameKey key, FrameDesc desc, IExecutor& io) noexcept;
    FrameStore* store_;
    FrameKey key_;
    FrameDesc desc_;
    IExecutor* io_;
    std::shared_ptr<IFrame> frame_;
};

// Spec §7.1.
class ISource {
public:
    virtual ~ISource() = default;
    [[nodiscard]] virtual const SourceInfo& info() const noexcept = 0;
    // May be called concurrently for different indices. Errc::Internal for an index outside
    // [0, frameCount); Errc::Io for read errors and truncated frames.
    [[nodiscard]] virtual Task<Result<FrameRef>> produce(std::int64_t index,
                                                         ProduceContext& ctx) = 0;
};

}  // namespace dvv::media
