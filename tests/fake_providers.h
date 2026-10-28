#pragma once

// Three scripted source providers for the factory tests: fake_a, fake_b and fake_c. Each is a
// Kind::Source component for the schemes "fake" and "file" that needs the capability "fake". Only
// dvv_media_factory_tests links them, so they never compete with the real providers.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/media/source_factory.h"

namespace dvv::media::fakes {

// What one fake provider does and what it saw. The factory calls the providers while the test
// thread waits in syncWait, which orders these plain fields between the threads.
struct FakeScript {
    int confidence = 0;
    std::string why;
    bool throwInMatch = false;
    bool throwInOpen = false;
    bool refuseInOpen = false;  // open() fails Unsupported "<id> refuses the content"
    int matchCalls = 0;
    int openCalls = 0;
    std::vector<std::byte> lastHead;       // the head passed to the last match()
    std::optional<Sidecar> lastSidecar{};  // the request's sidecar in the last match()
};

// The script of "fake_a", "fake_b" or "fake_c".
[[nodiscard]] FakeScript& script(std::string_view id);
// Resets all three scripts to FakeScript{}.
void resetScripts();

}  // namespace dvv::media::fakes
