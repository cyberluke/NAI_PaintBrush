// glitter.vert — NAI Paintbrush GPU glitter grains (quad per grain)
//
// One quad per grain, laid out in LAYER-LOCAL world units (1 unit = 1 px at
// 1x). The CPU golden path (glitter_material.cpp) rasterizes the same
// footprint with the same coverage; here the grain data rides in per-vertex
// attributes so the fragment shader reproduces the CPU shading exactly.
#version 440

layout(location = 0) in vec2 aPos;          // quad corner in grain-local units (-1..1)
layout(location = 1) in vec2 aCenter;       // grain center (world units, layer-local)
layout(location = 2) in float aRadius;      // flake radius (world units)
layout(location = 3) in float aRot;         // facet rotation (radians)
layout(location = 4) in vec4 aColor;        // straight rgb + alpha (0 for contact-shadow quads)
layout(location = 5) in float aHeight;      // 0..1 layering (loose sit higher)
layout(location = 6) in float aBonded;      // 1.0 bonded, 0.0 loose
layout(location = 7) in float aShadowLen;   // contact-shadow offset length (world units), 0 = grain
layout(location = 8) in vec2 aShadowDir;    // unit shadow direction (away from light)
layout(location = 9) in float aSeed;        // per-grain hash seed (0..2^32 as float)

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec3 uLightL;        // unit vector from surface toward light
    float uPad0;
    vec3 uLightH;        // half vector (L + view)
    float uPad1;
    float uSpecExp;      // specular exponent
    float uKeyIntensity;
    float uAmbient;
    float uSoftness;
    vec4 uKeyColor;
    float uToothShade;
    float uAbsorption;
    vec2 uViewSize;      // output size in device px (pixel -> clip mapping)
    float uPad2;
};

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vRadius;
layout(location = 3) out float vRot;
layout(location = 4) out float vHeight;
layout(location = 5) out float vBonded;
layout(location = 6) out float vShadowLen;
layout(location = 7) out vec2 vShadowDir;
layout(location = 8) out float vSeed;

void main()
{
    vLocal = aPos;
    vColor = aColor;
    vRadius = aRadius;
    vRot = aRot;
    vHeight = aHeight;
    vBonded = aBonded;
    vShadowLen = aShadowLen;
    vShadowDir = aShadowDir;
    vSeed = aSeed;

    vec2 center = aCenter + aShadowDir * aShadowLen;
    // quad spans radius + 2px so the AA band (CPU: [center-r-1, center+r+1])
    // is always inside the geometry
    vec2 pos = center + aPos * (aRadius + 2.0);
    // pixel coords (origin top-left, y down) -> clip space
    vec2 clip = vec2(pos.x / uViewSize.x * 2.0 - 1.0,
                     1.0 - pos.y / uViewSize.y * 2.0);
    gl_Position = qt_Matrix * vec4(clip, 0.0, 1.0);
}