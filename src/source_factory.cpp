#include "dvv/media/source_factory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dvv/base/log.h"
#include "dvv/media/clip_json.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"

namespace dvv::media {

namespace {

// The best confidence that opens without asking, and the margin it needs over the second.
constexpr int OPEN_THRESHOLD = 60;
constexpr int TIE_MARGIN = 10;
// The bytes of a file that match() sees.
constexpr std::uint64_t HEAD_BYTES = 4096;

// Describes the exception being handled; call only from a catch block.
std::string describeCurrentException() {
    try {
        throw;
    } catch (const std::bad_expected_access<Error>& error) {
        return error.error().describe();
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "unknown exception";
    }
}

// The first HEAD_BYTES bytes of the request's file; runs on the io executor.
Result<std::vector<std::byte>> readHead(const Uri& uri) {
    DVV_TRY(const platform::FileReader reader, platform::FileReader::open(pathFromUtf8(uri.path)));
    std::vector<std::byte> head(static_cast<std::size_t>(std::min(reader.size(), HEAD_BYTES)));
    DVV_TRY(const std::size_t read, reader.readAt(0, head));
    head.resize(read);
    return head;
}

// The sidecar `<file>.json` of the request's file; runs on the io executor. A missing file (or
// anything that is not a regular file) is no sidecar; a file that cannot be read carries its error.
std::optional<Sidecar> readSidecar(const Uri& uri) {
    std::string path = uri.path + ".json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(pathFromUtf8(path), error)) {
        return std::nullopt;
    }
    auto text = readSmallFile(path);
    if (!text) {
        return Sidecar{.path = std::move(path), .text = {}, .error = text.error().message};
    }
    return Sidecar{.path = std::move(path), .text = std::move(*text), .error = {}};
}

struct Scored {
    std::size_t index = 0;  // into the factory's providers
    Match match;
};

// The source location every no-match error carries as its `where`: the mark that
// isNoProviderMatch() looks for. A location adds nothing a user reads beyond the usual
// "[source_factory.cpp:<line>]", unlike a marker cause, which describe() would print.
constexpr std::source_location NO_MATCH_WHERE = std::source_location::current();

bool isNoMatchWhere(const std::source_location& where) noexcept {
    return where.line() == NO_MATCH_WHERE.line() && where.column() == NO_MATCH_WHERE.column() &&
           std::string_view(where.file_name()) == NO_MATCH_WHERE.file_name();
}

// Unsupported `message` for a request no provider could match, marked by NO_MATCH_WHERE (the
// message names the case, so the call site's line is not needed).
std::unexpected<Error> noMatchError(std::string message) {
    return makeError(Errc::Unsupported, std::move(message), NO_MATCH_WHERE);
}

}  // namespace

bool isNoProviderMatch(const Error& error) {
    for (const Error* next = &error; next != nullptr; next = next->cause.get()) {
        if (isNoMatchWhere(next->where)) {
            return true;
        }
    }
    return false;
}

OpenTicket::OpenTicket(ObjectId owner, std::source_location where) noexcept
    : owner_(owner), where_(where), valid_(true) {}

OpenTicket::OpenTicket(OpenTicket&& other) noexcept
    : owner_(other.owner_), where_(other.where_), valid_(std::exchange(other.valid_, false)) {}

