#pragma once

// Fakes for the frame store tests: a source, a frame, a converter, a gate, a counting executor
// and the fixture.

#include <doctest/doctest.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "converters.h"
#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/testing/test_authority.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "store_state.h"

namespace dvv::media::test {

// The byte the fake source writes at offset i of frame `index`.
[[nodiscard]] constexpr std::byte pattern(std::int64_t index, std::size_t i) noexcept {
    return static_cast<std::byte>((static_cast<std::uint64_t>(index) * 31U + i) & 0xFFU);
}

// Suspends its awaiters until open(); then resumes each through a post to its executor.
class Gate {
public:
    auto wait(IExecutor& executor) {
        struct Awaiter {
            Gate* gate;
            IExecutor* executor;
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            bool await_suspend(std::coroutine_handle<> handle) {
                const std::scoped_lock lock(gate->mutex_);
                if (gate->open_) {
                    return false;
                }
                gate->waiters_.emplace_back(handle, executor);
                return true;
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{this, &executor};
    }
    void open() {
        std::vector<std::pair<std::coroutine_handle<>, IExecutor*>> waiters;
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
            waiters.swap(waiters_);
        }
        for (auto& [handle, executor] : waiters) {
            executor->post([handle] { handle.resume(); });
        }
    }
    [[nodiscard]] std::size_t waiting() const {
        const std::scoped_lock lock(mutex_);
        return waiters_.size();
    }

private:
    mutable std::mutex mutex_;
    bool open_ = false;
    std::vector<std::pair<std::coroutine_handle<>, IExecutor*>> waiters_;
};

class FakeSource final : public ISource, public Managed<FakeSource> {
public:
    FakeSource(CreationKey<dvv::test::TestAuthority> key, FrameDesc desc, std::int64_t frames);

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override;

    std::atomic<int> attempts{0};
    std::atomic<int> produced{0};
    std::atomic<int> failNext{0};  // that many produces return Io "fake read error"
    std::atomic<bool> throwNext{false};
    Gate* gate = nullptr;  // awaited on ctx.io() before allocating

private:
    SourceInfo info_;
};

class FakeFrame final : public IFrame, public Managed<FakeFrame> {
public:
    FakeFrame(CreationKey<dvv::test::TestAuthority> key, FrameDesc desc,
              std::vector<ViewSpec> views, std::vector<std::byte> payload);

    [[nodiscard]] const FrameDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] std::span<const ViewSpec> nativeViews() const noexcept override { return views_; }
    // One plane over the payload for any native view.
    [[nodiscard]] Binding bind(const ViewSpec& native) const override;
    [[nodiscard]] std::size_t bytes() const noexcept override { return payload_.size(); }

private:
    FrameDesc desc_;
    std::vector<ViewSpec> views_;
    std::vector<std::byte> payload_;
};

class FakeConverter final : public detail::Converter {
public:
    struct Config {
        std::string name;
        detail::StateKey from;
        detail::StateKey to;
        LayoutPolicy layout = LayoutPolicy::Tight;
        Fidelity fidelity = Fidelity::Lossless;
        double cost = 1;
        Gate* gate = nullptr;
    };
    explicit FakeConverter(Config config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return config_.name; }
    [[nodiscard]] detail::StateKey from() const noexcept override { return config_.from; }
    [[nodiscard]] detail::StateKey to() const noexcept override { return config_.to; }
    [[nodiscard]] LayoutPolicy outputLayout() const noexcept override { return config_.layout; }
    [[nodiscard]] Fidelity fidelity() const noexcept override { return config_.fidelity; }
    [[nodiscard]] double cost(const FrameDesc& /*desc*/) const noexcept override {
        return config_.cost;
    }
    [[nodiscard]] Task<Result<std::shared_ptr<IFrame>>> run(
        detail::ConvertInput input, detail::ConvertContext context) const override;

    mutable std::atomic<int> runs{0};
    mutable std::atomic<int> failNext{0};
    mutable std::atomic<Errc> failWith{Errc::Io};
    mutable std::atomic<bool> wrongView{false};  // the frame's view gets layout RowAlign256

private:
    Config config_;
};

// Forwards every post to a target and counts the posts.
class CountingExecutor final : public IExecutor {
public:
    explicit CountingExecutor(IExecutor& target) noexcept : target_(&target) {}
    void post(std::move_only_function<void()> work) override;
    [[nodiscard]] std::size_t posts() const noexcept {
        return posts_.load(std::memory_order_relaxed);
    }

private:
    IExecutor* target_;
    std::atomic<std::size_t> posts_{0};
};

// Polls every 1 ms for up to 10 s; REQUIREs that the predicate became true.
void waitUntil(const std::function<bool()>& predicate);
// The same poll without REQUIRE, for helper threads (a REQUIRE there would terminate the process):
// true when the predicate became true. Openers do `reached = waitFor(...); gate.open();` and the
// test thread checks `reached` after joining them.
[[nodiscard]] bool waitFor(const std::function<bool()>& predicate);

class StoreTest {
public:
    StoreTest();

    // A store with these converters and test domains; `tweak` adjusts the options first.
    [[nodiscard]] std::shared_ptr<FrameStore> makeStore(
        std::vector<std::unique_ptr<detail::Converter>> converters = {},
        detail::StoreAccess::TestDomains domains = {},
        const std::function<void(FrameStoreOptions&)>& tweak = {});
    [[nodiscard]] std::shared_ptr<FakeSource> makeSource(std::int64_t frames = 4) const;

    std::shared_ptr<ThreadPool> cpu;
    std::shared_ptr<ThreadPool> io;
    std::shared_ptr<ThreadPool> requester;
    FrameDesc desc;
    ViewSpec hostView;
    ViewSpec planesView;
};

}  // namespace dvv::media::test
