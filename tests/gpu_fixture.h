#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/gpu/device_id.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/gpu_registry.h"
#include "dvv/gpu/submission.h"

namespace dvv::media::test {

// A new directory under the system's temporary directory, removed with its content afterwards.
class TemporaryDirectory {
public:
    TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// co_await context.submit(executor, record) from a coroutine; blocks until it has resumed.
[[nodiscard]] Result<std::uint64_t> submitAndWait(gpu::ExecutionContext& context,
                                                  IExecutor& executor, gpu::RecordFunction record);

// Base of every GPU test (TEST_CASE_FIXTURE): a registry configured from the environment
// (DVV_VK_VALIDATION, DVV_VK_VALIDATION_REQUIRED) and, on first use, a context on the best device
// and a one-thread executor for the coroutines that await submissions. At the end of the test the
// context is destroyed first (destroying a device reports leaked Vulkan objects), then no
// validation message may have been counted.
class GpuTest {
public:
    GpuTest();
    GpuTest(const GpuTest&) = delete;
    GpuTest& operator=(const GpuTest&) = delete;
    GpuTest(GpuTest&&) = delete;
    GpuTest& operator=(GpuTest&&) = delete;
    ~GpuTest();

    [[nodiscard]] gpu::GpuRegistry& registry() const noexcept { return *registry_; }
    // The best usable device (main() checked that one exists).
    [[nodiscard]] gpu::DeviceId device() const;
    // A context on device(), created on first use.
    [[nodiscard]] gpu::ExecutionContext& context();
    // The executor submissions resume on, created on first use.
    [[nodiscard]] ThreadPool& executor();
    // submitAndWait(context(), executor(), record).
    [[nodiscard]] Result<std::uint64_t> submitAndWait(gpu::RecordFunction record);

private:
    std::shared_ptr<gpu::GpuRegistry> registry_;
    std::shared_ptr<gpu::ExecutionContext> context_;
    std::shared_ptr<ThreadPool> executor_;
};

}  // namespace dvv::media::test
