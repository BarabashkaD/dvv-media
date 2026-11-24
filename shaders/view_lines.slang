// Lines on the shown image, for shaders: `import dvv_media.view_lines;`, then `dvv::media::lineAt`
// or `using namespace dvv::media;`. Must match dvv/media/view_lines.h function by function: the
// same names in the same namespace, the same statements, one float operation per statement and
// `precise` where the C++ is built without FP contraction. The header documents every function;
// only the shader's differences are noted here.
//
// A Vulkan divide need not be correctly rounded, so imageCoordinateAt may be a few ulp off the C++
// value at a zoom that is not a power of two. Where the driver divides by a power of two exactly
// (every tested driver), every function equals the C++ exactly at such a zoom.

namespace dvv::media
{

public static const uint LINE_LEVELS = 3;  // the levels of a lineAt and a tickLevel call
public static const uint NO_LEVEL = 3;     // tickLevel: no drawn level's step divides k

// The largest pixel magnitude: pixels are saturated to [-2^30, 2^30].
static const float PIXEL_LIMIT = 1073741824.0;

// The largest thickness a span uses: with saturated pixels no span end overflows an int.
static const uint THICKNESS_LIMIT = 0x20000000u;

public struct AxisView  // one axis of the shown image
{
    public int offset;   // the viewport pixel of image coordinate 0
    public float zoom;   // viewport pixels per image pixel
    public uint size;    // image pixels on this axis
    public uint extent;  // the viewport's pixels on this axis
};

public struct PixelSpan  // [begin, end) in viewport pixels; an empty span is {0, 0}
{
    public int begin;
    public int end;
    public bool empty()
    {
        return end <= begin;
    }
};

// Coarsest first: index 0 wins in lineAt and tickLevel.
public struct LineLevel
{
    public uint stepLog2;   // step = 1 << stepLog2; 32 or more: the single multiple 0
    public uint thickness;  // viewport pixels
    public bool drawn;
    public bool halo;
};

public struct LineHit
{
    public bool hit;    // false: no level holds the pixel (the C++ nullopt)
    public uint level;
    public uint k;
    public bool halo;   // the pixel is on the level's halo, not its line
};

// 2^e as an exact float for e <= 127, infinity above (the C++ ldexp).
float powerOfTwo(uint e)
{
    return e > 127u ? asfloat(0x7F800000u) : asfloat((127u + e) << 23u);
}

bool showsImage(AxisView axis)
{
    return axis.size > 0u && axis.extent > 0u && isfinite(axis.zoom) && axis.zoom > 0.0;
}

PixelSpan clipSpan(PixelSpan span, PixelSpan image)
{
    const int begin = max(span.begin, image.begin);
    const int end = min(span.end, image.end);
    if (end <= begin)
    {
        return { 0, 0 };
    }
    return { begin, end };
}

public float imageCoordinateAt(AxisView axis, int pixel)
{
    precise const float centre = float(pixel) + 0.5;
    precise const float shifted = centre - float(axis.offset);
    precise const float u = shifted / axis.zoom;
    return u;
}

public float boundaryAt(AxisView axis, uint k)
{
    precise const float product = float(k) * axis.zoom;
    precise const float b = float(axis.offset) + product;
    return b;
}

public int firstPixelOf(AxisView axis, uint k)
{
    const float b = boundaryAt(axis, k);
    precise const float edge = b - 0.5;
    const float estimate = ceil(edge);
    // Written so that a NaN estimate (no image) saturates too.
    const float raised = estimate >= -PIXEL_LIMIT ? estimate : -PIXEL_LIMIT;
    const float saturated = raised > PIXEL_LIMIT ? PIXEL_LIMIT : raised;
    int p = int(saturated);
    const float target = float(k);
    if (imageCoordinateAt(axis, p - 1) >= target)
    {
        p -= 1;
    }
    else if (imageCoordinateAt(axis, p) < target)
    {
        p += 1;
    }
    return p;
}

public PixelSpan imageSpan(AxisView axis)
{
    if (!showsImage(axis))
    {
        return { 0, 0 };
    }
    const int extent = int(min(axis.extent, 0x7FFFFFFFu));
    return clipSpan({ firstPixelOf(axis, 0u), firstPixelOf(axis, axis.size) }, { 0, extent });
}

// The unclipped line span; the caller has checked k <= size and t > 0.
PixelSpan lineWide(AxisView axis, uint k, uint thickness)
{
    const int p = firstPixelOf(axis, k);
    const int t = int(min(thickness, THICKNESS_LIMIT));
    if (k == 0u)
    {
        return { p, p + t };
    }
    if (k == axis.size)
    {
        return { p - t, p };
    }
    const int begin = p - t / 2;
    return { begin, begin + t };
}

// The unclipped line span grown by one pixel on each side, then clipped.
PixelSpan haloOf(PixelSpan line, PixelSpan image)
{
    return clipSpan({ line.begin - 1, line.end + 1 }, image);
}

public PixelSpan lineSpan(AxisView axis, uint k, uint thickness)
{
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || k > axis.size || thickness == 0u)
    {
        return { 0, 0 };
    }
    return clipSpan(lineWide(axis, k, thickness), image);
}

