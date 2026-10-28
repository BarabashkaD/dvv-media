#pragma once

// A unique directory under the system temporary directory, removed with its content (the contract
// of dvv-gpu's TemporaryDirectory). For the unit tests; the GPU tests have their own in
// gpu_fixture.h.

#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <random>
#include <system_error>
#include <utility>

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::filesystem::path candidate =
                base / std::format("dvv-media-unit-{:08x}{:08x}", random(), random());
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
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
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};
