// powder.vert — NAI Paintbrush GPU glitter powder (fullscreen heightfield pass)
//
// A fullscreen quad. The fragment shader rasterises the powder heightfield
// from textures exactly like the reference (WebGL) demo: the powder field is a
// per-cell heightfield and the fragment shader turns it into packed glass
// beads (Voronoi), smooth multi-scale normals, image-based specular,
// holographic flakes that shimmer over time, wet-glue gloss, contact shadows
// and bloom.
//
// vPos is the device-pixel coordinate of each fragment with a TOP-LEFT origin
// and Y DOWN (matching the layer raster the canvas writes into), so the shader
// is invariant to the RHI backend's framebuffer Y convention.
#version 440

layout(location = 0) in vec2 aPos;      // clip-space corner (-1..1)

layout(std140, binding = 0) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uViewSize;      // output size in device px
    vec2 uPad0;
};

layout(location = 0) out vec2 vPos;     // device px, top-left origin, y down

void main()
{
    // clip corner -> device px, top-left origin, y down
    float px = (aPos.x * 0.5 + 0.5) * uViewSize.x;
    float py = (0.5 - aPos.y * 0.5) * uViewSize.y;
    vPos = vec2(px, py);
    gl_Position = qt_Matrix * vec4(aPos, 0.0, 1.0);
}