// powder.frag — NAI Paintbrush GPU glitter powder (reference-faithful port)
//
// Direct port of cray4pasta/glitter-powder's GPU fragment shader to the
// Paintbrush RHI. The powder heightfield (uSim, RGBA16F floating point) is
// turned into packed glass beads (Voronoi), smooth multi-scale normals,
// image-based specular, holographic flakes that shimmer with uTime, wet-glue
// gloss, a soft contact shadow and additive bloom.
//
// Coordinate model: the fragment position `vPin` is device px (top-left
// origin, y down). Every local quantity is derived ONCE as
//     world = uWorldOrigin + px * uWorldPerPx
// and the grain + holographic-flake FIELDS are anchored to ABSOLUTE world
// coordinates at fixed WORLD pitches (uWorldCell for grains, uFlakeWorld for
// flakes). The render crop's origin/size never enter grain/flake identity,
// jitter, seed or holo phase — they only decide which pixels are emitted — so
// preview / commit / export / left-crop / right-crop all agree on the same
// world pixels (crop invariance).
//
// Lighting: uLightDir is the normalized direction to the key light (from the
// shared ArtLightRig, in the same coordinate unit as the surface) — no
// pixel-space light, no hardcoded top-left source.
#version 440

layout(location = 0) in vec2 vPin;      // device px, top-left, y down

layout(binding = 0) uniform sampler2D uSim;    // RGBA16F: r=height g=seed b=stuck a=glue (real cell units)
layout(binding = 1) uniform highp sampler2D uCol;  // RGBA8 packed, reference layout:
                                                  //  r = loose A<<4 | B, g = loose B share (0..255)
                                                  //  b = bonded A<<4 | B, a = bonded B share
layout(binding = 3) uniform sampler2D uGlue;   // P0.2: full-res world-anchored glue field (R8),
                                               // sub-cell coverage at 0..1 with uGrid*glueSub res.

layout(std140, binding = 2) uniform ubuf {
    mat4 qt_Matrix;
    vec2 uViewSize;        // device px
    vec2 uWorldOrigin;     // world coords at vPin = (0,0)
    float uWorldPerPx;     // world units per device px
    float uWorldCell;      // world units per sim cell
    float uDpr;            // device-pixel ratio (>= 1 for hi-dpi)
    float uTime;           // seconds, drives the holographic shimmer
    vec2 uGrid;            // sim grid size (cells)
    float uHolo;           // 0 metallic ... 1 rainbow (shimmer)
    float uFlakePx;        // holographic flake spacing control (device px)
    float uFlakeWorld;     // world-space flake pitch (world units per candidate):
                           // anchors flake identity + centres to ABSOLUTE world so
                           // crop/zoom never change which flake a pixel sees.
    float uTooth;          // 0..1 paper tooth (subtle base noise)
    float uMaterialMode;   // 0 = REFERENCE_SURFACE (opaque paper+powder)
                           // 1 = MATERIAL_LAYER (transparent material only)
    vec3 uLightDir;        // normalized direction to the key light
    float uPad1;
    vec3 uPaper;           // paper colour
    float uPad2;
    vec3 uPal[16];         // powder palette slots
};

layout(location = 0) out vec4 fragColor;

// ---- hashing (reference noise) ---------------------------------------
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

// ---- heightfield (real float values, no quantization) ----------------
vec2 simUV(vec2 world) { return world / (uWorldCell * uGrid); }
float heightAt(vec2 world)
{
    return texture(uSim, clamp(simUV(world), 0.001, 0.999)).r;
}
float seedAt(vec2 world)
{
    return texture(uSim, clamp(simUV(world), 0.001, 0.999)).g;
}
vec4 simAtCell(vec2 worldCell)
{
    ivec2 c = ivec2(clamp(ivec2(floor(worldCell)), ivec2(0), ivec2(uGrid) - ivec2(1)));
    return texelFetch(uSim, c, 0);
}

// P0.2: the full-resolution glue field (uGlue) — same world extent as uSim, so
// simUV(world) maps to it at sub-cell resolution. Returns 0..1 coverage.
float glueAt(vec2 world)
{
    return texture(uGlue, clamp(simUV(world), 0.0, 1.0)).r;
}

