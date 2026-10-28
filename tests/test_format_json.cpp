#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <latch>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/format_json.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "patterns.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
namespace json = dvv::json;

namespace {

// The schema example of the plan and the README: p010le with every key.
constexpr std::string_view P010LE_JSON = R"({
  "name": "p010le",
  "model": "yuv",
  "widthMultiple": 1,
  "components": [
    {"kind": "Y", "bits": 10, "log2SubX": 0, "log2SubY": 0},
    {"kind": "U", "bits": 10, "log2SubX": 1, "log2SubY": 1},
    {"kind": "V", "bits": 10, "log2SubX": 1, "log2SubY": 1}
  ],
  "planes": [
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 0, "index": 0, "word": 0, "shift": 6}]},
    {"groupPixels": 2, "groupBytes": 4, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 1, "slots": [{"component": 1, "index": 0, "word": 0, "shift": 6},
                              {"component": 2, "index": 0, "word": 1, "shift": 6}]}
  ]
})";

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

json::Value parseOk(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.has_value(), value.error().message);
    return *value;
}

media::FormatId loadOk(const json::Value& value) {
    auto id = media::formatFromJson(value);
    REQUIRE_MESSAGE(id.has_value(), id.error().message);
    return *id;
}

void checkRejects(const json::Value& value, std::string_view text) {
    const auto id = media::formatFromJson(value);
    REQUIRE_FALSE(id.has_value());
    CHECK(id.error().code == dvv::Errc::Format);
    CHECK_MESSAGE(id.error().message.find(text) != std::string::npos, id.error().message);
}

// A descriptor with another name, for comparisons that ignore the name.
media::PixelFormatDescriptor renamed(media::PixelFormatDescriptor descriptor,
                                     std::string_view name) {
    descriptor.name = name;
    return descriptor;
}

json::Value catalogueJson(std::string_view source, std::string_view name) {
    json::Value value = media::formatToJson(idOf(source));
    value["name"] = name;
    return value;
}

// my_y210: y210le under another name (tests 4, 5 and 8).
media::FormatId loadMyY210() {
    return loadOk(catalogueJson("y210le", "my_y210"));
}

media::CodeValues noise(media::FormatId id, std::uint32_t width, std::uint32_t height,
                        std::uint64_t seed) {
    auto values = detail::generate(detail::Pattern::Noise, id, width, height, seed, 0);
    REQUIRE_MESSAGE(values.has_value(), values.error().message);
    return *values;
}

}  // namespace

TEST_CASE("formatToJson writes every key") {
    CHECK(media::formatToJson(idOf("p010le")) == parseOk(P010LE_JSON));
    CHECK(media::formatToJson(100000).is_null());
    // Unpacked formats are written too.
    const json::Value unpacked = media::formatToJson(media::unpackedFormat(idOf("v210")));
    CHECK(unpacked["name"] == "v210@planes");
    CHECK(unpacked["planes"].size() == 3);
}

TEST_CASE("every catalogue entry round-trips") {
    for (const media::FormatId id : media::catalogue()) {
        const media::PixelFormatDescriptor& original = media::descriptor(id);
        CAPTURE(original.name);
        json::Value value = media::formatToJson(id);
        value["name"] = original.name + "_copy";
        const media::FormatId copy = loadOk(value);
        CHECK(copy != id);
        CHECK(renamed(media::descriptor(copy), original.name) == original);
        CHECK(media::formatToJson(copy) == value);
    }
}

TEST_CASE("defaults") {
    json::Value value = parseOk(P010LE_JSON);
    value["name"] = "p010_min";
    value.erase("widthMultiple");
    value["components"][0].erase("log2SubX");
    value["components"][0].erase("log2SubY");
    for (json::Value& plane : value["planes"]) {
        plane.erase("byteOrder");
        plane.erase("rowAlign");
    }
    value["planes"][0].erase("log2SubY");
    const media::FormatId id = loadOk(value);
    CHECK(renamed(media::descriptor(id), "p010le") == media::descriptor(idOf("p010le")));
}

TEST_CASE("interning is idempotent") {
    const media::FormatId first = loadMyY210();
    const media::FormatId second = loadMyY210();
    CHECK(first == second);
    CHECK(media::findFormat("my_y210") == first);
    CHECK(media::catalogue().size() <= first);
}

TEST_CASE("a different descriptor under a loaded name is rejected") {
    const media::FormatId id = loadMyY210();
    json::Value value = catalogueJson("y210le", "my_y210");
    value["planes"][0]["slots"][0]["shift"] = 5;
    checkRejects(value, "already loaded");
    CHECK(media::findFormat("my_y210") == id);
}

TEST_CASE("reserved names") {
    checkRejects(catalogueJson("nv12", "nv12"), "built-in");
    checkRejects(catalogueJson("nv12", "nv12@planes"), "reserved");
    checkRejects(catalogueJson("nv12", "x@display"), "reserved");
    checkRejects(catalogueJson("nv12", "a b"), "invalid name");
}

