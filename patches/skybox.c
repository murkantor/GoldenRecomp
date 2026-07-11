#include "patches.h"
#include "gbi_extension.h"

// Fallback fog-colour fill patches, superseded by the full skyRender
// reimplementation below. Only called by the original skyRender, so they are
// unreachable while the skyRender patch is enabled.
#if 0
RECOMP_PATCH Gfx* sub_GAME_7F097818(Gfx* gdl, SkyRelated38* v1, SkyRelated38* v2, SkyRelated38* v3, f32 scale,
                                    bool textured) {
    u32 width = viGetX();
    u32 height = viGetY();

    // Fetch the current environment (this is where fog and sky color are stored)
    struct CurrentEnvironmentRecord* env = fogGetCurrentEnvironmentp();

    // Get the fog color components (assumes fog color is stored in RGBA format)
    u8 red = env->Red;
    u8 green = env->Green;
    u8 blue = env->Blue;

    // Set the fog color as the fill color for the skybox
    u32 fill_color = GPACK_RGBA5551(red, green, blue, 1); // Fog color instead of hardcoded light blue

    // Fill the skybox with the fog color
    gDPPipeSync(gdl++);
    gDPSetCycleType(gdl++, G_CYC_FILL);
    gDPSetColorImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, width, osViGetCurrentFramebuffer());
    gDPSetFillColor(gdl++, (fill_color << 16) | fill_color);
    gDPFillRectangle(gdl++, 0, 0, (width - 1), (height - 1));
    gDPPipeSync(gdl++);

    return gdl;
}

RECOMP_PATCH Gfx* sub_GAME_7F098A2C(Gfx* gdl, SkyRelated38* arg1, SkyRelated38* arg2, SkyRelated38* arg3,
                                    SkyRelated38* arg4, f32 arg5) {
    u32 width = viGetX();
    u32 height = viGetY();

    // Fetch the current environment to get fog color
    struct CurrentEnvironmentRecord* env = fogGetCurrentEnvironmentp();

    // Get the fog color components
    u8 red = env->Red;
    u8 green = env->Green;
    u8 blue = env->Blue;

    // Set the fog color as the fill color for the skybox
    u32 fill_color = GPACK_RGBA5551(red, green, blue, 1); // Use fog color for the background

    // Fill the skybox with the fog color
    gDPPipeSync(gdl++);
    gDPSetCycleType(gdl++, G_CYC_FILL);
    gDPSetColorImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, width, osViGetCurrentFramebuffer());
    gDPSetFillColor(gdl++, (fill_color << 16) | fill_color);
    gDPFillRectangle(gdl++, 0, 0, (width - 1), (height - 1));
    gDPPipeSync(gdl++);

    return gdl;
}

#endif

/**
 * @recomp SKY PORT NOTES (2026-07-10, runtime-verified on Dam/Surface/Cradle)
 *
 * The original skyRender rasterises the sky/water planes on the CPU into raw
 * RDP triangle commands, which RT64's HLE cannot execute. PORTSKY replaces
 * those rasterised polygons with real RSP geometry: a camera-centred ring fan
 * per plane (skyDrawPlaneFan below). Everything else in skyRender — the
 * no-cloud fills, corner codes, texture/combiner setup — is vanilla.
 *
 * Engine facts this port depends on (each cost a debugging round; sources in
 * comments at the relevant code):
 *
 * 1. RT64 decomposes and camera-tracks whatever sits in the PROJECTION slot
 *    (rt64_rsp.cpp isMatrixViewProj/matrixDecomposeViewProj). Loading a
 *    custom view-bearing matrix there view-locks the geometry and breaks
 *    room/portal rendering. The sky's rotation therefore rides in the
 *    MODELVIEW slot, like room geometry; the projection slot is never
 *    touched by the fan.
 *
 * 2. The RSP's s15.16 fixed-point Mtx cannot hold true-world translations
 *    (camera at ~25000 model units x perspective terms is ~45000 > 32767) —
 *    this is WHY the game rebases all RSP geometry against room origins.
 *    The fan rebase origin is the CAMERA instead (verts camera-relative,
 *    view translation zeroed), which kills all room-transition coupling:
 *    current_model_pos, field_10E0 and D_800364CC appear nowhere here.
 *
 * 3. RT64/RSP honours the far plane, and zfar = the environment's FarFog
 *    (viSetZRange in bgfog.c), as low as 2000 and changing mid-level. The
 *    original CPU rasteriser discarded Z entirely. The fan compensates by
 *    shrinking the disc radially around the eye per frame (angles, hence the
 *    on-screen image, are unchanged) until it fits the depth range.
 *
 * 4. D_800364CC (model->RSP scale) is PER-LEVEL: 1.0 on Dam, ~0.2 on others.
 *    Any RSP-space budget computed in fixed world units breaks off-Dam.
 *
 * 5. The original fog-on-sky law is a pure view-angle function:
 *    frac = 1 - min(2*tan(elevation), 1)   (skyIsScreenCornerInSky,
 *    sky.c:47-61) fed into vertex colours; the combiner
 *    (SHADE-ENV)*TEXEL+ENV with ENV = fog colour makes the texture dissolve
 *    exactly where frac -> 1. The sky never uses RSP G_FOG (that system is
 *    world-only, bgfog.c), and is opaque throughout (alpha 0xff, sky.c:190).
 *
 * 6. GE's sky tiles are MIRRORED: one visual texture period is 4096 tc
 *    units, not 2048 — any UV rebase must step in multiples of 4096 or the
 *    cloud pattern mirror-flips.
 */
#define PORTSKY 1
// Debug isolation switch: draws the fans untextured (G_CC_SHADE) with the
// env colour forced into every vertex, plus a rate-limited SKYFAN state
// print — separates geometry/colour bugs from texture-sampling bugs in one
// build flip. Keep 0 for normal play.
#define SKYDBG 0
// World-space (model-unit) radius of the sky/water plane fans. The game caps
// its sky rays at 300000, but two RSP budgets bind tighter: the depth-fit
// budget (worst level seen: znear 30 / zfar 2000), and the s16 texture
// coordinate range — the ORIGINAL gives every vertex its true world UV at
// real density (no frozen/stretched outer band, and no transparency: alpha
// is 0xff throughout, sky.c:190), so the rim must carry real UVs too:
// 2 * radius * SKY_TEXEL_PER_WORLD(0.2) + 4096 rebase remainder <= 32767
// => radius <= 70000. Rim elevation at h~4700 is 3.8 degrees; below that the
// fog-coloured base fill shows, matching the original's ~97% fog at its ray
// cap.
#define SKY_PLANE_RADIUS 70000.0f

// The disc is scaled radially around the CAMERA before rebasing:
// v' = cam + (v - cam) * s. Directions from the eye (and thus the on-screen
// image) are unchanged — the sky is angular-only — but every vertex lands
// inside the projection's depth range, so the far plane (zfar = FarFog, as
// low as 2000 on some levels) can never clip the disc. The sky draws without
// Z, before the world, so the tiny real depth is harmless.
// s must be computed PER FRAME in RSP units: zfar varies per level and even
// mid-level, and D_800364CC (model->RSP scale) is per-level too (1.0 on Dam,
// ~0.2 elsewhere) — a fixed world-unit scale breached znear/zfar on non-Dam
// levels. Fit the rim at 85% of zfar; if the overhead centre would then dip
// under ~3x znear, favour the near plane and let the rim clip instead
// (fog-heavy levels hide the horizon anyway).

#if SKYDBG
// One shared gate so each sampled frame logs as a single coherent block
// (~once a second at 60fps).
static u32 g_SkyDbgFrame = 0;
static u8 g_SkyDbgPrint = 0;
#endif

// KSEG0 -> physical address, computed locally. Do NOT call the game's
// osVirtualToPhysical here: resolved through dump.toml it returned garbage at
// runtime (observed 2026-07-10: 0x800d5b10 -> 0x800d2ea4), which made the
// gSPBranchList below jump backwards into the DL buffer and locked RT64's
// interpreter in an infinite loop. All pointers this file translates are
// KSEG0, so the mask is exact.
#define SKY_K0_TO_PHYS(p) ((u32) (p) & 0x1FFFFFFF)

#if PORTSKY
/**
 * Draws an "infinite" sky/water plane as a camera-centred ring fan of real
 * RSP geometry (225 verts: centre + four 8-vertex rings to the 70000 rim,
 * a per-quad-rebased textured band out to ~290000 = the original's textured
 * extent at ~97% fog, and a shade-only feather skirt down to the horizon),
 * replacing the original's CPU-rasterised polygons.
 *
 * Transform, derived from the original's own chain (sub_GAME_7F097388
 * projects sky corners through camGetWorldToScreenMtxf x projF; its
 * float1/30 scale multiplies in and divides straight back out):
 *   - verts: camera-relative model units, disc scaled radially around the
 *     eye by depth_s to fit znear..zfar (pure direction — image unchanged);
 *   - MODELVIEW: fit-unscale x camGetWorldToScreenMtxf ROTATION (translation
 *     zeroed: verts are camera-relative, and a true-world translation would
 *     overflow the s15.16 Mtx), CPU-composed into a Mtx embedded in this DL;
 *   - PROJECTION: untouched (see port note 1 — RT64 camera-tracks that slot).
 * Nothing here references current_model_pos / field_10E0 / D_800364CC, so
 * room transitions cannot move the sky (port note 2).
 *
 * The four rings sample the original fog law frac = 1 - min(2 tan(elev), 1)
 * at its landmarks 2h / 4h / 10h / rim (port note 5); four rings because the
 * law is concave and 3-ring linear interpolation visibly under-fogs the
 * mid-sky. Every ring carries true world-anchored UVs at real density — the
 * original never freezes or stretches its far texture; far clouds are hidden
 * purely by the frac->1 colour convergence.
 *
 * Caller must have set up texture/combiner/render state. UVs are anchored to
 * world position so the texture stays put as the fan slides, with the
 * original world-to-texel factor (0.1) and g_SkyCloudOffset scrolling;
 * rebased per draw to the texture period so the s16 range is never hit.
 */
// Raw S10.5 tc units per world unit. Calibrated against emulator captures
// (~one 64-texel cloud period per ~10000 world units => 64*32/10000). The
// "x32 texel" correction attempted earlier was ~15x too dense (radial moire).
#define SKY_TEXEL_PER_WORLD 0.2f

// Outer edge of the TEXTURED sky: the original's own ray cap is 300000
// (skyIsScreenCornerInSky), where the fog law reads ~97% — effectively the
// fog's opacity boundary; beyond its cap the original's UVs freeze (capped
// positions), i.e. it carries no true texture either. 290000 instead of
// 300000 keeps the worst-case per-quad UV span of the outer band inside the
// s16 budget with 16 sectors and 2 radial sub-bands:
// span <= (32767 - 4096 rebase remainder) / 0.2 = 143355 world units, and
// the worst quad (r_mid 180000 -> 290000, 22.5 deg) spans ~141600.
#define SKY_BAND_OUTER 290000.0f

// 22.5-degree direction table for the outer band / skirt rings. Even entries
// are exactly the disc's octagon directions.
static const f32 DIR16[16][2] = {
    { 1.0f, 0.0f },       { 0.92388f, 0.38268f },   { 0.7071f, 0.7071f },   { 0.38268f, 0.92388f },
    { 0.0f, 1.0f },       { -0.38268f, 0.92388f },  { -0.7071f, 0.7071f },  { -0.92388f, 0.38268f },
    { -1.0f, 0.0f },      { -0.92388f, -0.38268f }, { -0.7071f, -0.7071f }, { -0.38268f, -0.92388f },
    { 0.0f, -1.0f },      { 0.38268f, -0.92388f },  { 0.7071f, -0.7071f },  { 0.92388f, -0.38268f },
};

