# Changelog

All notable changes to dvv-media. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.2.0] - 2026-11-20

### Added
- `Lease::share()`: a second, counted lease on the same view, listed in `outstandingLeases()`
  with its own location.
- `Lease::planes()`: the host planes of a lease.
- `dvv/media/frame_vulkan.h`: `vulkan::image(lease, plane)` and `vulkan::buffer(lease)` reach a
  lease's GPU resources, valid for the lease's life; `vulkan::BUFFER` and `vulkan::IMAGE`, the
  GPU binding kinds.
- `dvv/media/display_vulkan.h` with `DISPLAY_IMAGE_FORMAT`, the GPU format of the display image.
- `BindingKind` (alias of `Api`), `bindingIf<B>(binding)` and `LayoutPolicy::Opaque`.
- `TierStats::pending`.
- `dvv/media/rate_size_text.h`: `parseSize`, `parseDimension`, `parseFps` and `formatFps`, the
  size and rate grammar of the source URIs and file names.
- `ProduceContext::stopToken()`, `OpenContext::stop` and `SourceFactory::open(request, stop)`.
- `SourceInfo::fixed` (`FixedFields`): the format fields the source's header fixes; Y4M sources
  fix all three.
- `SourceFactory::unavailableProviders()` and `UnavailableProvider` (id, the registry's reason
  and the missing capability).
- `DEFAULT_FILE_READS`, the file-read mode of a `SourceRequest`, and `FALLBACK_FPS`, the frame
  rate a source reports when its input states none.
- `OriginalRequest`, `OriginalFidelity` and `acquireOriginal(key, request, resume_on)`.
- `PixelFormatDesc` and `formatDesc(FormatId)`.
- `pack(values, out)`: packs code values straight into caller memory.

### Changed
- Requires dvv-base 0.5.0, dvv-registry 0.2.0, dvv-platform 0.3.0 and dvv-gpu 0.3.0.
- `ReadLease` and `PinLease` are one class template, `Lease<LeaseKind>`; both names stay as
  aliases.
- Because `ReadLease` and `PinLease` are now aliases of `Lease<K>`, a forward declaration
  `class ReadLease;` or `class PinLease;` no longer compiles: include `dvv/media/frame_store.h`
  instead. This is the one rename in 0.2.0 where the old name is not the declared entity.
- Taking the address of `SourceFactory::open` or `FrameStore::acquireOriginal` without a cast is
  now ambiguous: each gained an overload.
- `SourceFactory::create`'s "no source providers registered" error now ends with the list of
  unavailable providers.
- An explicit destructor call such as `p->~ReadLease()` now needs the alias visible where it is
  written (for example `using dvv::media::ReadLease;`); `std::destroy_at(p)` needs nothing.
- A tier counts the memory of frames that are being made, from allocation to publish; eviction
  keeps `bytes + pending` under the budget, so a tier near its budget evicts cached frames
  earlier. `trim()` counts pending bytes too.
- Detaching an origin cancels its queued and running produce and conversion steps; their acquires
  end with `Errc::Cancelled`, and cancelled steps are not counted as failures.
- Every no-match error of `SourceFactory::open` names the unavailable Source providers with the
  registry's reason; `?impl=` naming an unavailable provider says so.
- Path text comes from dvv-base (`dvv/base/utf8.h`). On POSIX `pathFromUtf8` keeps bytes that are
  not UTF-8, so such a file opens from its URI; on Windows `utf8FromPath` gives U+FFFD for an
  unpaired surrogate instead of a placeholder for the whole path.
- frame.h includes `dvv/platform/aligned_buffer.h` instead of the whole file reader; include
  `dvv/platform/file_reader.h` yourself where you use it.
- Diagnostics name the GPU image layout "Opaque" instead of "VkOptimal".
- The clip description's version error ends "(dvv-media reads 1)" and names no application.
- The source providers take no parameters and declare no schema; the factory lists providers by
  interface.
- The synthetic source packs straight into its host frame (one full-frame copy less).
- Uses dvv-gpu's interop functions, map scopes and named recorder forms; no API change for
  applications. frame.h includes `dvv/gpu/gpu_fwd.h` instead of declaring `GpuBuffer` and
  `GpuImage` itself.

### Deprecated (removed in 1.0.0)
- `VkBufferB::buffer`, `VkImageB::planes` and `StagingFrame::buffer()`: use
  `vulkan::buffer(lease)` and `vulkan::image(lease, plane)`. The GPU frame classes and the
  `VkBufferB` and `VkImageB` bindings move from frame.h to frame_vulkan.h, and frame.h names no
  dvv-gpu type.
- `Api::VkBuffer` and `Api::VkImage`: use `vulkan::BUFFER` and `vulkan::IMAGE`.
  `LayoutPolicy::VkOptimal`: use `LayoutPolicy::Opaque`.
- Reading a `Binding` with `std::get_if`, `std::get`, `std::visit` or `std::holds_alternative`:
  use `bindingIf<B>(binding)` or `Lease::planes()`. `Binding` becomes a class.
- `acquireOriginal(key, api, domain, resume_on, require_exact)`: use the request form.
- `PixelFormatDescriptor` and `descriptor(FormatId)`: use `PixelFormatDesc` and `formatDesc`.
- `pathFromUtf8` and `utf8FromPath`: use `dvv::pathFromText`, `dvv::pathText` or
  `dvv::pathDisplayText`.
- `InterfaceKind<ISourceProvider>`: the registry lists source providers by interface.
- The `SourceFactory` constructor without the unavailable list (for `create()` only).

## [0.1.1] - 2026-11-07

### Changed
- Built and tested on macOS (Apple Silicon): the GPU unpack is bit-exact for the whole catalogue on
  KosmicKrisp. CI runs macOS jobs.
- Requires dvv-build 0.2.0, dvv-base 0.2.0, dvv-registry 0.1.1, dvv-platform 0.2.0 and dvv-gpu 0.2.0.
- The tests use `dvv::MoveOnlyFunction`.

## [0.1.0] - 2026-10-28

First versioned release.

### Added
- 37 built-in pixel formats (FFmpeg `pix_fmt` names) and custom formats described in JSON, with
  their plane geometry and the clip description JSON.
- Host and GPU frames and the frame store: host, staging and device tiers with budgets, LRU eviction
  and conversion plans.
- The source factory with the built-in providers in the component library `dvv::media_sources`:
  raw files, Y4M and synthetic patterns.
- GPU unpack and display shaders (Slang, embedded): the display transform applies the colour matrix,
  range and transfer, with PQ and HLG tone mapped to SDR.
- The CMake package `dvv_media` (`find_package(dvv_media 0.1)`, `SameMajorVersion`); it requires
  dvv-base, dvv-registry, dvv-platform and dvv-gpu 0.1.0 or later in the 0.x series.
