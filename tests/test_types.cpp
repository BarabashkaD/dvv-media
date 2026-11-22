#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "dvv/base/utf8.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"

namespace media = dvv::media;
namespace fs = std::filesystem;

static_assert(media::FALLBACK_FPS == media::Rational{25, 1});

TEST_CASE("frame keys compare and hash by value") {
    const media::FrameKey key{.origin = 1, .index = 2};
    CHECK(key == media::FrameKey{.origin = 1, .index = 2});
    CHECK(key != media::FrameKey{.origin = 1, .index = 3});
    CHECK(key != media::FrameKey{.origin = 2, .index = 2});
    const media::FrameKeyHash hash;
    CHECK(hash(key) == hash(media::FrameKey{.origin = 1, .index = 2}));

    std::unordered_set<media::FrameKey, media::FrameKeyHash> keys;
    for (media::OriginId origin = 1; origin <= 10; ++origin) {
        for (std::int64_t index = 0; index < 100; ++index) {
            keys.insert(media::FrameKey{.origin = origin, .index = index});
        }
    }
    CHECK(keys.size() == 1000);
    bool all_found = true;
    for (media::OriginId origin = 1; origin <= 10; ++origin) {
        for (std::int64_t index = 0; index < 100; ++index) {
            all_found =
                all_found && keys.contains(media::FrameKey{.origin = origin, .index = index});
        }
    }
    CHECK(all_found);
}

TEST_CASE("frame key hashes spread nearby keys") {
    // 1024 consecutive indices, and 1024 consecutive origins: a well-mixed hash gives 1024
    // distinct values whose low 8 bits (a small hash table's bucket) cover nearly all 256 buckets
    // (about 251 expected); an unmixed one, e.g. origin ^ index, would cover far fewer.
    const media::FrameKeyHash hash;
    constexpr std::size_t KEYS = 1024;
    std::set<std::size_t> by_index;
    std::set<std::size_t> by_origin;
    std::set<std::size_t> index_buckets;
    std::set<std::size_t> origin_buckets;
    for (std::size_t k = 0; k < KEYS; ++k) {
        const std::size_t h_index =
            hash(media::FrameKey{.origin = 1, .index = static_cast<std::int64_t>(k)});
        const std::size_t h_origin = hash(media::FrameKey{.origin = k + 1, .index = 0});
        by_index.insert(h_index);
        by_origin.insert(h_origin);
        index_buckets.insert(h_index & 0xFFU);
        origin_buckets.insert(h_origin & 0xFFU);
    }
    CHECK(by_index.size() == KEYS);
    CHECK(by_origin.size() == KEYS);
    CHECK(index_buckets.size() >= 200);
    CHECK(origin_buckets.size() >= 200);
}

TEST_CASE("value types compare by every field") {
    CHECK(media::FrameDesc{} == media::FrameDesc{});
    CHECK(media::ViewSpec{} == media::ViewSpec{});
    CHECK(media::Domain{} == media::Domain{});
    CHECK(media::Rational{} == media::Rational{});

    const media::ViewSpec base{};
    auto differs = [&base](auto change) {
        media::ViewSpec changed = base;
        change(changed);
        return changed != base;
    };
    CHECK(differs([](media::ViewSpec& spec) { spec.format = 1; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.colour = 1; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.layout = media::LayoutPolicy::Tight; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.binding = media::vulkan::BUFFER; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.domain.kind = media::MemKind::HostVisible; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.domain.device = dvv::gpu::DeviceId{1}; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.domain.generation = 1; }));
    CHECK(differs([](media::ViewSpec& spec) { spec.downscaleLog2 = 1; }));

    CHECK(media::Rational{.num = 50, .den = 2} != media::Rational{.num = 25, .den = 1});
}

TEST_CASE("binding kinds compare by name") {
    static_assert(media::HOST_PLANES == media::BindingKind{"HostPlanes"});
    static_assert(media::vulkan::BUFFER != media::vulkan::IMAGE);
    static_assert(media::vulkan::IMAGE != media::HOST_PLANES);
    CHECK(media::HOST_PLANES.name() == "HostPlanes");
    CHECK(media::vulkan::BUFFER.name() == "VkBuffer");
    CHECK(media::vulkan::IMAGE.name() == "VkImage");
}

