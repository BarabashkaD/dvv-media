#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "patterns.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;

namespace {

constexpr media::FormatId UNKNOWN_ID = 100000;

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

bool isV210(media::FormatId id) {
    return media::descriptor(id).name == "v210";
}

// The frame sizes of the round trip; v210 needs even widths.
std::vector<std::pair<std::uint32_t, std::uint32_t>> roundTripSizes(media::FormatId id) {
    if (isV210(id)) {
        return {{2, 2}, {34, 17}, {50, 3}, {64, 64}};
    }
    return {{1, 1}, {2, 2}, {33, 17}, {49, 3}, {64, 64}};
}

// 33x17, or 34x17 for v210.
std::uint32_t oddWidth(media::FormatId id) {
    return isV210(id) ? 34U : 33U;
}

media::CodeValues noise(media::FormatId id, std::uint32_t width, std::uint32_t height,
                        std::uint64_t seed) {
    auto values = detail::generate(detail::Pattern::Noise, id, width, height, seed, 0);
    REQUIRE(values.has_value());
    return *values;
}

std::vector<std::byte> packOk(const media::CodeValues& values) {
    auto bytes = media::pack(values);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().message);
    return *bytes;
}

// Every value of every component at its maximum, 2^bits - 1.
media::CodeValues maxValues(media::FormatId id, std::uint32_t width, std::uint32_t height) {
    media::CodeValues values{.format = id, .width = width, .height = height};
    const media::PixelFormatDescriptor& d = media::descriptor(id);
    for (std::size_t c = 0; c < d.componentCount; ++c) {
        const auto [cw, ch] = media::componentSize(id, c, width, height);
        values.components[c].assign(std::size_t{cw} * ch,
                                    static_cast<std::uint16_t>((1U << d.components[c].bits) - 1U));
    }
    return values;
}

// Bytes from a SplitMix64 stream: word k is splitMix64(seed + k), little-endian.
std::vector<std::byte> randomBytes(std::size_t size, std::uint64_t seed) {
    std::vector<std::byte> bytes(size);
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (i % 8 == 0) {
            word = detail::splitMix64(seed + (i / 8));
        }
        bytes[i] = static_cast<std::byte>((word >> (8U * (i % 8))) & 0xFFU);
    }
    return bytes;
}

void checkError(const dvv::Error& error, dvv::Errc code, std::string_view text) {
    CHECK(error.code == code);
    CHECK_MESSAGE(error.message.find(text) != std::string::npos, error.message);
}

}  // namespace

TEST_CASE("pattern generators") {
    CHECK(detail::splitMix64(0) == 0xE220A8397B1DCDAFULL);
    CHECK(detail::splitMix64(1) == 0x910A2DEC89025CC1ULL);
    static_assert(detail::splitMix64(0) == 0xE220A8397B1DCDAFULL);

    CHECK(detail::noiseValue(10, 1, 0, 0, 0, 0) == 0x0E5);
    CHECK(detail::noiseValue(16, 1, 0, 0, 1, 0) == 0xC8F9);

    CHECK(detail::gradientValue(8, 0, 0, 0, 33, 17, 0) == 0);
    CHECK(detail::gradientValue(8, 0, 32, 16, 33, 17, 0) == 255);
    CHECK(detail::gradientValue(8, 1, 0, 0, 33, 17, 0) == 64);
    CHECK(detail::gradientValue(8, 0, 0, 0, 33, 17, 1) == 1);
    CHECK(detail::gradientValue(10, 3, 0, 0, 1, 1, 0) == 768);

    const media::FormatId yuv = idOf("yuv420p10le");
    const auto first = detail::generate(detail::Pattern::Noise, yuv, 33, 17, 1, 0);
    const auto again = detail::generate(detail::Pattern::Noise, yuv, 33, 17, 1, 0);
    const auto other = detail::generate(detail::Pattern::Noise, yuv, 33, 17, 2, 0);
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    REQUIRE(other.has_value());
    CHECK(*first == *again);
    CHECK(*first != *other);
    CHECK(first->format == yuv);
    CHECK(first->width == 33);
    CHECK(first->height == 17);
    CHECK(first->components[0].size() == 33U * 17U);
    CHECK(first->components[1].size() == 17U * 9U);
    CHECK(first->components[2].size() == 17U * 9U);
    CHECK(first->components[3].empty());
    bool in_range = true;
    for (const std::vector<std::uint16_t>& component : first->components) {
        in_range =
            in_range && std::ranges::all_of(component, [](std::uint16_t v) { return v < 1024; });
    }
    CHECK(in_range);

    const auto gray = detail::generate(detail::Pattern::Noise, idOf("gray16le"), 64, 64, 1, 0);
    REQUIRE(gray.has_value());
    CHECK(std::ranges::any_of(gray->components[0], [](std::uint16_t v) { return v >= 0x8000; }));

    const auto v210 = detail::generate(detail::Pattern::Gradient, idOf("v210"), 33, 17, 1, 0);
    REQUIRE_FALSE(v210.has_value());
    CHECK(v210.error().code == dvv::Errc::Format);
}

TEST_CASE("round trip for every format") {
    for (const media::FormatId id : media::catalogue()) {
        for (const auto [width, height] : roundTripSizes(id)) {
            for (const std::uint64_t seed : {1U, 2U}) {
                INFO(std::format("{} {}x{} seed {}", media::descriptor(id).name, width, height,
                                 seed));
                const media::CodeValues x = noise(id, width, height, seed);
                const std::vector<std::byte> bytes = packOk(x);
                const auto frame = media::geometry(id, width, height);
                REQUIRE(frame.has_value());
                CHECK(bytes.size() == frame->bytes);
                const auto back = media::unpack(id, width, height, bytes);
                REQUIRE(back.has_value());
                CHECK(*back == x);
            }
        }
    }
}

