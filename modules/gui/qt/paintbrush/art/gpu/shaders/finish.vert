// finish.vert — NAI Paintbrush GPU finish passes (DirectedBlur/Shadow/Glow)
// One textured quad per effect pass over the layer's source texture.
#version 440

layout(location = 0) in vec2 aPos;          // quad corner 0..1
layout(location = 1) in vec2 aTexCoord;     // matching texture coordinate 0..1

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uTexSize;         // source size in px
    vec2 uOutSize;         // padded output size in px
    vec2 uDir;             // blur direction (unit)
    float uDistancePx;     // blur distance / shadow blur radius
    float uFalloff;        // blur taper
    int uMode;             // 0 blur, 1 shadow, 2 glow
    int uSamples;
    vec2 uPad;             // padding offset in px (source top-left in output)
    vec4 uColor;           // shadow/glow color (rgb + opacity)
    vec2 uOffsetPx;        // shadow offset (px, world-scaled)
    float uThreshold;      // glow alpha threshold (0..1)
    float uIntensity;      // glow multiplier (0..4)
};

layout(location = 0) out vec2 vTexCoord;

void main()
{
    // D3D clip y=-1 is the BOTTOM of the screen while texture y=0 is the
    // TOP row: flip so the output reads top-down like the CPU images.
    vTexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
    gl_Position = qt_Matrix * vec4(aPos, 0.0, 1.0);
}