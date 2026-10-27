#pragma once

#include <cstdint>
#include <optional>

#include <dolphin/gx.h>

// Port extension: a native draw for Remastered's VFX particle materials. The shader is not the
// GX TEV pipeline: it is one composable shader (build/mpr/vfx/SPEC.md section 2, spec2/{a,b}.md
// "Feature model") that computes colour and opacity from up to four textures, a ramp (or two), an
// indirect warp, a threshold mask, a fresnel/fade factor and an erosion, then tone maps and fogs
// the result like the PBR path does.
//
// Order of operations: uv chain (Indirect warps the slots flagged `warped`) -> texture reads
// (Ramp/DualMod, ColorIndexing or ColorTex) -> alpha factors (OpacityTex, Thresholding,
// OpacityFresnel, OpacityFade) -> Erosion (max(0, a - e), the only subtraction) -> x vc.w ->
// colour x vc.rgb x modulate (AddColor: x row.w + row.xyz).
namespace aurora::gfx::vfx {
enum Feature : uint32_t {
  ColorTex = 1,         // colorSlot is the colour map (BCLR): rgb = t.rgb, a = t.w
  OpacityTex = 2,       // opacitySlot: a *= t.x (with no other colour source, rgb is vc only)
  Erosion = 4,          // a = max(0, a - erosion)
  Ramp = 8,             // rampSlot: t3 = r.x^3, rgb = mix(c0, c1, t3), a = r.y * mix(c0.w, c1.w, t3)
  Indirect = 16,        // indirectSlot: w = d.xy * 0.99609375 - 0.5; a `warped` slot's uv += w * warpScale
  DepthSoften = 32,     // fades the alpha where the particle nears the opaque scene's depth (capture_scene)
  Thresholding = 64,    // thresholdSlot (xy), thrX/thrY, thrW: a *= S(t1) * S(t2)
  DualMod = 128,        // with Ramp: ramp2Slot too, t3 = (r.x * r2.x)^3, a = r.y * r2.y * mix(c0.w, c1.w, t3)
  OpacityFresnel = 256, // a *= S(sat((|vec.z|/|vec| - F.x) / (F.y - F.x)))
  ColorIndexing = 512,  // colorSlot = lookup (x index, y alpha), paletteSlot read at (i.x*s + o, z), layer 0
  AddColor = 1024,      // extra[addRow] = (add.rgb, scale): rgb = rgb * scale + add
  OpacityFade = 2048,   // a *= S(sat(-p.x / (p.y - p.x))), p = fadeX/fadeY
  ColorRgbOnly = 4096,  // with ColorTex: the map's alpha is not opacity (a stays 1)
  Pyro = 8192,          // VFX_Pyro: colorSlot = P, rampSlot = gradient LUT read at (P.r, 0), (P.g, 0); e = extra[1]:
                        // rgb = P.b * (P.r*e.x*G(P.r) + P.g*e.y*G(P.g)) / exposure, a = S(sat((P.a - e.w)/(1 - e.w))) * e.z
  Disintegration = 16384, // VFX_Disintegration: colorSlot = mask, s = t.x^2, extra[0..2] = A, B, edge E, extra[3] = (thr,
                          // edge width, alpha lo, alpha hi): m = sat(s - thr), q = sat(1 - m - width), e = q - q^2,
                          // rgb = mix(A, B, s) * vc.rgb + e * E, a = sat(m * (e > 0 ? hi : lo))
  SceneWarp = 32768,    // VFX_FrameBuffer_Indirect_*: rgb *= the scene copy (capture_scene) at the fragment, offset by
                        // the indirectSlot map d: uv = frag / size + s * k * (d.x * 0.99609375 - 0.5, 0.5 - d.y * 0.99609375),
                        // k = the indirect slot's warpScale (CCH0.xy), s = near / view depth (per vertex)
  VisorSplat = 65536,   // VFX_VisorSplat: the indirectSlot map read at the world position (extra[2].xy) x PMTR row 1.xy
                        // + row 0.zw gives w = (d * 0.99609375 - 0.5) * row 0.xy, a view-space offset taken to the quad's
                        // own axes by extra[3] = the inverse basis's rows 0, 1 (xy each): t = colorSlot at uv + that,
                        // rgb = t.rgb * vc.rgb, a = max(0, t.w + row 1.z - 1). The runtime fills extra[2..3] (sprites)
  // Explicit model raster policy in the backward-compatible VMAT features word. Only the
  // audited Ice wedge MATI opts in; absent in older imports and every sprite recipe.
  ModelBackCull = 1u << 17,
  // VFX_FrameBuffer_Color_Erosion_Unlit (d8ece62c, FS perm 000_0; the frozen-player HUD effect GENP
  // c23f13f7): t = colorSlot (BCLR) at the sprite's uv and layer; the scene copy (capture_scene) at the
  // fragment, offset by (t.x, -t.y) x near / view depth x CCH0.xy and read at lod max(0, CCH0.z - 1), is
  // added to it: rgb = (t.rgb + scene) x vc.rgb, a = t.w (Erosion then subtracts PMTR row 0's x).
  // params = CCH0 (SMTR-animated). The depth is the HUD billboard's, as VisorRain's. No copy: t alone.
  FrameBufferColorErosion = 1u << 18,
  // VFX_VisorSteam (53eceaa4, FS perm 000_0, VS 000_1; the SteamMP1 HUD billboard's model particle, MATI
  // f4970293): colorSlot = TCH0 (the repeat noise, wrap repeat), indirectSlot = TCH1 (the macro map), both at
  // the TRSS uv; params = CCH0..4 (SMTR-animated from the Ship1 particle variables). TCH1 offsets the TCH0
  // read; their channels make a radial cloud mask and a shade over CCH2.rgb: rgb = (shade + CCH2.rgb) x
  // vc.rgb x PCMD, alpha the mask x vc.w (blend Alpha). No scene copy; Unfogged (DFOG 1).
  VisorSteam = 1u << 19,
  // VFX_IceChargeBeam (9e0605cd, perms 000_1/000_0): a model particle whose CPU feed (CPortVfxMeshBatch::Add) has
  // turned and shaken the mesh by the shader's hash and filled tan. colorSlot = BCLR, normalSlot = NMAP, params =
  // CCH0..2, uv[1].x = the mesh's COLOR.z, extra rows 0..2 = PMTR A, B, C. Unfogged (DFOG 1: perm 000).
  IceChargeBeam = 1u << 20,
  // VFX_IceChargeCollision_Unlit (1c651f97, perms 000_1/000_0 and fog perm 001): a model particle whose CPU feed
  // fills the view normal, tangent and handedness. colorSlot = BCLR, normalSlot = NMAP, envSlot = TCH0, params =
  // CCH0..6: the BCLR-wobbled NMAP bump refracts the scene copy (capture_scene, half-res, lod 0) by
  // (facing + BCLR.y) x min(|near / view z|, 0.2) x CCH2.w, desaturated by CCH3.x, plus the colour ramp, the TCH0
  // reflection and the CCH6 rim, x (CCH5.z / exposure + vc.rgb) x PCMD; alpha vc.a. Fogged unless Unfogged.
  IceChargeCollision = 1u << 21,
  // With IceChargeCollision: the draw is Remastered's perm 000 (DFOG 1), so the port's fog is not mixed in.
  Unfogged = 1u << 22,
  // VFX_VisorRunOff (b5e12668, FS perm 000): a model particle whose TCH0 (colorSlot) flow map, scrolled
  // by CCH1, gives a mask m and a glow; the scene copy (capture_scene) at the fragment, offset by
  // CCH0.x x CCH1.z x the map x 0.01, is tinted between CCH3.rgb and CCH4.rgb by m, and the result
  // scaled by a fade (CCH1.z x CCH3.w) x vc.rgb. params = CCH0..4 (SMTR-animated); alpha vc.a.
  VisorRunOff = 1u << 23,
  // VFX_VisorRunOff's second variant (d85d4169, the lit one: FS perm 000 with c1[0].x = 0.01): the
  // same feed and mask, but the scene is lerped toward the tint by m^CCH4.w instead of being
  // multiplied by it. Drawn Unfogged (the visor composite pass has no fog).
  VisorRunOffLit = 1u << 24,
  // VFX_IceChargeCollision_Lit (390fc33b, the lit perm of IceChargeCollision): the same feed and refraction
  // as IceChargeCollision, plus Remastered's clustered light gather — the 0xa30 constant block (c5[0..2]
  // header, then 32 unshadowed lights of 80 bytes) and its R32 mask texture, one 32-bit word per
  // (tile, slice) — and the baked probe: rgb = (vc.rgb * base * lit * BLCM) / pi + base * CCH5.z /
  // exposure, x PCMD, where lit = AMBS + the probe's three lobes + the mask's lights (build/
  // s-tiledlight-20261020/REPORT.md sections 3 to 5). Set with IceChargeCollision's slots (colorSlot,
  // normalSlot, envSlot) and its 7 params rows, and a DrawDesc::lit for the ambient and the probe.
  IceChargeCollisionLit = 1u << 25,
  // The lit perm of a VFX_*_Lit sprite shader (20e149b2 VFX_Color_Opacity_Lit and the converter's
  // IsLitTailShader list; perms 080_0..091_0): whatever the recipe's colour is, rgb x (AMBS + the baked
  // probe + the clustered lights) x BLCM / pi, alpha unchanged. N is the view normal from vec (toward the
  // eye for a sprite). Takes DrawDesc::lit and DrawDesc::cluster as IceChargeCollisionLit does.
  ParticleLit = 1u << 26,
  // Fogged whatever the blend: the fog follows the generator's PBDM/SBDM/EBDM row (SetupFogFromBlendMode), the
  // factors its MATI's EBlendMode, so a MATI additive or premultiplied draw over a fogged row keeps the fog.
  Fogged = 1u << 27,
  // VFX_VisorRain (0ec41df0, FS perm 000): the visor's rain splash, a sprite. TCH0 (colorSlot) is a
  // two-channel noise map read twice, at a per-particle patch (offset by PMTR row 0's xy, scaled by
  // CCH0.y) and at the constant texel (0, 0) (the roundEven sample lands on it across the whole
  // quad). The two channels warp a sum of sines into the drop mask H; 0.5 < H < 1 is inside the
  // drop, whose height Z = (H - 0.5) x 1.8 + 0.1 gives the normal n that offsets the scene copy
  // (capture_scene) at the fragment by (1 - fade) x CCH0.w x near / view depth, and the edge
  // darkening f = Z^4 x 10 + 0.75 + 0.25 x fade; elsewhere the fragment is discarded. The bigger
  // drops ("runners": PMTR row 1's y < CCH0.z and size > row 2's y) slide the pattern down by
  // min(2 x row 1's x, 0.5) x 0.33 and take their own fade (row 1's z; row 0's z otherwise).
  // rgb = f x scene x vc.rgb x PCMD, alpha vc.a. params = CCH0 (SMTR-animated), extra 0..2 = PMTR
  // rows 0..2. Unfogged: the visor composite pass has no fog.
  VisorRain = 1u << 28,
  // VFX_BarrierShield (f197ec38, the Parasite Queen barrier's MATI 6429c234: FS perm 002, the refraction
  // of the material flag MFC0 USE_FRAME_BUFFER_DISTORTION, "No displacement" perm; build/
  // s-queenbarrier-20261024/SHADER-IMPLEMENTATION.md): a model particle whose CPU feed (AddBarrierShield)
  // fills the mesh's own vertex colour (VMSH v2, flags bit 1) into Vertex::mesh, separate from the
  // instance colour. colorSlot = TCH0 (read at (-2u, 2v): the pattern's scroll .b and its .rg, .a),
  // indirectSlot = TCH1 (the two noise reads that make the mask), params = CCH0 (.x breakup, .y alpha
  // gain), time = the modular simulation time. The scene copy at the fragment, offset by the .45-period
  // sawtooth of 2 x (.rg - .5) + time x .008 (three samples: +0, -.002 and +.002, the first's .rg, the
  // second's .rb, the third's .r), is the base colour; the rim/fresnel/mesh-colour glow is added over it,
  // all x vc.rgb x PCMD. Unfogged: perm 002 carries DFOG 1 and the matted sibling perm 003 is not ported.
  BarrierShield = 1u << 29,
  // VFX_BarrierGlow (b1eb2b67, the same barrier's one-variable MATI ba0acbd6: FS perm 000, the
  // non-refraction perm, whose MATI carries neither MFC0 nor MFC1; build/
  // s-queenbarrier-20261024/GLOW-IMPLEMENTATION.md): a model particle with no scene copy. Its CPU feed
  // (AddBarrierGlow) is BarrierShield's (the mesh's own colour in Vertex::mesh, the inverse-transpose view
  // normal) plus the VS's displacement in the model coordinates: along the normal by
  // .065 (1 + 3 mc.r) max(sin(20 phase), -.33) with phase = time - 10 mc.b, and on Remastered +Y (the
  // port's model +Z, the mesh data is R2G) by .035 max(sin(20 (phase + 114)), -.33), the normal itself
  // undisplaced. colorSlot = TCH0 (three reads at (-u, v)), params = CCH0 (.x breakup), time = the
  // modular simulation time. rgb = rim x gain x vec3(k 1.125, k 7.5 - .5 mc.r + 1.4 p + .5,
  // k 30 - .9 mc.r + 3 p + .9) x vc.rgb x PCMD, alpha vc.a. Unfogged: perm 000 has DFOG 1 and no fog code.
  BarrierGlow = 1u << 30,
  // Not a shader feature: a VMAT blob's exposure policy, carried in its features word. The loader moves
  // it to DrawDesc::alreadyExposed; a draw ignores it in `features` (no shader or pipeline key sees it).
  AlreadyExposed = 1u << 31
};
// Multiply is port-only: the stand-in for the FrameBuffer_* shaders without a SceneWarp recipe
// (3db95827). The warp is dropped and the target is multiplied by mix(1, rgb, a) instead.
// AdditiveOne is the wave orb's model blend: RGB One/One, alpha Zero/One, drawn with depth write off.
// PremultipliedAlpha and AdditiveAlpha are Remastered's EBlendMode 5 and 6 (SBlendModeState 0xf0164, tables
// 0x1d0cc48/0x1d0cd38): One/InvSrcAlpha and SrcAlpha/One for colour and alpha alike, where the PBDM rows
// Premultiplied and Additive keep the target's alpha (Zero/One).
// The values are serialised in VMAT blobs (0..4 older, 5..7 new) and form part of the pipeline key.
enum class Blend : uint32_t {
  Alpha = 0,
  Premultiplied = 1,
  Additive = 2,
  Opaque = 3,
  Multiply = 4,
  AdditiveOne = 5,
  PremultipliedAlpha = 6,
  AdditiveAlpha = 7
};

// The highest Blend value a VMAT blob may carry.
inline constexpr uint32_t MaxBlend = uint32_t(Blend::AdditiveAlpha);

// A model draw of this blend never writes depth and always depth-tests (LEqual), whatever its generator asked for.
inline constexpr bool forces_depth_write_off(Blend blend) { return blend == Blend::AdditiveOne; }

// The depth state of a model draw: the generator's own (test = zTest, write = !AAPH && ZBUF) for every blend
// but AdditiveOne, which tests always and never writes.
struct DepthSelection {
  bool compare;
  bool write;
  constexpr bool operator==(const DepthSelection& o) const { return compare == o.compare && write == o.write; }
};
inline constexpr DepthSelection select_model_depth(Blend blend, bool zTest, bool aaph, bool zbuf) {
  if (forces_depth_write_off(blend))
    return {true, false};
  return {zTest, !aaph && zbuf};
}

struct Vertex {
  float pos[3];      // PNMTX0 space
  float uv[3][3];    // (u, v, layer) per UV set
  float color[4];    // rgb is HDR and unclamped
  float extra[4][4]; // PMTR rows 0..3
  float vec[3];      // the vector whose |z|/|v| feeds the fresnel term (view-space quad normal for sprites; the runtime fills it)
  // IceCharge*: the camera-local tangent in vec's (x, z, y) order (the GX view tangent is (x, y, -z)), w the
  // handedness with the model matrix's mirror folded in. Unused (zero) elsewhere.
  float tan[4];
  // BarrierShield: the mesh's own vertex colour (VMSH v2, flags bit 1), kept apart from `color` (the
  // instance's): the fragment's alpha gain and its glow read this one, and only the instance colour
  // scales the result. A full vec4 row so the two never alias, as Remastered passes them as v4 and v3.
  // Unused (zero) elsewhere.
  float mesh[4];
};
static_assert(sizeof(Vertex) == 172);

struct Texture {
  const GXTexObj* obj = nullptr;
  uint32_t uvSet = 0;       // 0..2: which Vertex.uv set
  uint32_t cols = 1, rows = 1, layers = 1; // layers > 1: the texture is a cols x rows atlas
  GXTexWrapMode wrapS = GX_CLAMP, wrapT = GX_CLAMP;
  bool linear = true;
  bool warped = false;      // the Indirect warp distorts this slot's uv
  float warpScale[2] = {};  // the warp's scale for this slot (from params0: .xy, or .zw for the second ramp)
};

// A per-particle scalar: row >= 0 reads Vertex.extra[row][comp], row = -1 uses value.
struct Src {
  int8_t row = -1;
  int8_t comp = 0;
  float value = 0;
};

// ---- The clustered light gather (IceChargeCollisionLit) ----
// Remastered's CClusteredLightingMaterialData block (build_clustered_light_material_data 0x3b8750), 0xa30
// bytes: c5[0..2] is a 12-float header — (1/pptX, 1/pptY, tilesX, tilesY), (1, slices/maxDepth, depth
// origin, maxDepth), then tilesX*tilesY and tilesX as int bits, the validity mask and the mask source —
// and c5[3] is the first of 32 records of 20 floats (80 bytes: colour x intensity, widen, view position,
// direction, A/B, zeros, the falloff cubic). The port's CPU gather is platform/port_clustered_lights.h,
// whose Frame::header and Frame::records are that block contiguously; these constants mirror its.
inline constexpr uint32_t ClusterHeaderFloats = 12;
inline constexpr uint32_t ClusterRecordFloats = 20;
inline constexpr uint32_t ClusterMaxLights = 32;
inline constexpr uint32_t ClusterBlockFloats = ClusterHeaderFloats + ClusterMaxLights * ClusterRecordFloats; // 652
// Where the block sits in the vfx uniform, in bytes: after every field an earlier recipe reads, so no
// existing shader's offsets move. The shader reads it as u.cluster (c5[i] = u.cluster[i]).
inline constexpr uint32_t ClusterBlockOffset = 768;
// The uniform's size: the fields before the block, then the block itself.
inline constexpr uint32_t VfxUniformSize = ClusterBlockOffset + 4 * ClusterBlockFloats; // 3376
// The mask texture: one R32Uint word per (tile, slice), tilesX*tilesY wide (at most 34*19) and 32 tall.
inline constexpr uint32_t ClusterMaskMaxWidth = 34 * 19;
inline constexpr uint32_t ClusterMaskHeight = 32;

// The clustered light gather a draw carries. Its block is copied into the uniform, its mask uploaded to
// the draw's frame slot; both are read only by IceChargeCollisionLit.
struct ClusterView {
  // ClusterBlockFloats floats: the header, then the 32 records (PortClusteredLights::Frame::header).
  const float* block = nullptr;
  // maskWidth x maskHeight words, texel (tileY * maskWidth + tileX, slice). Null: the shader reads a 1x1
  // dummy instead (maskWidth and maskHeight then 0).
  const uint32_t* mask = nullptr;
  uint32_t maskWidth = 0, maskHeight = 0;
};

// Whether the view can be bound: a block, and either no mask at all (a null mask with zero dimensions)
// or one sized 1..ClusterMaskMaxWidth by 1..ClusterMaskHeight. A draw with an invalid view binds the dummy
// mask and leaves the block zeroed.
constexpr bool valid_cluster_view(const ClusterView& view) noexcept {
  if (view.block == nullptr) {
    return false;
  }
  if (view.mask == nullptr) {
    return view.maskWidth == 0 && view.maskHeight == 0;
  }
  return view.maskWidth >= 1 && view.maskWidth <= ClusterMaskMaxWidth && view.maskHeight >= 1 &&
         view.maskHeight <= ClusterMaskHeight;
}

// Writes view.block into a uniform's cluster block (the bytes at ClusterBlockOffset). `uniform` holds at
// least VfxUniformSize / 4 floats; this is the copy draw_prims makes.
void write_cluster_block(const ClusterView& view, float* uniform);

// IceChargeCollisionLit's other input: Remastered's baked lighting for the particle, in the shape
// 390fc33b reads it (REPORT.md section 4). The shader evaluates, per colour channel c and for the
// fragment's view normal n,
//   mean[c] - lobe[c] + 2 * lobe[c] * (1 + s[c]) * q ^ (1 + 2 * s[c]),  q = sat(0.5 + 0.5 * dot(n, dir[c]))
// with s[c] = (dir[c][3] - 1) / 2 (the record's lane w is 1 + 2s), and adds it to ambient[] (AMBS,
// c6[0].xyz) and the clustered lights as the `lit` term. The baked light is radiance before the
// frame's exposure; the directions are view space, as the shader's normal is, and need not be unit.
struct LitAmbient {
  float ambient[3] = {};      // AMBS (c6[0].xyz): the flat ambient light colour
  float mean[3] = {};         // BLPD +0x00: the bake's midpoint
  float lobe[3] = {};         // BLPD +0x10: its range
  float dir[3][4] = {};       // BLPD +0x20/+0x30/+0x40: dir[3] is view space, dir[c][3] = 1 + 2 * sharpness
  // BLCM (c6[2]): the baked-light colour modulation. The IcePop MATI carries no such parameter, so the
  // port leaves it white; it rides here rather than in params[] so the 7 rows stay CCH0..CCH6.
  float modulate[3] = {1.f, 1.f, 1.f};
};

struct DrawDesc {
  uint32_t features = 0;
  Blend blend = Blend::Alpha;
  Texture tex[4];
  // Which tex[] a feature reads, -1 = none. colorSlot is also the ColorIndexing lookup.
  int8_t colorSlot = -1, opacitySlot = -1, rampSlot = -1, ramp2Slot = -1, thresholdSlot = -1, indirectSlot = -1,
         paletteSlot = -1;
  float modulate = 1.f;
  float depthSoften = 0.f; // DepthSoften's softness (SFTD): the view-space distance the fade spans; <= 0: none
  float opacityGain = 1.f, opacityBias = 0.f; // gain * clamp(smooth3(f) - bias, 0, 1)
  int8_t rampRow[2] = {0, 1}; // extra[] rows read whole as the ramp colours c0, c1
  int8_t addRow = 0;          // AddColor: the extra[] row (add.rgb, scale)
  Src erosion;
  Src thrX, thrY, thrW;       // Thresholding: p, q and the softness w
  Src fresnelX, fresnelY;     // OpacityFresnel: F.x, F.y
  Src fadeX, fadeY;           // OpacityFade: p.x, p.y
  Src indexScale, indexOffset, indexRow; // ColorIndexing: palette coordinate (i.x*s + o, z)
  // The colour is already exposed light: into the HDR scene it is written at exposure 1 instead of the
  // room's (the GX tone row's exposure, as every other draw takes it). An LDR draw ignores it.
  // Set only for the material the converter validated (the wave orb's MATI), not for VFX in general.
  bool alreadyExposed = false;
  // Explicit forward exposure for this draw's uniform only, never GX or pipeline state.
  // LDR ignores it; an AlreadyExposed HDR draw still takes exposure 1.
  std::optional<float> exposureOverride;
  // IceCharge*: the NMAP slot, Remastered's CCHn uniforms (MATI with the SMTR overrides) and its modular
  // simulation time (CScene::GetModularSimulationTime, the port's CGraphics::GetSimTime).
  int8_t normalSlot = -1;
  int8_t envSlot = -1; // IceChargeCollision: TCH0, the reflection map
  float params[7][4] = {};
  float time = 0.f;
  // IceChargeCollisionLit, ParticleLit: the clustered light gather's block and mask (see ClusterView).
  // Ignored by every other feature.
  ClusterView cluster;
  // IceChargeCollisionLit, ParticleLit: the baked ambient and probe (see LitAmbient). Ignored by every other feature.
  LitAmbient lit;
};

// Records quadCount quads (4 vertices each, drawn as two triangles) into the current pass with the
// projection, PNMTX0, depth state, fog and tone curve the GX state holds at this call. Waits for the
// FIFO to be processed first. The first use of a texture layout builds its array texture in an
// encoder task, and draws that need one that isn't built yet are skipped until it is.
void draw_quads(const DrawDesc& desc, const Vertex* verts, uint32_t quadCount);
// The same for triangles: 3 vertices each, drawn in order (no index sharing). Large batches are split
// into draws of MaxTrianglesPerDraw (a draw holds at most 65536 vertices; this is a multiple of 3).
constexpr uint32_t MaxTrianglesPerDraw = (1u << 14) * 4 / 3;
void draw_triangles(const DrawDesc& desc, const Vertex* verts, uint32_t triCount);
// Remastered's CopyOpaqueColorBuffer point, between the opaque and the sorted geometry: copies the
// EFB's depth (raw, full size) and its colour (half size with a mip chain; linear RGBA16F in the HDR
// scene, else the RGBA8 display copy) for the later draws of this frame to read, at one pass break.
// Call once per frame from the game thread. Draws of a frame without a copy, before it, or into
// another target (an offscreen pass) bind 1x1 dummies, do no DepthSoften and draw no SceneWarp.
// again: copy anew over this frame's copy of the same kind (the pause screen's copy before its
// silhouettes, RenderPauseScreenElements, flags 1); draws recorded after it read the new one, its
// top level alone.
void capture_scene(bool again = false);
// Whether draws recorded now would read this frame's copy (capture_scene ran this frame, into the
// current target, in the same HDR state). Game thread.
bool scene_copy_available();
// The pixel size of the target draws recorded now land in (the pass's colour attachment, else the
// window's framebuffer): the space a fragment's position is in. Waits for the FIFO first. Game thread.
void render_target_size(uint32_t& width, uint32_t& height);
void shutdown();
} // namespace aurora::gfx::vfx
