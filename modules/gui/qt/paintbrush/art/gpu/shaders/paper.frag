// paper.frag — NAI Paintbrush GPU paper (exact port of PaperRenderer::sample)
// Deterministic uint32 hashing + float interpolation identical to the CPU
// golden path (paper_renderer.cpp + art_noise.hpp).
#version 440

layout(location = 0) in vec2 vCanvasPos;

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uWorldOrigin;
    vec2 uViewSize;
    float uWorldPerPx;
    float uOpaque;
    vec4 uBase;
    vec4 uGrain;
    vec4 uSurface;
    uint uSeed;
};

layout(location = 0) out vec4 fragColor;

// --- exact ports of art_noise.hpp -----------------------------------------

uint artHash2(uint a, uint b)
{
    uint h = 2166136261u;
    h ^= a; h *= 16777619u;
    h ^= b; h *= 16777619u;
    h ^= h >> 16u; h *= 0x85ebca6bu;
    h ^= h >> 13u; h *= 0xc2b2ae35u;
    h ^= h >> 16u;
    return h;
}

float artHashUnitf(uint seed, uint i)
{
    return float(artHash2(seed, i)) / 4294967296.0;
}

float artCellUnitf(uint seed, int x, int y)
{
    return artHashUnitf(seed, artHash2(uint(x), uint(y)));
}

float artValueNoise(uint seed, float u, float v, float frequency)
{
    float g = frequency;
    int x0 = int(floor(u * g));
    int y0 = int(floor(v * g));
    float fx = u * g - float(x0);
    float fy = v * g - float(y0);
    float sx = fx * fx * (3.0 - 2.0 * fx);
    float sy = fy * fy * (3.0 - 2.0 * fy);
    float n00 = artCellUnitf(seed, x0, y0);
    float n10 = artCellUnitf(seed, x0 + 1, y0);
    float n01 = artCellUnitf(seed, x0, y0 + 1);
    float n11 = artCellUnitf(seed, x0 + 1, y0 + 1);
    float nx0 = n00 + (n10 - n00) * sx;
    float nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sy;
}

float artFbm(uint seed, float u, float v, int octaves, float frequency,
             float lacunarity, float gain)
{
    float amp = 1.0;
    float freq = frequency;
    float sum = 0.0;
    float norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        uint s = seed + uint(o) * 2654435761u;
        sum += amp * artValueNoise(s, u, v, freq);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

float clamp01(float x) { return clamp(x, 0.0, 1.0); }

void main()
{
    // vCanvasPos is clip space (-1..1). The CPU golden path samples at
    // integer pixel coordinates (worldView + x*worldPerPx), so map clip to
    // the same integer grid: pixel x is at canvasPx = x, and D3D clip y=+1
    // is the TOP row (row 0) — flip y to match the CPU's top-down scan.
    vec2 canvasPx = (vCanvasPos * 0.5 + 0.5) * uViewSize - 0.5;
    float wx = uWorldOrigin.x + canvasPx.x * uWorldPerPx;
    float wy = uWorldOrigin.y + (uViewSize.y - 1.0 - canvasPx.y) * uWorldPerPx;

    float baseR = uBase.r;
    float baseG = uBase.g;
    float baseB = uBase.b;

    float cell = uGrain.w > 1e-4 ? uGrain.w : 0.04;
    float freq = 1.0 / cell;

    float g = artFbm(uSeed, wx, wy, 2, freq, 2.0, 0.5);
    float grainVar = (g - 0.5) * 2.0 * uGrain.x * (0.4 + uGrain.y);

    float fibre = 0.0;
    if (uGrain.z > 0.0)
    {
        float f = artFbm(uSeed ^ 0xFC01u, wx, wy * 1.3, 3, freq * 1.4, 2.2, 0.6);
        float strand = pow(clamp01(f), 3.0);
        fibre = (strand - 0.5) * 2.0 * uGrain.z * 0.10;
    }

    float lum = 1.0 + grainVar + fibre;

    if (uSurface.x > 0.0 && uSurface.y < 0.6)
    {
        float glintNoise = artCellUnitf(uSeed ^ 0xBEAFu,
                                        int(wx * 0.5), int(wy * 0.5));
        if (glintNoise > 0.85)
            lum += uSurface.x * (glintNoise - 0.85) * 8.0;
    }

    lum = clamp(lum, 0.55, 1.45);

    vec3 col = clamp(vec3(baseR, baseG, baseB) * lum, 0.0, 1.0);
    fragColor = vec4(col, uOpaque);
}