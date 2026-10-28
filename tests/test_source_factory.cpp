#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "fake_providers.h"

namespace media = dvv::media;
namespace fakes = dvv::media::fakes;
namespace fs = std::filesystem;

namespace {

using OpenResult = dvv::Result<std::variant<std::unique_ptr<media::ISource>, media::Ambiguous>>;

// A unique directory under the system temporary directory, removed with its contents.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        const fs::path base = fs::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            fs::path candidate =
                base / std::format("dvv-source-factory-{:08x}{:08x}", random(), random());
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
        }
        FAIL("no temporary directory could be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// Bytes i % 251 for i in [0, count).
std::vector<std::byte> patternBytes(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(i % 251);
    }
    return bytes;
}

void writeFile(const fs::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

// The fixture of every case: fresh scripts, a registry with the capability "fake", an io pool of
// two threads and the factory.
class FactoryTest {
public:
    FactoryTest() {
        fakes::resetScripts();
        registry = dvv::ComponentRegistry::create(dvv::Capabilities{"fake"});
        auto pool = dvv::ExecutorFactory::makeThreadPool(2);
        REQUIRE(pool.has_value());
        io = std::move(*pool);
        auto created = media::SourceFactory::create(*registry, io);
        REQUIRE(created.has_value());
        factory = std::move(*created);
    }

    [[nodiscard]] OpenResult open(std::string_view text) const {
        auto uri = dvv::parseUri(text);
        REQUIRE(uri.has_value());
        return dvv::syncWait(factory->open({.uri = *uri, .hint = std::nullopt}));
    }
    // parseUri keeps the body of an opaque scheme such as fake: unparsed, so a fake: URI with
    // ?impl= is built directly, as parseUri builds file: and synthetic: URIs.
    [[nodiscard]] OpenResult openWithImpl(std::string impl) const {
        dvv::Uri uri;
        uri.scheme = "fake";
        uri.path = "x";
        uri.params.emplace_back("impl", std::move(impl));
        return dvv::syncWait(factory->open({.uri = uri, .hint = std::nullopt}));
    }

    std::unique_ptr<dvv::ComponentRegistry> registry;
    std::shared_ptr<dvv::ThreadPool> io;
    std::unique_ptr<media::SourceFactory> factory;
};

fakes::FakeScript& a() {
    return fakes::script("fake_a");
}
fakes::FakeScript& b() {
    return fakes::script("fake_b");
}
fakes::FakeScript& c() {
    return fakes::script("fake_c");
}

// The opened source; REQUIREs that the result holds one.
const media::ISource& sourceOf(const OpenResult& result) {
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<std::unique_ptr<media::ISource>>(*result));
    const auto& source = std::get<std::unique_ptr<media::ISource>>(*result);
    REQUIRE(source != nullptr);
    return *source;
}

const media::Ambiguous& ambiguousOf(const OpenResult& result) {
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<media::Ambiguous>(*result));
    return std::get<media::Ambiguous>(*result);
}

void checkError(const OpenResult& result, dvv::Errc code) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
}

}  // namespace

