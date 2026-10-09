#ifndef DOLPHIN_GXEXTRA_H
#define DOLPHIN_GXEXTRA_H
// Extra types for PC
#ifdef TARGET_PC
#include <dolphin/gx/GXStruct.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float r;
  float g;
  float b;
  float a;
} GXColorF32;

void GXDestroyTexObj(GXTexObj* obj);
void GXDestroyTlutObj(GXTlutObj* obj);
void GXDestroyCopyTex(void* dest);
// Aurora extension: offsets indexed fetches from an array by `base` elements.
// Stays in effect until changed; GXSetArray does not reset it.
void GXSetArrayBaseIndex(GXAttr attr, u32 base);
// Aurora extension: PBR shading for the following draws (see GX_AURORA_SET_PBR).
void GXSetPBR(GXBool enable);
// Aurora extension: distance-field texturing for the following draws (see
// GX_AURORA_SET_SDF). 0 turns it off.
void GXSetSDF(u8 edge);
// Port extension: how the following draws sample a converted Remastered HUD picture (see
// GX_AURORA_SET_HUD_SAMPLE). Mode 0 turns it off.
void GXSetHudSample(u8 mode, f32 dyinX, f32 dyinY);
// Port extension: a depth pre-pass for surfaces that alpha-test. Pass 1 writes only depth,
// and its pixel shader keeps nothing but what the alpha compare needs; pass 2 draws the same
// surfaces again where the depth is equal, without writing it, so each pixel is shaded once
// and the GPU can still test depth before shading (a discard stops that only while depth is
// written). The result is what one depth-tested pass gives. Draws that don't write depth
// are left alone in pass 2 and draw nothing in pass 1. 0 turns it off.
void GXPortSetDepthPrepass(u8 pass);
// Aurora extension: names the following draws in Aurora's warnings about them (the
// model's asset id, its index when it has no id, and the material); draws nothing
// differently. Asset 0 with model 0xFFFFFFFF is no name (the initial state).
void GXSetDrawTag(u32 asset, u32 model, u32 material);
// Port extension, for finding what drew a pixel: the serial (24 bits, 0 = none) of the following draws, and
// the "drawid" mode, which draws every draw as its serial in a flat colour (R, G, B = its bytes, low first),
// with no blend, fog, bloom or grade, so the bytes reach the frame as they are.
void GXPortSetDrawSerial(u32 serial);
void GXPortSetDrawIdMode(GXBool on);
// Port extension, shader debugging. Dump writes every WGSL module built so far, and from now on, as
// <dir>/<hash16>.wgsl (with a header listing its config) and a line in <dir>/index.tsv; returns how many it
// wrote now (null or empty: stop). OverrideDir sets where an edited <hash16>.wgsl replaces a generated module
// (null or empty: off); Reload drops the pipeline cache so the modules are built again at the next draw.
// MP_WGSL_DUMP and MP_WGSL_OVERRIDE set the directories at startup.
u32 GXPortShaderDump(const char* dir);
void GXPortShaderOverrideDir(const char* dir);
void GXPortShaderReload(void);
// DrawLog(on) makes the draws record the shader hash they ran; DrawShader(serial) reads it (0 = unknown or
// too old), and ShaderOverridden says whether an edited source for the hash is in the override directory.
void GXPortDrawLog(GXBool on);
u64 GXPortDrawShader(u32 serial);
GXBool GXPortShaderOverridden(u64 hash);
// Port extension: goes up with every GXCopyTex, so a caller can tell whether a copy it made
// is still the latest (nothing has copied into, or cleared through, a texture since).
u32 GXPortCopySerial(void);
// Port extension: keeps a copy of `data`, a vertex array or a display list that stays as it is,
// on the GPU, so a draw that uses it no longer copies it into the frame's buffers: a vertex
// array given to GXSetArray at this pointer, or a GXCallDisplayList of it. Retains are counted
// by pointer; the last release must come before the memory is freed or reused. Without room
// for it (AuroraConfig::residentGeometryMiB), it is sent every frame as before.
void GXPortRetainResident(const void* data, u32 size);
void GXPortReleaseResident(const void* data);
// Aurora extension: the PBR environment probe (see GX_AURORA_COPY_PROBE_FACE and
// GX_AURORA_SET_PBR_PROBE).
void GXCopyProbeFace(u32 face);
void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight);
// As GXSetPBRProbe, plus Remastered's reflection occlusion for draws with an ambient volume:
// the reflection is scaled by mix(occlusionMin, 1, saturate(max channel of the baked mean *
// occlusionInvMax)). occlusionInvMax 0 is none.
void GXSetPBRProbeEx(const f32 viewToProbe[3][3], f32 weight, f32 occlusionMin, f32 occlusionInvMax);
// Aurora extension: the emissive multiplier and backlight weight of the following PBR
// draws (see GX_AURORA_SET_PBR_MATERIAL). `heightBlend` above 0 is the threshold of a
// height-blended alpha (0: the base map's alpha is the opacity), and `mode` 1 draws the
// surface's own colour with no lighting, 2 has the base map's alpha mask the glow instead
// of being the opacity, 4 has the vertex colour tint the surface, 8 is Remastered's
// ColorUnlit and 16 a sky, whose unlit colour `backlight` multiplies; the sum of those. `layer` is the blend of a second layer (texture maps 4-6:
// base, MR, normal) over the first by the vertex alpha and the two base maps' alphas: the
// width of its edge, then the scale and offset of the first layer's height and of the
// second's. A width of 0 is no second layer. `kind` is one of Remastered's special
// surfaces, a strength and four parameters of it: 1 lays the second layer on what faces
// `up` (world up in view space; the vertex alpha lifts it), 2 has map 4 as a detail map
// multiplied into the base, 3 scales the glow by the vertex alpha (lava), 4 is ice: map 4
// is seen inside the surface, at a depth of the base map's alpha times the fourth
// parameter, through a fresnel of power and weight the first two; the third scales the
// normal map and the strength is the inside's glow. Kinds 5-8 are liquids, falling water
// and glass (see shader.cpp). 9 is a beam's glow: map 4's channels are noise scrolling at
// the layer's four scale/offset values (red, green) and the second and third parameters
// (blue) times the first (the time); their sum less twice the vertex colour's, plus the
// fourth, picks the glow from map 5, a ramp whose row is the vertex alpha, times the strength.
void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3], f32 heightBlend, f32 mode, const f32 layer[5],
                      const f32 kind[6], const f32 up[3]);