TEST_CASE("a binding gives only its own kind") {
    const media::Binding empty;
    CHECK(empty.kind() == media::HOST_PLANES);
    REQUIRE(empty.planes() != nullptr);
    const media::HostPlanesB* none = empty.planes();
    CHECK(none->planeCount == 0);

    const std::array<std::byte, 6> bytes{};
    media::HostPlanesB host;
    host.planes[0] = std::span<const std::byte>(bytes).first(4);
    host.planes[1] = std::span<const std::byte>(bytes).subspan(4);
    host.strides[0] = 2;
    host.strides[1] = 1;
    host.planeCount = 2;
    const media::Binding binding{host};
    CHECK(binding.kind() == media::HOST_PLANES);
    const media::HostPlanesB* read = binding.planes();
    REQUIRE(read != nullptr);
    CHECK(read->planeCount == 2);
    CHECK(read->planes[0].data() == bytes.data());
    CHECK(read->planes[0].size() == 4);
    CHECK(read->planes[1].data() == bytes.data() + 4);
    CHECK(read->planes[1].size() == 2);
    CHECK(read->strides == host.strides);
    CHECK(media::vulkan::buffer(binding) == nullptr);
    CHECK(media::vulkan::image(binding, 0) == nullptr);
    CHECK(media::vulkan::imageCount(binding) == 0);
}

static_assert(media::FixedFields::all().covers(media::FixedFields{.format = true}) &&
              !media::FixedFields{}.covers(media::FixedFields::all()));

TEST_CASE("colour interning") {
    CHECK(media::internColour(media::ColourParams{}) == 0);
    CHECK(media::colourParams(0) == media::ColourParams{});

    const media::ColourParams bt709{.matrix = media::ColourMatrix::Bt709,
                                    .range = media::ColourRange::Limited,
                                    .transfer = media::TransferFn::Bt1886,
                                    .primaries = media::Primaries::Bt709};
    const media::ColourId id = media::internColour(bt709);
    CHECK(id != 0);
    CHECK(media::internColour(bt709) == id);
    CHECK(media::colourParams(id) == bt709);

    media::ColourParams full = bt709;
    full.range = media::ColourRange::Full;
    const media::ColourId full_id = media::internColour(full);
    CHECK(full_id != id);
    CHECK(full_id != 0);
    CHECK(media::colourParams(full_id) == full);

    CHECK(media::colourParams(0xFFFFFFFFU) == media::ColourParams{});

    // The custom matrix's kr and kb are part of the value.
    const media::ColourParams custom{.matrix = media::ColourMatrix::Custom,
                                     .range = media::ColourRange::Limited,
                                     .kr = 0.212f,
                                     .kb = 0.087f};
    const media::ColourId custom_id = media::internColour(custom);
    CHECK(media::internColour(custom) == custom_id);
    CHECK(media::colourParams(custom_id) == custom);
    media::ColourParams other_kb = custom;
    other_kb.kb = 0.088f;
    CHECK(media::internColour(other_kb) != custom_id);
}

namespace {

// Every combination of the colour enums without the custom matrix (whose kr and kb are free):
// 5 matrices x 3 ranges x 6 transfers x 5 primaries.
std::vector<media::ColourParams> allColourCombinations() {
    std::vector<media::ColourParams> all;
    for (std::uint8_t matrix = 0; matrix < 5; ++matrix) {
        for (std::uint8_t range = 0; range < 3; ++range) {
            for (std::uint8_t transfer = 0; transfer < 6; ++transfer) {
                for (std::uint8_t primaries = 0; primaries < 5; ++primaries) {
                    all.push_back({.matrix = static_cast<media::ColourMatrix>(matrix),
                                   .range = static_cast<media::ColourRange>(range),
                                   .transfer = static_cast<media::TransferFn>(transfer),
                                   .primaries = static_cast<media::Primaries>(primaries)});
                }
            }
        }
    }
    return all;
}

}  // namespace

