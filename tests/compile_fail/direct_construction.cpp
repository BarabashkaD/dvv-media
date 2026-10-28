// Compile-fail cases for dvv-media's managed types (spec §9.4). DVV_COMPILE_FAIL_CASE selects one:
//   0  control: a store created through FrameStore::create compiles
//   1  constructing a FrameStore without a key
//   2  creating a HostFrame without a key
//   3  forging a CreationKey<FrameStore> outside the store
//   4  constructing a source with a CreationKey<SourceFactory> forged without a ticket
//   5  constructing an OpenTicket outside the SourceFactory
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/platform/file_reader.h"

namespace {

// A concrete source as the providers write them; only SourceFactory::issueKey can make its key.
class TicketlessSource final : public dvv::media::ISource, public dvv::Managed<TicketlessSource> {
public:
    explicit TicketlessSource(dvv::CreationKey<dvv::media::SourceFactory> key) noexcept
        : Managed(std::move(key)) {}

    [[nodiscard]] const dvv::media::SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] dvv::Task<dvv::Result<dvv::media::FrameRef>> produce(
        std::int64_t /*index*/, dvv::media::ProduceContext& /*ctx*/) override {
        co_return dvv::makeError(dvv::Errc::Internal, "not produced");
    }

private:
    dvv::media::SourceInfo info_;
};

}  // namespace

int main() {
#if DVV_COMPILE_FAIL_CASE == 1
    dvv::media::FrameStore direct(dvv::media::FrameStoreOptions{});
#elif DVV_COMPILE_FAIL_CASE == 2
    // Every argument but the key.
    auto frame = std::make_shared<dvv::media::HostFrame>(
        dvv::media::FrameDesc{}, dvv::media::ViewSpec{}, dvv::platform::AlignedBuffer{},
        std::size_t{0}, std::size_t{0});
#elif DVV_COMPILE_FAIL_CASE == 3
    dvv::CreationKey<dvv::media::FrameStore> forged(
        dvv::LifetimeRegistry::instance().issue<dvv::media::HostFrame>());
#elif DVV_COMPILE_FAIL_CASE == 4
    TicketlessSource source(dvv::CreationKey<dvv::media::SourceFactory>(
        dvv::LifetimeRegistry::instance().issue<TicketlessSource>()));
#elif DVV_COMPILE_FAIL_CASE == 5
    dvv::media::OpenTicket ticket(dvv::NO_OWNER, std::source_location::current());
#endif
    auto pool = dvv::ExecutorFactory::makeThreadPool(1);
    if (!pool) {
        return 1;
    }
    dvv::media::FrameStoreOptions options;
    options.cpu = *pool;
    options.io = *pool;
    const auto store = dvv::media::FrameStore::create(options);
    return store.has_value() ? 0 : 1;
}
