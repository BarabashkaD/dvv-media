# Changelog

All notable changes to dvv-media. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same 0.minor and at least the minimum version; a replaced or
unused API is removed in the next 0.x minor, with no deprecation period. This file is the release
notes (there are no GitHub Releases).

## [0.8.0] - 2026-11-26

### Added
- `dvv/media/sample_grid.h` and `shaders/sample_grid.slang` (`import dvv_media.sample_grid;`): the
  sample grid of a component, one rule for the CPU and shaders. `sampleOf` gives the sample of a
  subsampled component an image pixel belongs to (nearest, a shift per axis) and `blockOf` the
  image pixels a sample covers (empty outside the component's grid). ToDisplay reads its planes
  with it.
- Stored words and padding masks: `StoredWord`, `storedWord(format, width, height, component,
  sample)` (the word, the sample's shift and bits, and the padding mask of a sample in the original
  bytes), `readStoredWord`, `codeOf`, and `StoredWordLayout` with `storedWordLayout()` (the same
  rule as shader data, 72 bytes). The GPU unpack reads the stored words through them.

### Changed
- The dvv-build minimum is 0.6.1; dvv-media requires dvv-gpu 0.7.0.

## [0.7.0] - 2026-11-26

### Added
- `HiddenComponents` and `ChromaFill` (`dvv/media/types.h`), and `ViewSpec::hidden`, the last
  member of `ViewSpec`: the colour components the display view replaces before its colour matrix.
  A hidden Y, R, G or B becomes code 0; a hidden U or V becomes the mid code 2^(bits - 1)
  (`ChromaFill::Mid`) or 0 (`ChromaFill::Zero`). Alpha is never hidden. Only the display view may
  hide: any other view with a non-default `hidden` is `Errc::Unsupported`. The store keeps one
  display frame per canonical hidden set and makes a new set by running only ToDisplay again.
- `hasChroma(format, resolved)` (`dvv/media/display.h`): whether c1 and c2 are colour-difference
  components (a YUV format whose resolved matrix is not `rgb`).

### Changed
- `toDisplayReference()` takes a `HiddenComponents` as its last parameter (default: nothing
  hidden).
- With the default (nothing hidden) every view and every display pixel is unchanged.

## [0.6.0] - 2026-11-26

### Changed
- The display image's alpha is now the frame's alpha, straight and full range (also in a
  limited-range YUV format), in [0, 1]; it stays 1 for a format without an A component.
  `toDisplayReference()` returns it from the A entry (the format's last component), a missing
  alpha entry counting as 1 (callers that pass only the colour values keep getting alpha 1).

## [0.5.0] - 2026-11-25

### Added
- `dvv/media/view_lines.h` and `shaders/view_lines.slang`, the geometry of ruler numbers and tags:
  `visiblePixels` (`PixelRange`), the first and last image pixel an axis shows, a partly visible
  one counted, the whole picture giving 0 and size - 1 at any zoom; `pixelCentreAt`, the viewport
  position of an image pixel's centre, without FP contraction so that the CPU and a shader agree;
  `numberStepLog2`, log2 of a ruler's number step, the finest step from the major step on whose
  numbers are at least a given spacing apart.

### Changed
- Requires dvv-gpu 0.6.0.

## [0.4.0] - 2026-11-24

### Added
- `dvv/media/view.h`, the view geometry moved in from an application: `ViewRequest`,
  `ViewTransform` (`fit`, `zoomed`, `offset()`, `origin()`), `resolveView`, `imagePointAt`,
  `imagePixelAt`, `zoomAround`, `panBy` and `canPan`.
- `dvv/media/view_lines.h`, the lines on the shown image by the pixel-centre rule: `AxisView`,
  `axisX`, `axisY`, `imageCoordinateAt`, `boundaryAt`, `firstPixelOf`, `imageSpan`, `lineSpan`,
  `haloSpan`, `cursorSpan`, the line levels (`LineLevel`, `levelVisible`, `lineAt`, `tickLevel`,
  `linesInView`), `pictureRegion` (`PixelRect` with `contains`), `cursorPixel`, `visiblePicture`
  and `rulerRects` (`RulerRects`) for rulers that hug the visible picture, and `nearestTick`.
- `shaders/view_lines.slang`, the same rule for shaders, exported with the package: an
  application's shaders take it with `dvv_add_shaders(... IMPORTS dvv::media)` and
  `import dvv_media.view_lines;`.

### Changed
- Requires dvv-gpu 0.5.0 (shader modules a dependency exports).

## [0.3.0] - 2026-11-22

### Added
- `Binding::planes()` and `Binding::kind()`; `vulkan::bufferBinding`, `vulkan::imageBinding`,
  `vulkan::buffer(binding)`, `vulkan::image(binding, plane)` and `vulkan::imageCount(binding)` in
  `dvv/media/frame_vulkan.h`.
- `HOST_PLANES` and `BindingKind::name()`.

### Changed
- Requires dvv-build 0.6.0, dvv-base 0.6.0, dvv-registry 0.3.0, dvv-platform 0.4.0 and
  dvv-gpu 0.4.0.
- `BindingKind` is a class of named constants: `HOST_PLANES`, `vulkan::BUFFER` and
  `vulkan::IMAGE`. Compare it with `==`.
- `ViewSpec::api` and `OriginalRequest::api` are named `binding`; `originalView` and `planesView`
  take a `BindingKind`.
- `Binding` is a class: `planes()` reads host planes, the `vulkan::` functions read and make GPU
  bindings. `dvv/media/frame.h` names no dvv-gpu type and no longer includes `dvv/gpu/gpu_fwd.h`.
- The CPU side of the display transform (`toDisplayReference`, `srgbEncode`, `srgbDecode`,
  `encodeSrgb8`, `floatToHalf`, `halfToFloat`) is documented in the README.
- The package is compatible within the same 0.minor (`SameMinorVersion`).

### Removed
- `Api` with `Api::VkBuffer` and `Api::VkImage`: use `BindingKind`, `HOST_PLANES`,
  `vulkan::BUFFER` and `vulkan::IMAGE`.
- `LayoutPolicy::VkOptimal` (use `LayoutPolicy::Opaque`) and `LayoutPolicy::RowAlign256` (no
  converter made it).
- `bindingIf<B>`, `VkBufferB` and `VkImageB`, and reading a `Binding` with `std::get_if`,
  `std::get`, `std::visit` or `std::holds_alternative`: use `Binding::planes()`, `Lease::planes()`
  and the `vulkan::` functions.
- `StagingFrame`, `VkBufferFrame` and `VkPlanesFrame` from the public headers, with
  `StagingFrame::buffer()`: only the store makes them; `vulkan::buffer(lease)` and
  `vulkan::image(lease, plane)` reach their resources.
- `acquireOriginal(key, api, domain, resume_on, require_exact)`: use
  `acquireOriginal(key, request, resume_on)`. Taking the address of `acquireOriginal` is no longer
  ambiguous.
- The `SourceFactory` constructor without the unavailable list.
- `InterfaceKind<ISourceProvider>`.
- `PixelFormatDescriptor` and `descriptor(FormatId)`: use `PixelFormatDesc` and `formatDesc`.
- `pathFromUtf8` and `utf8FromPath`: use `dvv::pathFromText`, `dvv::pathText` or
  `dvv::pathDisplayText`.
- `colourFromQuery` (use `merged(colourSpecFromQuery(uri))`), `parseColourMatrix`,
  `parseTransferFn`, `parsePrimaries`, `matchColourPreset`, `isUnpacked` and
  `HostFrame::setDataOffset`.

## [0.2.0] - 2026-11-21

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