TEST_CASE_FIXTURE(FactoryTest, "factory: a clear best at or above 60 opens it") {
    a().confidence = 95;
    b().confidence = 30;
    const auto result = open("fake:x");
    CHECK(sourceOf(result).info().provider == "fake_a");
    CHECK(a().openCalls == 1);
    CHECK(b().openCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: a near tie is Ambiguous") {
    a().confidence = 70;
    a().why = "a likes it";
    b().confidence = 65;
    b().why = "b likes it too";
    const auto result = open("fake:x");
    const media::Ambiguous& ambiguous = ambiguousOf(result);
    REQUIRE(ambiguous.candidates.size() == 2);
    CHECK(ambiguous.candidates[0].component == "fake_a");
    CHECK(ambiguous.candidates[0].confidence == 70);
    CHECK(ambiguous.candidates[0].why == "a likes it");
    CHECK(ambiguous.candidates[1].component == "fake_b");
    CHECK(ambiguous.candidates[1].confidence == 65);
    CHECK(ambiguous.candidates[1].why == "b likes it too");
    CHECK(a().openCalls + b().openCalls + c().openCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: a difference of exactly 10 is not a tie") {
    a().confidence = 70;
    b().confidence = 60;
    const auto result = open("fake:x");
    CHECK(sourceOf(result).info().provider == "fake_a");
    CHECK(a().openCalls == 1);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: a best below 60 is Ambiguous") {
    a().confidence = 30;
    const auto result = open("fake:x");
    const media::Ambiguous& ambiguous = ambiguousOf(result);
    REQUIRE(ambiguous.candidates.size() == 1);
    CHECK(ambiguous.candidates[0].component == "fake_a");
    CHECK(ambiguous.candidates[0].confidence == 30);
    CHECK(a().openCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: no confidence is Unsupported naming every provider") {
    a().why = "why a";
    b().why = "why b";
    c().why = "why c";
    const auto result = open("fake:x");
    checkError(result, dvv::Errc::Unsupported);
    const std::string& message = result.error().message;
    CHECK(message.find("fake_a: why a") != std::string::npos);
    CHECK(message.find("fake_b: why b") != std::string::npos);
    CHECK(message.find("fake_c: why c") != std::string::npos);
    CHECK(media::isNoProviderMatch(result.error()));
}

// rawreel's format dialog is only for a request no provider could match, never for a refusal.
TEST_CASE_FIXTURE(FactoryTest, "factory: isNoProviderMatch tells no match from a refusal") {
    SUBCASE("a provider that matches and refuses in open") {
        a().confidence = 95;
        a().refuseInOpen = true;
        const auto result = open("fake:x");
        checkError(result, dvv::Errc::Unsupported);
        CHECK(result.error().message == "fake_a refuses the content");
        CHECK_FALSE(media::isNoProviderMatch(result.error()));
    }
    SUBCASE("the ?impl= provider at confidence 0") {
        const auto result = openWithImpl("fake_b");
        checkError(result, dvv::Errc::Unsupported);
        CHECK(media::isNoProviderMatch(result.error()));
    }
    SUBCASE("an unknown scheme") {
        const auto result = open("nosuch:x");
        checkError(result, dvv::Errc::Unsupported);
        CHECK(media::isNoProviderMatch(result.error()));
    }
    SUBCASE("an unknown impl and a throwing provider are other errors") {
        a().confidence = 95;
        CHECK_FALSE(media::isNoProviderMatch(openWithImpl("nope").error()));
        a().throwInOpen = true;
        const auto thrown = open("fake:x");
        checkError(thrown, dvv::Errc::Internal);
        CHECK_FALSE(media::isNoProviderMatch(thrown.error()));
    }
    SUBCASE("a wrapped no-match error and a look-alike") {
        const auto result = open("fake:x");
        checkError(result, dvv::Errc::Unsupported);
        CHECK(media::isNoProviderMatch(result.error().wrap(dvv::Errc::Unsupported, "outer")));
        const dvv::Error copy_text =
            dvv::makeError(dvv::Errc::Unsupported, result.error().message).error();
        CHECK_FALSE(media::isNoProviderMatch(copy_text));
    }
}

// The no-match mark is invisible: users read describe() in the infobar and on CLI stderr.
TEST_CASE_FIXTURE(FactoryTest, "factory: a no-match error describes only its own message") {
    auto result = open("nosuch:x");
    SUBCASE("no provider for the scheme") {
        result = open("nosuch:x");
    }
    SUBCASE("no provider with a confidence above 0") {
        result = open("fake:x");
    }
    SUBCASE("the ?impl= provider at confidence 0") {
        result = openWithImpl("fake_b");
    }
    checkError(result, dvv::Errc::Unsupported);
    const dvv::Error& error = result.error();
    CHECK(media::isNoProviderMatch(error));
    CHECK(error.cause == nullptr);
    const std::string text = error.describe();
    CHECK(text.find(" <- ") == std::string::npos);
    CHECK(text.find("no source provider matched") == std::string::npos);
    CHECK(text.starts_with("Unsupported: " + error.message + " ["));

    const dvv::Error inner = error.wrap(dvv::Errc::Unsupported, "inner");
    const dvv::Error twice = inner.wrap(dvv::Errc::Unsupported, "outer");
    CHECK(media::isNoProviderMatch(twice));
    CHECK(twice.describe().find("no source provider matched") == std::string::npos);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: impl selects one provider") {
    a().confidence = 95;
    b().confidence = 30;
    const auto result = openWithImpl("fake_b");  // fake:x?impl=fake_b
    CHECK(sourceOf(result).info().provider == "fake_b");
    CHECK(a().matchCalls == 0);
    CHECK(b().openCalls == 1);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: an unknown impl is Unsupported listing the ids") {
    a().confidence = 95;
    const auto result = openWithImpl("nope");  // fake:x?impl=nope
    checkError(result, dvv::Errc::Unsupported);
    CHECK(result.error().message.find("fake_a, fake_b, fake_c") != std::string::npos);
    CHECK(a().openCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: an unknown scheme is Unsupported") {
    a().confidence = 95;
    checkError(open("nosuch:x"), dvv::Errc::Unsupported);
    CHECK(a().matchCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: a missing file is Io before any match") {
    const TemporaryDirectory directory;
    a().confidence = 95;
    const std::string path = media::utf8FromPath(directory.path() / "missing.bin");
    checkError(open(path), dvv::Errc::Io);
    CHECK(a().matchCalls == 0);
    CHECK(b().matchCalls == 0);
    CHECK(c().matchCalls == 0);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: the file head is passed to match") {
    const TemporaryDirectory directory;
    a().confidence = 95;

    const auto large = patternBytes(5000);
    writeFile(directory.path() / "large.bin", large);
    const auto opened_large = open(media::utf8FromPath(directory.path() / "large.bin"));
    CHECK(sourceOf(opened_large).info().provider == "fake_a");
    REQUIRE(a().lastHead.size() == 4096);
    CHECK(std::equal(a().lastHead.begin(), a().lastHead.end(), large.begin()));

    const auto small = patternBytes(10);
    writeFile(directory.path() / "small.bin", small);
    const auto opened_small = open(media::utf8FromPath(directory.path() / "small.bin"));
    CHECK(sourceOf(opened_small).info().provider == "fake_a");
    CHECK(a().lastHead == small);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: the sidecar <file>.json is passed to match") {
    const TemporaryDirectory directory;
    a().confidence = 95;
    writeFile(directory.path() / "clip.bin", patternBytes(10));
    const std::string path = media::utf8FromPath(directory.path() / "clip.bin");

    // No sidecar file: no sidecar and no error.
    CHECK(sourceOf(open(path)).info().provider == "fake_a");
    CHECK_FALSE(a().lastSidecar.has_value());

    const std::string text = R"({"rawreelClip": 1, "frameRate": 30})";
    const auto bytes = std::as_bytes(std::span(text));
    writeFile(directory.path() / "clip.bin.json", {bytes.begin(), bytes.end()});
    CHECK(sourceOf(open(path)).info().provider == "fake_a");
    const auto& found = a().lastSidecar;
    REQUIRE(found.has_value());
    if (found) {  // the REQUIRE above ends the test first; the if is for clang-tidy's dataflow
        CHECK(found->path == path + ".json");
        CHECK(found->text == text);
        CHECK(found->error.empty());
    }

    // clip= replaces the sidecar: it is not read.
    CHECK(sourceOf(open(path + "?clip=@other.json")).info().provider == "fake_a");
    CHECK_FALSE(a().lastSidecar.has_value());

    // A sidecar that cannot be read is passed with its error (the provider warns), not an error.
    writeFile(directory.path() / "clip.bin.json", std::vector<std::byte>((1U << 20U) + 1U));
    CHECK(sourceOf(open(path)).info().provider == "fake_a");
    const auto& unreadable = a().lastSidecar;
    REQUIRE(unreadable.has_value());
    if (unreadable) {  // as above
        CHECK(unreadable->text.empty());
        CHECK(unreadable->error.find("is too large") != std::string::npos);
    }

    // A caller's sidecar is replaced: with no file next to the clip, match() sees none.
    std::filesystem::remove(directory.path() / "clip.bin.json");
    auto uri = dvv::parseUri(path);
    REQUIRE(uri.has_value());
    media::SourceRequest request{.uri = *uri, .hint = std::nullopt};
    request.sidecar = media::Sidecar{.path = path + ".json", .text = text, .error = {}};
    CHECK(sourceOf(dvv::syncWait(factory->open(request))).info().provider == "fake_a");
    CHECK_FALSE(a().lastSidecar.has_value());
}

TEST_CASE_FIXTURE(FactoryTest,
                  "factory: an exception in match is Internal and disables the provider") {
    a().confidence = 95;
    a().throwInMatch = true;
    const auto first = open("fake:x");
    checkError(first, dvv::Errc::Internal);
    CHECK(first.error().message.find("fake_a") != std::string::npos);
    CHECK(a().matchCalls == 1);

    b().confidence = 95;
    const auto second = open("fake:x");
    CHECK(sourceOf(second).info().provider == "fake_b");
    CHECK(a().matchCalls == 1);
}

TEST_CASE_FIXTURE(FactoryTest,
                  "factory: an exception in open is Internal and disables the provider") {
    a().confidence = 95;
    a().throwInOpen = true;
    const auto first = open("fake:x");
    checkError(first, dvv::Errc::Internal);
    CHECK(first.error().message.find("fake_a") != std::string::npos);
    CHECK(a().openCalls == 1);

    b().confidence = 95;
    const auto second = open("fake:x");
    CHECK(sourceOf(second).info().provider == "fake_b");
    CHECK(a().matchCalls == 1);
    CHECK(a().openCalls == 1);
}

TEST_CASE("factory: create without available providers is Unsupported") {
    fakes::resetScripts();
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    auto pool = dvv::ExecutorFactory::makeThreadPool(2);
    REQUIRE(pool.has_value());
    const auto created = media::SourceFactory::create(*registry, *pool);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == dvv::Errc::Unsupported);
    CHECK(created.error().message == "no source providers registered");
}

TEST_CASE("factory: create with a null io executor is Internal") {
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{"fake"});
    const auto created = media::SourceFactory::create(*registry, nullptr);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == dvv::Errc::Internal);
}

TEST_CASE_FIXTURE(FactoryTest, "factory: the ticket keys the source to the factory") {
    a().confidence = 95;
    const auto result = open("fake:x");
    const auto& managed = dynamic_cast<const dvv::ManagedObject&>(sourceOf(result));
    CHECK(managed.ownerId() == factory->objectId());
}

TEST_CASE("factory: InterfaceKind of ISourceProvider is Source") {
    static_assert(dvv::InterfaceKind<media::ISourceProvider>::value == dvv::Kind::Source);
    static_assert(dvv::HasInterfaceKind<media::ISourceProvider>);
    CHECK(true);
}