static Gfx* skyDrawPlaneFan(Gfx* gdl, f32 plane_y, bool isWater) {
    s32 i;
    s32 k;
    f32 maxc;
    f32 fit = 1.0f;
    // Rings sample the ORIGINAL fog law (skyIsScreenCornerInSky, sky.c:47-61:
    // frac = 1 - min(2*tan(elevation), 1), a pure view-angle function) at its
    // own landmarks: r1 = 2h (frac 0, 26.6 deg), r2 = 4h (0.5, 14 deg),
    // r3 = 10h (0.8, 5.7 deg), rim (1.0, pure fog). Four rings because the
    // curve is concave: linear interpolation between sparse samples
    // systematically under-fogs the mid-sky (measured -0.15 with 3 rings).
    f32 wx[33], wz[33];
    f32 tcs[33], tct[33];
    f32 radius[4];
    f32 smin, tmin;
    f32 rsp_y;
    f32 h;
    f32 uoff;
    f32 depth_s = 1.0f;
    // Outer textured band (rim -> fog opacity boundary): ring 0 = attach ring
    // on the rim octagon, ring 1 = r_mid, ring 2 = r_outer. World coords,
    // then converted camera-relative in place once the UVs are taken.
    f32 bx[48], bz[48];
    f32 bs[48], bt[48];
    f32 r_mid;
    f32 r_outer;
    s32 m;
    s32 j;
    coord3d* eye = bondviewGetCurrentPlayersPosition();
    // Embed the vertex and matrix data directly inside the master display
    // list: reserve a gap in the command stream and branch over it. The DL
    // memory is the one region provably shared correctly between the CPU and
    // RT64 (it executes these commands), which removes every address/timing
    // question about external buffers.
    Gfx* bp = gdl++;
    Vtx* verts = (Vtx*) gdl;
    Mtx* mtx_render;
    Mtxf fitmtx;
    SkyRelated18 cols[8];

    // 33 disc + 128 outer-band (32 quads x 4, per-quad UV rebase) + 64 skirt.
    gdl = (Gfx*) ((u8*) gdl + 225 * sizeof(Vtx));
    mtx_render = (Mtx*) gdl;
    gdl = (Gfx*) ((u8*) gdl + sizeof(Mtx));
    gSPBranchList(bp, SKY_K0_TO_PHYS(gdl));

    h = plane_y - eye->y;
    if (h < 0.0f) {
        h = -h;
    }
    if (h < 50.0f) {
        h = 50.0f;
    }
    // All rings carry real UVs (the s16 budget is enforced by
    // SKY_PLANE_RADIUS itself); inner rings chained below r3 so the ring
    // ordering survives the caps.
    radius[2] = 10.0f * h;
    if (radius[2] > SKY_PLANE_RADIUS * 0.8f) {
        radius[2] = SKY_PLANE_RADIUS * 0.8f;
    }
    radius[1] = 4.0f * h;
    if (radius[1] > radius[2] * 0.6f) {
        radius[1] = radius[2] * 0.6f;
    }
    radius[0] = 2.0f * h;
    if (radius[0] > radius[1] * 0.6f) {
        radius[0] = radius[1] * 0.6f;
    }
    radius[3] = SKY_PLANE_RADIUS;
    // Vertex layout: 0 = centre, 1-8 = r1, 9-16 = r2, 17-24 = r3,
    // 25-32 = rim; 33-160 = outer textured band (32 quads x 4, per-quad UV
    // rebase); 161-224 = feather skirt (16 quads x 4). Band and skirt are
    // emitted after the disc loop below.
    wx[0] = eye->x;
    wz[0] = eye->z;
    for (k = 0; k < 4; k++) {
        f32 r = radius[k];
        f32 rd = r * 0.7071f;
        f32* px = &wx[1 + k * 8];
        f32* pz = &wz[1 + k * 8];
        px[0] = eye->x + r;   pz[0] = eye->z;
        px[1] = eye->x + rd;  pz[1] = eye->z + rd;
        px[2] = eye->x;       pz[2] = eye->z + r;
        px[3] = eye->x - rd;  pz[3] = eye->z + rd;
        px[4] = eye->x - r;   pz[4] = eye->z;
        px[5] = eye->x - rd;  pz[5] = eye->z - rd;
        px[6] = eye->x;       pz[6] = eye->z - r;
        px[7] = eye->x + rd;  pz[7] = eye->z - rd;
    }

    // Depth scale, per frame, in MODEL units. This port projects through the
    // ORIGINAL sky transform (camGetWorldToScreenMtxf x projF — the exact
    // matrix chain sub_GAME_7F097388 uses, with its float1/30 scale dance
    // cancelled out), which operates on raw model-unit world positions. The
    // original CPU rasteriser discarded Z so it never met the far plane; on
    // the RSP we shrink the whole geometry radially around the eye instead —
    // angles (the on-screen image) are unchanged. Fit the OUTER BAND edge at
    // 85% of zfar; if the overhead centre would then dip under 1.5x znear,
    // favour the near plane. The band's outer radius then adapts to whatever
    // still fits the depth range, down to just past the rim (fog-heavy
    // levels hide the horizon anyway; a far-clipped band edge there lands on
    // the fog-coloured base fill, which is what that region shows regardless).
    {
        f32 zrange[2];
        f32 near_floor;
        viGetZRange(zrange);
        depth_s = (0.85f * zrange[1]) / SKY_BAND_OUTER;
        if (depth_s > 1.0f) {
            depth_s = 1.0f;
        }
        near_floor = 1.5f * zrange[0];
        if (h * depth_s < near_floor) {
            depth_s = near_floor / h;
        }
        r_outer = (0.85f * zrange[1]) / depth_s;
        if (r_outer > SKY_BAND_OUTER) {
            r_outer = SKY_BAND_OUTER;
        }
        if (r_outer < SKY_PLANE_RADIUS * 1.05f) {
            r_outer = SKY_PLANE_RADIUS * 1.05f;
        }
        r_mid = (SKY_PLANE_RADIUS + r_outer) * 0.5f;
    }

    // Colour per ring from the ORIGINAL fog law frac = 1 - min(2h/r, 1)
    // (equivalent to 1 - min(2 tan(elev), 1); 0 = full sky colour, 1 = pure
    // fog), evaluated at the CAPPED radii so the samples stay on the curve.
    // Every ring including the band's outer edge carries its TRUE law value —
    // the original fades continuously to ~97% at its 300000 ray cap and to
    // frac ~1 exactly at the horizon row (skyIsScreenCornerInSky with y->0).
    // cols[4] = rim/attach ring (~87% on Dam), cols[5] = r_mid,
    // cols[6] = r_outer (~97%), cols[7] = pure fog for the skirt's horizon
    // edge.
    {
        f32 fr[8];
        fr[0] = 0.0f;
        for (k = 0; k < 4; k++) {
            f32 f = 1.0f - (2.0f * h) / radius[k];
            if (f < 0.0f) f = 0.0f;
            fr[k + 1] = f;
        }
        fr[5] = 1.0f - (2.0f * h) / r_mid;
        if (fr[5] < 0.0f) fr[5] = 0.0f;
        fr[6] = 1.0f - (2.0f * h) / r_outer;
        if (fr[6] < 0.0f) fr[6] = 0.0f;
        fr[7] = 1.0f;
        for (k = 0; k < 8; k++) {
            if (isWater) {
                sub_GAME_7F093FA4(&cols[k], fr[k]);
            } else {
                skyChooseCloudVtxColour(&cols[k], fr[k]);
            }
        }
    }

    // World-anchored scrolling UVs, true density on EVERY ring including the
    // rim — the original never freezes or stretches its far texture; the far
    // clouds are hidden purely by the frac->1 colour convergence.
    uoff = isWater ? g_SkyCloudOffset : 0.0f;
    smin = 3.4e38f;
    tmin = 3.4e38f;
    for (i = 0; i < 33; i++) {
        tcs[i] = wx[i] * SKY_TEXEL_PER_WORLD + uoff;
        tct[i] = wz[i] * SKY_TEXEL_PER_WORLD + g_SkyCloudOffset;
        if (tcs[i] < smin) smin = tcs[i];
        if (tct[i] < tmin) tmin = tct[i];
    }
    // Rebase step must be a whole VISUAL period of the tile. 2048 tc units is
    // one 64-texel period only for a wrapped tile; the sky tiles are mirrored,
    // so the visual period is 4096 — an odd multiple of 2048 flips the cloud
    // pattern (observed as "new origin, scrolls the wrong way" in rooms that
    // straddle a rebase boundary). 4096 is safe for both wrap and mirror.
    smin = (f32) (((s32) (smin / 4096.0f)) * 4096);
    tmin = (f32) (((s32) (tmin / 4096.0f)) * 4096);

    // CAMERA-RELATIVE model-unit vertices, disc shrunk radially around the
    // eye. Camera-relative because the RSP's s15.16 matrix cannot hold the
    // true-world view translation (-cam.R reaches ~45000 on Dam — this is
    // the very reason the game room-rebases everything RSP-side); with the
    // eye at the origin the view needs no translation at all. Still no
    // current_model_pos / D_800364CC / field_10E0: zero room coupling.
    rsp_y = (plane_y - eye->y) * depth_s;
    maxc = rsp_y < 0.0f ? -rsp_y : rsp_y;
    for (i = 0; i < 33; i++) {
        f32 cx = (wx[i] - eye->x) * depth_s;
        f32 cz = (wz[i] - eye->z) * depth_s;
        f32 c;
        wx[i] = cx;
        wz[i] = cz;
        c = cx < 0.0f ? -cx : cx;
        if (c > maxc) maxc = c;
        c = cz < 0.0f ? -cz : cz;
        if (c > maxc) maxc = c;
    }

    // Outer band rings, same treatment: world positions -> world-anchored
    // UVs -> camera-relative in place, folded into the fit bound. The attach
    // ring (ring 0) lies exactly ON the rim octagon: even entries are the
    // rim vertices themselves, odd entries the octagon edge midpoints
    // (radius x cos 22.5), so the band meets batch 4 with no cracks; their
    // colour is the rim colour (cols[4]) and their UVs are the linear
    // midpoint of the rim UVs (tc is linear in world position), so shading
    // and texture are seam-free across the joint too.
    for (m = 0; m < 16; m++) {
        f32 attach_r = (m & 1) ? SKY_PLANE_RADIUS * 0.92388f : SKY_PLANE_RADIUS;
        bx[m] = eye->x + attach_r * DIR16[m][0];
        bz[m] = eye->z + attach_r * DIR16[m][1];
        bx[16 + m] = eye->x + r_mid * DIR16[m][0];
        bz[16 + m] = eye->z + r_mid * DIR16[m][1];
        bx[32 + m] = eye->x + r_outer * DIR16[m][0];
        bz[32 + m] = eye->z + r_outer * DIR16[m][1];
    }
    for (m = 0; m < 48; m++) {
        f32 c;
        bs[m] = bx[m] * SKY_TEXEL_PER_WORLD + uoff;
        bt[m] = bz[m] * SKY_TEXEL_PER_WORLD + g_SkyCloudOffset;
        bx[m] = (bx[m] - eye->x) * depth_s;
        bz[m] = (bz[m] - eye->z) * depth_s;
        c = bx[m] < 0.0f ? -bx[m] : bx[m];
        if (c > maxc) maxc = c;
        c = bz[m] < 0.0f ? -bz[m] : bz[m];
        if (c > maxc) maxc = c;
    }

    if (maxc > 32000.0f) {
        fit = 32000.0f / maxc;
    }

    guScaleF(fitmtx.m, 1.0f / fit, 1.0f / fit, 1.0f / fit);

    // MODELVIEW = fit-unscale, then the camera's view ROTATION (translation
    // zeroed — verts are camera-relative; the true-world translation would
    // overflow the s15.16 Mtx anyway). The PROJECTION slot is deliberately
    // left untouched: viSetupCurrentPlayerView already loaded the pure
    // perspective there, and RT64 decomposes/camera-tracks whatever sits in
    // the projection slot (rt64_rsp.cpp isMatrixViewProj/
    // matrixDecomposeViewProj) — embedding view rotation there made the sky
    // view-locked with pitch-zoom artifacts. In the modelview slot RT64
    // treats the rotation as ordinary world placement, like room geometry.
    // Rotation source: camGetWorldToScreenMtxf, the matrix the original sky
    // projects its corners through (sub_GAME_7F097388); translation cells
    // are m[3][0..2] per that same function (sky.c:1377-80).
    {
        Mtxf viewrot;
        Mtxf composed;
        Mtxf* wts = camGetWorldToScreenMtxf();
        f32 conc = fogGetCurrentEnvironmentp()->WaterConcavity;
        for (i = 0; i < 4; i++) {
            for (k = 0; k < 4; k++) {
                viewrot.m[i][k] = wts->m[i][k];
            }
        }
        viewrot.m[3][0] = 0.0f;
        viewrot.m[3][1] = 0.0f;
        viewrot.m[3][2] = 0.0f;
        // Game convention (bondview.c:11552): multiply(A, B, C) => v.C
        // applies B first, then A — so this is "unscale by fit, then rotate".
        matrix_4x4_multiply(&viewrot, &fitmtx, &composed);

        // WaterConcavity: the original biases every sky/water RAY down by
        // `conc` screen pixels (skyGetWorldPosFromScreenPos adds it to the
        // screen y) and shifts every DRAWN vertex up by the same amount
        // (sub_GAME_7F097388: unk2c = y - conc*4) — net, the whole plane
        // system renders lifted `conc` pixels above the geometric horizon,
        // which is what widens the fog band at the horizon on levels like
        // Frigate (Dam has conc = 0, which is why the fan matched there
        // without this). For real 3D geometry the equivalent is a small
        // camera pitch: tan(angle) = conc / (P11 * half screen height in
        // px). Applied in view space AFTER the view rotation; row-vector
        // rotation (y' = y*c - z*s) lifts content ahead (view z < 0) for
        // positive sin, matching the original's upward shift.
        if (conc != 0.0f) {
            Mtxf pitch;
            Mtxf lifted;
            f32 t = conc / (currentPlayerGetProjectionMatrixF()->m[1][1] * (getPlayer_c_screenheight() * 0.5f));
            f32 cs = 1.0f / sqrtf(1.0f + t * t);
            f32 sn = t * cs;
            guScaleF(pitch.m, 1.0f, 1.0f, 1.0f);
            pitch.m[1][1] = cs;
            pitch.m[1][2] = sn;
            pitch.m[2][1] = -sn;
            pitch.m[2][2] = cs;
            matrix_4x4_multiply(&pitch, &composed, &lifted);
            composed = lifted;
        }

        matrix_4x4_f32_to_s32(&composed, (Mtxf*) mtx_render);
    }

    for (i = 0; i < 33; i++) {
        SkyRelated18* col;
        if (i == 0) col = &cols[0];
        else if (i < 9) col = &cols[1];
        else if (i < 17) col = &cols[2];
        else if (i < 25) col = &cols[3];
        else col = &cols[4];
        verts[i].v.ob[0] = wx[i] * fit;
        verts[i].v.ob[1] = rsp_y * fit;
        verts[i].v.ob[2] = wz[i] * fit;
        verts[i].v.tc[0] = skyClamp(tcs[i] - smin, -32768.f, 32767.f);
        verts[i].v.tc[1] = skyClamp(tct[i] - tmin, -32768.f, 32767.f);
#if SKYDBG
        // Colour isolation: bypass the choosers with the env colour already
        // proven correct by the base fill. Expect a uniform sky; the SKYFAN
        // print shows what the choosers returned.
        verts[i].v.cn[0] = fogGetCurrentEnvironmentp()->Red;
        verts[i].v.cn[1] = fogGetCurrentEnvironmentp()->Green;
        verts[i].v.cn[2] = fogGetCurrentEnvironmentp()->Blue;
        verts[i].v.cn[3] = 0xff;
#else
        verts[i].v.cn[0] = col->r;
        verts[i].v.cn[1] = col->g;
        verts[i].v.cn[2] = col->b;
        verts[i].v.cn[3] = col->a;
#endif
    }

    // Outer band verts 33-160: 2 radial sub-bands x 16 sectors, one QUAD per
    // vertex-load (4 verts) so each quad gets its OWN 4096-period UV rebase —
    // this is what lets true-density texture continue past the disc's shared
    // rebase budget, out to r_outer (~97% fog, the original's own textured
    // extent). Adjacent quads differ only by whole visual periods (4096), so
    // the texture is continuous across every quad boundary.
    for (j = 0; j < 2; j++) {
        for (m = 0; m < 16; m++) {
            s32 m2 = (m + 1) & 15;
            s32 q[4];
            f32 qsmin, qtmin;
            Vtx* v = verts + 33 + (j * 16 + m) * 4;
            q[0] = j * 16 + m;
            q[1] = j * 16 + m2;
            q[2] = (j + 1) * 16 + m;
            q[3] = (j + 1) * 16 + m2;
            qsmin = bs[q[0]];
            qtmin = bt[q[0]];
            for (k = 1; k < 4; k++) {
                if (bs[q[k]] < qsmin) qsmin = bs[q[k]];
                if (bt[q[k]] < qtmin) qtmin = bt[q[k]];
            }
            qsmin = (f32) (((s32) (qsmin / 4096.0f)) * 4096);
            qtmin = (f32) (((s32) (qtmin / 4096.0f)) * 4096);
            for (k = 0; k < 4; k++) {
                SkyRelated18* bcol = (k < 2) ? &cols[4 + j] : &cols[5 + j];
                v[k].v.ob[0] = bx[q[k]] * fit;
                v[k].v.ob[1] = rsp_y * fit;
                v[k].v.ob[2] = bz[q[k]] * fit;
                v[k].v.tc[0] = skyClamp(bs[q[k]] - qsmin, -32768.f, 32767.f);
                v[k].v.tc[1] = skyClamp(bt[q[k]] - qtmin, -32768.f, 32767.f);
                v[k].v.cn[0] = bcol->r;
                v[k].v.cn[1] = bcol->g;
                v[k].v.cn[2] = bcol->b;
                v[k].v.cn[3] = bcol->a;
            }
        }
    }

    // Feather skirt verts 161-224 (16 sectors x 4): from the band's outer
    // ring down to EYE level (ob y = 0), which on screen is exactly the
    // plane's horizon line — the anchor the original used (its horizon-edge
    // vertices got frac ~1 from skyIsScreenCornerInSky as ray y -> 0). Top
    // colour = the band edge's true law colour (cols[6], ~97%), bottom =
    // pure fog (cols[7]). Shade-only: at 97% fog the texel term is
    // imperceptible (the original carries no true texture past its cap
    // either — its UVs freeze at the capped positions).
    for (m = 0; m < 16; m++) {
        s32 m2 = (m + 1) & 15;
        Vtx* v = verts + 161 + m * 4;
        for (k = 0; k < 4; k++) {
            s32 src = 32 + ((k & 1) ? m2 : m);
            SkyRelated18* scol = (k < 2) ? &cols[6] : &cols[7];
            v[k].v.ob[0] = bx[src] * fit;
            v[k].v.ob[1] = (k < 2) ? (s16) (rsp_y * fit) : 0;
            v[k].v.ob[2] = bz[src] * fit;
            v[k].v.tc[0] = 0;
            v[k].v.tc[1] = 0;
            v[k].v.cn[0] = scol->r;
            v[k].v.cn[1] = scol->g;
            v[k].v.cn[2] = scol->b;
            v[k].v.cn[3] = scol->a;
        }
    }

    // Clear culling AND G_FOG, exactly like explosion.c:883 — the original
    // sky never uses RSP fog (its fog is the vertex-colour law above), and a
    // leaked G_FOG here would double-fog the disc.
    gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_FOG);

