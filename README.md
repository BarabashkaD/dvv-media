# dvv-media

Media layer of the dvv stack used by rawreel: pixel formats, frames, the frame store and sources.
C++23; depends on [dvv-base](https://github.com/BarabashkaD/dvv-base),
[dvv-registry](https://github.com/BarabashkaD/dvv-registry),
[dvv-platform](https://github.com/BarabashkaD/dvv-platform) and
[dvv-gpu](https://github.com/BarabashkaD/dvv-gpu).

## Build

Builds and tests on Windows (Visual Studio 2026) and Linux (Clang 19 with libstdc++ 14) with
[dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the presets, the shared CMake
modules, the third-party dependencies (including `slangc` for the shaders) and the CI workflow; its
README lists the prerequisites. dvv-build is cloned next to dvv-media at the version of the
`dvv-build` line in `dvv-deps.txt`; `fetch-deps.cmake` clones dvv-base, dvv-registry, dvv-platform
and dvv-gpu next to them at their minimum versions from the same file:

```powershell
git clone https://github.com/BarabashkaD/dvv-media
git clone --branch v0.1.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-media -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-media
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-media as a git submodule in their `libs/` folder, next to dvv-build.

`dvv::media` holds the formats, frames, the store and the factory; the built-in providers are in the
component library `dvv::media_sources`:

```cmake
find_package(dvv_media 0.1 REQUIRED)   # or dvv_media in-tree
target_link_libraries(my_app PRIVATE dvv::media)
dvv_link_components(my_app dvv::media_sources)
```

## Pixel formats

A format is a `PixelFormatDescriptor` (`dvv/media/pixel_format.h`) named by a `FormatId`. The 37
built-in formats have the ids 0 to 36 in the order below. Names are FFmpeg `pix_fmt` names (`v210`
is the codec name); `findFormat(name)` matches them exactly and case-sensitively.

| Ids | Class | Names |
| --- | --- | --- |
| 0–4 | gray | `gray`, `gray10le`, `gray12le`, `gray16le`, `gray16be` |
| 5–16 | planar YUV | `yuv420p`, `yuv422p`, `yuv444p`, `yuv420p10le`, `yuv422p10le`, `yuv444p10le`, `yuv420p12le`, `yuv422p12le`, `yuv444p12le`, `yuv420p16le`, `yuv444p16le`, `yuv420p10be` |
| 17–20 | semi-planar YUV 4:2:0 | `nv12`, `nv21`, `p010le`, `p016le` |
| 21–24 | packed YUV 4:2:2 | `yuyv422`, `uyvy422`, `y210le`, `v210` |
| 25–33 | packed RGB and RGBA | `rgb24`, `bgr24`, `rgba`, `bgra`, `argb`, `rgb48le`, `rgb48be`, `rgba64le`, `x2rgb10le` |
| 34–36 | planar RGB | `gbrp`, `gbrp10le`, `gbrp16le` |

Formats without a byte order in the name, or with `le`, are little-endian. `p010le` and `y210le`
keep each sample in the high 10 bits of a 16-bit word; the other 16-bit-word formats use the low
bits. `x2rgb10le` packs R, G and B into one 32-bit word (bits 20, 10 and 0). `v210` packs 6 pixels
into four 32-bit words, needs a width that is a multiple of 2 (`Errc::Format` "v210 needs a width
that is a multiple of 2") and aligns rows to 128 bytes.

`geometry(format, width, height)` gives the tight layout: planes back to back, each row
`ceil(width / groupPixels) * groupBytes` bytes padded to the plane's `rowAlign`, and
`ceil(height / 2^log2SubY)` rows; odd sizes work.

`unpackedFormat(format)`, named `<name>@planes`, is the lossless planar form: one plane per
component (`Y, U, V[, A]` or `R, G, B[, A]`), the component's own subsampling, 1 byte per sample up
to 8 bits and 2 bytes little-endian above, values in the low bits, no row padding.

`formatAlias` maps FFmpeg's `yuvj420p`/`yuvj422p`/`yuvj444p` to `yuv420p`/`yuv422p`/`yuv444p` with
full range; `findFormat` does not know these names.

## Custom pixel formats (JSON)

A format is a list of components and one to four planes. Each plane row is a sequence of groups:
a group covers `groupPixels` pixels in `groupBytes` bytes, read as words of `wordBytes` bytes in one
byte order, and each sample of a group is a slot `{component, index, word, shift}` whose value is
`(word >> shift) & (2^bits - 1)`; `index` counts the component's own samples in the group.

`dvv::media::formatToJson` writes any loaded format with every key; `formatFromJson` reads one
strictly, checks it and loads it. Example (the built-in `p010le`; load a copy under another name):

```json
{
  "name": "p010le",
  "model": "yuv",
  "widthMultiple": 1,
  "components": [
    {"kind": "Y", "bits": 10, "log2SubX": 0, "log2SubY": 0},
    {"kind": "U", "bits": 10, "log2SubX": 1, "log2SubY": 1},
    {"kind": "V", "bits": 10, "log2SubX": 1, "log2SubY": 1}
  ],
  "planes": [
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 0, "index": 0, "word": 0, "shift": 6}]},
    {"groupPixels": 2, "groupBytes": 4, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 1, "slots": [{"component": 1, "index": 0, "word": 0, "shift": 6},
                              {"component": 2, "index": 0, "word": 1, "shift": 6}]}
  ]
}
```

| Key | Type | Range | Default |
| --- | --- | --- | --- |
| `name` | string | 1–64 bytes of `[A-Za-z0-9_.-]` | required |
| `model` | string | `"gray"`, `"yuv"`, `"rgb"` | required |
| `widthMultiple` | integer | 1–255; the width must be a multiple of it | 1 |
| `components` | array | 1–4 entries | required |
| `components[].kind` | string | `"Y"`, `"U"`, `"V"`, `"R"`, `"G"`, `"B"`, `"A"` | required |
| `components[].bits` | integer | 1–16 | required |
| `components[].log2SubX`, `log2SubY` | integer | 0–2 for U and V, else 0 | 0 |
| `planes` | array | 1–4 entries | required |
| `planes[].groupPixels` | integer | 1–255, a multiple of `widthMultiple` | required |
| `planes[].groupBytes` | integer | 1–255, a multiple of `wordBytes` | required |
| `planes[].wordBytes` | integer | 1, 2 or 4 | required |
| `planes[].byteOrder` | string | `"little"`, `"big"` | `"little"` |
| `planes[].rowAlign` | integer | a power of two, 1–4096 (row stride alignment in bytes) | 1 |
| `planes[].log2SubY` | integer | the `log2SubY` of the plane's components | 0 |
| `planes[].slots` | array | 1–12 entries | required |
| `slots[].component` | integer | index into `components` | required |
| `slots[].index` | integer | 0 to `(groupPixels >> log2SubX) - 1` | required |
| `slots[].word` | integer | 0 to `groupBytes / wordBytes - 1` | required |
| `slots[].shift` | integer | `shift + bits` at most `8 * wordBytes` | required |

Rules:
- Unknown keys are errors.
- Components come in the model's order: gray `Y[, A]`, yuv `Y, U, V[, A]`, rgb `R, G, B[, A]`; U
  and V share their subsampling.
- Each component lives in exactly one plane, and its slots in that plane are indices `0..n-1`
  once each, with `n = groupPixels >> log2SubX`. Slots never share bits.
- At most 6 samples of one component per group.
- Built-in names and names ending in `@planes` are rejected.
- Loading an equal format under a loaded name returns the same id; a different one is an error.

Every error is `Errc::Format`. Raw file URIs load a format file with `fmt=@<path>`, e.g.
`file:clip.yuv?fmt=@my_format.json&size=1920x1080`. A relative path is relative to the working
directory; the file may be at most 1 MiB. Only the raw provider loads it.

## Frames and the frame store

A frame is an `IFrame` (`dvv/media/frame.h`) with a `FrameDesc` (format, colour, width, height).
It offers views, described by a `ViewSpec`:

| Field | Meaning |
| --- | --- |
| `format` | the original format, or its unpacked form `unpackedFormat(format)` (the planes) |
| `colour` | a `ColourId`; must equal the frame's own |
| `layout` | `Any`, `Tight`, `RowAlign256` or `VkOptimal` |
| `api` | `Api::HostPlanes`, `Api::VkBuffer` or `Api::VkImage` |
| `domain` | a `Domain`: `MemKind` (`Host`, `HostVisible`, `DeviceLocal`), device and generation |
| `downscaleLog2` | must be 0 |

The bindings are `HostPlanesB` (read-only spans and strides per plane), `VkBufferB` (the whole
original in one buffer) and `VkImageB` (one `R8_UINT`/`R16_UINT` image per component, layout
`GENERAL`). The store's domains are `hostDomain()`, and with a GPU `stagingDomain()` and
`deviceDomain()`. A view's `Fidelity` is `Exact` (the original bytes), `Lossless` (the unpacked
planes) or `Derived` (the display view).

| Format | Api | Memory | Made by | Fidelity |
| --- | --- | --- | --- | --- |
| original | `HostPlanes` | `Host` | `Produce` (the source) | Exact |
| planes | `HostPlanes` | `Host` | `CpuUnpack` | Lossless |
| original | `VkBuffer` | `HostVisible` | `HostToStaging` | Exact |
| original | `VkBuffer` | `DeviceLocal` | `Upload` | Exact |
| planes | `VkImage` | `DeviceLocal` | `GpuUnpack` | Lossless |
| original | `HostPlanes` | `HostVisible` | `Readback` | Exact |
| planes | `HostPlanes` | `HostVisible` | `Readback` | Lossless |
| display | `VkImage` | `DeviceLocal` | `ToDisplay` | Derived |

A host-only store (no GPU context) has only the first two rows. The store takes the cheapest path
with at most `maxPlanDepth` converters; any other view is `Errc::Unsupported`.

```cpp
auto store = FrameStore::create({.cpu = pool, .io = pool, .gpu = context}).value();
const FrameDesc desc = source->info().desc;
const OriginId origin = store->attach(std::move(source)).value();
Result<ReadLease> lease = co_await store->acquire(
    {origin, 0}, planesView(desc, Api::HostPlanes, store->hostDomain()), executor);
```

- `acquire(key, want, resume_on)` returns a `ReadLease`: the view, its `binding()`, `fidelity()`
  and `desc()`. An existing view is reused and one being made is joined; otherwise the store runs
  the converters.
- `pin(key, view, resume_on)` returns a `PinLease`, the same handle counted as a pin.
- `acquireOriginal(key, api, domain, resume_on, require_exact = false)` returns an `OriginalRead`:
  the unpacked planes by default, the original format with `require_exact`.
- `adopt(key, frame)` puts a ready frame into the store as the key's original.
- The awaiting coroutine always resumes on `resume_on`, never inline; it must outlive the
  operation.

A lease keeps its view alive and blocks its eviction; `outstandingLeases()` lists the live leases
with the `source_location` of each request.

`FrameStoreOptions`:

| Field | Default | Meaning |
| --- | --- | --- |
| `cpu` | required | converters |
| `io` | required | handed to the sources |
| `gpu` | null | an `ExecutionContext`; null gives a host-only store |
| `hostBudget` | 2 GiB | host tier |
| `stagingBudget` | 1 GiB | staging tier (`HostVisible`) |
| `deviceBudget` | 0 = half of the device-local memory | device tier (`DeviceLocal`) |
| `maxPlanDepth` | 6 | converters per plan |

Each domain is one LRU tier kept to its budget; leased and pinned views are never evicted.
`trim(domain, target_bytes)` evicts one tier down to `target_bytes`. `stats()` returns `StoreStats`:
one `TierStats` per tier, the counters `frames`, `leases`, `pins`, `hits`, `misses`, `creations`,
`evictions`, `failures`, and one `ConverterStats` `{name, runs, bytes, nanoseconds}` per converter
name.

## Sources

`SourceFactory::create(registry, io)` (`dvv/media/source_factory.h`) creates one instance of every
`Kind::Source` component in the registry. `open(request)` returns a source or `Ambiguous`. A
`SourceRequest` holds the `Uri` and an optional `FormatHint`. A bare path means `file:`. Unknown
query parameters are `Errc::Format` "unknown parameter 'x'"; `impl=<id>` picks a provider.

Shared parameters: `size=WxH`, or `w=` and `h=` together (each 1–65535); `fps=N`, `N/D` or `I.F`
(`29.97` is `2997/100`); `fmt` takes a catalogue name.

Colour parameters (every provider; absent means unspecified):

| Parameter | Values |
| --- | --- |
| `colour` | a colour standard (see "Display" below): `bt601-ntsc`, `bt601-pal`, `bt709`, `bt2020` (BT.1886, the standard's matrix and primaries), `bt2100-pq`, `bt2100-hlg` (BT.2020 matrix and primaries, PQ or HLG). It sets the matrix (YUV formats only), the curve and the primaries, never the range |
| `range` | `full`, `limited`; allowed with every standard and every format |

The range comes from `range=`, else the file (clip description, Y4M header, container tags), else
limited for YUV and full for RGB and gray. The resolved URI keeps the query's form (`colour`, then
`range`). A bad value is `Errc::Format` naming the parameter and the allowed values, e.g. "bad
colour 'bt601' (bt601-ntsc, bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)". The keys `matrix`,
`kr`, `kb`, `transfer` and `primaries` are gone: they give "unknown parameter 'matrix' (set the
colour with colour=<standard>: bt601-ntsc, ...)". Providers in other repositories parse them with
`dvv/media/uri_colour.h` (`colourSpecFromQuery`, `merged` with the format's colour model,
`colourParameters`, `COLOUR_PARAMETERS`, `unknownParameterText`); the standards are in
`dvv/media/colour_preset.h`.

### synthetic

```text
synthetic:<gradient|noise>?fmt=<name>&size=WxH[&fps=][&frames=][&seed=][&<colour>]
synthetic:noise?fmt=p010le&w=64&h=32&fps=29.97&frames=100&seed=7&colour=bt709&range=limited
```

Defaults: `fps` 25, `frames` 10, `seed` 1 (noise only). Frames are deterministic, for any
catalogue format and size.

### raw (`file:`)

```text
<path>[?fmt=<name>|@<file.json>][&size=WxH][&fps=][&clip=@<clip.json>][&<colour>]
clip_1920x1080_v210_50.yuv
clip_3840x2160_yuv420p10le_50_bt2100-pq.yuv
file:clip.yuv?fmt=nv12&size=1920x1080&fps=30000/1001
file:clip.yuv?clip=@clip_desc.json
```

Headerless frames back to back. Each value comes from the query, else the `FormatHint`, else the
clip description of `clip=@<file>`, else the sidecar, else the file name
`<name>_WxH[_fmt][_fps][_colour].<ext>` (fps such as `50`, `29.97`, `25fps`; colour one of the
`colour=` tokens, e.g. `clip_1920x1080_bt709.yuv`). Defaults: fps 25; with a size but no format,
`yuv420p`. Without a size the file cannot be opened. Trailing bytes give the warning "N trailing
bytes ignored". The colour standard comes from `colour=`, then the clip description (`clip=` or the
sidecar), then the name's colour token, then BT.709; the range from `range=`, then the clip
description, then the default. The token gives no range and does not change the match confidence.
`dvv/media/file_name.h` (`parseFileName`) reads the name for other code (e.g. a format dialog).

- `clip=@<file>` names a clip description (below); the path is used as given, like `fmt=@`. A bad
  or missing file is an open error.
- **The sidecar** `<file>.<ext>.json` (e.g. `clip.yuv.json` next to `clip.yuv`) is read by the
  factory for a `file:` URI without `clip=`; only the raw provider uses it. The open logs the note
  "clip.yuv.json describes clip.yuv" (`SourceInfo::notes`). A bad sidecar gives the warning
  "clip.yuv.json ignored: <reason>" and the file opens by its name. `SourceInfo::clipFile` names
  the description used.

### Clip descriptions (JSON)

`dvv/media/clip_json.h` (`parseClipJson`, `loadClipJson`) reads them:

```json
{ "rawreelClip": 1, "format": "yuv420p10le", "size": { "width": 3840, "height": 2160 },
  "frameRate": "60000/1001", "colour": { "standard": "bt2100-pq", "range": "limited" } }
```

`rawreelClip` (`1`) is required; the groups `format` (a catalogue name or `"@<path>"`, relative to
the clip file's folder), `size` (`width` and `height`), `frameRate` (an integer, `"N"` or `"N/D"`;
no decimals) and `colour` are optional, at least one present. `colour` is
`{"standard": "<token>", "range": "full"|"limited"}`: `standard` (a `colour=` token) is required,
`range` optional; they act as `colour=` and `range=` below the query. Unknown keys are errors
(`Errc::Format` "clip description '<file>': <json path>: <reason>", e.g. "colour: missing key
'standard'", "colour: unknown key 'custom' (standard, range)").

### y4m (`file:`)

```text
<path>[?fmt=<name>][&size=WxH][&fps=][&<colour>]
clip.y4m?colour=bt2100-pq&range=full
```

YUV4MPEG2 with the header in the first 4096 bytes. `fmt`, `size` and `fps` only check the header
(a mismatch is `Errc::Format` "Y4M header says ..."). A missing `F` is 25:1, a missing `C` is
`420jpeg`. `XCOLORRANGE=FULL|LIMITED` sets the range;
`range=` overrides it, `colour=` does not change it.
The `C` tags:

| `C` | Format | `C` | Format |
| --- | --- | --- | --- |
| `420jpeg`, `420paldv`, `420mpeg2`, `420` | `yuv420p` | `420p12` | `yuv420p12le` |
| `422` | `yuv422p` | `422p12` | `yuv422p12le` |
| `444` | `yuv444p` | `444p12` | `yuv444p12le` |
| `mono` | `gray` | `420p16` | `yuv420p16le` |
| `420p10` | `yuv420p10le` | `444p16` | `yuv444p16le` |
| `422p10` | `yuv422p10le` | `mono16` | `gray16le` |
| `444p10` | `yuv444p10le` | | |

### Choosing the provider

| Provider | Confidence | When |
| --- | --- | --- |
| `synthetic` | 100 | a `synthetic:` URI |
| `y4m` | 100 | the file starts with `YUV4MPEG2 ` |
| `raw` | 95 | size and format both given (query or `FormatHint`; `fmt=@...` counts) |
| `raw` | 95 | `clip=@<file>` |
| `raw` | 75 | size and format known, the sidecar gives at least one |
| `raw` | 70 | size and format known, at least one from the file name |
| `raw` | 30 | size only; format guessed as `yuv420p` |

With `impl=<id>` that provider opens if its confidence is above 0. Otherwise the best provider
opens when it has at least 60 and every other is at least 10 lower; else the result is `Ambiguous`
with the candidates, best first. So `clip_1920x1080.yuv` (30) needs `fmt=` or `impl=raw`. No
provider above 0 is `Errc::Unsupported` with every provider's reason.

### The resolved URI

`SourceInfo::resolvedUri` reopens the same source without guessing: it always carries `fmt`,
`size` and `fps`, plus the colour parameters given, `frames` and `seed` for synthetic, `fmt=@<path>`
and `clip=@<path>` for raw and `impl=y4m` for y4m. The sidecar is not written; it is read again on
every open. `SourceInfo` also names the `provider`, the `warnings` and, for
files, the `reader` (`FileReader::modeText()`). File sources read as `SourceRequest::fileReads`
says (`FileReadMode::Auto` by default), one read at a time.

```text
clip_64x32_nv12_50.yuv
  -> file:clip_64x32_nv12_50.yuv?fmt=nv12&size=64x32&fps=50
clip.y4m                     (a 64x32 4:2:0 file with F50:1)
  -> file:clip.y4m?fmt=yuv420p&size=64x32&fps=50&impl=y4m
```

## Display

`dvv/media/display.h` defines the display view, made on the GPU: `displayFormat()` is
`rgba16f@display` (linear light), `displayColour()` is `{rgb, full, linear, bt709}`, and
`displayView(store->deviceDomain().value())` is the `ViewSpec` to acquire (one
`R16G16B16A16_SFLOAT` image, layout `GENERAL`). A host-only store has no path to it.

`ToDisplay` (`shaders/to_display.slang`, CPU reference `toDisplayReference()`) maps code values to
R'G'B' (`displayAffine()`: range, matrix, component order), clamps, applies the transfer's EOTF
and clamps again. Chroma is taken from the nearest sample; alpha is written as 1. The primaries
are information only: they are not converted, so BT.2020 shows like BT.709 (as on a monitor that
ignores them).

Colour is one standard (`colour_preset.h`):

| Standard | Token | Matrix | Range (default) | Curve | Primaries |
| --- | --- | --- | --- | --- | --- |
| BT.601 (NTSC) | `bt601-ntsc` | BT.601 | limited | BT.1886 | BT.601-525 |
| BT.601 (PAL) | `bt601-pal` | BT.601 | limited | BT.1886 | BT.601-625 |
| BT.709 | `bt709` | BT.709 | limited | BT.1886 | BT.709 |
| BT.2020 | `bt2020` | BT.2020 | limited | BT.1886 | BT.2020 |
| BT.2100 PQ | `bt2100-pq` | BT.2020 | limited | PQ | BT.2020 |
| BT.2100 HLG | `bt2100-hlg` | BT.2020 | limited | HLG | BT.2020 |

`standardOf(given, model, hint)` names the standard that a stream's given fields point to: the
curve PQ or HLG gives BT.2100; else the matrix (YUV only; not the GBR matrix) names BT.2020, BT.709
or BT.601; else the primaries do. BT.601 is NTSC or PAL by the primaries, else by the container's
matrix code (`FileHint`), else PAL. Nothing given is BT.709 ("colour not signalled"). Fields that
fit no standard (for example the sRGB or linear curve, or a BT.601 matrix with BT.2020 primaries)
give "no standard": they are used as they are, nothing is snapped to a standard.

`resolveColour(desc, hint)` fills the fields a frame leaves unspecified (the `assumed` bits record
which) and records the standard:

| Field | YUV | RGB and gray |
| --- | --- | --- |
| matrix | the standard's, else `bt709` | `rgb` (not used) |
| range | the frame's, else `limited` | the frame's, else `full` |
| transfer | the standard's, else `bt1886` | the standard's, else `bt1886` |
| primaries | the standard's, else `bt709` | the standard's, else `bt709` |

So an untagged stream is BT.709 at any size, with the BT.1886 curve (power 2.4). The range never
comes from a standard: a range the file gives always wins.

Matrices: `bt601` (Kr 0.299, Kb 0.114), `bt709` (0.2126, 0.0722), `bt2020` (0.2627, 0.0593),
`rgb` (the Y, U and V planes hold G, B and R) and `custom` (`kr`, `kb`). Transfers: `srgb`,
`bt1886` (power 2.4), `linear`, and `pq` and `hlg` tone mapped to SDR through the BT.2390 EETF
from a fixed 1000-nit peak to 100 nits.

## Adding a built-in format

1. Append one line to `makeBuiltInFormats()` in `src/catalogue.cpp` (appending keeps the ids) and
   raise `CATALOGUE_SIZE` in `src/format_table.h`.
2. Add one layout golden to `goldens()` in `tests/test_layout_goldens.cpp`, derived by hand from
   FFmpeg's component descriptors, never from `pack()`. Raise the counts in "every format has a
   layout golden".
3. In `tests/test_pixel_format.cpp`, add the name to `NAMES`, a row to `geometryTable()` and raise
   `CATALOGUE_SIZE`; raise `CATALOGUE_SIZE` in `tests/test_unpacked_race.cpp` too.

The tests that loop over `catalogue()` (including the GPU unpack in `tests/gpu_test_unpack.cpp`)
then cover the format. A Y4M `C` tag needs a line in `src/sources/y4m.cpp`.

## Paths

URI and command-line paths are UTF-8 (`dvv::media::pathFromUtf8`, `utf8FromPath`); they never pass
through the ANSI code page. Invalid UTF-8 becomes U+FFFD.

Licence: MIT.
