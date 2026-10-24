// paper.vert — NAI Paintbrush GPU paper quad (world-space, deterministic)
// Vertex positions are clip-space quad corners; the fragment shader maps
// them to world coordinates through uWorldOrigin + uWorldPerPx and samples
// the SAME continuous heightmap as the CPU PaperRenderer.
#version 440

layout(location = 0) in vec2 aPosition;

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uWorldOrigin;     // world coordinates at canvas (0,0)
    vec2 uViewSize;        // output size in device px
    float uWorldPerPx;     // world units per device pixel (1x)
    float uOpaque;
    vec4 uBase;            // base color rgb (0..1) + pad
    vec4 uGrain;           // microHeight, tooth, fiberStrength, fiberScale
    vec4 uSurface;         // specular, roughness, ambient, pad
    uint uSeed;            // paper stock seed (exact)
};

layout(location = 0) out vec2 vCanvasPos;

void main()
{
    vCanvasPos = aPosition;
    gl_Position = qt_Matrix * vec4(aPosition, 0.0, 1.0);
}