#if SKYDBG
    // Variable isolation: draw the fan untextured (shade only). If the sky
    // shows a clean radial colour fade, geometry/colours are correct and the
    // ray artefacts live entirely in texture sampling (tile/format/persp).
    gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
#endif

    gSPMatrix(gdl++, SKY_K0_TO_PHYS(mtx_render), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

    // Batch 1: centre fan (verts 0-8).
    gSPVertex(gdl++, SKY_K0_TO_PHYS(verts), 9, 0);
    gSP4Triangles(gdl++, 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5);
    gSP4Triangles(gdl++, 0, 5, 6, 0, 6, 7, 0, 7, 8, 0, 8, 1);

    // Batch 2: ring r1 -> r2 (verts 1-16 as slots 0-15: inner k, outer k+8).
    gSPVertex(gdl++, SKY_K0_TO_PHYS(verts + 1), 16, 0);
    for (k = 0; k < 8; k += 2) {
        s32 a = k, b = (k + 1) & 7, a2 = k + 8, b2 = ((k + 1) & 7) + 8;
        s32 c = k + 1, d = (k + 2) & 7, c2 = k + 1 + 8, d2 = ((k + 2) & 7) + 8;
        gSP4Triangles(gdl++, a, b, b2, b2, a2, a, c, d, d2, d2, c2, c);
    }

    // Batch 3: ring r2 -> r3 (verts 9-24 as slots 0-15).
    gSPVertex(gdl++, SKY_K0_TO_PHYS(verts + 9), 16, 0);
    for (k = 0; k < 8; k += 2) {
        s32 a = k, b = (k + 1) & 7, a2 = k + 8, b2 = ((k + 1) & 7) + 8;
        s32 c = k + 1, d = (k + 2) & 7, c2 = k + 1 + 8, d2 = ((k + 2) & 7) + 8;
        gSP4Triangles(gdl++, a, b, b2, b2, a2, a, c, d, d2, d2, c2, c);
    }

    // Batch 4: ring r3 -> rim (verts 17-32 as slots 0-15).
    gSPVertex(gdl++, SKY_K0_TO_PHYS(verts + 17), 16, 0);
    for (k = 0; k < 8; k += 2) {
        s32 a = k, b = (k + 1) & 7, a2 = k + 8, b2 = ((k + 1) & 7) + 8;
        s32 c = k + 1, d = (k + 2) & 7, c2 = k + 1 + 8, d2 = ((k + 2) & 7) + 8;
        gSP4Triangles(gdl++, a, b, b2, b2, a2, a, c, d, d2, d2, c2, c);
    }

    // Batch 5: textured outer band — 32 quads, one vertex load each (their
    // UVs are individually rebased; see the vertex build above).
    for (k = 0; k < 32; k++) {
        gSPVertex(gdl++, SKY_K0_TO_PHYS(verts + 33 + k * 4), 4, 0);
        gSP2Triangles(gdl++, 0, 1, 3, 0, 3, 2, 0, 0);
    }

    // Batch 6: feather skirt, band edge -> horizon, SHADE-ONLY: dropping
    // TEXEL0 is what frees it from the UV budget entirely. The combine leak
    // is within the existing contract — each pass re-establishes its own
    // texture/combine before drawing (water sub_GAME_7F09343C, sky
    // gDPSetCombineLERP), and downstream world rendering sets its own state.
    gDPPipeSync(gdl++);
    gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
    for (k = 0; k < 16; k++) {
        gSPVertex(gdl++, SKY_K0_TO_PHYS(verts + 161 + k * 4), 4, 0);
        gSP2Triangles(gdl++, 0, 1, 3, 0, 3, 2, 0, 0);
    }

#if SKYDBG
    {
        if (g_SkyDbgPrint) {
            recomp_printf("SKYFAN water=%d planey=%d eye=(%d,%d,%d) h=%d rspy=%d fit1e6=%d tc0=%d tc1=%d tc9=%d vp=%x phys=%x w0=%x cn0=%x\n",
                          isWater, (s32) plane_y, (s32) eye->x, (s32) eye->y, (s32) eye->z, (s32) h,
                          (s32) rsp_y, (s32) (fit * 1000000.0f),
                          verts[0].v.tc[0], verts[1].v.tc[0], verts[9].v.tc[0],
                          (u32) verts, SKY_K0_TO_PHYS(verts), *(u32*) verts,
                          *(u32*) &verts[0].v.cn[0]);
            recomp_printf(" cols0=(%d,%d,%d,%d) cols2=(%d,%d,%d,%d) cols3=(%d,%d,%d,%d) env=(%d,%d,%d) cloudRGB=(%d,%d,%d)\n",
                          cols[0].r, cols[0].g, cols[0].b, cols[0].a,
                          cols[2].r, cols[2].g, cols[2].b, cols[2].a,
                          cols[3].r, cols[3].g, cols[3].b, cols[3].a,
                          fogGetCurrentEnvironmentp()->Red, fogGetCurrentEnvironmentp()->Green,
                          fogGetCurrentEnvironmentp()->Blue,
                          (s32) fogGetCurrentEnvironmentp()->CloudRed,
                          (s32) fogGetCurrentEnvironmentp()->CloudGreen,
                          (s32) fogGetCurrentEnvironmentp()->CloudBlue);
        }
    }
#endif

    // Projection slot was never touched (pure perspective still loaded);
    // only restore the back-face culling the texture setup helpers enabled.
    gSPSetGeometryMode(gdl++, G_CULL_BACK);

    return gdl;
}
#endif

