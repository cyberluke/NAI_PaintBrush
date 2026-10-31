// powder.frag — NAI Paintbrush GPU glitter powder (reference-faithful port)
//
// Direct port of cray4pasta/glitter-powder's GPU fragment shader to the
// Paintbrush RHI: the powder heightfield (uSim) is turned into packed glass
// beads (Voronoi), smooth multi-scale normals, image-based specular,
// holographic flakes that shimmer with uTime, wet-glue gloss, a soft contact
// shadow and additive bloom. Coordinates are world-space (top-left origin);
// the fragment position `vPin` is device px (y down) matched to the layer
// raster the canvas composites into.
#version 440

layout(location = 0) in vec2 vPin;      // device px, top-left, y down

layout(binding = 0) uniform sampler2D uSim;   // r=height(grain),g=seed,b=stuck,a=glue
layout(binding = 1) uniform sampler2D uCol;   // per-cell: r=looseA,g=looseF,b=bondedA,a=bondedF

layout(std140, binding = 2) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uViewSize;        // device px
    vec2 uWorldOrigin;     // world coords at vPin = (0,0)
    float uWorldPerPx;     // world units per device px
    float uWorldCell;      // world units per sim cell
    float uDpr;            // device-pixel ratio (>= 1 for hi-dpi)
    float uTime;           // seconds, drives the holographic shimmer
    vec2 uGrid;            // sim grid size (cells)
    vec2 uPadA;
    vec2 uLight;           // key light position (scene px)
    float uHolo;           // 0 metallic ... 1 rainbow (shimmer)
    float uFlakePx;        // holographic flake spacing (device px)
    float uTooth;          // 0..1 paper tooth (subtle base noise)
    float uPadB;
    vec3 uPaper;           // paper colour
    float uPadC;
    vec3 uPal[4];          // powder palette (slots; tint + two mix slots)
};

layout(location = 0) out vec4 fragColor;

// ---- hashing (reference art_noise-style) ------------------------------
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec3 hash32(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}
vec3 clamp01v(vec3 x) { return clamp(x, 0.0, 1.0); }

// ---- heightfield -------------------------------------------------------
vec2 simUV(vec2 world) { return world / (uWorldCell * uGrid); }
float heightAt(vec2 world)
{
    return texture(uSim, clamp(simUV(world), 0.001, 0.999)).r;
}
float seedAt(vec2 world)
{
    return texture(uSim, clamp(simUV(world), 0.001, 0.999)).g;
}
vec4 simAtCell(vec2 worldCell)   // per-cell (texelFetch) data
{
    ivec2 c = ivec2(clamp(ivec2(floor(worldCell)), ivec2(0),
                          ivec2(uGrid) - ivec2(1)));
    return texelFetch(uSim, c, 0);
}
vec3 cellColour(ivec2 c, float pick, bool loose)
{
    vec4 v = texelFetch(uCol, c, 0);
    vec2 lv = loose ? v.rg : v.ba;
    int a = int(lv.x);
    int b = int(lv.x) + 1;   // blend toward the next slot by share
    float share = clamp(lv.y, 0.0, 1.0);
    vec3 A = uPal[clamp(a, 0, 3)];
    vec3 B = uPal[clamp(b, 0, 3)];
    return mix(A, B, pick < share ? 1.0 : 0.0);
}
// thin-film interference for holographic flakes
vec3 thinFilm(float cosI, float d)
{
    const float n = 1.5;
    float cosT = sqrt(max(1.0 - (1.0 - cosI * cosI) / (n * n), 0.0));
    float opd = 2.0 * n * d * cosT;
    vec3 c = 0.5 + 0.5 * cos(6.28318 * opd / vec3(650.0, 530.0, 450.0) + 3.14159);
    c = pow(c, vec3(1.6));
    float l = dot(c, vec3(0.333));
    return clamp01v(mix(vec3(l), c, 2.4));
}
vec3 tintSpec(vec3 c) { return c / max(max(c.r, max(c.g, c.b)), 0.04); }