OpenTicket& OpenTicket::operator=(OpenTicket&& other) noexcept {
    if (this != &other) {
        owner_ = other.owner_;
        where_ = other.where_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

Result<std::unique_ptr<SourceFactory>> SourceFactory::create(ComponentRegistry& registry,
                                                             std::shared_ptr<IExecutor> io,
                                                             std::source_location where) {
    if (!io) {
        return makeError(Errc::Internal, "SourceFactory: no io executor");
    }
    std::vector<ProviderEntry> providers;
    for (const ComponentStatus& status : registry.list(Kind::Source)) {
        if (!status.available()) {
            continue;
        }
        auto provider = registry.createComponent<ISourceProvider>(status.info.id, {}, where);
        if (!provider) {
            DVV_LOG_WARN("source provider '{}' is skipped: {}", status.info.id,
                         provider.error().describe());
            continue;
        }
        providers.push_back(ProviderEntry{.id = status.info.id, .provider = std::move(*provider)});
    }
    if (providers.empty()) {
        return makeError(Errc::Unsupported, "no source providers registered");
    }
    // list() is sorted by id, so providers_ is too.
    return std::make_unique<SourceFactory>(
        CreationKey<SourceFactory>(
            LifetimeRegistry::instance().issue<SourceFactory>(NO_OWNER, where)),
        std::move(providers), std::move(io));
}

SourceFactory::SourceFactory(CreationKey<SourceFactory> key, std::vector<ProviderEntry> providers,
                             std::shared_ptr<IExecutor> io) noexcept
    : Managed(std::move(key)), providers_(std::move(providers)), io_(std::move(io)) {}

bool SourceFactory::disabled(std::size_t index) const {
    const std::scoped_lock lock(mutex_);
    return std::ranges::find(disabled_, index) != disabled_.end();
}

Error SourceFactory::providerFailed(std::size_t index, std::string_view what) {
    const std::string_view id = providers_[index].id;
    DVV_LOG_ERROR("source provider '{}' threw and is disabled: {}", id, what);
    {
        const std::scoped_lock lock(mutex_);
        if (std::ranges::find(disabled_, index) == disabled_.end()) {
            disabled_.push_back(index);
        }
    }
    return makeError(Errc::Internal, std::format("source provider '{}' failed: {}", id, what))
        .error();
}

Task<Result<SourceFactory::OpenResult>> SourceFactory::open(const SourceRequest& request) {
    // The caller's request with the factory's sidecar (step 2); it lives in this frame, so it
    // outlives every match() and open() below.
    SourceRequest full_request = request;
    full_request.sidecar.reset();
    const Uri& uri = full_request.uri;

    // 1. The providers of the scheme that are not disabled, in id order.
    std::vector<std::size_t> providers;
    for (std::size_t index = 0; index < providers_.size(); ++index) {
        if (disabled(index)) {
            continue;
        }
        const auto schemes = providers_[index].provider->schemes();
        if (std::ranges::find(schemes, std::string_view(uri.scheme)) != schemes.end()) {
            providers.push_back(index);
        }
    }
    if (providers.empty()) {
        co_return noMatchError(std::format("no source provider for scheme '{}'", uri.scheme));
    }

    // 2. The head of a file, read on the io executor; a file that cannot be read is Io at once.
    //    With it the sidecar <file>.json unless clip= names the description (D5 design §3.5).
    std::vector<std::byte> head;
    if (uri.scheme == "file") {
        co_await resumeOn(*io_);
        head = co_try(readHead(uri));
        if (!uri.param("clip")) {
            full_request.sidecar = readSidecar(uri);
        }
    }

    // 3. ?impl=<id>: the user chose, so any confidence above 0 opens it.
    if (const auto impl = uri.param("impl")) {
        const std::string_view impl_id = *impl;
        const auto chosen = std::ranges::find_if(
            providers, [&](std::size_t index) { return providers_[index].id == impl_id; });
        if (chosen == providers.end()) {
            std::string ids;
            for (const std::size_t index : providers) {
                ids += ids.empty() ? "" : ", ";
                ids += providers_[index].id;
            }
            co_return makeError(
                Errc::Unsupported,
                std::format("unknown source provider '{}' (available: {})", impl_id, ids));
        }
        const std::size_t index = *chosen;
        std::optional<Match> match;
        std::string failure;
        try {
            match = providers_[index].provider->match(full_request, head);
        } catch (...) {
            failure = describeCurrentException();
        }
        if (!match) {
            co_return std::unexpected(providerFailed(index, failure));
        }
        if (match->confidence <= 0) {
            co_return noMatchError(std::format("{} cannot open '{}': {}", providers_[index].id,
                                               formatUri(uri), match->why));
        }
        co_return co_await openWith(index, full_request, match->guess);
    }

    // 4. Every provider's match; the confident ones sorted by confidence, ties by id.
    std::vector<Scored> scored;
    std::string reasons;
    for (const std::size_t index : providers) {
        std::optional<Match> match;
        std::string failure;
        try {
            match = providers_[index].provider->match(full_request, head);
        } catch (...) {
            failure = describeCurrentException();
        }
        if (!match) {
            co_return std::unexpected(providerFailed(index, failure));
        }
        reasons +=
            std::format("{}{}: {}", reasons.empty() ? "" : "; ", providers_[index].id, match->why);
        if (match->confidence > 0) {
            scored.push_back(Scored{.index = index, .match = std::move(*match)});
        }
    }
    if (scored.empty()) {
        co_return noMatchError(
            std::format("no source provider can open '{}': {}", formatUri(uri), reasons));
    }
    // `providers` is in id order and the sort is stable, so equal confidences stay in id order.
    std::ranges::stable_sort(scored, std::ranges::greater{},
                             [](const Scored& entry) { return entry.match.confidence; });
    const int best = scored.front().match.confidence;
    const bool near_tie = scored.size() > 1 && scored[1].match.confidence > best - TIE_MARGIN;
    if (best < OPEN_THRESHOLD || near_tie) {
        Ambiguous ambiguous;
        ambiguous.candidates.reserve(scored.size());
        for (Scored& entry : scored) {
            ambiguous.candidates.push_back(Candidate{.component = providers_[entry.index].id,
                                                     .confidence = entry.match.confidence,
                                                     .guess = entry.match.guess,
                                                     .why = std::move(entry.match.why)});
        }
        co_return OpenResult(std::move(ambiguous));
    }

    // 5. Open the best.
    co_return co_await openWith(scored.front().index, full_request, scored.front().match.guess);
}

Task<Result<SourceFactory::OpenResult>> SourceFactory::openWith(std::size_t index,
                                                                const SourceRequest& request,
                                                                const FormatGuess& guess) {
    const ProviderEntry& entry = providers_[index];
    std::optional<Result<std::unique_ptr<ISource>>> opened;
    std::string failure;
    try {
        // Awaited here, so `request`, `guess` and the context outlive the provider's task.
        const OpenContext context{.io = *io_};
        opened = co_await entry.provider->open(
            OpenTicket(objectId(), std::source_location::current()), request, guess, context);
    } catch (...) {
        failure = describeCurrentException();
    }
    if (!opened) {
        co_return std::unexpected(providerFailed(index, failure));
    }
    if (!*opened) {
        co_return std::unexpected(std::move(*opened).error());
    }
    if (**opened == nullptr) {
        co_return makeError(Errc::Internal,
                            std::format("source provider '{}' returned no source", entry.id));
    }
    co_return OpenResult(std::move(**opened));
}

}  // namespace dvv::media