// Real two-slot colour from the reference-packed uCol texture. Each sim cell
// holds up to two colours of loose glitter (in RG) and two of glitter stuck to
// the glue (in BA); a grain is one or the other by chance, so mixed glitter
// reads as speckles of each colour rather than a blend.
vec3 cellColour(ivec2 c, float pick, bool loose)
{
    vec4 v = floor(texelFetch(uCol, c, 0) * 255.0 + 0.5);
    vec2 lv = loose ? v.rg : v.ba;
    int ab = int(lv.x);
    int a = ab / 16;
    int b = ab - a * 16;
    return uPal[pick < lv.y / 255.0 ? b : a];
}

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
    /* reference phase: the random `gh3.y * 6.28` is INSIDE the sine modulation,
     * so each flake's shimmer rests on its own static phase, not added outside. */
    float a = gh2.y * 6.2832 + (gh.x > 0.5 ? 1.0 : -1.0) * uTime * rate
            + 0.6 * sin(uTime * rate * (0.6 + 1.6 * gh3.x) + gh3.y * 6.28);
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
    // the one world mapping used everywhere below (crop/zoom invariant)
    vec2 world = uWorldOrigin + vPin * uWorldPerPx;

    // ---- surface from the float heightfield -------------------------
    vec4 sim = simAtCell(world / uWorldCell);
    float h = texture(uSim, clamp(simUV(world), 0.001, 0.999)).r;
    float stuckLow = sim.b;
    float glueAmtCell = sim.a;                     // cell-res glue (our sim's truth)
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
    vec3 N = normalize(vec3(-grad * (uWorldCell / uWorldPerPx), 1.0));

    vec3 L = normalize(uLightDir);
    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));

    // ---- paper with a soft cast shadow ------------------------------
    vec2 away = normalize(uLightDir.xy) * -1.0;
    float sh = 0.0;
    for (int k = 1; k <= 3; k++)
        sh += smoothstep(0.3, 7.0, heightAt(world - away * float(k) * 7.0 * uWorldCell));
    float shadow = 0.10 * sh / 3.0 + 0.06 * smoothstep(0.1, 1.0, h);
    vec3 bg = uPaper - shadow * vec3(1.0, 1.0, 0.97);

    // ---- grain field (packed glass beads via Voronoi) ---------------
    // ABSOLUTE world-anchored Voronoi at a fixed grain pitch = uWorldCell (one
    // sim cell per grain, matching the CPU renderer's grainPitch). The crop's
    // origin/size never enter this: the same world pixel always maps to the
    // same integer grain id, jitter, seed and holo phase. The render crop only
    // decides WHICH pixels are emitted (crop invariance, P1 reviewer #4).
    vec2 g = world / uWorldCell;
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

    // grain centre in ABSOLUTE WORLD (bPt is in grain-pitch == cell units)
    vec2 gcWorld = bPt * uWorldCell;
    ivec2 scC = ivec2(clamp(ivec2(floor(gcWorld / uWorldCell)), ivec2(0), ivec2(uGrid) - ivec2(1)));
    float s = seedAt(gcWorld);
    vec2 sid = bId + s * 13.17;
    vec3 gh  = hash32(sid + 71.3);
    vec3 gh2 = hash32(sid * 1.37 + 5.1);
    vec3 gh3 = hash32(sid * 0.73 + 19.0);

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
    /* DARK-MATERIAL CHROMA LIMIT: a near-black tint must not turn the flake's
     * thin-film rainbow into isolated RGB dots. Bleed the chromatic energy to
     * its luminance (neutral silver mirror), keeping a specular flash not a
     * coloured particle. */
    float tintLuma = dot(tint, vec3(0.299, 0.587, 0.114));
    float darkness = 1.0 - smoothstep(0.04, 0.32, tintLuma);
    float flakeLum = dot(flake, vec3(0.299, 0.587, 0.114));
    flake = mix(vec3(flakeLum), flake, 1.0 - 0.95 * darkness);
    bead += flake * isFlake * inBead * 0.7;

    /* DARK-MATERIAL BROAD NEUTRAL MIRROR: for very dark powder the diffuse
     * albedo contributes ~nothing, so a BROAD normal/light-controlled neutral
     * reflection reveals the graphite-lit side vs the black shadow side — the
     * same geometry that coloured glitter shows via its albedo. The macro
     * normal, rig light and half-vector drive the split (not a per-grain dot). */
    float macroSpec = pow(pow(max(dot(N, H), 0.0), 3.0), 3.0);   /* broad ~x9 cone */
    float shadowFade = 0.12 + 0.88 * macroDiff;
    float broadMirror = darkness * (0.10 + 0.42 * macroSpec) * shadowFade;
    bead += vec3(broadMirror);

    vec3 gapPowder = tint * 0.935 * (0.76 + 0.26 * macroDiff);
    vec3 col = mix(mix(bg, gapPowder, deep), bead, inBead * present);

    // ---- wet glue (clear raised glossy line; cell-res measured surface)
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
    // ABSOLUTE world-anchored flake candidates at a fixed world pitch
    // (uFlakeWorld). `id`/`c` are in flake-pitch units; the flake centre in
    // WORLD is c * uFlakeWorld, so crop/zoom never change which flake a pixel
    // sees or where its mirror sits. Only `d2`/`radius` stay in pitch units to
    // measure coverage of this pixel relative to the flake.
    vec2 fg = world / uFlakeWorld;
    vec2 fi = floor(fg);
    vec3 bloom = vec3(0.0);
    for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++)
    {
        vec2 id = fi + vec2(float(x), float(y));
        vec3 r1 = hash32(id * 1.91 + 2.7);
        vec2 c = id + 0.2 + 0.6 * r1.yz;
        // flake centre in ABSOLUTE WORLD
        vec2 cWorld = c * uFlakeWorld;
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
        /* dark flake chroma limit: no isolated RGB dot on near-black material */
        float tLum = dot(tintF, vec3(0.299, 0.587, 0.114));
        float tDark = 1.0 - smoothstep(0.04, 0.32, tLum);
        float litLum = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(vec3(litLum), lit, 1.0 - 0.95 * tDark);
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

    /* ---- MATERIAL_LAYER output contract -------------------------------
     * A Paintbrush powder/material layer must be transparent outside its
     * physical material: background paper is NEVER emitted (the document
     * compositor owns PaperStock). The powder/glue/shadow contribution is
     * assembled over transparent and given a physical coverage alpha, so the
     * layer composites cleanly onto the paper beneath it.
     *
     * REFERENCE_SURFACE (mode 0) keeps the standalone reference harness's
     * contract: opaque paper + powder. Same shading equations in both modes;
     * only the final assembly (bg/alpha) differs. */
    if (uMaterialMode >= 0.5)
    {
        /* physical material coverage: a grain bead, loose/gap powder under
         * it, and wet glue are all supporting material. P0.2: the full-res
         * glue field provides the crisp presence even where the coarse cell
         * says no powder (glue-only cells must show the wet adhesive). */
        float gluePres = glueAt(world);
        float glueA = smoothstep(0.08, 0.5,
                                 max(glueAmtCell, gluePres)) * (1.0 - bonded)
                    * (1.0 - smoothstep(0.05, 0.6, h));
        glueA = max(glueA, 0.0);

        /* material colour over transparent: dust/gap powder fades in with the
         * heap depth, then beads sit on top and wet glue glosses over. */
        vec3 mat = gapPowder * deep;
        mat = mix(mat, bead, inBead * present);
        /* ---- WET GLUE as a CLEAR adhesive (P0, not white paint) ----
         * Over transparent the glue must read as "clear wet glue", never a
         * white/paper-coloured brush. We emit only:
         *   - a near-transparent cool body (clear, very low contribution),
         *   - a subtle darker refractive rim,
         *   - a small WHITE specular glint from the light rig half-vector.
         * glueCover stays LOW so the film composites transparently. */
        if (glueA > 0.001)
        {
            vec2 gGm = vec2(heightAt(world + vec2(uWorldCell, 0.0)) - heightAt(world - vec2(uWorldCell, 0.0)),
                            heightAt(world + vec2(0.0, uWorldCell)) - heightAt(world - vec2(0.0, uWorldCell)));
            vec3 Ng = normalize(vec3(-gGm * 2.2, 1.0));
            float rim = clamp(length(gGm) * 1.6, 0.0, 1.0);
            /* clear body: a whisper of cool, near-neutral, low contribution */
            vec3 clearBody = vec3(0.995, 0.997, 1.0) * (1.0 - 0.06 * rim);
            float bodyA = 0.05 * glueA;           /* very low alpha */
            mat = mix(mat, clearBody, min(bodyA, 0.12));
            /* small white specular glint (highest of the clear-film alpha) */
            float gloss = pow(max(dot(Ng, H), 0.0), 72.0);
            float hli = 0.3 + 0.7 * max(dot(Ng, L), 0.0);
            mat += vec3(1.0) * (gloss * hli * (0.08 + 0.17 * glueA));
        }
        /* soft contact shadow rides WITH the material (dimming it), never
         * baked onto paper. Matches the CPU material renderer, which darkens
         * the tint colour by the shadow but never drives a grain to pure
         * black: cap the reduction so the material body keeps its colour. */
        float shadowCap = clamp(shadow, 0.0, 0.5);
        mat = mat * (1.0 - 0.55 * shadowCap);

/* coverage alpha: real powder is opaque-supportive; a glue-only (clear
         * adhesive) film stays TRANSPARENT (low alpha, never a white paint fill). */
        float a = clamp(max(inBead * present, deep), 0.0, 1.0)
                + (0.06 + 0.16 * smoothstep(0.15, 0.85, glueA)) * gluePres;
        a = min(a, 1.0);

        /* no pure-black material: this palette has no black colour, so a
         * material pixel with real coverage must keep a faint tinted body even
         * inside the deepest contact shadow — never collapse to (0,0,0).
         * Apply the floor to the PREMULTIPLIED output (mat*a), so a grain with
         * low alpha still reads a tinted color rather than rounding to black. */
        vec3 outCol = mat * a;
        float outLum = dot(outCol, vec3(0.299, 0.587, 0.114));
        float floorV = 0.012 + 0.05 * a * (1.0 - dot(mat, vec3(0.333)));
        if (a > 0.03 && outLum < floorV)
            outCol = vec3(floorV, floorV * 0.97, floorV * 1.02);

        /* outside support: fully transparent (no opaque rectangle) */
        fragColor = vec4(outCol, a);
    }
    else
    {
        /* reference surface: opaque paper + powder (standalone harness) */
        fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
    }
}