// Aurora extension: room cubes (see GX_AURORA_CREATE_PBR_CUBE). `texels` is RGBA16Float,
// every mip of face 0 from the largest down, then face 1 and so on; it is copied.
void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length);
void GXDestroyPBRCube(u32 id);
// params: exposure, the mip a roughness of 1 samples, the mip the ambient samples along
// the normal, and the scale that takes that sample to 1 for an average direction (0 = the
// ambient is left alone). Id 0, or one never created, selects the probe.
void GXSetPBRCube(u32 id, const f32 params[4]);
// Renders sum(weights[i] * cube src[i]) (up to four room cubes) into room cube `dst`, made
// or resized to the first's size. Not a FIFO command: it ends the current pass, and draws
// recorded after it see the result. False (dst unusable) when a source is missing or it
// could not be recorded. A dst that changes texture is only picked up by a GXSetPBRCube to
// a different id, so alternate between two.
GXBool GXPortBlendPBRCube(u32 dst, const u32* src, const f32* weights, u32 count);
// Aurora extension: baked ambient light as a function of the normal n (view space), per
// colour channel c: base[c] + lobe[c] * pow(clamp(0.5 + 0.5 * dot(n, dir[c]), 0, 1), power[c]).
// The rows are base, lobe, power, then the direction of red, green and blue; the
// directions are not unit length (shorter is more even). The luminance of the GX ambient
// colour scales the result. Null goes back to the GX ambient alone.
void GXSetPBRAmbient(const f32 rows[6][3], f32 mode);
// Aurora extension: ambient volumes (see GX_AURORA_CREATE_PBR_VOLUME). `texels` is, for
// every point with x fastest and z slowest: all the means (RGBA16Float), then all the lobes
// (RGBA16Float), then the direction of red, of green and of blue (RGBA8, 0..255 is -1..1
// along the volume's axes, with that channel's sharpness in alpha); 28 bytes a point.
void GXCreatePBRVolume(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, const void* texels, u32 length);
void GXDestroyPBRVolume(u32 id);
// Rows 0 to 2 take a view-space position (w: the offset) to the volume's texture
// coordinates, rows 3 to 5 a view-space normal to the volume's axes. w of row 3 scales the
// light (0: no volume), w of row 4 is how far along the normal the sample is taken.
void GXSetPBRVolume(u32 id, const f32 rows[6][4]);
// Aurora extension: baked lightmaps (see GX_AURORA_CREATE_PBR_LIGHTMAP), a 2D-array texture
// of `layers` (4): layer 0 the irradiance colour L0, layers 1 to 3 the first-order terms L1
// x, y and z (RGB, each divided by L0, signed). The lit ambient of a pixel is
// L0 * (1 + L1x * n.x + L1y * n.y + L1z * n.z), n the normal in the lightmap's axes.
typedef enum {
  GX_PBR_LIGHTMAP_BC6H_SFLOAT = 0,
  GX_PBR_LIGHTMAP_BC6H_UFLOAT = 1,
  GX_PBR_LIGHTMAP_RGBA16F = 2,
} GXPBRLightmapFormat;
// Whether BC texture compression is available on this device (BC6H lightmaps).
GXBool GXPBRLightmapBC6HSupported(void);
// `data` is every layer's mip-0 texels, layer-major (BC6H: 16-byte blocks, rows top to bottom;
// RGBA16F: 8 bytes a texel). Returns an id above 0, or 0 on failure (an unsupported format,
// a size that doesn't fit `size`, or a device without the extra texture binding).
u32 GXCreatePBRLightmap(u32 width, u32 height, u32 layers, GXPBRLightmapFormat format, const void* data, u32 size);
void GXDestroyPBRLightmap(u32 id);
// For the draws that follow; id 0 turns it off. rect: offU, offV, scale, level: the atlas UV is
// rect.xy + uv * rect.z, and level multiplies the light. axes: rows taking the shader's
// view-space normal to the lightmap's axes.
void GXSetPBRLightmap(u32 id, const f32 rect[4], const f32 axes[3][3]);
// Per material: the vertex attribute (GX_VA_TEX0 to GX_VA_TEX7) that holds the lightmap UV, or
// GX_VA_NULL for none. Part of the shader config (it adds a varying).
void GXSetPBRLightmapAttr(GXAttr attr);
// Aurora extension: what PBR surfaces drawn from now on show, for debugging: 0 the shaded
// result, 1 base colour, 2 normal (view space), 3 roughness, 4 metalness, 5 occlusion,
// 6 the diffuse ambient, 7 the reflection, 8 the glow, 9 the lit level in stops around
// middle grey (blue under, red over), 10 the special surface's kind. Not a FIFO command:
// it rides with the next GXSetPBRMaterial.
void GXSetPBRDebugView(u32 view);
// Aurora extension, a performance diagnostic: PBR surfaces enabled from now on leave out
// part of their shading, to see what it costs by the frame rate. 0 off, 1 flat (the base
// map alone; cut-outs still cut), 2 no lights, 3 no ambient volume, 4 no reflection cube,
// 5 no normal maps, 6 no metal/roughness and emissive maps, 7 flat but with every map
// still read, 8 full shading with the maps' anisotropy off, 9 as 8 with nearest mips.
// Takes effect at the next GXSetPBR(GX_TRUE). Two leave the shading alone and measure the
// frame around it: 10 no post-processing (GXPortPostProcess does nothing), 11 no screen
// copies (GXCopyTex without a clear copies nothing, so the frame stays one render pass;
// what samples the copy sees stale texels).
void GXSetPBRCostTest(u32 test);
// Port extension: per-render-pass GPU times from timestamp queries, for seeing what each pass
// (bloom levels, EFB passes, ...) costs on a GPU where no profiler can be attached. Off by default
// and free while off. Averages over 60-frame windows, so nothing is published for the first
// second after it is switched on.
typedef struct {
  const char* name; // static lifetime
  float msPerFrame;
  float passesPerFrame;
} GXPortGpuTime;
void GXPortSetGpuTimes(GXBool on);
// 0 if the device has no timestamp queries. Writes up to `max` entries, slowest first, and
// returns how many there are; the totals are the sum of all passes and first begin to last end.
GXBool GXPortGpuTimesSupported(void);
u32 GXPortGetGpuTimes(GXPortGpuTime* out, u32 max, float* totalMs, float* spanMs);
u32 GXGetPBRCostTest(void);
// Aurora extension: a three-piece tone curve over the lit colour x, which is taken as
// already exposed (see GX_AURORA_SET_PBR_TONE). Row 0 is the toe, (a x + b) x^2 + c x below
// z of row 1; row 1 the line S x + y0 (x, y) from there to its w; row 2 the shoulder
// x t / (1 + t) + w with t = y x + z. Null, or a slope of 0, is no curve. With a curve,
// w of row 0 multiplies the glow and unlit colour before it (0 is taken as 1).
void GXSetPBRTone(const f32 rows[3][4]);
// Aurora extension: the environment BRDF table the PBR specular reads instead of Karis'
// analytic fit. 16x8 RG8, row-major with 16 texels a row: x is N.V, y is roughness (row 0
// the smoothest), red the scale and green the bias of F0. Sampled bilinearly, clamped to
// the edge. `length` other than 256, or null, goes back to the fit.
void GXSetPBRBrdfLut(const void* texels, u32 length);
// Aurora extension: lights the following PBR draws leave out although the channel enables
// them (bit n: GX_LIGHTn), such as lights whose light a baked ambient already holds. 0 for
// none.
void GXSetPBRLightSkip(u32 mask);
// Aurora extension: Remastered's baked-lighting modulation colour (BakedLightingColorModulate).
// The following PBR draws multiply their baked ambient (GXSetPBRAmbient, GXSetPBRVolume) by it,
// and the reflection occlusion's argument (GXSetPBRProbeEx) by its luminance. Linear, not
// clamped; 1, 1, 1 (or null) is neutral.
void GXSetPBRBakedLightModulation(const f32 rgb[3]);
// Aurora extension: Remastered's CharacterBacklight for the following lit PBR draws whose
// material has a backlight (GXSetPBRMaterial's `backlight`: back strength, top strength,
// falloff power + 1). Two lights with no position: one from world up (`up`), coloured like the
// baked ambient along the camera's up, and one from behind the model, fixed in view space at
// (1, 1, -1) / sqrt(3), coloured like the baked ambient along `backDir` (a view-space
// direction). Each colour is brought up to a luminance of 1. Both fade with
// pow(saturate(dot(plane, (view position, 1))), power): `plane` gives 0 at the low end of the
// model's bounds along its own y and 1 at the high end (power 1: no fade). `back` and `top` scale the materials'
// strengths (4 and 2 in Remastered). Null plane turns it off.
void GXSetPBRBacklight(const f32 plane[4], const f32 backDir[3], f32 back, f32 top, f32 bindScale, f32 bindOffset);
// Aurora extension: the constants of a kind 14 PBR material (Remastered's BoundaryShield force
// field), as the shader reads them. Rows 0-6 are the material's CCH0..CCH6, row 7 is
// DIFC (x, y, z, w). Stays in effect until changed; null is all zero.
void GXSetPBRShield(const f32 rows[8][4]);
// Aurora extension: the following PBR draws light with this in place of the GX light's colour,
// position and attenuation: a linear colour (no gamma, not clamped), a view-space position,
// and a falloff from full at r0 to none at r1 (0 none, 1 linear, 2 quadratic, 3 1 - smoothstep,
// as Remastered's particle lights). Non-PBR draws keep the GX light. r1 <= 0 (or a null colour)
// turns it off.
void GXSetPBRLightHdr(GXLightID light, const f32 color[3], const f32 viewPos[3], f32 r0, f32 r1, u32 falloff);
// Aurora extension: the following PBR draws multiply the diffuse colour (before the lights and
// the ambient) and F0 (before the direct Fresnel and the environment BRDF) by these; nothing
// else is scaled. 1, 1 is neutral, and it is what a caller sets for every material that has no
// scale of its own. An f0 of exactly 0 also marks a LITS back copy, whose stored normal is
// turned round (like every back copy's): the shader turns it back before shading.
// alpha fades the draws: their alpha is multiplied by it, or is it where alphaReplaces is set
// (an opaque material drawn blended, whose own alpha means nothing). 1 and GX_FALSE are neutral.
void GXSetPBRLightScale(f32 diffuse, f32 f0, f32 alpha, GXBool alphaReplaces);

