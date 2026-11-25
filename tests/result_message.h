#pragma once

// The message of a failed Result, for REQUIRE_MESSAGE and CHECK_MESSAGE in any test.

#include <string>

#include "dvv/base/error.h"

namespace dvv::media::test {

// The message of a failed result, empty on success.
template <typename T>
[[nodiscard]] std::string messageOf(const Result<T>& result) {
    return result.has_value() ? std::string{} : result.error().describe();
}

}  // namespace dvv::media::test