// @recomp: paint the letterbox rows (the rows outside the 220-line view) with
// the sky colour instead of the black frame clear. Thin strips only, on
// purpose: a single full-frame fill covers the whole colour target and RT64
// classifies that as a whole-render-target clear (rt64_framebuffer_renderer
// FillRect path) — runtime-tested 2026-07-11, it wiped later draws (models,
// portal rooms, menu backgrounds, fades). Partial rects take the same
// scissored-clear path the view-rect base coat has always used. LEFT/RIGHT
// rect-align origins stretch each strip to the extended widescreen edges.
// Caller must already have FILL cycle + the sky fill colour set. Single
// player only: in split-screen these rows belong to the other players.
static Gfx* skyFillLetterboxBars(Gfx* gdl, s32 viewtop, s32 viewbottom) {
    if (getPlayerCount() != 1) {
        return gdl;
    }
    gEXSetRectAlign(gdl++, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, 0);
    if (viewtop > 0) {
        gDPFillRectangle(gdl++, 0, 0, 0, viewtop - 1);
    }
    if (viewbottom < 240) {
        gDPFillRectangle(gdl++, 0, viewbottom, 0, 239);
    }
    gEXSetRectAlign(gdl++, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
    return gdl;
}

#if 1
RECOMP_PATCH Gfx* skyRender(Gfx* gdl) __attribute__((optnone)) {
    coord3d sp6a4;
    coord3d sp698;
    coord3d sp68c;
    coord3d sp680;
    coord3d sp674;
    coord3d sp668;
    coord3d sp65c;
    coord3d sp650;
    coord3d sp644;
    coord3d sp638;
    coord3d sp62c;
    coord3d sp620;
    coord3d sp614;
    coord3d sp608;
    coord3d sp5fc;
    coord3d sp5f0;
    coord3d sp5e4;
    coord3d sp5d8;
    coord3d sp5cc;
    coord3d sp5c0;
    coord3d sp5b4;
    coord3d sp5a8;
    coord3d sp59c;
    coord3d sp590;
    f32 sp58c;
    f32 sp588;
    f32 sp584;
    f32 sp580;
    f32 sp57c;
    f32 sp578;
    f32 sp574;
    f32 sp570;
    f32 sp56c;
    f32 sp568;
    f32 sp564;
    f32 sp560;
    f32 sp55c;
    f32 sp558;
    f32 sp554;
    f32 sp550;
    f32 sp54c;
    f32 sp548;
    s32 s1;
    s32 j;
    s32 k;
    s32 sp538;
    s32 sp534;
    s32 sp530;
    s32 sp52c;
    SkyRelated18 sp4b4[5];
    SkyRelated18 sp43c[5];
    f32 tmp;
    f32 scale;
    bool sp430;
    struct CurrentEnvironmentRecord* env;

    scale = get_room_data_float1() / 30.0f;
    sp430 = FALSE;
    env = fogGetCurrentEnvironmentp();

    // @recomp TEMP telemetry (2026-07-11, remove after the Frigate fog
    // session): one line/sec of the live environment + depth state, to nail
    // the cloud-whiteness / horizon-fade comparison against gopher64.
    {
        static u32 envdbg_frame = 0;
        if ((envdbg_frame++ & 63) == 0) {
            f32 zr[2];
            viGetZRange(zr);
            recomp_printf("SKYENV rgb=(%d,%d,%d) clouds=%d cloudRGB=(%d,%d,%d) crept=%d water=%d waterRGB=(%d,%d,%d) wrept=%d conc=%d farI=%d difI=%d znear=%d zfar=%d\n",
                          env->Red, env->Green, env->Blue, env->Clouds,
                          (s32) env->CloudRed, (s32) env->CloudGreen, (s32) env->CloudBlue,
                          (s32) env->CloudRepeat, env->IsWater,
                          (s32) env->WaterRed, (s32) env->WaterGreen, (s32) env->WaterBlue,
                          (s32) env->WaterRepeat, (s32) env->WaterConcavity,
                          env->FarIntensity, env->DifferenceFromFarIntensity,
                          (s32) zr[0], (s32) zr[1]);
        }
    }

#if SKYDBG
    g_SkyDbgPrint = (g_SkyDbgFrame++ & 63) == 0;
#endif

    if (!fogGetCurrentEnvironmentp()->Clouds) {
        if (getPlayerCount() == 1) {
            gDPSetCycleType(gdl++, G_CYC_FILL);

            gdl = viSetFillColor(gdl, env->Red, env->Green, env->Blue);

            gDPFillRectangle(gdl++, viGetViewLeft(), viGetViewTop(), viGetViewLeft() + viGetViewWidth() - 1,
                             viGetViewTop() + viGetViewHeight() - 1);

            // @recomp: sky-colour the letterbox rows too (see helper).
            gdl = skyFillLetterboxBars(gdl, viGetViewTop(), viGetViewTop() + viGetViewHeight());

            gDPPipeSync(gdl++);
            return gdl;
        }

        gDPPipeSync(gdl++);
        gDPSetCycleType(gdl++, G_CYC_FILL);

        gDPSetRenderMode(gdl++, G_RM_NOOP, G_RM_NOOP2);

        gDPFillRectangle(gdl++, g_CurrentPlayer->viewleft, g_CurrentPlayer->viewtop,
                         (g_CurrentPlayer->viewleft + g_CurrentPlayer->viewx) - 1,
                         (g_CurrentPlayer->viewtop + g_CurrentPlayer->viewy) - 1);

        gDPPipeSync(gdl++);
        return gdl;
    }

    gdl = viSetFillColor(gdl, env->Red, env->Green, env->Blue);

#if PORTSKY
    // Base coat: fill the view with the fog colour first. GE never clears the
    // framebuffer here, and the horizon gap between the far-clipped sky/water
    // planes and infinity needs covering; fog colour is exactly what belongs
    // there.
    gDPPipeSync(gdl++);
    gDPSetCycleType(gdl++, G_CYC_FILL);
    gDPFillRectangle(gdl++, g_CurrentPlayer->viewleft, g_CurrentPlayer->viewtop,
                     (g_CurrentPlayer->viewleft + g_CurrentPlayer->viewx) - 1,
                     (g_CurrentPlayer->viewtop + g_CurrentPlayer->viewy) - 1);

    // @recomp: sky-colour the letterbox rows too (see helper).
    gdl = skyFillLetterboxBars(gdl, g_CurrentPlayer->viewtop, g_CurrentPlayer->viewtop + g_CurrentPlayer->viewy);

    gDPPipeSync(gdl++);
#endif

    if (&sp6a4)
        ;

    sub_GAME_7F093880(0.0f, 0.0f, &sp6a4);
    sub_GAME_7F093880(getPlayer_c_screenwidth() - 0.1f, 0.0f, &sp698);
    sub_GAME_7F093880(0.0f, getPlayer_c_screenheight() - 0.1f, &sp68c);
    sub_GAME_7F093880(getPlayer_c_screenwidth() - 0.1f, getPlayer_c_screenheight() - 0.1f, &sp680);

    sp538 = sub_GAME_7F0938FC(&sp6a4, &sp644, &sp58c);
    sp534 = sub_GAME_7F0938FC(&sp698, &sp638, &sp588);
    sp530 = sub_GAME_7F0938FC(&sp68c, &sp62c, &sp584);
    sp52c = sub_GAME_7F0938FC(&sp680, &sp620, &sp580);

    sub_GAME_7F093A78(&sp6a4, &sp5e4, &sp56c);
    sub_GAME_7F093A78(&sp698, &sp5d8, &sp568);
    sub_GAME_7F093A78(&sp68c, &sp5cc, &sp564);
    sub_GAME_7F093A78(&sp680, &sp5c0, &sp560);

    if (sp538 != sp530) {
        sp54c = getPlayer_c_screentop() + getPlayer_c_screenheight() * (sp6a4.f[1] / (sp6a4.f[1] - sp68c.f[1]));

        sub_GAME_7F093880(0.0f, sp54c, &sp65c);
        sub_GAME_7F093BFC(&sp6a4, &sp68c, &sp65c);
        sub_GAME_7F0938FC(&sp65c, &sp5fc, &sp574);
        sub_GAME_7F093A78(&sp65c, &sp59c, &sp554);
    } else {
        sp54c = 0.0f;
    }

    if (sp534 != sp52c) {
        sp548 = getPlayer_c_screentop() + getPlayer_c_screenheight() * (sp698.f[1] / (sp698.f[1] - sp680.f[1]));

        sub_GAME_7F093880(getPlayer_c_screenwidth() - 0.1f, sp548, &sp650);
        sub_GAME_7F093BFC(&sp698, &sp680, &sp650);
        sub_GAME_7F0938FC(&sp650, &sp5f0, &sp570);
        sub_GAME_7F093A78(&sp650, &sp590, &sp550);
    } else {
        sp548 = 0.0f;
    }

    if (sp538 != sp534) {
        sub_GAME_7F093880(getPlayer_c_screenleft() +
                              getPlayer_c_screenwidth() * (sp6a4.f[1] / (sp6a4.f[1] - sp698.f[1])),
                          0.0f, &sp674);
        sub_GAME_7F093BFC(&sp6a4, &sp698, &sp674);
        sub_GAME_7F0938FC(&sp674, &sp614, &sp57c);
        sub_GAME_7F093A78(&sp674, &sp5b4, &sp55c);
    }

    if (sp530 != sp52c) {
        tmp = getPlayer_c_screenleft() + getPlayer_c_screenwidth() * (sp68c.f[1] / (sp68c.f[1] - sp680.f[1]));

        sub_GAME_7F093880(tmp, getPlayer_c_screenheight() - 0.1f, &sp668);
        sub_GAME_7F093BFC(&sp68c, &sp680, &sp668);
        sub_GAME_7F0938FC(&sp668, &sp608, &sp578);
        sub_GAME_7F093A78(&sp668, &sp5a8, &sp558);
    }

    switch ((sp538 << 3) | (sp534 << 2) | (sp530 << 1) | sp52c) {
        case 15:
            s1 = 0;
            break;

        case 0:
            s1 = 4;
            sp43c[0].unk00 = sp5e4.f[0] * scale;
            sp43c[0].unk04 = sp5e4.f[1] * scale;
            sp43c[0].unk08 = sp5e4.f[2] * scale;
            sp43c[1].unk00 = sp5d8.f[0] * scale;
            sp43c[1].unk04 = sp5d8.f[1] * scale;
            sp43c[1].unk08 = sp5d8.f[2] * scale;
            sp43c[2].unk00 = sp5cc.f[0] * scale;
            sp43c[2].unk04 = sp5cc.f[1] * scale;
            sp43c[2].unk08 = sp5cc.f[2] * scale;
            sp43c[3].unk00 = sp5c0.f[0] * scale;
            sp43c[3].unk04 = sp5c0.f[1] * scale;
            sp43c[3].unk08 = sp5c0.f[2] * scale;
            sp43c[0].unk0c = sp5e4.f[0];
            sp43c[0].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5d8.f[0];
            sp43c[1].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5cc.f[0];
            sp43c[2].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp5c0.f[0];
            sp43c[3].unk10 = sp5c0.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp56c);
            sub_GAME_7F093FA4(&sp43c[1], sp568);
            sub_GAME_7F093FA4(&sp43c[2], sp564);
            sub_GAME_7F093FA4(&sp43c[3], sp560);
            break;

        case 3:
            s1 = 4;
            sp43c[0].unk00 = sp5e4.f[0] * scale;
            sp43c[0].unk04 = sp5e4.f[1] * scale;
            sp43c[0].unk08 = sp5e4.f[2] * scale;
            sp43c[1].unk00 = sp5d8.f[0] * scale;
            sp43c[1].unk04 = sp5d8.f[1] * scale;
            sp43c[1].unk08 = sp5d8.f[2] * scale;
            sp43c[2].unk00 = sp59c.f[0] * scale;
            sp43c[2].unk04 = sp59c.f[1] * scale;
            sp43c[2].unk08 = sp59c.f[2] * scale;
            sp43c[3].unk00 = sp590.f[0] * scale;
            sp43c[3].unk04 = sp590.f[1] * scale;
            sp43c[3].unk08 = sp590.f[2] * scale;
            sp43c[0].unk0c = sp5e4.f[0];
            sp43c[0].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5d8.f[0];
            sp43c[1].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp59c.f[0];
            sp43c[2].unk10 = sp59c.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp590.f[0];
            sp43c[3].unk10 = sp590.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp56c);
            sub_GAME_7F093FA4(&sp43c[1], sp568);
            sub_GAME_7F093FA4(&sp43c[2], sp554);
            sub_GAME_7F093FA4(&sp43c[3], sp550);
            break;

        case 12:
            s1 = 4;
            sp430 = TRUE;
            sp43c[0].unk00 = sp5c0.f[0] * scale;
            sp43c[0].unk04 = sp5c0.f[1] * scale;
            sp43c[0].unk08 = sp5c0.f[2] * scale;
            sp43c[1].unk00 = sp5cc.f[0] * scale;
            sp43c[1].unk04 = sp5cc.f[1] * scale;
            sp43c[1].unk08 = sp5cc.f[2] * scale;
            sp43c[2].unk00 = sp590.f[0] * scale;
            sp43c[2].unk04 = sp590.f[1] * scale;
            sp43c[2].unk08 = sp590.f[2] * scale;
            sp43c[3].unk00 = sp59c.f[0] * scale;
            sp43c[3].unk04 = sp59c.f[1] * scale;
            sp43c[3].unk08 = sp59c.f[2] * scale;
            sp43c[0].unk0c = sp5c0.f[0];
            sp43c[0].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5cc.f[0];
            sp43c[1].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp590.f[0];
            sp43c[2].unk10 = sp590.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp59c.f[0];
            sp43c[3].unk10 = sp59c.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp560);
            sub_GAME_7F093FA4(&sp43c[1], sp564);
            sub_GAME_7F093FA4(&sp43c[2], sp550);
            sub_GAME_7F093FA4(&sp43c[3], sp554);
            break;

        case 10:
            s1 = 4;
            sp43c[0].unk00 = sp5d8.f[0] * scale;
            sp43c[0].unk04 = sp5d8.f[1] * scale;
            sp43c[0].unk08 = sp5d8.f[2] * scale;
            sp43c[1].unk00 = sp5c0.f[0] * scale;
            sp43c[1].unk04 = sp5c0.f[1] * scale;
            sp43c[1].unk08 = sp5c0.f[2] * scale;
            sp43c[2].unk00 = sp5b4.f[0] * scale;
            sp43c[2].unk04 = sp5b4.f[1] * scale;
            sp43c[2].unk08 = sp5b4.f[2] * scale;
            sp43c[3].unk00 = sp5a8.f[0] * scale;
            sp43c[3].unk04 = sp5a8.f[1] * scale;
            sp43c[3].unk08 = sp5a8.f[2] * scale;
            sp43c[0].unk0c = sp5d8.f[0];
            sp43c[0].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5c0.f[0];
            sp43c[1].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5b4.f[0];
            sp43c[2].unk10 = sp5b4.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp5a8.f[0];
            sp43c[3].unk10 = sp5a8.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp568);
            sub_GAME_7F093FA4(&sp43c[1], sp560);
            sub_GAME_7F093FA4(&sp43c[2], sp55c);
            sub_GAME_7F093FA4(&sp43c[3], sp558);
            break;

        case 5:
            s1 = 4;
            sp43c[0].unk00 = sp5cc.f[0] * scale;
            sp43c[0].unk04 = sp5cc.f[1] * scale;
            sp43c[0].unk08 = sp5cc.f[2] * scale;
            sp43c[1].unk00 = sp5e4.f[0] * scale;
            sp43c[1].unk04 = sp5e4.f[1] * scale;
            sp43c[1].unk08 = sp5e4.f[2] * scale;
            sp43c[2].unk00 = sp5a8.f[0] * scale;
            sp43c[2].unk04 = sp5a8.f[1] * scale;
            sp43c[2].unk08 = sp5a8.f[2] * scale;
            sp43c[3].unk00 = sp5b4.f[0] * scale;
            sp43c[3].unk04 = sp5b4.f[1] * scale;
            sp43c[3].unk08 = sp5b4.f[2] * scale;
            sp43c[0].unk0c = sp5cc.f[0];
            sp43c[0].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5e4.f[0];
            sp43c[1].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5a8.f[0];
            sp43c[2].unk10 = sp5a8.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp5b4.f[0];
            sp43c[3].unk10 = sp5b4.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp564);
            sub_GAME_7F093FA4(&sp43c[1], sp56c);
            sub_GAME_7F093FA4(&sp43c[2], sp558);
            sub_GAME_7F093FA4(&sp43c[3], sp55c);
            break;

        case 14:
            s1 = 3;
            sp43c[0].unk00 = sp5c0.f[0] * scale;
            sp43c[0].unk04 = sp5c0.f[1] * scale;
            sp43c[0].unk08 = sp5c0.f[2] * scale;
            sp43c[1].unk00 = sp5a8.f[0] * scale;
            sp43c[1].unk04 = sp5a8.f[1] * scale;
            sp43c[1].unk08 = sp5a8.f[2] * scale;
            sp43c[2].unk00 = sp590.f[0] * scale;
            sp43c[2].unk04 = sp590.f[1] * scale;
            sp43c[2].unk08 = sp590.f[2] * scale;
            sp43c[0].unk0c = sp5c0.f[0];
            sp43c[0].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5a8.f[0];
            sp43c[1].unk10 = sp5a8.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp590.f[0];
            sp43c[2].unk10 = sp590.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp560);
            sub_GAME_7F093FA4(&sp43c[1], sp558);
            sub_GAME_7F093FA4(&sp43c[2], sp550);
            break;

        case 13:
            s1 = 3;
            sp43c[0].unk00 = sp5cc.f[0] * scale;
            sp43c[0].unk04 = sp5cc.f[1] * scale;
            sp43c[0].unk08 = sp5cc.f[2] * scale;
            sp43c[1].unk00 = sp59c.f[0] * scale;
            sp43c[1].unk04 = sp59c.f[1] * scale;
            sp43c[1].unk08 = sp59c.f[2] * scale;
            sp43c[2].unk00 = sp5a8.f[0] * scale;
            sp43c[2].unk04 = sp5a8.f[1] * scale;
            sp43c[2].unk08 = sp5a8.f[2] * scale;
            sp43c[0].unk0c = sp5cc.f[0];
            sp43c[0].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp59c.f[0];
            sp43c[1].unk10 = sp59c.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5a8.f[0];
            sp43c[2].unk10 = sp5a8.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp564);
            sub_GAME_7F093FA4(&sp43c[1], sp554);
            sub_GAME_7F093FA4(&sp43c[2], sp558);
            break;

        case 11:
            s1 = 3;
            sp43c[0].unk00 = sp5d8.f[0] * scale;
            sp43c[0].unk04 = sp5d8.f[1] * scale;
            sp43c[0].unk08 = sp5d8.f[2] * scale;
            sp43c[1].unk00 = sp590.f[0] * scale;
            sp43c[1].unk04 = sp590.f[1] * scale;
            sp43c[1].unk08 = sp590.f[2] * scale;
            sp43c[2].unk00 = sp5b4.f[0] * scale;
            sp43c[2].unk04 = sp5b4.f[1] * scale;
            sp43c[2].unk08 = sp5b4.f[2] * scale;
            sp43c[0].unk0c = sp5d8.f[0];
            sp43c[0].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp590.f[0];
            sp43c[1].unk10 = sp590.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5b4.f[0];
            sp43c[2].unk10 = sp5b4.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp568);
            sub_GAME_7F093FA4(&sp43c[1], sp550);
            sub_GAME_7F093FA4(&sp43c[2], sp55c);
            break;

        case 7:
            s1 = 3;
            sp43c[0].unk00 = sp5e4.f[0] * scale;
            sp43c[0].unk04 = sp5e4.f[1] * scale;
            sp43c[0].unk08 = sp5e4.f[2] * scale;
            sp43c[1].unk00 = sp5b4.f[0] * scale;
            sp43c[1].unk04 = sp5b4.f[1] * scale;
            sp43c[1].unk08 = sp5b4.f[2] * scale;
            sp43c[2].unk00 = sp59c.f[0] * scale;
            sp43c[2].unk04 = sp59c.f[1] * scale;
            sp43c[2].unk08 = sp59c.f[2] * scale;
            sp43c[0].unk0c = sp5e4.f[0];
            sp43c[0].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5b4.f[0];
            sp43c[1].unk10 = sp5b4.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp59c.f[0];
            sp43c[2].unk10 = sp59c.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp56c);
            sub_GAME_7F093FA4(&sp43c[1], sp55c);
            sub_GAME_7F093FA4(&sp43c[2], sp554);
            break;

        case 1:
            s1 = 5;
            sp43c[0].unk00 = sp5cc.f[0] * scale;
            sp43c[0].unk04 = sp5cc.f[1] * scale;
            sp43c[0].unk08 = sp5cc.f[2] * scale;
            sp43c[1].unk00 = sp5e4.f[0] * scale;
            sp43c[1].unk04 = sp5e4.f[1] * scale;
            sp43c[1].unk08 = sp5e4.f[2] * scale;
            sp43c[2].unk00 = sp5d8.f[0] * scale;
            sp43c[2].unk04 = sp5d8.f[1] * scale;
            sp43c[2].unk08 = sp5d8.f[2] * scale;
            sp43c[3].unk00 = sp590.f[0] * scale;
            sp43c[3].unk04 = sp590.f[1] * scale;
            sp43c[3].unk08 = sp590.f[2] * scale;
            sp43c[4].unk00 = sp5a8.f[0] * scale;
            sp43c[4].unk04 = sp5a8.f[1] * scale;
            sp43c[4].unk08 = sp5a8.f[2] * scale;
            sp43c[0].unk0c = sp5cc.f[0];
            sp43c[0].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5e4.f[0];
            sp43c[1].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5d8.f[0];
            sp43c[2].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp590.f[0];
            sp43c[3].unk10 = sp590.f[2] + g_SkyCloudOffset;
            sp43c[4].unk0c = sp5a8.f[0];
            sp43c[4].unk10 = sp5a8.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp564);
            sub_GAME_7F093FA4(&sp43c[1], sp56c);
            sub_GAME_7F093FA4(&sp43c[2], sp568);
            sub_GAME_7F093FA4(&sp43c[3], sp550);
            sub_GAME_7F093FA4(&sp43c[4], sp558);
            break;

        case 2:
            s1 = 5;
            sp43c[0].unk00 = sp5e4.f[0] * scale;
            sp43c[0].unk04 = sp5e4.f[1] * scale;
            sp43c[0].unk08 = sp5e4.f[2] * scale;
            sp43c[1].unk00 = sp5d8.f[0] * scale;
            sp43c[1].unk04 = sp5d8.f[1] * scale;
            sp43c[1].unk08 = sp5d8.f[2] * scale;
            sp43c[2].unk00 = sp5c0.f[0] * scale;
            sp43c[2].unk04 = sp5c0.f[1] * scale;
            sp43c[2].unk08 = sp5c0.f[2] * scale;
            sp43c[3].unk00 = sp5a8.f[0] * scale;
            sp43c[3].unk04 = sp5a8.f[1] * scale;
            sp43c[3].unk08 = sp5a8.f[2] * scale;
            sp43c[4].unk00 = sp59c.f[0] * scale;
            sp43c[4].unk04 = sp59c.f[1] * scale;
            sp43c[4].unk08 = sp59c.f[2] * scale;
            sp43c[0].unk0c = sp5e4.f[0];
            sp43c[0].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5d8.f[0];
            sp43c[1].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5c0.f[0];
            sp43c[2].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp5a8.f[0];
            sp43c[3].unk10 = sp5a8.f[2] + g_SkyCloudOffset;
            sp43c[4].unk0c = sp59c.f[0];
            sp43c[4].unk10 = sp59c.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp56c);
            sub_GAME_7F093FA4(&sp43c[1], sp568);
            sub_GAME_7F093FA4(&sp43c[2], sp560);
            sub_GAME_7F093FA4(&sp43c[3], sp558);
            sub_GAME_7F093FA4(&sp43c[4], sp554);
            break;

        case 4:
            s1 = 5;
            sp43c[0].unk00 = sp5c0.f[0] * scale;
            sp43c[0].unk04 = sp5c0.f[1] * scale;
            sp43c[0].unk08 = sp5c0.f[2] * scale;
            sp43c[1].unk00 = sp5cc.f[0] * scale;
            sp43c[1].unk04 = sp5cc.f[1] * scale;
            sp43c[1].unk08 = sp5cc.f[2] * scale;
            sp43c[2].unk00 = sp5e4.f[0] * scale;
            sp43c[2].unk04 = sp5e4.f[1] * scale;
            sp43c[2].unk08 = sp5e4.f[2] * scale;
            sp43c[3].unk00 = sp5b4.f[0] * scale;
            sp43c[3].unk04 = sp5b4.f[1] * scale;
            sp43c[3].unk08 = sp5b4.f[2] * scale;
            sp43c[4].unk00 = sp590.f[0] * scale;
            sp43c[4].unk04 = sp590.f[1] * scale;
            sp43c[4].unk08 = sp590.f[2] * scale;
            sp43c[0].unk0c = sp5c0.f[0];
            sp43c[0].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5cc.f[0];
            sp43c[1].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5e4.f[0];
            sp43c[2].unk10 = sp5e4.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp5b4.f[0];
            sp43c[3].unk10 = sp5b4.f[2] + g_SkyCloudOffset;
            sp43c[4].unk0c = sp590.f[0];
            sp43c[4].unk10 = sp590.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp560);
            sub_GAME_7F093FA4(&sp43c[1], sp564);
            sub_GAME_7F093FA4(&sp43c[2], sp56c);
            sub_GAME_7F093FA4(&sp43c[3], sp55c);
            sub_GAME_7F093FA4(&sp43c[4], sp550);
            break;

        case 8:
            s1 = 5;
            sp43c[0].unk00 = sp5d8.f[0] * scale;
            sp43c[0].unk04 = sp5d8.f[1] * scale;
            sp43c[0].unk08 = sp5d8.f[2] * scale;
            sp43c[1].unk00 = sp5c0.f[0] * scale;
            sp43c[1].unk04 = sp5c0.f[1] * scale;
            sp43c[1].unk08 = sp5c0.f[2] * scale;
            sp43c[2].unk00 = sp5cc.f[0] * scale;
            sp43c[2].unk04 = sp5cc.f[1] * scale;
            sp43c[2].unk08 = sp5cc.f[2] * scale;
            sp43c[3].unk00 = sp59c.f[0] * scale;
            sp43c[3].unk04 = sp59c.f[1] * scale;
            sp43c[3].unk08 = sp59c.f[2] * scale;
            sp43c[4].unk00 = sp5b4.f[0] * scale;
            sp43c[4].unk04 = sp5b4.f[1] * scale;
            sp43c[4].unk08 = sp5b4.f[2] * scale;
            sp43c[0].unk0c = sp5d8.f[0];
            sp43c[0].unk10 = sp5d8.f[2] + g_SkyCloudOffset;
            sp43c[1].unk0c = sp5c0.f[0];
            sp43c[1].unk10 = sp5c0.f[2] + g_SkyCloudOffset;
            sp43c[2].unk0c = sp5cc.f[0];
            sp43c[2].unk10 = sp5cc.f[2] + g_SkyCloudOffset;
            sp43c[3].unk0c = sp59c.f[0];
            sp43c[3].unk10 = sp59c.f[2] + g_SkyCloudOffset;
            sp43c[4].unk0c = sp5b4.f[0];
            sp43c[4].unk10 = sp5b4.f[2] + g_SkyCloudOffset;

            sub_GAME_7F093FA4(&sp43c[0], sp568);
            sub_GAME_7F093FA4(&sp43c[1], sp560);
            sub_GAME_7F093FA4(&sp43c[2], sp564);
            sub_GAME_7F093FA4(&sp43c[3], sp554);
            sub_GAME_7F093FA4(&sp43c[4], sp55c);
            break;

        default:
            return gdl;
    }

    if (s1 > 0) {
        Mtxf sp3cc;
        Mtxf sp38c;
        SkyRelated38 sp274[5];
        s32 i;
        s32 unused[3];

        matrix_4x4_multiply(currentPlayerGetProjectionMatrixF(), camGetWorldToScreenMtxf(), &sp3cc);
        guScaleF(dword_CODE_bss_80079E98.m, 1.0f / scale, 1.0f / scale, 1.0f / scale);
        matrix_4x4_multiply(&sp3cc, &dword_CODE_bss_80079E98, &sp38c);

        for (i = 0; i < s1; i++) {
            sub_GAME_7F097388(&sp43c[i], &sp38c, 130, 65535.0f, 65535.0f, &sp274[i]);

            sp274[i].unk28 = skyClamp(sp274[i].unk28, getPlayer_c_screenleft() * 4.0f,
                                      (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f);
            sp274[i].unk2c = skyClamp(sp274[i].unk2c, getPlayer_c_screentop() * 4.0f,
                                      (getPlayer_c_screentop() + getPlayer_c_screenheight()) * 4.0f - 1.0f);

            if (sp274[i].unk2c > getPlayer_c_screentop() * 4.0f + 4.0f &&
                sp274[i].unk2c < (getPlayer_c_screentop() + getPlayer_c_screenheight()) * 4.0f - 4.0f) {
                sp274[i].unk2c -= 4.0f;
            }
        }

        if (!fogGetCurrentEnvironmentp()->IsWater) {
            f32 f14 = 1279.0f;
            f32 f2 = 0.0f;
            f32 f16 = 959.0f;
            f32 f12 = 0.0f;

            for (j = 0; j < s1; j++) {
                if (sp274[j].unk28 < f14) {
                    f14 = sp274[j].unk28;
                }
                if (sp274[j].unk28 > f2) {
                    f2 = sp274[j].unk28;
                }

                if (sp274[j].unk2c < f16) {
                    f16 = sp274[j].unk2c;
                }
                if (sp274[j].unk2c > f12) {
                    f12 = sp274[j].unk2c;
                }
            }

            gDPPipeSync(gdl++);
            gDPSetCycleType(gdl++, G_CYC_FILL);
            gDPSetRenderMode(gdl++, G_RM_NOOP, G_RM_NOOP2);
            gDPSetTexturePersp(gdl++, G_TP_NONE);
            gDPFillRectangle(gdl++, (s32) (f14 * 0.25f), (s32) (f16 * 0.25f), (s32) (f2 * 0.25f), (s32) (f12 * 0.25f));
            gDPPipeSync(gdl++);
            gDPSetTexturePersp(gdl++, G_TP_PERSP);
        } else {
            gDPPipeSync(gdl++);

            texSelect(&gdl, &skywaterimages[fogGetCurrentEnvironmentp()->WaterImageId], 1, 0, 2);
            gdl = sub_GAME_7F09343C(gdl, 0); // ???
            gDPSetRenderMode(gdl++, G_RM_OPA_SURF, G_RM_OPA_SURF2);

#if PORTSKY == 0
            if (s1 == 4) {
                gdl = sub_GAME_7F097818(gdl, &sp274[0], &sp274[1], &sp274[3], 130.0f, TRUE);

                if (sp430) {
                    sp274[0].unk2c++;
                    sp274[1].unk2c++;
                    sp274[2].unk2c++;
                    sp274[3].unk2c++;
                }

                gdl = sub_GAME_7F097818(gdl, &sp274[3], &sp274[2], &sp274[0], 130.0f, TRUE);
            } else if (s1 == 5) {
                gdl = sub_GAME_7F097818(gdl, &sp274[0], &sp274[1], &sp274[2], 130.0f, TRUE);
                gdl = sub_GAME_7F097818(gdl, &sp274[0], &sp274[2], &sp274[3], 130.0f, TRUE);
                gdl = sub_GAME_7F097818(gdl, &sp274[0], &sp274[3], &sp274[4], 130.0f, TRUE);
            } else if (s1 == 3) {
                gdl = sub_GAME_7F097818(gdl, &sp274[0], &sp274[1], &sp274[2], 130.0f, TRUE);
            }
#else
            // Camera-independent water plane fan (see skyDrawPlaneFan).
            gdl = skyDrawPlaneFan(gdl, fogGetCurrentEnvironmentp()->WaterRepeat, TRUE);
#endif
        }
    }

    switch ((sp538 << 3) | (sp534 << 2) | (sp530 << 1) | sp52c) {
        case 0:
            return gdl;

        case 15:
            s1 = 4;
            sp4b4[0].unk00 = sp644.f[0] * scale;
            sp4b4[0].unk04 = sp644.f[1] * scale;
            sp4b4[0].unk08 = sp644.f[2] * scale;
            sp4b4[1].unk00 = sp638.f[0] * scale;
            sp4b4[1].unk04 = sp638.f[1] * scale;
            sp4b4[1].unk08 = sp638.f[2] * scale;
            sp4b4[2].unk00 = sp62c.f[0] * scale;
            sp4b4[2].unk04 = sp62c.f[1] * scale;
            sp4b4[2].unk08 = sp62c.f[2] * scale;
            sp4b4[3].unk00 = sp620.f[0] * scale;
            sp4b4[3].unk04 = sp620.f[1] * scale;
            sp4b4[3].unk08 = sp620.f[2] * scale;
            sp4b4[0].unk0c = sp644.f[0] * 0.1f;
            sp4b4[0].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp638.f[0] * 0.1f;
            sp4b4[1].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[2].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp620.f[0] * 0.1f;
            sp4b4[3].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp58c);
            skyChooseCloudVtxColour(&sp4b4[1], sp588);
            skyChooseCloudVtxColour(&sp4b4[2], sp584);
            skyChooseCloudVtxColour(&sp4b4[3], sp580);
            break;

        case 12:
            s1 = 4;
            sp4b4[0].unk00 = sp644.f[0] * scale;
            sp4b4[0].unk04 = sp644.f[1] * scale;
            sp4b4[0].unk08 = sp644.f[2] * scale;
            sp4b4[1].unk00 = sp638.f[0] * scale;
            sp4b4[1].unk04 = sp638.f[1] * scale;
            sp4b4[1].unk08 = sp638.f[2] * scale;
            sp4b4[2].unk00 = sp5fc.f[0] * scale;
            sp4b4[2].unk04 = sp5fc.f[1] * scale;
            sp4b4[2].unk08 = sp5fc.f[2] * scale;
            sp4b4[3].unk00 = sp5f0.f[0] * scale;
            sp4b4[3].unk04 = sp5f0.f[1] * scale;
            sp4b4[3].unk08 = sp5f0.f[2] * scale;
            sp4b4[0].unk0c = sp644.f[0] * 0.1f;
            sp4b4[0].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp638.f[0] * 0.1f;
            sp4b4[1].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[2].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[3].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp58c);
            skyChooseCloudVtxColour(&sp4b4[1], sp588);
            skyChooseCloudVtxColour(&sp4b4[2], sp574);
            skyChooseCloudVtxColour(&sp4b4[3], sp570);
            break;

        case 3:
            s1 = 4;
            sp4b4[0].unk00 = sp620.f[0] * scale;
            sp4b4[0].unk04 = sp620.f[1] * scale;
            sp4b4[0].unk08 = sp620.f[2] * scale;
            sp4b4[1].unk00 = sp62c.f[0] * scale;
            sp4b4[1].unk04 = sp62c.f[1] * scale;
            sp4b4[1].unk08 = sp62c.f[2] * scale;
            sp4b4[2].unk00 = sp5f0.f[0] * scale;
            sp4b4[2].unk04 = sp5f0.f[1] * scale;
            sp4b4[2].unk08 = sp5f0.f[2] * scale;
            sp4b4[3].unk00 = sp5fc.f[0] * scale;
            sp4b4[3].unk04 = sp5fc.f[1] * scale;
            sp4b4[3].unk08 = sp5fc.f[2] * scale;
            sp4b4[0].unk0c = sp620.f[0] * 0.1f;
            sp4b4[0].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[1].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[2].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[3].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp580);
            skyChooseCloudVtxColour(&sp4b4[1], sp584);
            skyChooseCloudVtxColour(&sp4b4[2], sp570);
            skyChooseCloudVtxColour(&sp4b4[3], sp574);
            break;

        case 5:
            s1 = 4;
            sp4b4[0].unk00 = sp638.f[0] * scale;
            sp4b4[0].unk04 = sp638.f[1] * scale;
            sp4b4[0].unk08 = sp638.f[2] * scale;
            sp4b4[1].unk00 = sp620.f[0] * scale;
            sp4b4[1].unk04 = sp620.f[1] * scale;
            sp4b4[1].unk08 = sp620.f[2] * scale;
            sp4b4[2].unk00 = sp614.f[0] * scale;
            sp4b4[2].unk04 = sp614.f[1] * scale;
            sp4b4[2].unk08 = sp614.f[2] * scale;
            sp4b4[3].unk00 = sp608.f[0] * scale;
            sp4b4[3].unk04 = sp608.f[1] * scale;
            sp4b4[3].unk08 = sp608.f[2] * scale;
            sp4b4[0].unk0c = sp638.f[0] * 0.1f;
            sp4b4[0].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp620.f[0] * 0.1f;
            sp4b4[1].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp614.f[0] * 0.1f;
            sp4b4[2].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp608.f[0] * 0.1f;
            sp4b4[3].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp588);
            skyChooseCloudVtxColour(&sp4b4[1], sp580);
            skyChooseCloudVtxColour(&sp4b4[2], sp57c);
            skyChooseCloudVtxColour(&sp4b4[3], sp578);
            break;

        case 10:
            s1 = 4;
            sp4b4[0].unk00 = sp62c.f[0] * scale;
            sp4b4[0].unk04 = sp62c.f[1] * scale;
            sp4b4[0].unk08 = sp62c.f[2] * scale;
            sp4b4[1].unk00 = sp644.f[0] * scale;
            sp4b4[1].unk04 = sp644.f[1] * scale;
            sp4b4[1].unk08 = sp644.f[2] * scale;
            sp4b4[2].unk00 = sp608.f[0] * scale;
            sp4b4[2].unk04 = sp608.f[1] * scale;
            sp4b4[2].unk08 = sp608.f[2] * scale;
            sp4b4[3].unk00 = sp614.f[0] * scale;
            sp4b4[3].unk04 = sp614.f[1] * scale;
            sp4b4[3].unk08 = sp614.f[2] * scale;
            sp4b4[0].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[0].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp644.f[0] * 0.1f;
            sp4b4[1].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp608.f[0] * 0.1f;
            sp4b4[2].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp614.f[0] * 0.1f;
            sp4b4[3].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp584);
            skyChooseCloudVtxColour(&sp4b4[1], sp58c);
            skyChooseCloudVtxColour(&sp4b4[2], sp578);
            skyChooseCloudVtxColour(&sp4b4[3], sp57c);
            break;

        case 1:
            s1 = 3;
            sp4b4[0].unk00 = sp620.f[0] * scale;
            sp4b4[0].unk04 = sp620.f[1] * scale;
            sp4b4[0].unk08 = sp620.f[2] * scale;
            sp4b4[1].unk00 = sp608.f[0] * scale;
            sp4b4[1].unk04 = sp608.f[1] * scale;
            sp4b4[1].unk08 = sp608.f[2] * scale;
            sp4b4[2].unk00 = sp5f0.f[0] * scale;
            sp4b4[2].unk04 = sp5f0.f[1] * scale;
            sp4b4[2].unk08 = sp5f0.f[2] * scale;
            sp4b4[0].unk0c = sp620.f[0] * 0.1f;
            sp4b4[0].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp608.f[0] * 0.1f;
            sp4b4[1].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[2].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp580);
            skyChooseCloudVtxColour(&sp4b4[1], sp578);
            skyChooseCloudVtxColour(&sp4b4[2], sp570);
            break;

        case 2:
            s1 = 3;
            sp4b4[0].unk00 = sp62c.f[0] * scale;
            sp4b4[0].unk04 = sp62c.f[1] * scale;
            sp4b4[0].unk08 = sp62c.f[2] * scale;
            sp4b4[1].unk00 = sp5fc.f[0] * scale;
            sp4b4[1].unk04 = sp5fc.f[1] * scale;
            sp4b4[1].unk08 = sp5fc.f[2] * scale;
            sp4b4[2].unk00 = sp608.f[0] * scale;
            sp4b4[2].unk04 = sp608.f[1] * scale;
            sp4b4[2].unk08 = sp608.f[2] * scale;
            sp4b4[0].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[0].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[1].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp608.f[0] * 0.1f;
            sp4b4[2].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp584);
            skyChooseCloudVtxColour(&sp4b4[1], sp574);
            skyChooseCloudVtxColour(&sp4b4[2], sp578);
            break;

        case 4:
            s1 = 3;
            sp4b4[0].unk00 = sp638.f[0] * scale;
            sp4b4[0].unk04 = sp638.f[1] * scale;
            sp4b4[0].unk08 = sp638.f[2] * scale;
            sp4b4[1].unk00 = sp5f0.f[0] * scale;
            sp4b4[1].unk04 = sp5f0.f[1] * scale;
            sp4b4[1].unk08 = sp5f0.f[2] * scale;
            sp4b4[2].unk00 = sp614.f[0] * scale;
            sp4b4[2].unk04 = sp614.f[1] * scale;
            sp4b4[2].unk08 = sp614.f[2] * scale;
            sp4b4[0].unk0c = sp638.f[0] * 0.1f;
            sp4b4[0].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[1].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp614.f[0] * 0.1f;
            sp4b4[2].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp588);
            skyChooseCloudVtxColour(&sp4b4[1], sp570);
            skyChooseCloudVtxColour(&sp4b4[2], sp57c);
            break;

        case 8:
            s1 = 3;
            sp4b4[0].unk00 = sp644.f[0] * scale;
            sp4b4[0].unk04 = sp644.f[1] * scale;
            sp4b4[0].unk08 = sp644.f[2] * scale;
            sp4b4[1].unk00 = sp614.f[0] * scale;
            sp4b4[1].unk04 = sp614.f[1] * scale;
            sp4b4[1].unk08 = sp614.f[2] * scale;
            sp4b4[2].unk00 = sp5fc.f[0] * scale;
            sp4b4[2].unk04 = sp5fc.f[1] * scale;
            sp4b4[2].unk08 = sp5fc.f[2] * scale;
            sp4b4[0].unk0c = sp644.f[0] * 0.1f;
            sp4b4[0].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp614.f[0] * 0.1f;
            sp4b4[1].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[2].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp58c);
            skyChooseCloudVtxColour(&sp4b4[1], sp57c);
            skyChooseCloudVtxColour(&sp4b4[2], sp574);
            break;

        case 14:
            s1 = 5;
            sp4b4[0].unk00 = sp62c.f[0] * scale;
            sp4b4[0].unk04 = sp62c.f[1] * scale;
            sp4b4[0].unk08 = sp62c.f[2] * scale;
            sp4b4[1].unk00 = sp644.f[0] * scale;
            sp4b4[1].unk04 = sp644.f[1] * scale;
            sp4b4[1].unk08 = sp644.f[2] * scale;
            sp4b4[2].unk00 = sp638.f[0] * scale;
            sp4b4[2].unk04 = sp638.f[1] * scale;
            sp4b4[2].unk08 = sp638.f[2] * scale;
            sp4b4[3].unk00 = sp5f0.f[0] * scale;
            sp4b4[3].unk04 = sp5f0.f[1] * scale;
            sp4b4[3].unk08 = sp5f0.f[2] * scale;
            sp4b4[4].unk00 = sp608.f[0] * scale;
            sp4b4[4].unk04 = sp608.f[1] * scale;
            sp4b4[4].unk08 = sp608.f[2] * scale;
            sp4b4[0].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[0].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp644.f[0] * 0.1f;
            sp4b4[1].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp638.f[0] * 0.1f;
            sp4b4[2].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[3].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[4].unk0c = sp608.f[0] * 0.1f;
            sp4b4[4].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp584);
            skyChooseCloudVtxColour(&sp4b4[1], sp58c);
            skyChooseCloudVtxColour(&sp4b4[2], sp588);
            skyChooseCloudVtxColour(&sp4b4[3], sp570);
            skyChooseCloudVtxColour(&sp4b4[4], sp578);
            break;

        case 13:
            s1 = 5;
            sp4b4[0].unk00 = sp644.f[0] * scale;
            sp4b4[0].unk04 = sp644.f[1] * scale;
            sp4b4[0].unk08 = sp644.f[2] * scale;
            sp4b4[1].unk00 = sp638.f[0] * scale;
            sp4b4[1].unk04 = sp638.f[1] * scale;
            sp4b4[1].unk08 = sp638.f[2] * scale;
            sp4b4[2].unk00 = sp620.f[0] * scale;
            sp4b4[2].unk04 = sp620.f[1] * scale;
            sp4b4[2].unk08 = sp620.f[2] * scale;
            sp4b4[3].unk00 = sp608.f[0] * scale;
            sp4b4[3].unk04 = sp608.f[1] * scale;
            sp4b4[3].unk08 = sp608.f[2] * scale;
            sp4b4[4].unk00 = sp5fc.f[0] * scale;
            sp4b4[4].unk04 = sp5fc.f[1] * scale;
            sp4b4[4].unk08 = sp5fc.f[2] * scale;
            sp4b4[0].unk0c = sp644.f[0] * 0.1f;
            sp4b4[0].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp638.f[0] * 0.1f;
            sp4b4[1].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp620.f[0] * 0.1f;
            sp4b4[2].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp608.f[0] * 0.1f;
            sp4b4[3].unk10 = sp608.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[4].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[4].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp58c);
            skyChooseCloudVtxColour(&sp4b4[1], sp588);
            skyChooseCloudVtxColour(&sp4b4[2], sp580);
            skyChooseCloudVtxColour(&sp4b4[3], sp578);
            skyChooseCloudVtxColour(&sp4b4[4], sp574);
            break;

        case 11:
            s1 = 5;
            sp4b4[0].unk00 = sp620.f[0] * scale;
            sp4b4[0].unk04 = sp620.f[1] * scale;
            sp4b4[0].unk08 = sp620.f[2] * scale;
            sp4b4[1].unk00 = sp62c.f[0] * scale;
            sp4b4[1].unk04 = sp62c.f[1] * scale;
            sp4b4[1].unk08 = sp62c.f[2] * scale;
            sp4b4[2].unk00 = sp644.f[0] * scale;
            sp4b4[2].unk04 = sp644.f[1] * scale;
            sp4b4[2].unk08 = sp644.f[2] * scale;
            sp4b4[3].unk00 = sp614.f[0] * scale;
            sp4b4[3].unk04 = sp614.f[1] * scale;
            sp4b4[3].unk08 = sp614.f[2] * scale;
            sp4b4[4].unk00 = sp5f0.f[0] * scale;
            sp4b4[4].unk04 = sp5f0.f[1] * scale;
            sp4b4[4].unk08 = sp5f0.f[2] * scale;
            sp4b4[0].unk0c = sp620.f[0] * 0.1f;
            sp4b4[0].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[1].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp644.f[0] * 0.1f;
            sp4b4[2].unk10 = sp644.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp614.f[0] * 0.1f;
            sp4b4[3].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[4].unk0c = sp5f0.f[0] * 0.1f;
            sp4b4[4].unk10 = sp5f0.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp580);
            skyChooseCloudVtxColour(&sp4b4[1], sp584);
            skyChooseCloudVtxColour(&sp4b4[2], sp58c);
            skyChooseCloudVtxColour(&sp4b4[3], sp57c);
            skyChooseCloudVtxColour(&sp4b4[4], sp570);
            break;

        case 7:
            s1 = 5;
            sp4b4[0].unk00 = sp638.f[0] * scale;
            sp4b4[0].unk04 = sp638.f[1] * scale;
            sp4b4[0].unk08 = sp638.f[2] * scale;
            sp4b4[1].unk00 = sp620.f[0] * scale;
            sp4b4[1].unk04 = sp620.f[1] * scale;
            sp4b4[1].unk08 = sp620.f[2] * scale;
            sp4b4[2].unk00 = sp62c.f[0] * scale;
            sp4b4[2].unk04 = sp62c.f[1] * scale;
            sp4b4[2].unk08 = sp62c.f[2] * scale;
            sp4b4[3].unk00 = sp5fc.f[0] * scale;
            sp4b4[3].unk04 = sp5fc.f[1] * scale;
            sp4b4[3].unk08 = sp5fc.f[2] * scale;
            sp4b4[4].unk00 = sp614.f[0] * scale;
            sp4b4[4].unk04 = sp614.f[1] * scale;
            sp4b4[4].unk08 = sp614.f[2] * scale;
            sp4b4[0].unk0c = sp638.f[0] * 0.1f;
            sp4b4[0].unk10 = sp638.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[1].unk0c = sp620.f[0] * 0.1f;
            sp4b4[1].unk10 = sp620.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[2].unk0c = sp62c.f[0] * 0.1f;
            sp4b4[2].unk10 = sp62c.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[3].unk0c = sp5fc.f[0] * 0.1f;
            sp4b4[3].unk10 = sp5fc.f[2] * 0.1f + g_SkyCloudOffset;
            sp4b4[4].unk0c = sp614.f[0] * 0.1f;
            sp4b4[4].unk10 = sp614.f[2] * 0.1f + g_SkyCloudOffset;

            skyChooseCloudVtxColour(&sp4b4[0], sp588);
            skyChooseCloudVtxColour(&sp4b4[1], sp580);
            skyChooseCloudVtxColour(&sp4b4[2], sp584);
            skyChooseCloudVtxColour(&sp4b4[3], sp574);
            skyChooseCloudVtxColour(&sp4b4[4], sp57c);
            break;

        default:
            return gdl;
    }

    gDPPipeSync(gdl++);

    texSelect(&gdl, &skywaterimages[fogGetCurrentEnvironmentp()->SkyImageId], 1, 0, 2);

    if (1)
        ;

    gDPSetEnvColor(gdl++, fogGetCurrentEnvironmentp()->Red, fogGetCurrentEnvironmentp()->Green,
                   fogGetCurrentEnvironmentp()->Blue, 0xff);
    gDPSetCombineLERP(gdl++, SHADE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0, 0, 0, SHADE, SHADE, ENVIRONMENT, TEXEL0,
                      ENVIRONMENT, 0, 0, 0, SHADE);

    {
        Mtxf sp1ec;
        Mtxf sp1ac;
        SkyRelated38 sp94[5];
        s32 i;
        s32 stack[2];

        matrix_4x4_multiply(currentPlayerGetProjectionMatrixF(), camGetWorldToScreenMtxf(), &sp1ec);
        guScaleF(dword_CODE_bss_80079E98.m, 1.0f / scale, 1.0f / scale, 1.0f / scale);
        matrix_4x4_multiply(&sp1ec, &dword_CODE_bss_80079E98, &sp1ac);

        for (i = 0; i < s1; i++) {
            sub_GAME_7F097388(&sp4b4[i], &sp1ac, 130, 65535.0f, 65535.0f, &sp94[i]);

            sp94[i].unk28 = skyClamp(sp94[i].unk28, getPlayer_c_screenleft() * 4.0f,
                                     (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f);
            sp94[i].unk2c = skyClamp(sp94[i].unk2c, getPlayer_c_screentop() * 4.0f,
                                     (getPlayer_c_screentop() + getPlayer_c_screenheight()) * 4.0f - 1.0f);
        }

#if PORTSKY == 0
        if (s1 == 4) {
            if (((sp538 << 3) | (sp534 << 2) | (sp530 << 1) | sp52c) == 12) {
                if (sp548 < sp54c) {
                    if (sp94[3].unk2c >= sp94[1].unk2c + 4.0f) {
                        sp94[0].unk28 = getPlayer_c_screenleft() * 4.0f;
                        sp94[0].unk2c = getPlayer_c_screentop() * 4.0f;
                        sp94[1].unk28 = (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f;
                        sp94[1].unk2c = getPlayer_c_screentop() * 4.0f;
                        sp94[2].unk28 = getPlayer_c_screenleft() * 4.0f;
                        sp94[3].unk28 = (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f;

                        gdl = sub_GAME_7F098A2C(gdl, &sp94[0], &sp94[1], &sp94[2], &sp94[3], 130.0f);
                    } else {
                        gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[1], &sp94[2], 130.0f, TRUE);
                    }
                } else if (sp94[2].unk2c >= sp94[0].unk2c + 4.0f) {
                    sp94[0].unk28 = getPlayer_c_screenleft() * 4.0f;
                    sp94[0].unk2c = getPlayer_c_screentop() * 4.0f;
                    sp94[1].unk28 = (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f;
                    sp94[1].unk2c = getPlayer_c_screentop() * 4.0f;
                    sp94[2].unk28 = getPlayer_c_screenleft() * 4.0f;
                    sp94[3].unk28 = (getPlayer_c_screenleft() + getPlayer_c_screenwidth()) * 4.0f - 1.0f;

                    gdl = sub_GAME_7F098A2C(gdl, &sp94[1], &sp94[0], &sp94[3], &sp94[2], 130.0f);
                } else {
                    gdl = sub_GAME_7F097818(gdl, &sp94[1], &sp94[0], &sp94[3], 130.0f, TRUE);
                }
            } else {
                gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[1], &sp94[3], 130.0f, TRUE);
                gdl = sub_GAME_7F097818(gdl, &sp94[3], &sp94[2], &sp94[0], 130.0f, TRUE);
            }
        } else if (s1 == 5) {
            gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[1], &sp94[2], 130.0f, TRUE);
            gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[2], &sp94[3], 130.0f, TRUE);
            gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[3], &sp94[4], 130.0f, TRUE);
        } else if (s1 == 3) {
            gdl = sub_GAME_7F097818(gdl, &sp94[0], &sp94[1], &sp94[2], 130.0f, TRUE);
        }
    }
#else
        // Camera-independent cloud plane fan (see skyDrawPlaneFan).
        gdl = skyDrawPlaneFan(gdl, fogGetCurrentEnvironmentp()->CloudRepeat, FALSE);
    }
#endif

    return gdl;
}
#endif
