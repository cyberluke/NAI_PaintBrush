// glitter.frag — NAI Paintbrush GPU glitter (exact port of the CPU grain
// shading: glitter_material.cpp render() + drawFlake()).
//
// The CPU rasterizes per grain: rounded-square coverage (Chebyshev blended
// with Euclid), facet lighting from the shared rig (diffuse + specular +
// twinkle flash), holographic hue shift for a few grains, paper tooth /
// absorption interaction. This shader reproduces that math per fragment.
#version 440

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vRadius;
layout(location = 3) in float vRot;
layout(location = 4) in float vHeight;
layout(location = 5) in float vBonded;
layout(location = 6) in float vShadowLen;
layout(location = 7) in vec2 vShadowDir;
layout(location = 8) in float vSeed;

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec3 uLightL;
    float uPad0;
    vec3 uLightH;
    float uPad1;
    float uSpecExp;
    float uKeyIntensity;
    float uAmbient;
    float uSoftness;
    vec4 uKeyColor;
    float uToothShade;
    float uAbsorption;
    float uPad2;
    float uPad3;
};

layout(location = 0) out vec4 fragColor;

// --- exact port of art_noise.hpp hashing (uint32 wrap-around) -------------

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

float clamp01(float x) { return clamp(x, 0.0, 1.0); }

// --- rounded-square coverage (drawFlake) -----------------------------------
// CPU: px = (x+0.5)-cxPx; rotate by cos(-rot)/sin(-rot);
// d = cheb*0.7 + eucl*0.3; cov = (rPx - d) / edge, edge = max(0.5, rPx*0.35).
// The quad spans radius+2px and local.x/y are in [-1,1]; the rasterizer
// interpolates so that at pixel center x the local coordinate is
// (x+0.5-cxPx)/(radius+2), hence px = local * (radius+2) WITHOUT an extra
// half-pixel (that would shift the footprint by 0.5px vs the CPU).
float flakeCoverage(vec2 local, float rWorld, float rot)
{
    float px = local.x * (rWorld + 2.0);
    float py = local.y * (rWorld + 2.0);
    float cr = cos(-rot), sr = sin(-rot);
    float fx = px * cr - py * sr;
    float fy = px * sr + py * cr;
    float cheb = max(abs(fx), abs(fy));
    float eucl = sqrt(fx * fx + fy * fy);
    float d = cheb * 0.7 + eucl * 0.3;
    float edge = max(0.5, rWorld * 0.35);
    return clamp01((rWorld - d) / edge);
}

// --- HSV conversion for holographic shift (QColor::fromHsv equivalent) -----
vec3 rgb2hsv(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float h = 0.0;
    if (d > 1e-6)
    {
        if (mx == c.r)
            h = mod((c.g - c.b) / d, 6.0);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0;
        else
            h = (c.r - c.g) / d + 4.0;
        h *= 60.0;
    }
    float s = mx < 1e-6 ? 0.0 : d / mx;
    return vec3(h, s, mx);
}

vec3 hsv2rgb(vec3 hsv)
{
    float h = mod(hsv.x, 360.0);
    float s = hsv.y;
    float v = hsv.z;
    float c = v * s;
    float hp = h / 60.0;
    float x = c * (1.0 - abs(mod(hp, 2.0) - 1.0));
    vec3 rgb = vec3(0.0);
    if (hp < 1.0) rgb = vec3(c, x, 0.0);
    else if (hp < 2.0) rgb = vec3(x, c, 0.0);
    else if (hp < 3.0) rgb = vec3(0.0, c, x);
    else if (hp < 4.0) rgb = vec3(0.0, x, c);
    else if (hp < 5.0) rgb = vec3(x, 0.0, c);
    else rgb = vec3(c, 0.0, x);
    return rgb + (v - c);
}

void main()
{
    uint gi = uint(vSeed);
    float cov = flakeCoverage(vLocal, vRadius, vRot);
    if (cov <= 0.0)
        discard;

    if (vShadowLen > 0.0)
    {
        // contact shadow: black footprint, alpha scaled by height and
        // softness (CPU: 0.28*(0.5+0.5h)*(1-softness*0.5) * cov * 0.5)
        float a = 0.28 * (0.5 + 0.5 * vHeight) * (1.0 - uSoftness * 0.5) * cov * 0.5;
        if (a <= 0.0)
            discard;
        fragColor = vec4(0.0, 0.0, 0.0, a);
        return;
    }

    // facet normal (CPU facetNormal): bonded flakes lie nearly flat, loose
    // flakes tilt more on the pile
    float tiltMax = vBonded >= 0.5 ? 0.55 : 0.9;
    float tilt = tiltMax * sqrt(artHashUnitf(gi, 21u));
    float st = sin(tilt), ct = cos(tilt);
    vec3 n = normalize(vec3(st * cos(vRot), st * sin(vRot), ct));

    float diff = max(0.0, dot(n, uLightL));
    float ndh = max(0.0, dot(n, uLightH));
    float spec = pow(ndh, uSpecExp);

    vec3 base = vColor.rgb;
    // holographic hue shift: ~30% of grains (CPU: artHashUnitf(gi,33) > 0.7)
    float holo = artHashUnitf(gi, 33u);
    if (holo > 0.7)
    {
        vec3 hsv = rgb2hsv(base);
        if (hsv.z > 1e-6)
        {
            float shift = (ndh - 0.5) * 120.0 * (holo - 0.7) * 3.3;
            hsv.x = mod(hsv.x + shift, 360.0);
            base = hsv2rgb(hsv);
        }
    }

    // paper interaction: tooth micro-shadowing + glue sinking
    float tooth = 1.0 - 0.18 * uToothShade * artHashUnitf(gi, 44u);
    float sink = vBonded >= 0.5 ? (1.0 - 0.12 * uAbsorption) : 1.0;

    float kd = (uAmbient * 0.75 + diff * uKeyIntensity * 1.1) * tooth * sink;
    float glint = spec * uKeyIntensity * (vBonded >= 0.5 ? 1.6 : 2.0);
    float flash = glint > 0.55 ? (glint - 0.55) * 2.2 : 0.0;

    vec3 key = uKeyColor.rgb;
    vec3 col = clamp(base * kd * key + glint * 0.35 * key + flash, 0.0, 1.0);

    float alpha = vColor.a * cov;
    if (alpha <= 0.0)
        discard;
    // CPU blendOver is straight-alpha "over"; QSG blends premultiplied, so
    // output premultiplied color (the composited result matches the CPU)
    fragColor = vec4(col * alpha, alpha);
}