// Aurora extension: Remastered's bloom over what the EFB holds now (see lib/gfx/bloom.cpp).
// The EFB is taken as drawn through the tone curve `tone` (as GXSetPBRTone); light above
// `threshold` (exposed luminance) blooms, tints 0 to 3 weight the levels from the coarsest
// and tint 4 the bright pass. Not a FIFO command: it ends the current pass. False when it
// could not be recorded. Then the colour grade: the LUTs gradeA and gradeB (ids from
// GXPortColorGradeLut, 0 = none) over the tone-mapped colour, mixed by gradeWeight (1 = B).
// Without bloom (bloom false) only the grade runs, and nothing when there is none either.
// Before both, with an `exposure` above 0 (and a tone curve), the frame is averaged for auto
// exposure: the mean exposed level, undone by the curve and divided by `exposure`, comes
// back through GXPortFrameRadiance a few frames later.
GXBool GXPortPostProcess(GXBool bloom, f32 threshold, const f32 tints[5][3], const f32 tone[3][4], u32 gradeA,
                         u32 gradeB, f32 gradeWeight, f32 exposure);
// Port extension: Remastered's volumetric fog over the EFB as drawn so far: a froxel grid
// (16x16 pixels by 64 slices out to `fog[0]`) of height and noise shaped density, lit by the
// ambient volume's mean light, integrated front to back and applied to every pixel at the
// world's depth (the viewmodel, drawn nearer than depth[2], is left alone). The EFB is undone
// through the tone curve and exposure the frame was drawn with, fogged, and drawn through them
// again. Call it before GXPortPostProcess. View space is GX's (x right, y up, z towards the camera).
typedef struct {
  f32 viewToWorld[3][4];   // rows: world = row . (view, 1)
  f32 worldToVolume[3][4]; // rows: world -> the ambient volume's texture coordinates
  f32 frustum[4];          // left, right, bottom, top at a view depth of 1
  f32 depth[4];            // near, far, and the GX z range the world draws in (min, max)
  f32 fog[4];              // range, scatter, absorb, density
  f32 shape[4];            // height slope, height bias (over world z), noise frequency, noise strength
  f32 noise[4];            // xyz: the noise's offset, w: the light's largest channel
  f32 colorB[4];           // the light's multiplier; w: the volume's level
  f32 colorA[4];           // the light added; w: the exposure the EFB was drawn at
  f32 tone[3][4];          // the tone curve the EFB was drawn through (as GXSetPBRTone)
  f32 lut[64];             // density over distance: entry i at (i / 63)^2 * range
  // Remastered's fog regions in the order they chain, each 7 rows: world -> 0..1 over its box
  // (3 rows), (edge scale xyz, mult), (edge bias xyz, light cap), colour, (density, 0, 0, 0).
  // With l = 2 uvw - 1, mask = clamp(min(|l| scale + bias), 0, 1) and k = 1 + mask (mult - 1),
  // the light added becomes max(0, mask colour + it k), the height-shaped density
  // max(0, it k + mask density) and the cap max(0, it k + mask cap).
  f32 regions[8][7][4];
  u32 regionCount;         // up to 8
  u32 pad[3];
  u32 volume;              // the ambient volume (GXPBRVolume id), 0 for none
} GXPortFogParams;
GXBool GXPortVolumetricFog(const GXPortFogParams* params);
// The draws after GXPortVolumetricFog fog themselves as Remastered's transparents do, until this:
// opaque ones per pixel as the full-screen pass, blended and additive ones per vertex (blended:
// colour T + in-scatter, additive: colour T). Harmless when no fog was drawn.
void GXPortVolumetricFogEnd(void);
// Marks the draws up to the next call with false as particles' (inside the volumetric fog). As
// Remastered's particle renderers, they fog themselves per vertex as colour T + in-scatter unless
// their blend mode is additive or premultiplied (the static render state's "no fog" flag), when
// they are not fogged at all and only add to the already fogged frame.
void GXPortSetParticleFog(GXBool on);
// Stores a 33x33x33 RGBA8 colour grade LUT (red fastest) under a non-zero id.
void GXPortColorGradeLut(u32 id, const u8* rgba);
// The average radiance (linear rgb) of the latest frame measured by GXPortPostProcess, and a
// count that goes up with each new one. False before the first.
GXBool GXPortFrameRadiance(f32 out[3], u32* serial);
// The sun's shadow. Each frame, GXPortSetShadowFrame gives the view (world -> view rows), the sun's
// direction (world, the way its light travels), the radius around the camera its map covers (<= 0:
// none) and an extra light's colour (0 for none: the sun then only shadows the room's directional
// light along that direction). Lit PBR draws made with GXPortSetShadowCaster on cast into the map
// and receive the previous frame's; GXPortRenderShadowMap draws the map after the frame's casters.
// GXPortSetShadowCasterOnly makes the following draws (with the caster on) cast but not draw, for
// what is outside the view; GXPortShadowBoxCasts says whether a world box reaches into the map of
// the last GXPortSetShadowFrame (the FIFO's, so a frame behind the game thread).
void GXPortSetShadowCaster(GXBool on);
void GXPortSetShadowCasterOnly(GXBool on);
void GXPortSetShadowFrame(const f32 worldToView[3][4], const f32 sunDir[3], f32 radius, const f32 color[3]);
GXBool GXPortShadowBoxCasts(const f32 min[3], const f32 max[3]);
// For the console: the last frame's sun direction, colour, radius (0: no map) and the map's centre
// in world space, and how many draws the last map drawn had.
void GXPortGetShadowInfo(f32 sunDir[3], f32 color[3], f32* radius, f32 center[3], u32* casters);
GXBool GXPortRenderShadowMap(void);
// The room's Remastered point and spot lights for the rest of the frame, added on every PBR draw
// (lit, lightmapped or not) as the sun is. Each record is 16 f32: view-space position, A; colour
// (linear, scaled), B; view-space axis toward the light, cone scale; cone bias, falloff (0 none,
// 1 linear, 2 squared, 3 smooth), spot (0/1), 0. The distance falloff runs on
// t = clamp(dist * A + B, 0, 1), the cone on clamp(dot(L, axis) * scale + bias, 0, 1).
void GXPortSetRoomLights(const f32* records, u32 count);

void GXColor4f32(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif

#endif
