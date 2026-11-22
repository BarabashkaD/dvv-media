// Compile-only check: frame.h names no dvv-gpu type. The enums below take the names of the dvv-gpu
// classes, so a frame.h that declares any of them as a class (gpu_fwd.h does) fails to compile.
// This file is its own target (tests/CMakeLists.txt): its enums would clash with the classes the
// test executables see.
namespace dvv::gpu {
enum class GpuBuffer {};
enum class GpuImage {};
enum class MappedRead {};
}  // namespace dvv::gpu

#include "dvv/media/frame.h"

static_assert(sizeof(dvv::media::Binding) != 0);
