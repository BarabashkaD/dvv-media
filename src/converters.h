#pragma once
// Converters (M2 design §5.3): edges of the store's static graph. Private to dvv-media.

#include <memory>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/task.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/media/frame.h"
#include "dvv/media/types.h"

namespace dvv::media {
class FrameStore;
}

namespace dvv::media::detail {

// A graph state: the original format or its unpacked form, the API and the memory kind; with
// `display` the display format and colour (M3 D6) instead of the frame's format and colour.
// `display` is the last member, so positional initialisers of the M2 states stay valid.
struct StateKey {
    bool unpacked = false;
    Api api = Api::HostPlanes;
    MemKind kind = MemKind::Host;
    bool display = false;
    bool operator==(const StateKey&) const = default;
};
// What Produce makes: the original, as host planes in host memory.
inline constexpr StateKey PRODUCED{
    .unpacked = false, .api = Api::HostPlanes, .kind = MemKind::Host};
// What ToDisplay makes: the display image in device memory (displayFormat(), displayColour()).
inline constexpr StateKey DISPLAY_STATE{
    .unpacked = false, .api = Api::VkImage, .kind = MemKind::DeviceLocal, .display = true};

struct ConvertInput {
    const FrameKey& key;     // the key being converted
    const FrameDesc& desc;   // the key's frame description
    const ViewSpec& view;    // the input view (a native view of `frame`)
    const IFrame& frame;     // kept alive by the step for the whole call
    const Binding& binding;  // frame.bind(view)
};
struct ConvertContext {
    FrameStore& store;  // makes frames: StoreAccess::makeFrame<T>(store, ...)
    IExecutor& cpu;
    gpu::ExecutionContext* gpu;  // null in a host-only store
    const ViewSpec& output;      // the planned output view; the frame must offer it
};

class Converter {
public:
    Converter() = default;
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;
    Converter(Converter&&) = delete;
    Converter& operator=(Converter&&) = delete;
    virtual ~Converter() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;  // stats key
    [[nodiscard]] virtual StateKey from() const noexcept = 0;
    [[nodiscard]] virtual StateKey to() const noexcept = 0;
    [[nodiscard]] virtual LayoutPolicy outputLayout() const noexcept = 0;
    [[nodiscard]] virtual Fidelity fidelity() const noexcept = 0;
    // Weighted bytes moved (§5.3): host copy 1x, PCIe 2x, GPU-local 0.5x, CPU unpack 4x.
    [[nodiscard]] virtual double cost(const FrameDesc& desc) const noexcept = 0;
    // Makes the output frame. Hops to its own executor itself (cpu, or a GPU submission that
    // resumes on cpu); a GPU converter co_awaits its submission (D10).
    [[nodiscard]] virtual Task<Result<std::shared_ptr<IFrame>>> run(
        ConvertInput input, ConvertContext context) const = 0;
};

// The store's converters (§5.3, M2 plan Task 8). Readback and CpuUnpack have two edges each, one
// converter per edge under the same name (one stats entry).
// Host-only store: CpuUnpack (its edge from staging memory is dropped by a store without that
// tier).
[[nodiscard]] std::vector<std::unique_ptr<Converter>> makeHostConverters();
// Store with a context: HostToStaging, Upload, GpuUnpack, Readback, CpuUnpack, ToDisplay. Creates
// the unpack and ToDisplay pipelines and the ToDisplay dummies (their Unsupported passes
// through). Submissions resume on `cpu`.
[[nodiscard]] Result<std::vector<std::unique_ptr<Converter>>> makeGpuConverters(
    std::shared_ptr<gpu::ExecutionContext> context, std::shared_ptr<IExecutor> cpu);

}  // namespace dvv::media::detail
