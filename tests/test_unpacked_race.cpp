// The first creation of the unpacked formats under a race. This file is its own test executable
// (dvv_media.unpacked_race): in the unit test process earlier cases have already created every
// unpacked format, so the threads there would only read existing entries.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <latch>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dvv/media/pixel_format.h"

namespace media = dvv::media;

namespace {

constexpr std::size_t CATALOGUE_SIZE = 37;

std::string unpackedName(media::FormatId format) {
    return media::descriptor(format).name + "@planes";
}

}  // namespace

TEST_CASE("concurrent first creation of unpacked formats") {
    REQUIRE(media::catalogue().size() == CATALOGUE_SIZE);
    // The precondition that makes the race real: no unpacked format exists yet in this process.
    std::size_t existing = 0;
    for (const media::FormatId format : media::catalogue()) {
        existing += media::findFormat(unpackedName(format)).has_value() ? 1U : 0U;
    }
    REQUIRE(existing == 0);

    // All threads start together and ask for the formats in the same order, so several of them
    // create the same format at the same time.
    constexpr std::size_t THREADS = 8;
    std::vector<std::vector<media::FormatId>> results(
        THREADS, std::vector<media::FormatId>(CATALOGUE_SIZE, 0));
    {
        std::latch start(THREADS);
        std::vector<std::thread> threads;
        threads.reserve(THREADS);
        for (std::size_t t = 0; t < THREADS; ++t) {
            threads.emplace_back([t, &results, &start] {
                start.arrive_and_wait();
                for (std::size_t id = 0; id < CATALOGUE_SIZE; ++id) {
                    results[t][id] = media::unpackedFormat(static_cast<media::FormatId>(id));
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    // Every thread got the same id, and exactly one entry was interned per format: the ids are
    // 37..73 and nothing follows them.
    std::set<media::FormatId> ids;
    for (std::size_t id = 0; id < CATALOGUE_SIZE; ++id) {
        CAPTURE(id);
        const media::FormatId expected = results[0][id];
        for (std::size_t t = 1; t < THREADS; ++t) {
            CHECK(results[t][id] == expected);
        }
        CHECK(media::isUnpacked(expected));
        CHECK(media::findFormat(unpackedName(static_cast<media::FormatId>(id))) == expected);
        CHECK(media::unpackedFormat(static_cast<media::FormatId>(id)) == expected);
        ids.insert(expected);
    }
    CHECK(ids.size() == CATALOGUE_SIZE);
    CHECK(*ids.begin() == CATALOGUE_SIZE);
    CHECK(*ids.rbegin() == (2 * CATALOGUE_SIZE) - 1);
    CHECK(media::descriptor(static_cast<media::FormatId>(2 * CATALOGUE_SIZE)).name.empty());
}