TEST_CASE("pack of unpack clears exactly the padding") {
    for (const media::FormatId id : media::catalogue()) {
        const std::uint32_t width = oddWidth(id);
        const std::uint32_t height = 17;
        INFO(media::descriptor(id).name);
        const auto frame = media::geometry(id, width, height);
        REQUIRE(frame.has_value());
        const std::vector<std::byte> bytes = randomBytes(frame->bytes, 7);
        const std::vector<std::byte> mask = packOk(maxValues(id, width, height));
        const auto values = media::unpack(id, width, height, bytes);
        REQUIRE(values.has_value());
        const std::vector<std::byte> repacked = packOk(*values);
        REQUIRE(repacked.size() == bytes.size());
        REQUIRE(mask.size() == bytes.size());
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            mismatches += repacked[i] != (bytes[i] & mask[i]) ? 1U : 0U;
        }
        CHECK(mismatches == 0);

        if (isV210(id)) {
            REQUIRE(frame->planes[0].stride == 128);
            bool padding_clear = true;
            for (std::size_t row = 0; row < height; ++row) {
                for (std::size_t i = 92; i < 128; ++i) {
                    padding_clear = padding_clear && mask[(row * 128) + i] == std::byte{0};
                }
            }
            CHECK(padding_clear);
            CHECK(mask[3] == std::byte{0x3F});
        }
    }
}

TEST_CASE("unpacked planes equal packing in the unpacked format") {
    for (const media::FormatId id : media::catalogue()) {
        const std::uint32_t width = oddWidth(id);
        const std::uint32_t height = 17;
        INFO(media::descriptor(id).name);
        const media::CodeValues x = noise(id, width, height, 1);
        const media::FormatId unpacked = media::unpackedFormat(id);
        const auto unpacked_frame = media::geometry(unpacked, width, height);
        REQUIRE(unpacked_frame.has_value());
        std::vector<std::byte> planes(unpacked_frame->bytes);
        const auto result = media::unpackToPlanes(id, width, height, packOk(x), planes);
        REQUIRE_MESSAGE(result.has_value(), result.error().message);
        CHECK(
            planes ==
            packOk(media::CodeValues{
                .format = unpacked, .width = width, .height = height, .components = x.components}));
    }
}

TEST_CASE("v210 planes are a yuv422p10le frame") {
    const media::FormatId v210 = idOf("v210");
    const media::CodeValues x = noise(v210, 34, 17, 1);
    const media::FormatId yuv = idOf("yuv422p10le");
    const auto frame = media::geometry(yuv, 34, 17);
    REQUIRE(frame.has_value());
    std::vector<std::byte> planes(frame->bytes);
    const auto result = media::unpackToPlanes(v210, 34, 17, packOk(x), planes);
    REQUIRE(result.has_value());
    CHECK(planes == packOk(media::CodeValues{
                        .format = yuv, .width = 34, .height = 17, .components = x.components}));
}

TEST_CASE("errors") {
    const media::FormatId yuv10 = idOf("yuv420p10le");
    SUBCASE("a value out of range") {
        media::CodeValues values = noise(yuv10, 4, 4, 1);
        values.components[1][2] = 1024;
        const auto result = media::pack(values);
        REQUIRE_FALSE(result.has_value());
        checkError(result.error(), dvv::Errc::Internal, "does not fit in 10 bits");
    }
    SUBCASE("a component with one value too few") {
        media::CodeValues values = noise(yuv10, 4, 4, 1);
        values.components[2].pop_back();
        const auto result = media::pack(values);
        REQUIRE_FALSE(result.has_value());
        checkError(result.error(), dvv::Errc::Internal, "expected");
    }
    SUBCASE("an extra component") {
        media::CodeValues values = noise(idOf("yuv420p"), 4, 4, 1);
        values.components[3].push_back(0);
        const auto result = media::pack(values);
        REQUIRE_FALSE(result.has_value());
        checkError(result.error(), dvv::Errc::Internal, "component 3");
    }
    SUBCASE("an odd v210 width") {
        media::CodeValues values{.format = idOf("v210"), .width = 33, .height = 1};
        const auto result = media::pack(values);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == dvv::Errc::Format);
    }
    SUBCASE("original one byte too short") {
        const std::vector<std::byte> bytes = packOk(noise(yuv10, 4, 4, 1));
        const auto result = media::unpack(yuv10, 4, 4, std::span(bytes).first(bytes.size() - 1));
        REQUIRE_FALSE(result.has_value());
        checkError(result.error(), dvv::Errc::Internal, "expected at least");
    }
    SUBCASE("planes one byte too large") {
        const std::vector<std::byte> bytes = packOk(noise(yuv10, 4, 4, 1));
        const auto frame = media::geometry(media::unpackedFormat(yuv10), 4, 4);
        REQUIRE(frame.has_value());
        std::vector<std::byte> planes(frame->bytes + 1);
        const auto result = media::unpackToPlanes(yuv10, 4, 4, bytes, planes);
        REQUIRE_FALSE(result.has_value());
        checkError(result.error(), dvv::Errc::Internal, "exactly");
    }
    SUBCASE("an unknown format") {
        const std::array<std::byte, 16> bytes{};
        const auto result = media::unpack(UNKNOWN_ID, 2, 2, bytes);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == dvv::Errc::Internal);
    }
}