// ---- holographic flake sparkle (shimmers under uTime) -----------------
vec3 flakeLight(vec2 sidSeed, float chance, float flatness,
                vec3 N, vec3 L, vec3 H, vec3 tint,
                out vec3 rainbow, out float isFlake)
{
    vec3 gh2 = hash32(sidSeed * 1.37 + 5.1);
    isFlake = step(gh2.x, chance);
    rainbow = vec3(1.0);
    if (isFlake == 0.0) return vec3(0.0);
    vec3 gh  = hash32(sidSeed + 71.3);
    vec3 gh3 = hash32(sidSeed * 0.73 + 19.0);
    float tilt = (0.05 + 0.85 * gh3.z) * (1.0 - 0.55 * flatness);
    float rate = exp2(mix(-2.5, 0.8, gh2.z));
    float a = gh2.y * 6.2832 + (gh.x > 0.5 ? 1.0 : -1.0) * uTime * rate
            + 0.6 * sin(uTime * rate * (0.6 + 1.6 * gh3.x)) + gh3.y * 6.28;
    vec3 Nf = normalize(N + vec3(cos(a), sin(a), 0.0) * tilt);
    float fd = max(dot(Nf, H), 0.0);
    float sharp = mix(14.0, 140.0, gh3.x * gh3.x);
    float amp = 1.0 + 4.0 * pow(gh3.y, 3.0);
    float film = mix(250.0, 600.0, hash32(sidSeed * 2.7 + 1.3).x);
    rainbow = mix(tintSpec(tint), thinFilm(max(dot(Nf, L), 0.0), film), uHolo);
    float spec = pow(fd, sharp) * amp;
    return rainbow * (pow(fd, 12.0) * 0.15 + spec * 1.6)
         + vec3(1.0) * pow(spec, 3.0) * 0.08;
}