public PixelSpan haloSpan(AxisView axis, uint k, uint thickness)
{
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || k > axis.size || thickness == 0u)
    {
        return { 0, 0 };
    }
    return haloOf(lineWide(axis, k, thickness), image);
}

public PixelSpan cursorSpan(AxisView axis, uint c)
{
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || c >= axis.size)
    {
        return { 0, 0 };
    }
    const int last = firstPixelOf(axis, axis.size) - 1;
    const int begin = min(firstPixelOf(axis, c), last);
    const int end = max(begin + 1, firstPixelOf(axis, c + 1u));
    return clipSpan({ begin, end }, image);
}

public bool levelVisible(uint stepLog2, float zoom, float minimum)
{
    const float step = powerOfTwo(stepLog2);
    precise const float spacing = step * zoom;
    return spacing >= minimum;
}

// k is a multiple of 1 << stepLog2; a stepLog2 of 32 or more has the single multiple 0.
bool divides(uint stepLog2, uint k)
{
    if (stepLog2 >= 32u)
    {
        return k == 0u;
    }
    const uint mask = (1u << stepLog2) - 1u;
    return (k & mask) == 0u;
}

// The image span once per call and each candidate's unclipped line once, for its line and halo.
public LineHit lineAt(AxisView axis, LineLevel levels[LINE_LEVELS], int pixel)
{
    LineHit none = { false, 0u, 0u, false };
    const PixelSpan image = imageSpan(axis);
    if (image.empty())
    {
        return none;
    }
    const float u = imageCoordinateAt(axis, pixel);
    for (uint l = 0u; l < LINE_LEVELS; ++l)
    {
        const LineLevel level = levels[l];
        if (!level.drawn || level.thickness == 0u)
        {
            continue;
        }
        const uint shift = min(level.stepLog2, 31u);
        // The largest m with m << stepLog2 <= size.
        const uint last = level.stepLog2 >= 32u ? 0u : axis.size >> shift;
        // 2^-shift, exact.
        const float scale = asfloat((127u - shift) << 23u);
        precise const float q = u * scale;
        precise const float highBound = float(last) + 2.0;
        const float high = min(highBound, PIXEL_LIMIT);
        const float n = clamp(floor(q), -3.0, high);
        const int first = int(n) - 1;
        bool valid[4];
        uint ks[4];
        PixelSpan wide[4];
        for (int i = 0; i < 4; ++i)
        {
            const int m = first + i;
            valid[i] = m >= 0 && uint(m) <= last;
            ks[i] = 0u;
            wide[i] = { 0, 0 };
            if (valid[i])
            {
                ks[i] = uint(m) << shift;
                wide[i] = lineWide(axis, ks[i], level.thickness);
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            if (!valid[i])
            {
                continue;
            }
            const PixelSpan line = clipSpan(wide[i], image);
            if (line.begin <= pixel && pixel < line.end)
            {
                LineHit hit = { true, l, ks[i], false };
                return hit;
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            if (!level.halo || !valid[i])
            {
                continue;
            }
            const PixelSpan halo = haloOf(wide[i], image);
            if (halo.begin <= pixel && pixel < halo.end)
            {
                LineHit hit = { true, l, ks[i], true };
                return hit;
            }
        }
    }
    return none;
}

public uint tickLevel(uint k, LineLevel levels[LINE_LEVELS])
{
    for (uint l = 0u; l < LINE_LEVELS; ++l)
    {
        if (levels[l].drawn && divides(levels[l].stepLog2, k))
        {
            return l;
        }
    }
    return NO_LEVEL;
}

}  // namespace dvv::media