TEST_CASE("malformed JSON values") {
    const json::Value base = [] {
        json::Value value = parseOk(P010LE_JSON);
        value["name"] = "bad_format";
        return value;
    }();

    SUBCASE("not an object") {
        checkRejects(json::Value(42), "not an object");
    }
    SUBCASE("missing name") {
        json::Value value = base;
        value.erase("name");
        checkRejects(value, "name");
    }
    SUBCASE("unknown key") {
        json::Value value = base;
        value["components"][0]["bitz"] = 8;
        checkRejects(value, "unknown key 'bitz'");
    }
    SUBCASE("unknown model") {
        json::Value value = base;
        value["model"] = "xyz";
        checkRejects(value, "unknown value 'xyz'");
    }
    SUBCASE("unknown kind") {
        json::Value value = base;
        value["components"][1]["kind"] = "Q";
        checkRejects(value, "unknown value 'Q'");
    }
    SUBCASE("too many bits") {
        json::Value value = base;
        value["components"][0]["bits"] = 17;
        checkRejects(value, "bits must be 1 to 16");
    }
    SUBCASE("5 components") {
        json::Value value = base;
        value["components"].push_back(value["components"][0]);
        value["components"].push_back(value["components"][0]);
        checkRejects(value, "expected 1 to 4 entries");
    }
    SUBCASE("13 slots") {
        json::Value value = base;
        json::Value& slots = value["planes"][0]["slots"];
        for (int i = 0; i < 12; ++i) {
            slots.push_back(slots[0]);
        }
        REQUIRE(slots.size() == 13);
        checkRejects(value, "expected 1 to 12 entries");
    }
    SUBCASE("shift out of range") {
        json::Value value = base;
        value["planes"][0]["slots"][0]["shift"] = 300;
        checkRejects(value, "planes[0].slots[0].shift");
    }
    SUBCASE("negative shift") {
        json::Value value = base;
        value["planes"][0]["slots"][0]["shift"] = -1;
        checkRejects(value, "planes[0].slots[0].shift");
    }
    SUBCASE("fractional shift") {
        json::Value value = base;
        value["planes"][0]["slots"][0]["shift"] = 1.5;
        checkRejects(value, "planes[0].slots[0].shift");
    }
    SUBCASE("unknown byte order") {
        json::Value value = base;
        value["planes"][1]["byteOrder"] = "middle";
        checkRejects(value, "unknown value 'middle'");
    }
    SUBCASE("more than 6 samples of a component per group") {
        json::Value slots = json::Value::array();
        for (int i = 0; i < 7; ++i) {
            slots.push_back({{"component", 0}, {"index", i}, {"word", i}, {"shift", 0}});
        }
        const json::Value value = {
            {"name", "gray_x7"},
            {"model", "gray"},
            {"components", json::Value::array({{{"kind", "Y"}, {"bits", 8}}})},
            {"planes",
             json::Value::array(
                 {{{"groupPixels", 7}, {"groupBytes", 7}, {"wordBytes", 1}, {"slots", slots}}})}};
        checkRejects(value, "GPU limit");
    }
}

TEST_CASE("a custom format works with geometry and the reference") {
    const media::FormatId custom = loadMyY210();
    const media::FormatId y210 = idOf("y210le");
    const auto custom_geometry = media::geometry(custom, 33, 17);
    const auto y210_geometry = media::geometry(y210, 33, 17);
    REQUIRE(custom_geometry.has_value());
    REQUIRE(y210_geometry.has_value());
    CHECK(*custom_geometry == *y210_geometry);

    const media::CodeValues values = noise(custom, 33, 17, 7);
    media::CodeValues same_in_y210 = values;
    same_in_y210.format = y210;
    const auto custom_bytes = media::pack(values);
    const auto y210_bytes = media::pack(same_in_y210);
    REQUIRE(custom_bytes.has_value());
    REQUIRE(y210_bytes.has_value());
    CHECK(*custom_bytes == *y210_bytes);

    const auto unpacked = media::unpack(custom, 33, 17, *custom_bytes);
    REQUIRE(unpacked.has_value());
    CHECK(*unpacked == values);
}

TEST_CASE("concurrent interning") {
    constexpr std::size_t THREADS = 8;
    constexpr std::size_t FORMATS = 20;
    constexpr int READS = 1000;
    const json::Value gray10 = media::formatToJson(idOf("gray10le"));
    std::vector<std::string> catalogue_names;
    for (const media::FormatId id : media::catalogue()) {
        catalogue_names.push_back(media::descriptor(id).name);
    }

    std::vector<std::vector<std::optional<media::FormatId>>> results(
        THREADS, std::vector<std::optional<media::FormatId>>(FORMATS));
    std::atomic<bool> names_right{true};
    {
        std::latch start(THREADS + 1);
        std::vector<std::thread> threads;
        threads.reserve(THREADS + 1);
        for (std::size_t t = 0; t < THREADS; ++t) {
            threads.emplace_back([t, &gray10, &results, &start] {
                start.arrive_and_wait();
                for (std::size_t k = 0; k < FORMATS; ++k) {
                    json::Value value = gray10;
                    value["name"] = std::format("t{}_{}", t, k);
                    const auto id = media::formatFromJson(value);
                    if (id) {
                        results[t][k] = *id;
                    }
                }
            });
        }
        threads.emplace_back([&catalogue_names, &names_right, &start] {
            start.arrive_and_wait();
            for (int i = 0; i < READS; ++i) {
                for (const media::FormatId id : media::catalogue()) {
                    if (media::descriptor(id).name != catalogue_names[id]) {
                        names_right = false;
                    }
                }
            }
        });
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    std::set<media::FormatId> ids;
    for (std::size_t t = 0; t < THREADS; ++t) {
        for (std::size_t k = 0; k < FORMATS; ++k) {
            const std::string name = std::format("t{}_{}", t, k);
            CAPTURE(name);
            REQUIRE(results[t][k].has_value());
            const media::FormatId id = results[t][k].value_or(0);
            CHECK(media::findFormat(name) == id);
            CHECK(media::descriptor(id).name == name);
            ids.insert(id);
        }
    }
    CHECK(ids.size() == THREADS * FORMATS);
    CHECK(names_right.load());
}
