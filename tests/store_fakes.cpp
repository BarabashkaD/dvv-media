#include "store_fakes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/media/pixel_format.h"

namespace dvv::media::test {

namespace {

// Takes one unit of a countdown knob; true when there was one.
bool takeOne(std::atomic<int>& knob) noexcept {
    int current = knob.load();
    while (current > 0 && !knob.compare_exchange_weak(current, current - 1)) {
    }
    return current > 0;
}

}  // namespace

FakeSource::FakeSource(CreationKey<dvv::test::TestAuthority> key, FrameDesc desc,
                       std::int64_t frames)
    : Managed<FakeSource>(std::move(key)) {
    info_.desc = desc;
    info_.fps = Rational{25, 1};
    info_.frameCount = frames;
    info_.resolvedUri = "fake:";
    info_.provider = "fake";
}

Task<Result<FrameRef>> FakeSource::produce(std::int64_t index, ProduceContext& ctx) {
    if (index < 0 || index >= info_.frameCount.value_or(0)) {
        co_return makeError(Errc::Internal, std::format("fake: frame {} out of range", index));
    }
    attempts.fetch_add(1);
    co_await resumeOn(ctx.io());
    if (gate != nullptr) {
        co_await gate->wait(ctx.io());
    }
    if (takeOne(failNext)) {
        co_return makeError(Errc::Io, "fake read error");
    }
    if (throwNext.exchange(false)) {
        throw std::runtime_error("boom");
    }
    const FrameGeometry layout =
        co_try(geometry(info_.desc.format, info_.desc.width, info_.desc.height));
    const std::shared_ptr<HostFrame> frame =
        co_try(ctx.allocateHost(info_.desc, static_cast<std::size_t>(layout.bytes), 64, 0));
    const std::span<std::byte> data = frame->data();
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = pattern(index, i);
    }
    FrameRef ref = co_try(ctx.publish(frame));
    produced.fetch_add(1);
    co_return ref;
}

FakeFrame::FakeFrame(CreationKey<dvv::test::TestAuthority> key, FrameDesc desc,
                     std::vector<ViewSpec> views, std::vector<std::byte> payload)
    : Managed<FakeFrame>(std::move(key)),
      desc_(desc),
      views_(std::move(views)),
      payload_(std::move(payload)) {}

Binding FakeFrame::bind(const ViewSpec& native) const {
    if (std::ranges::find(views_, native) == views_.end()) {
        return HostPlanesB{};
    }
    HostPlanesB planes;
    planes.planes[0] = std::span<const std::byte>(payload_);
    planes.strides[0] =
        static_cast<std::uint32_t>(payload_.size() / std::max<std::uint32_t>(desc_.height, 1));
    planes.planeCount = 1;
    return planes;
}

Task<Result<std::shared_ptr<IFrame>>> FakeConverter::run(detail::ConvertInput input,
                                                         detail::ConvertContext context) const {
    co_await resumeOn(context.cpu);
    if (config_.gate != nullptr) {
        co_await config_.gate->wait(context.cpu);
    }
    runs.fetch_add(1);
    if (takeOne(failNext)) {
        co_return makeError(failWith.load(), "fake convert error");
    }
    const auto* planes = std::get_if<HostPlanesB>(&input.binding);
    if (planes == nullptr || planes->planeCount == 0) {
        co_return makeError(Errc::Internal, "fake converter: the input has no host planes");
    }
    std::vector<std::byte> payload(planes->planes[0].begin(), planes->planes[0].end());
    ViewSpec view = context.output;
    if (wrongView.load()) {
        view.layout = LayoutPolicy::RowAlign256;
    }
    co_return std::shared_ptr<IFrame>(dvv::test::TestAuthority::makeShared<FakeFrame>(
        input.desc, std::vector<ViewSpec>{view}, std::move(payload)));
}

void CountingExecutor::post(std::move_only_function<void()> work) {
    posts_.fetch_add(1, std::memory_order_relaxed);
    target_->post(std::move(work));
}

bool waitFor(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool reached = predicate();
    while (!reached && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        reached = predicate();
    }
    return reached;
}

void waitUntil(const std::function<bool()>& predicate) {
    REQUIRE(waitFor(predicate));
}

StoreTest::StoreTest() {
    auto made_cpu = ExecutorFactory::makeThreadPool(4);
    auto made_io = ExecutorFactory::makeThreadPool(2);
    auto made_requester = ExecutorFactory::makeThreadPool(1);
    REQUIRE(made_cpu.has_value());
    REQUIRE(made_io.has_value());
    REQUIRE(made_requester.has_value());
    cpu = std::move(*made_cpu);
    io = std::move(*made_io);
    requester = std::move(*made_requester);
    const auto gray = findFormat("gray");
    REQUIRE(gray.has_value());
    desc = FrameDesc{gray.value_or(FormatId{0}), 0, 16, 8};
    hostView = ViewSpec{desc.format, 0, LayoutPolicy::Any, Api::HostPlanes, Domain{}, 0};
    planesView = hostView;
    planesView.format = unpackedFormat(desc.format);
}

std::shared_ptr<FrameStore> StoreTest::makeStore(
    std::vector<std::unique_ptr<detail::Converter>> converters,
    detail::StoreAccess::TestDomains domains,
    const std::function<void(FrameStoreOptions&)>& tweak) {
    FrameStoreOptions options;
    options.cpu = cpu;
    options.io = io;
    if (tweak) {
        tweak(options);
    }
    auto store = detail::StoreAccess::create(std::move(options), std::move(converters), domains);
    REQUIRE(store.has_value());
    return std::move(*store);
}

std::shared_ptr<FakeSource> StoreTest::makeSource(std::int64_t frames) const {
    return dvv::test::TestAuthority::makeShared<FakeSource>(desc, frames);
}

}  // namespace dvv::media::test