TEST_CASE("colour interning is thread-safe and references stay valid") {
    const media::ColourParams p0{.matrix = media::ColourMatrix::Bt601,
                                 .range = media::ColourRange::Full,
                                 .transfer = media::TransferFn::Srgb,
                                 .primaries = media::Primaries::Bt601_625};
    const media::ColourParams& first = media::colourParams(media::internColour(p0));

    const std::vector<media::ColourParams> all = allColourCombinations();
    REQUIRE(all.size() == 450);
    constexpr std::size_t THREADS = 8;
    // ids[t][i] is the id thread t got for all[i]; no doctest assertion runs inside the threads.
    std::vector<std::vector<media::ColourId>> ids(THREADS,
                                                  std::vector<media::ColourId>(all.size()));
    {
        std::vector<std::thread> threads;
        threads.reserve(THREADS);
        for (std::size_t t = 0; t < THREADS; ++t) {
            threads.emplace_back([&all, &ids, t] {
                // Each thread starts at a different place in the list.
                const std::size_t start = t * all.size() / THREADS;
                for (std::size_t n = 0; n < all.size(); ++n) {
                    const std::size_t i = (start + n) % all.size();
                    ids[t][i] = media::internColour(all[i]);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    for (std::size_t t = 1; t < THREADS; ++t) {
        CHECK(ids[t] == ids[0]);
    }
    const std::set<media::ColourId> distinct(ids[0].begin(), ids[0].end());
    CHECK(distinct.size() == all.size());
    bool all_match = true;
    for (std::size_t i = 0; i < all.size(); ++i) {
        all_match = all_match && media::colourParams(ids[0][i]) == all[i];
    }
    CHECK(all_match);
    CHECK(&first == &media::colourParams(media::internColour(p0)));
    CHECK(first == p0);
}

TEST_CASE("pathFromText keeps non-ASCII file names") {
    const std::string name = "Grüße-日本-🎞.yuv";
    std::random_device random;
    const fs::path dir =
        fs::temp_directory_path() /
        ("dvv_media_test_types_" + std::to_string(random()) + "_" + std::to_string(random()));
    REQUIRE(fs::create_directory(dir));
    {
        std::ofstream file(dir / dvv::pathFromText(name), std::ios::binary);
        REQUIRE(file);
        file.write("abc", 3);
    }

    std::vector<fs::path> entries;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        entries.push_back(entry.path());
    }
    REQUIRE(entries.size() == 1);
    CHECK(dvv::pathText(entries[0].filename()) == name);
    CHECK(fs::file_size(entries[0]) == 3);

    std::error_code error;
    fs::remove_all(dir, error);
    CHECK(!error);
}

TEST_CASE("URI path text round-trips through dvv-base") {
    auto round_trip = [](const std::string& text) {
        return dvv::pathText(dvv::pathFromText(text));
    };
#if !defined(_WIN32)
    // POSIX: the bytes are the path, unchanged.
    CHECK(round_trip("a\xFF"
                     "b") ==
          "a\xFF"
          "b");
    CHECK(round_trip("\xE6\x97") == "\xE6\x97");                  // truncated
    CHECK(round_trip("\xC0\xAF") == "\xC0\xAF");                  // overlong
    CHECK(round_trip("\xED\xA0\x80") == "\xED\xA0\x80");          // surrogate
    CHECK(round_trip("\xF0\x9F\x8E\x9E") == "\xF0\x9F\x8E\x9E");  // U+1F39E, valid
#else
    // Windows: each maximal ill-formed subpart becomes U+FFFD.
    const std::string replacement = "\xEF\xBF\xBD";
    CHECK(round_trip("a\xFF"
                     "b") == "a" + replacement + "b");
    CHECK(round_trip("\xE6\x97") == replacement);                                  // truncated
    CHECK(round_trip("\xC0\xAF") == replacement + replacement);                    // overlong
    CHECK(round_trip("\xED\xA0\x80") == replacement + replacement + replacement);  // surrogate
    CHECK(round_trip("\xF0\x9F\x8E\x9E") == "\xF0\x9F\x8E\x9E");                   // U+1F39E, valid
#endif
    CHECK(round_trip("").empty());
}