void main()
{
    float unit = min(uViewSize.x, uViewSize.y);
    vec2 p = (vPin - 0.5 * uViewSize) / unit;

    // world coordinate of this fragment (top-left origin, y down)
    vec2 world = uWorldOrigin + vPin * uWorldPerPx;

    // ---- surface from the heightfield -------------------------------
    vec4 sim = simAtCell(world / uWorldCell);
    float h = texture(uSim, clamp(simUV(world), 0.001, 0.999)).r;
    float stuckLow = sim.b;
    float glueAmtCell = sim.a;
    float bonded = smoothstep(0.02, 0.25, stuckLow);
    float looseH = max(h - stuckLow, 0.0);

    // multi-scale slope -> smooth pile normal (reference)
    vec2 grad = vec2(0.0);
    for (int k = 0; k < 3; k++)
    {
        float o = uWorldCell * (1.5 + 3.0 * float(k));
        float w = 2.0 * o;
        grad += vec2(heightAt(world + vec2(o, 0.0)) - heightAt(world - vec2(o, 0.0)),
                     heightAt(world + vec2(0.0, o)) - heightAt(world - vec2(0.0, o))) / w;
    }
    grad /= (uWorldCell / (uWorldPerPx * uDpr));   // normalize by cell px
    vec3 N = normalize(vec3(-grad * 3.0, 1.0));

    vec3 L = normalize(vec3(uLight - p, 0.6));
    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));

    // ---- paper with a soft cast shadow ------------------------------
    vec2 away = normalize(p - uLight + 1e-4);
    float sh = 0.0;
    for (int k = 1; k <= 3; k++)
        sh += smoothstep(0.3, 7.0 * (uWorldPerPx / uDpr), heightAt(world - away * float(k) * 7.0 * uWorldCell));
    float shadow = 0.10 * sh / 3.0 + 0.06 * smoothstep(0.1, 1.0, h);
    vec3 bg = uPaper - shadow * vec3(1.0, 1.0, 0.97);

    // ---- grain field (packed glass beads via Voronoi) ---------------
    float scale = unit / (1.0 * uDpr);              // ~1 px per grain
    vec2 g = p * scale;
    vec2 gi = floor(g), gf = fract(g);
    float b1 = 8.0, b2 = 8.0;
    vec2 bR = vec2(0.0), bId = vec2(0.0), bPt = vec2(0.0);
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++)
    {
        vec2 cc = vec2(float(x), float(y));
        vec2 pt = cc + 0.15 + 0.7 * hash32(gi + cc).xy;
        vec2 r = pt - gf;
        float dd = dot(r, r);
        if (dd < b1) { b2 = b1; b1 = dd; bR = r; bId = gi + cc; bPt = gi + pt; }
        else if (dd < b2) { b2 = dd; }
    }
    float dist = sqrt(b1);
    float edge = sqrt(b2) - dist;

    // grain centre world cell + shuffle seed (disturbed grains look fresh)
    vec2 gcWorld = (bPt / scale) * unit + uWorldOrigin + vPin * uWorldPerPx;
    ivec2 scC = ivec2(clamp(ivec2(floor(gcWorld / uWorldCell)), ivec2(0), ivec2(uGrid) - ivec2(1)));
    float s = seedAt(gcWorld);
    vec2 sid = bId + s * 13.17;
    vec3 gh  = hash32(sid + 71.3);
    vec3 gh2 = hash32(sid * 1.37 + 5.1);
    vec3 gh3 = hash32(sid * 0.73 + 19.0);

    // grain colour from a jittered cell near its centre (loose or bonded)
    vec3 gj = hash32(sid * 2.31 + 9.7);
    vec2 gcJit = gcWorld + (gj.xy - 0.5) * uWorldCell * 1.6;
    ivec2 ccC = ivec2(clamp(ivec2(floor(gcJit / uWorldCell)), ivec2(0), ivec2(uGrid) - ivec2(1)));
    vec3 tint = cellColour(ccC, gj.z, gh.y < clamp(looseH / 0.9, 0.0, 1.0));

    float rad = 0.5 + 0.12 * gh.x;
    float rr = dist / rad;
    float inBead = 1.0 - smoothstep(0.82, 1.0, rr);

    float glueAmt = smoothstep(0.15, 0.85, glueAmtCell + (gh.x - 0.5) * 0.55) * bonded;
    float exposedGlue = glueAmt * (1.0 - smoothstep(0.25, 1.1, looseH));
    float present = gh.y < max(clamp(looseH / 0.9, 0.0, 1.0), glueAmt) ? 1.0 : 0.0;
    float deep = smoothstep(0.4, 1.4, looseH + pow(glueAmt, 1.5) * 1.6);

    vec2 nb = -bR / rad;
    vec3 Nb = normalize(N + vec3(nb * 0.9, 0.0) * step(rr, 1.0));
    float diff = clamp(dot(Nb, L), 0.0, 1.0);
    float macroDiff = clamp(dot(N, L), 0.0, 1.0);

    float tone = (0.80 + 0.26 * gh.z) * (1.0 + 0.05 * exposedGlue);
    tone *= mix(1.0, 0.72, step(0.95, gh3.y));
    vec3 albedo = tint * tone;
    vec3 bead = albedo * (0.80 + 0.30 * diff);
    bead *= mix(0.84, 1.0, smoothstep(0.0, 0.22, edge));
    bead += mix(vec3(1.0), tintSpec(tint), 0.5) * pow(max(dot(Nb, H), 0.0), 45.0) * 0.35;

    vec3 rainbow; float isFlake;
    vec3 flake = flakeLight(sid, 0.06, exposedGlue, N, L, H, tint, rainbow, isFlake);
    bead += flake * isFlake * inBead * 0.7;

    vec3 gapPowder = tint * 0.935 * (0.76 + 0.26 * macroDiff);
    vec3 col = mix(mix(bg, gapPowder, deep), bead, inBead * present);

    // ---- wet glue (clear raised glossy line) ------------------------
    float wet = smoothstep(0.08, 0.5, glueAmtCell) * (1.0 - bonded)
              * (1.0 - smoothstep(0.05, 0.6, h));
    if (wet > 0.001)
    {
        vec2 gG = vec2(heightAt(world + vec2(uWorldCell, 0.0)) - heightAt(world - vec2(uWorldCell, 0.0)),
                       heightAt(world + vec2(0.0, uWorldCell)) - heightAt(world - vec2(0.0, uWorldCell)));
        vec3 Ng = normalize(vec3(-gG * 2.2, 1.0));
        float rim = clamp(length(gG) * 1.6, 0.0, 1.0);
        float gloss = pow(max(dot(Ng, H), 0.0), 70.0);
        vec3 glueCol = uPaper * (0.925 - 0.12 * rim) * vec3(0.975, 0.985, 1.0) + gloss * 0.85;
        col = mix(col, glueCol, wet);
    }

    // ---- larger holographic flakes + bloom --------------------------
    float fscale = unit / (uFlakePx * uDpr);
    vec2 fg = p * fscale;
    vec2 fi = floor(fg);
    vec3 bloom = vec3(0.0);
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++)
    {
        vec2 id = fi + vec2(float(x), float(y));
        vec3 r1 = hash32(id * 1.91 + 2.7);
        vec2 c = id + 0.2 + 0.6 * r1.yz;
        vec2 cWorld = ((c / fscale) * unit) + uWorldOrigin + vPin * uWorldPerPx;
        ivec2 ct = ivec2(clamp(ivec2(floor(cWorld / uWorldCell)), ivec2(0), ivec2(uGrid) - ivec2(1)));
        vec4 cs = texelFetch(uSim, ct, 0);
        float glueF = cs.a * smoothstep(0.02, 0.25, cs.b);
        float looseF = max(cs.r - cs.b, 0.0);
        float onGlue = glueF * (1.0 - smoothstep(0.25, 1.1, looseF));
        if (r1.x > mix(0.45, 0.8, onGlue) * (uFlakePx / 5.0)) continue;
        vec2 fsid = id * 3.1 + cs.g * 13.17;
        vec3 r2 = hash32(fsid + 40.0);
        if (r2.y >= max(clamp(looseF / 1.2, 0.0, 1.0), glueF)) continue;

        float size = r2.x * r2.x * r2.x;
        float radius = mix(0.12, 0.45, size);
        vec3 fc = hash32(fsid * 1.7 + 3.3);
        vec3 tintF = cellColour(ct, fc.x, r2.y < clamp(looseF / 1.2, 0.0, 1.0));
        vec3 rb; float isF;
        vec3 lit = flakeLight(fsid, 1.0, onGlue, N, L, H, tintF, rb, isF);
        float peak = max(lit.r, max(lit.g, lit.b));
        float soft = radius * (0.18 + 0.7 * clamp(peak - 0.8, 0.0, 1.0) * (0.3 + size));
        float d2 = length(fg - c);
        float cov = 1.0 - smoothstep(radius - soft, radius + soft, d2);

        vec3 silver = tintF * 0.945 * (0.80 + 0.24 * macroDiff) * (0.92 + 0.14 * r2.z);
        float lift = clamp(peak, 0.0, 1.0);
        col = mix(col, silver + lit * (1.0 + size), cov * mix(0.45, 1.0, lift));
        float over = max(peak - 1.0, 0.0);
        bloom += normalize(lit + 1e-4) * over * exp(-pow(d2 / (radius * 1.5), 2.0))
               * smoothstep(1.15, 0.8, d2) * 0.25 * (0.4 + size);
    }
    col += bloom;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}