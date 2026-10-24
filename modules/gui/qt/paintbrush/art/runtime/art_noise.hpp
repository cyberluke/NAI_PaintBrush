/*****************************************************************************
 * art_noise.hpp : NAI Paintbrush — deterministic random / noise helpers
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_NOISE_HPP
#define PB_ART_NOISE_HPP

#include <QtGlobal>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace paintbrush {
namespace art {

/* Deterministic, seedable hashing for the Art Materials Runtime.
 *
 * Spec §37: art tools may use randomness but it must be *reproducible*. Every
 * material/stroke/effect gets a stable seed saved in the document; undo, reopen
 * and export must reproduce the identical result. Never use uncontrolled global
 * randomness. These helpers turn a (seed, x, y, index) tuple into repeatable
 * values with no global state. */

/* Integer hash: u32 hash of two u32s (a tiny, invertible avalanche). */
inline std::uint32_t artHash2(std::uint32_t a, std::uint32_t b)
{
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint32_t v)
    {
        h ^= v;
        h *= 16777619u;
    };
    mix(a);
    mix(b);
    /* final avalanche */
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* Deterministic uint32 in [0, n) from (seed, x, y). */
inline std::uint32_t artHashedIndex(std::uint32_t seed, int x, int y,
                                    std::uint32_t n)
{
    if (n == 0)
        return 0;
    return artHash2(seed, static_cast<std::uint32_t>(x * 73856093) ^
                            static_cast<std::uint32_t>(y * 19349663)) % n;
}

/* Deterministic unit float in [0,1) from (seed, i). */
inline float artHashUnitf(std::uint32_t seed, std::uint32_t i)
{
    std::uint32_t h = artHash2(seed, i);
    return static_cast<float>(h) / 4294967296.0f; /* / 2^32 */
}

/* Deterministic float in [lo, hi) from (seed, i). */
inline float artHashRangef(std::uint32_t seed, std::uint32_t i,
                           float lo, float hi)
{
    return lo + artHashUnitf(seed, i) * (hi - lo);
}

/* Deterministic unit float in [0,1) from (seed, x, y) cell coordinates. */
inline float artCellUnitf(std::uint32_t seed, int x, int y)
{
    return artHashUnitf(seed, artHash2(static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y)));
}

/* Deterministic signed unit value [-1,1). */
inline float artCellSignedf(std::uint32_t seed, int x, int y)
{
    return 2.0f * artCellUnitf(seed, x, y) - 1.0f;
}

/* Cheap, continuous value noise in [0,1] from a hash of a cell corner grid.
 * `frequency` = world-cell size in device pixels (multiples are tiled, not
 * repeated bitmaps). Interpolated bilinearly for smoothness. */
inline float artValueNoise(std::uint32_t seed, float u, float v,
                           float frequency = 1.0f)
{
    const float g = frequency;
    const int x0 = static_cast<int>(std::floor(u * g));
    const int y0 = static_cast<int>(std::floor(v * g));
    const float fx = u * g - static_cast<float>(x0);
    const float fy = v * g - static_cast<float>(y0);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);

    const float n00 = artCellUnitf(seed, x0,     y0);
    const float n10 = artCellUnitf(seed, x0 + 1, y0);
    const float n01 = artCellUnitf(seed, x0,     y0 + 1);
    const float n11 = artCellUnitf(seed, x0 + 1, y0 + 1);

    const float nx0 = n00 + (n10 - n00) * sx;
    const float nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sy;
}

/* Fractal value noise across `octaves` with `lacunarity` frequency growth and
 * `gain` amplitude falloff. Good for paper tooth, organic splodges, grain. */
inline float artFbm(std::uint32_t seed, float u, float v,
                    int octaves, float frequency = 1.0f,
                    float lacunarity = 2.0f, float gain = 0.5f)
{
    float amp = 1.0f;
    float freq = frequency;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * artValueNoise(seed + std::uint32_t(o * 2654435761u),
                                   u, v, freq);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace art
} // namespace paintbrush

#endif // PB_ART_NOISE_HPP