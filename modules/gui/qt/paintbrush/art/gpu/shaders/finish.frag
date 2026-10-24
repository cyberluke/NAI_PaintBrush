// finish.frag — NAI Paintbrush GPU finish effects
//
// Single-pass ports of the CPU FinishRuntime primitives (effects.cpp):
//   uMode 0 = DirectedBlur   (multi-tap along direction, exponential taper)
//   uMode 1 = Shadow         (colorize alpha, 3x box blur, offset, subject over)
//   uMode 2 = Glow           (colorize alpha, threshold, 3x box blur, subject over)
// The CPU path stays the golden reference; this shader reproduces the same
// math so the GPU picture matches within tolerance. All work is done on
// PREMULTIPLIED source pixels (no dark halo), like the CPU.
//
// Shadow/glow blur semantics match the CPU exactly:
//   blurPremultiplied() = THREE box passes with half-window r, each averaging
//   a clamped 2r+1 window. Three sequential box passes equal one convolution
//   with the B-spline kernel box(r) ⊛ box(r) ⊛ box(r) (piecewise quadratic,
//   support 6r+1). The kernel weights are exact integer counts of triples
//   (a,b,c) in [-r,r]^3 with a+b+c = k, computed in-shader in closed form.
//   The CPU's per-pass integer truncation is dropped (float accumulation
//   then a single rounding), which stays well within the harness tolerance.
// The final pixel composites the SUBJECT back over the effect with SourceOver
// (premultiplied): result = subject + effect * (1 - subject.a).
#version 440

layout(location = 0) in vec2 vTexCoord;

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uTexSize;
    vec2 uOutSize;
    vec2 uDir;
    float uDistancePx;
    float uFalloff;
    int uMode;
    int uSamples;
    vec2 uPad;
    vec4 uColor;
    vec2 uOffsetPx;
    float uThreshold;     /* glow alpha threshold (0..1) */
    float uIntensity;     /* glow multiplier (0..4) */
};

layout(binding = 1) uniform sampler2D uTexture;  // premultiplied ARGB source

layout(location = 0) out vec4 fragColor;

// sample with CPU-style out-of-bounds skip: the CPU ignores taps outside the
// padded canvas; emulate by returning zero (a skipped tap adds no weight).
// (CPU box passes clamp with qBound, but the padded border is transparent,
// so a clamped tap contributes 0 to the fixed-size window sum — identical.)
bool inBounds(vec2 uv)
{
    return uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
}

vec4 tapAt(vec2 uv)
{
    // Explicit LOD 0: implicit gradient sampling (texture()) inside a dynamic
    // loop makes HLSL force-unroll the loop (X3570/X3511). texelFetch-free
    // explicit-lod sampling keeps the loop a real loop on every backend.
    return textureLod(uTexture, uv, 0.0);
}

// Exact integer weight of the triple box-pass kernel at offset k (in -3r..3r):
// number of (a,b,c) in [-r,r]^3 with a+b+c == k. m = 2r, s = k + 3r.
float kernelWeight(int k, int r)
{
    int m = 2 * r;
    int s = k + 3 * r;
    float c;
    if (s <= m)
        c = float((s + 1) * (s + 2)) * 0.5;
    else if (s <= 2 * m)
        c = float((s + 1) * (s + 2)) * 0.5 - float((s - m) * (s - m + 1)) * 1.5;
    else
        c = float((3 * m - s + 1) * (3 * m - s + 2)) * 0.5;
    return c;
}

void main()
{
    // The texture IS the padded canvas (the CPU blurs the padded image with
    // the subject at uPad), so output pixel == padded canvas pixel.
    vec2 outPx = vTexCoord * uOutSize;

    if (uMode == 0)
    {
        // DirectedBlur: taps 0..distance along direction with exp taper
        // (CPU directedBlur). The destination pixel gathers source pixels
        // AHEAD of it along the motion (trailing streak) at lround'd
        // integer offsets. Fixed loop bound for HLSL unrolling.
        float wsum = 0.0;
        vec4 acc = vec4(0.0);
        float k = 1.0 + 5.0 * clamp(uFalloff, 0.0, 1.0);
        for (int i = 0; i < 32; ++i)
        {
            if (i >= uSamples)
                continue;
            float t = float(i) / float(max(1, uSamples - 1));
            float wgt = exp(-k * t);
            wgt = max(0.01, wgt);
            vec2 off = uDir * (t * uDistancePx);
            // CPU: sx = lround(x + off.x) -> nearest integer tap
            vec2 tapPx = outPx + floor(off + 0.5);
            vec2 tap = tapPx / uOutSize;
            if (!inBounds(tap))
                continue;
            acc += tapAt(tap) * wgt;
            wsum += wgt;
        }
        fragColor = wsum > 0.0 ? acc / wsum : vec4(0.0);
        return;
    }

    // Shadow / Glow: 3x box blur of the colorized alpha.
    //   r = CPU r = max(1, lround(radiusPx / 2)); kernel support = 6r+1;
    //   total weight = (2r+1)^3 (the three box windows).
    int r = max(1, int(floor(uDistancePx * 0.5 + 0.5)));
    float invTotal = 1.0 / float((2 * r + 1) * (2 * r + 1) * (2 * r + 1));
    float opacity = uColor.a;

    vec4 eff = vec4(0.0);
    for (int dy = -3 * r; dy <= 3 * r; ++dy)
    {
        float wy = kernelWeight(dy, r) * invTotal;
        if (wy <= 0.0)
            continue;
        for (int dx = -3 * r; dx <= 3 * r; ++dx)
        {
            float wx = kernelWeight(dx, r) * invTotal;
            if (wx <= 0.0)
                continue;
            // Shadow samples the blurred effect at (outPx - offset): the CPU
            // draws the blurred image shifted by uOffsetPx (effect at pixel p
            // comes from the blur at p - offset). Glow is drawn at 0.
            vec2 tap = (outPx - uOffsetPx + vec2(float(dx), float(dy))) / uOutSize;
            if (!inBounds(tap))
                continue;
            vec4 s = tapAt(tap);
            // Colorize: premultiplied color = uColor.rgb * (srcAlpha * opacity)
            // (CPU colorizeAlpha). Glow additionally thresholds faint alpha to
            // zero BEFORE the blur.
            vec4 c = vec4(uColor.rgb * (s.a * opacity), s.a * opacity);
            if (uMode == 2 && c.a < uThreshold)
                c = vec4(0.0);
            eff += c * (wx * wy);
        }
    }

    // Glow intensity (CPU applies it AFTER the blur): scale premultiplied
    // channels, clamp rgb to the new alpha.
    if (uMode == 2)
    {
        float k = clamp(uIntensity, 0.0, 4.0);
        float a = clamp(eff.a * k, 0.0, 1.0);
        eff = vec4(clamp(eff.rgb * k, vec3(0.0), vec3(a)), a);
    }

    // Composite the subject (the padded canvas at outPx) back over the effect
    // with SourceOver, exactly like the CPU's final drawImage(0,0,self):
    // result = subject + effect * (1 - subject.a).
    vec4 sub = tapAt(vTexCoord);
    fragColor = sub + eff * (1.0 - sub.a);
}