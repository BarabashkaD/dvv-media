#pragma once

// Source providers and the source factory (M2 design §4.5, §5.5; spec §7.2).
//
// A provider is a Kind::Source component implementing ISourceProvider. SourceFactory creates one
// instance of every available provider, picks the provider for a request (scheme, file head,
// ?impl=, confidences) and lets it open the source. A provider constructs its source with
// SourceFactory::issueKey<T>(std::move(ticket)): the OpenTicket it was handed is the only way to
// get a CreationKey<SourceFactory>, and it makes exactly one key.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

namespace dvv::media {

// What the caller already knows about the content (for example from a session file).
struct FormatHint {
    std::optional<FormatId> format;
    std::optional<std::uint32_t> width, height;
    std::optional<Rational> fps;
};

// The clip description next to a file, `<file>.<ext>.json` (D5 design §3.5).
struct Sidecar {
    std::string path;   // UTF-8: the file's path plus ".json"
    std::string text;   // the file's text; empty when it could not be read
    std::string error;  // why it could not be read (e.g. "'<file>' is too large"); empty if read
};

struct SourceRequest {
    Uri uri;
    std::optional<FormatHint> hint;
    // How file sources (raw, Y4M) read frames (M3.1 U-IO2; the application's config file).
    platform::FileReadMode fileReads = platform::FileReadMode::Auto;
    // Set by SourceFactory::open() for a file: URI without clip= when <file>.json exists; callers
    // leave it empty (it is replaced). Only the raw provider uses it.
    std::optional<Sidecar> sidecar{};
};

// What a provider's match() worked out; handed back to the same provider's open().
struct FormatGuess {
    std::optional<FormatId> format;
    std::uint32_t width = 0, height = 0;
    std::optional<Rational> fps;
};

struct Match {
    int confidence = 0;  // 0 = cannot open; 100 = certain
    FormatGuess guess;
    std::string why;
};

struct Candidate {
    std::string_view component;  // the provider's component id (static storage)
    int confidence = 0;
    FormatGuess guess;
    std::string why;
};

// Several providers could open the request and none is clearly best.
struct Ambiguous {
    std::vector<Candidate> candidates;  // best first
};

class SourceFactory;

// The right to create one source; move-only, and only SourceFactory makes one.
class OpenTicket {
public:
    OpenTicket(OpenTicket&& other) noexcept;  // leaves `other` empty
    OpenTicket& operator=(OpenTicket&& other) noexcept;
    OpenTicket(const OpenTicket&) = delete;
    OpenTicket& operator=(const OpenTicket&) = delete;
    ~OpenTicket() = default;

private:
    friend class SourceFactory;
    explicit OpenTicket(ObjectId owner, std::source_location where) noexcept;
    ObjectId owner_ = NO_OWNER;
    std::source_location where_;
    bool valid_ = false;
};

struct OpenContext {
    IExecutor& io;
};

// Component kind Source (spec §9.3). Implementations are stateless: match() and open() may run
// concurrently, and a source never keeps a pointer to its provider.
class ISourceProvider {
public:
    ISourceProvider() = default;
    ISourceProvider(const ISourceProvider&) = delete;
    ISourceProvider& operator=(const ISourceProvider&) = delete;
    ISourceProvider(ISourceProvider&&) = delete;
    ISourceProvider& operator=(ISourceProvider&&) = delete;
    virtual ~ISourceProvider() = default;

    // The URI schemes it opens ("file" includes bare paths).
    [[nodiscard]] virtual std::span<const std::string_view> schemes() const noexcept = 0;
    // Looks only at the request and the first bytes (empty for non-file schemes); no I/O.
    [[nodiscard]] virtual Match match(const SourceRequest& request,
                                      std::span<const std::byte> head) const = 0;
    // The factory awaits the task directly, so `request`, `guess` and `context` outlive it.
    [[nodiscard]] virtual Task<Result<std::unique_ptr<ISource>>> open(
        OpenTicket ticket, const SourceRequest& request, const FormatGuess& guess,
        const OpenContext& context) = 0;
};

// One provider instance owned by the factory.
struct ProviderEntry {
    std::string_view id;  // ComponentInfo::id (static storage)
    std::unique_ptr<ISourceProvider> provider;
};

class SourceFactory final : public Managed<SourceFactory> {
public:
    using OpenResult = std::variant<std::unique_ptr<ISource>, Ambiguous>;

    // Own authority. Creates one instance of every available Kind::Source component of
    // `registry`; one that fails to be created is logged and skipped. Errc::Internal for a null
    // `io`; Errc::Unsupported "no source providers registered" when no provider is left.
    [[nodiscard]] static Result<std::unique_ptr<SourceFactory>> create(
        ComponentRegistry& registry, std::shared_ptr<IExecutor> io,
        std::source_location where = std::source_location::current());
    // For create() only (the key).
    SourceFactory(CreationKey<SourceFactory> key, std::vector<ProviderEntry> providers,
                  std::shared_ptr<IExecutor> io) noexcept;

    // Picks a provider and opens the source (design §5.5); thread-safe. A file's head is read on
    // the io executor. Ambiguous when the best confidence is below 60 or another is within less
    // than 10 of it. Errc::Io when the file cannot be read, Errc::Unsupported when no provider
    // can open the request, Errc::Internal when a provider throws (that provider is then
    // disabled for the factory's lifetime) or returns no source. A provider's own open() error
    // (e.g. Unsupported for a codec it refuses) is returned unchanged; isNoProviderMatch() tells
    // the two Unsupported kinds apart.
    [[nodiscard]] Task<Result<OpenResult>> open(const SourceRequest& request);

    // For providers: the typed key of the one source a ticket may create. Throws
    // std::logic_error for a ticket that was already used (moved from or spent).
    template <class T>
    [[nodiscard]] static CreationKey<SourceFactory> issueKey(OpenTicket&& ticket);

private:
    [[nodiscard]] bool disabled(std::size_t index) const;
    // Disables the provider, logs the failure and returns Internal "source provider '<id>'
    // failed: <what>".
    [[nodiscard]] Error providerFailed(std::size_t index, std::string_view what);
    [[nodiscard]] Task<Result<OpenResult>> openWith(std::size_t index, const SourceRequest& request,
                                                    const FormatGuess& guess);

    std::vector<ProviderEntry> providers_;  // sorted by id
    std::shared_ptr<IExecutor> io_;
    mutable std::mutex mutex_;           // guards disabled_
    std::vector<std::size_t> disabled_;  // providers_ indices that threw from match() or open()
};

template <class T>
CreationKey<SourceFactory> SourceFactory::issueKey(OpenTicket&& ticket) {
    if (!ticket.valid_) {
        throw std::logic_error("SourceFactory::issueKey: the ticket was already used");
    }
    ticket.valid_ = false;
    return CreationKey<SourceFactory>(
        LifetimeRegistry::instance().issue<T>(ticket.owner_, ticket.where_));
}

// True when `error` is, or wraps, SourceFactory::open's Unsupported for a request no provider
// could match: no provider for the scheme, none with a confidence above 0, or the ?impl=
// provider at 0. False for every other error, e.g. a provider that matched and then refused the
// content in its open(). Such an error is marked by its `where` (one fixed location in the
// factory's source), so describe() shows nothing extra and it has no cause; the message text is
// never inspected.
[[nodiscard]] bool isNoProviderMatch(const Error& error);

}  // namespace dvv::media

template <>
struct dvv::InterfaceKind<dvv::media::ISourceProvider>
    : std::integral_constant<dvv::Kind, dvv::Kind::Source> {};
