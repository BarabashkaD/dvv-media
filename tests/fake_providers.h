#pragma once

// Four scripted source providers for the factory tests: fake_a, fake_b, fake_c and gated. Each is
// a component registered for ISourceProvider that needs the capability "fake"; the first three
// open the schemes "fake" and "file", gated opens "gated". Only dvv_media_factory_tests links
// them, so they never compete with the real providers.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/executor.h"
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
// Resets all three scripts to FakeScript{} and the gated state (see reset()).
void resetScripts();

// The provider "gated" (scheme "gated"): its open() waits at a test gate, then records whether
// the token was stopped and makes a source that counts its destructions.
class GatedState {
public:
    // Suspends its awaiter on `executor` until open().
    [[nodiscard]] auto wait(IExecutor& executor) {
        struct Awaiter {
            GatedState* state;
            IExecutor* executor;
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            bool await_suspend(std::coroutine_handle<> handle) {
                const std::scoped_lock lock(state->mutex_);
                if (state->open_) {
                    return false;
                }
                state->waiters_.emplace_back(handle, executor);
                return true;
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{this, &executor};
    }
    // Opens the gate: every waiter resumes through a post to its executor.
    void open();
    // Closes the gate again and clears what the provider saw; the destruction counter stays.
    void reset();
    // How many opens wait at the gate.
    [[nodiscard]] std::size_t waiting() const;

    std::atomic<bool> sawStop{false};  // context.stop.stop_requested() after the gate
    std::atomic<int> opens{0};         // opens that reached the gate
    std::atomic<int> destroyed{0};     // sources destroyed since the process started

private:
    mutable std::mutex mutex_;
    bool open_ = false;
    std::vector<std::pair<std::coroutine_handle<>, IExecutor*>> waiters_;
};

// The state of "gated".
[[nodiscard]] GatedState& gated();

}  // namespace dvv::media::fakes
