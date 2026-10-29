// The Remastered model converter's shader tables and material reader (ReadMaterial). See
// port_remastered_convert_internal.h.

#include "port_remastered_convert_internal.h"

namespace PortRemastered {
namespace ConvertDetail {

// The first four bytes of the id of a shader whose alpha is read below.
constexpr uint32_t kShaderHeightBlend = 0xCA10C453;  // snow and ice over rock
// Shaders with maps and parameters of their own (TCHn, CCHn), read from their code.
constexpr uint32_t kShaderUpLayer = 0x9EFE0D2E;   // TCH0-2 are a second layer on what faces up, CCH0.x its edge
constexpr uint32_t kShaderProjBlend = 0xD6AA2A3A;    // 9EFE0D2E's blend of TCH0-2 on what faces up (CCH0.x its edge), alpha lerp(BCLR.a, TCH0.a, w)
constexpr uint32_t kShaderDecalBlend = 0x17E458CD;   // a blended decal whose alpha is a smoothstep of TCH0.x about CCH0.x
constexpr uint32_t kShaderVertexBlend = 0xE9DF2188;  // TCH0-2 a second layer by the vertex alpha, CCH0.x its edge, TCH3 a detail map
constexpr uint32_t kShaderDetail = 0x9AB899E7;    // TCH0 is a detail map, on a texcoord of its own
constexpr uint32_t kShaderDetailTinted = 0x41A12C9E;  // the same with the vertex colour tint
constexpr uint32_t kShaderLava = 0x023388CD;      // the glow is CCH0.x times the vertex alpha
// Falling water, unlit: TCH0's three channels are sheets scrolling at CCH1's and CCH2's speeds
// (times CCH0.x), weighted by the vertex colour, and their sum picks the colour from TCH1, a
// ramp whose row is the vertex alpha. CCH0's y, z and w are the sum's softness and the ramp's
// offsets, CCH3 the colour.
constexpr uint32_t kShaderWaterfall = 0x50412FE7;
// 963E469B folds to the same output as 50412FE7 (same samplers, same params; kb material/963e469b.md): kind 7 too.
constexpr uint32_t kShaderWaterfall2 = 0x963E469B;
constexpr bool IsWaterfall(uint32_t shader) { return shader == kShaderWaterfall || shader == kShaderWaterfall2; }
constexpr uint32_t kShaderParallax = 0x2F3FB02B;  // TCH0 is seen inside the surface; CCH0 and CCH1.x say how
// Glass: the frame behind it, offset by TCH1's RG noise times CCH0.x and tinted by
// CCH1 x CCH3 x CCH4.x x CCH4.z, where TCH0's R (and the vertex blue) lets it through;
// TCH0's G plus the vertex green glows in CCH2 x CCH4.y, its B is the roughness and its
// A (times the vertex alpha) the opacity of a reflection CCH0.y and CCH1.w scale.
constexpr uint32_t kShaderGlass = 0x231F8383;
// A lava pool's surface (a LavaRenderVolume's model): BCLR is a colour ramp, TCH0 a pattern
// carried along TCH2's flow map in two phases that TCH1's noise offsets, CCH0 the flow's
// strength, its period in seconds and the brightness, CCH1 the maps' scales.
constexpr uint32_t kShaderLavaPool[] = {0x3ADE58B7, 0xB9C24545, 0xBB4CD79B};
// The arm cannon's beam glow (Wave, Plasma), over a lit surface: TCH0's three
// channels scroll at CCH1's and CCH2.xy's speeds (times CCH0.y) and, less twice
// the vertex colour, plus CCH0.w, pick a colour from TCH1, a ramp whose row is
// the vertex alpha and whose alpha scales it, times CCH0.z.
constexpr uint32_t kShaderGunGlow[] = {0xA13D6235, 0x62F671E0};
// The Ice Beam cannon's frost shell (kind 12), a dissolve: CCH1.y (PEMI) is the
// charge (CGunWeaponMP1::UpdateChargeEffects drives it from 0 to 1 and shows the
// shell only while it is above 0), TCH1's noise against it decides what is drawn,
// and the edge glows CCH1.x x CCH2. Without its maps it is not drawn: as a plain
// surface it froze the gun for good.
constexpr uint32_t kShaderFrozenShell = 0x2FC554A2;
// Ice (Model_IceSpreader: the Ice charge's shards, frozen nozzles; PBR kind 28): one
// light slot over 1/pi, its own REFL cube (x CCH5.x, tinted CCH2) with a light-luma
// specular occlusion, TCH0 as a frost layer seen a little below the surface (parallax
// CCH0.w) times CCH1.x, the base alpha squared and the vertex mask, plus a CCH3 rim.
constexpr uint32_t kShaderIceSpreader = 0x088E025E;
// A Metroid's dome: a matcap shell (PBR kind 13). Its glTF calls it
// opaque, so the vote put it on an opaque retail material, a solid white blob;
// it keeps retail's blended dome instead.
constexpr uint32_t kShaderMatcapShell = 0xC83E6FCD;
// The macro-normal shaders (a3c367be, 72b34e42, b9e899f3, 7feb9e94 without vertex colour, f84ac32d
// with MNMP on texcoord set 1; same whiteout fold, kb material/7feb9e94.md): NMAP in TANGENT_0's frame, then MNMP
// (kept in layer[kNormal], map 6) added to it in TANGENT_1's frame (mode bit 16384).
// 3c66aaef is d37039f7's two layers (both in TANGENT_0's frame) plus MNMP (kb material/3c66aaef.md): a layered
// material whose macro normal is an 8th map.
constexpr uint32_t kShaderLayeredMacro = 0x3C66AAEF;
constexpr uint32_t kShaderMacroNormal[] = {0xA3C367BE, 0x72B34E42, 0xB9E899F3, 0x7FEB9E94, 0xF84AC32D};
// The shaders whose fragment code multiplies ICAN x ICNC x INCI by the global system
// values' inverse tonemap exposure (c4[0].z; USE_INVERSEEXPOSURE, MFC4, which no
// material sets as a bit): heads, eyes, suits, pirates, creatures, the Metroid's body
// (b6268b63). The glow shows at that strength whatever the room's exposure, and the
// maps are authored dim for it (build/mpr/mtrl-tr/{b6268b63,ae819893}/NOTES.md). Read
// from the code (`re.sh fgrep`, build/mpr/invexp/), not from the pack's strings: 15
// more packs carry the define and never compile it in. 3392509e, dd387a18 and e1979471
// (Jellyzap, Flaahgra's lower body) match too but use c4[0] as their TransmissionColor and
// draw no glow, so they are left out. Sorted for binary_search.
constexpr uint32_t kShaderInverseExposure[] = {
    0x0A714D54, 0x17DD0A37, 0x1E462D99, 0x27A91776, 0x2835AB3B, 0x2E7A70CC, 0x2F540B27, 0x31E53C70,
    0x46CB52A7, 0x47908924, 0x50DB912E, 0x5104B751, 0x53AD3B78, 0x7342BCCE, 0x7A8C93DB, 0x7DFCA4A1,
    0x86B48EF1, 0x8A2049D9, 0x8C121639, 0x94DC56A5, 0x99370071, 0x9C023C70, 0x9DB310D1,
    0x9FD5E413, 0xA6D80F61, 0xAB87F3D5, 0xAE819893, 0xB6268B63, 0xBEDB1948, 0xC371A140,
    0xC72CA0EC, 0xC80BC2C1, 0xCB9736B8, 0xCE685F8A, 0xD2D3ACCA, 0xD6E5D629, 0xE5CA8683,
    0xEC024BAA, 0xEE64B773, 0xEFF78D93, 0xF0302441, 0xF55B835A, 0xFB2DE3A0,
};
// Of those, the ones compiled with USE_DIFFUSE_AS_INCAN_MASK: the glow is masked by the
// base map's alpha squared (the pirate trooper's limbs, bedb1948), whatever the
// material's own 0x200 flag says, and the base alpha is no opacity. Sorted.
constexpr uint32_t kShaderIncanMaskSquared[] = {
    0x0A714D54, 0x31E53C70, 0x46CB52A7, 0x53AD3B78, 0x86B48EF1, 0x8A2049D9, 0x99370071,
    0x9DB310D1, 0xA6D80F61, 0xBEDB1948, 0xC80BC2C1, 0xD6E5D629, 0xE5CA8683, 0xF0302441,
    0xF55B835A,
};
// The arm cannon's body: lit PBR that reflects its REFL cube (a Tallon forest, LDR) along
// the reflection vector, at the roughness's mip, instead of a room probe. Drawn with the
// room's cube the metal took the room's colours and read pale. The default REFL (black)
// that most materials carry keeps the room's. A469DAAD (the third-person insets) and
// 0C22AB97 (its barrel) are 917F1415's rigid twins: the same fragment code, unskinned.
constexpr uint32_t kShaderGunBody[] = {0x547E64E5, 0xD43DE005, 0x917F1415, 0xF495B260, 0xA469DAAD, 0x0C22AB97};
// The beam panels inside the Wave, Plasma and Phazon guns (lightning, magma, phazon): a
// black base whose alpha masks an ICAN glow of ICNC 4-7. Only those three use the shader.
// Under kPbrEmissive the magma read as faint dark red.
constexpr uint32_t kShaderGunPanel = 0x9A4515CE;
constexpr const char* kDefaultRefl = "7b98170f";
// The pause screen's visor (InvVisor, kb material/4c37625c.md, 17dd0a37.md): 4c37625c is 788360de's
// glass with a coverage C (COVG) and the pulse in DIFC/ICMC (kind 36); 17dd0a37 the lit additive
// layer over it (kind 37). Both keep their REFL cube 267ccd84.
constexpr uint32_t kShaderInventoryGlass = 0x4C37625C;
constexpr uint32_t kShaderInventoryAdd = 0x17DD0A37;
// Glass_DX11 (HoloGlass; only the Waste Disposal tank): the scene behind it, bent by TCH2
// and tinted, plus the reflection, BCLR and the vertex colour, blended with a second
// colour output (build/mpr/glass/NOTES.md). The port draws it from the screen copy.
constexpr uint32_t kShaderHoloGlass = 0x03407341;
// BoundaryShield_Ship1_DX11 (the Frigate's orange force fields; build/mpr/shield/NOTES.md): unlit,
// animated noise through two maps, the screen copy bent by it. PBR kind 14, with its CCH0..6 and DIFC
// carried in the record's PBR8 trailer.
constexpr uint32_t kShaderBoundaryShield = 0x6C1A4768;
// ForceField (6F53C775: the cyan Exterior Docking Hangar barrier 982D9D75, the Mines' orange fields):
// the same pixel and vertex code as 6C1A4768 (perms 002_0/002_1/004_0 fold identically), only a
// different shader id, so it takes the same kind.
constexpr uint32_t kShaderForceField = 0x6F53C775;
constexpr uint32_t kShaderEnergyField = 0x9C867F3C;
// PickUp (PickUp_Blue_Mat / PickUp_Orange_Mat, build/mpr/artifact/NOTES.md): unlit, a normal-map
// fresnel pair over a gradient that scrolls through world space. PBR kind 15, constants in the
// same PBR8 trailer (rows 0-3 CCH0..CCH3, row 7 DIFC, rows 4-5 filled at run time).
constexpr uint32_t kShaderPickUp = 0x3E95A9FE;
constexpr uint32_t kShaderIndirect = 0x8CE05ED0;
constexpr uint32_t kShaderGunFx = 0x98F0556D;
// dda64c97 (stored id 974ca6dd), the Eyon's eyeball_gloss: bfb300b6's actor lighting (normal map, GGX) with a
// matcap term, REFV x REFS(N'.xy) x luminance(L), in place of the reflection. Kind 25 (kb material/dda64c97.md).
constexpr uint32_t kShaderEyeGloss = 0x974CA6DD;
// 7cf91c66 (variasuit Eyeball_Exterior) is the same lighting and matcap. Its glow is DMGI(CCH2.y) x ICMC + ICNC (c4[0].y x
// c5[1] + c4[1]; c5[0] is DIFC), which is zero in all 11 materials whatever DMGI is: nothing to drive (kb material/7cf91c66.md).
constexpr uint32_t kShaderEyeGloss2 = 0x7CF91C66;
// f16bfffc (Thermal/X-ray VisorGlow) is the same matcap lighting plus an ICAN term (kb material/f16bfffc.md).
constexpr uint32_t kShaderEyeGloss3 = 0xF16BFFFC;
// 8c7a939f (the spider ball's glass without ICAN, kb material/8c7a939f.md) is dda64c97's maths exactly: ambient + lights,
// REFV x REFS(N'.xy) x lum(L), the 1/pi diffuse of BCLR x DIFC weighted by METL, unmasked GGX, ICNC + ICMC, alpha
// BCLR.a^2 x DIFC.a. Its light scale c4[5] is unverified and taken as 1, as dda64c97's.
constexpr uint32_t kShaderEyeGloss4 = 0x8C7A939F;
constexpr bool IsEyeGloss(uint32_t shader) {
  return shader == kShaderEyeGloss || shader == kShaderEyeGloss2 || shader == kShaderEyeGloss3 || shader == kShaderEyeGloss4;
}
// 1fbafbca (the Varia spider ball's glass, kb material/1fbafbca.md): 8c7a939f plus ICAN x ICNC. Kind 42.
constexpr uint32_t kShaderBallGlass = 0x1FBAFBCA;
// 5854e91b, the unlit Surface with a reflection cube, and bda080c0, the same with a normal map: kind 38
// (kb material/5854e91b.md, bda080c0.md).
constexpr uint32_t kShaderUnlitCube = 0x5854E91B;
constexpr uint32_t kShaderUnlitCubeNormal = 0xBDA080C0;
// af330de4 (the zoomer's blinn1/blinn90; kb material/af330de4.md): kind 16's unlit DIFT x DIFC + ICNC + ICMC with
// the vertex colour on the colour and the alpha (kind 39).
constexpr uint32_t kShaderUnlitVertex = 0xAF330DE4;
// 664ee5ca (the zoomer's blinn2; kb material/664ee5ca.md): kind 19's matcap without the light, DIFT x DIFC + REFS x
// REFV + ICNC + ICMC (kind 40).
constexpr uint32_t kShaderUnlitMatcap = 0x664EE5CA;
// 67053ffe (the gun fx CCFE3C49's CAdd layer; kb material/67053ffe.md): unlit METL-weighted BCLR x DIFC + ICAN x ICNC +
// ICMC (kind 41).
constexpr uint32_t kShaderUnlitAdd = 0x67053FFE;
// Unlit, the vertex colour times the base map (a door shield's noise), which
// scrolls at (CCH0.y, -CCH0.z) a second over texcoords scaled by CCH1.yz. Its
// vertex shader linearises the colour and doubles it (2 pow(|c|, 2.2)), the base
// map is sRGB, and the pixel shader scales the product by CCH0.x x CCH1.x; the
// alpha is the base map's times the vertex's, not squared.
constexpr uint32_t kShaderColorUnlit = 0x992941B7;
// Phazon3 (cc96c27d, the Phazon Mines' stone): a lit surface whose albedo and glow are a gradient ramp (TCH0)
// read at the normal map's rim, where the vertex alpha and BCLR's alpha say, pulsed by a mask (TCH1) and the
// time (kind 21). The vertex colour is raw data (alpha and red weights, blue a cavity mask), no tint.
constexpr uint32_t kShaderPhazon = 0xCC96C27D;
// The same family's other member (9e52aa74, 0x100010 RLTG stone of the Phazon Mines and the crater's flesh;
// kind 22): no mask scale, the phase is sin(mask + time), a second ramp read gives the cavity glow's alpha term,
// and the glow takes no exposure factor.
constexpr uint32_t kShaderPhazonB = 0x9E52AA74;
// PhazonPool (07acff46, the Phazon Mines' blisters, FNLI/EMSI/FNLS; kind 32): a lit PBR surface whose normal
// strength and roughness follow an animated 3D noise (TCH0, a 64^3 volume) and whose glow is a fresnel-driven ramp
// (TCH1), see kb material/07acff46.md.
constexpr uint32_t kShaderPhazonPool = 0x07ACFF46;
// Phazon_Veins_Mat (d61ae63a; kind 35), the Phazon Beam's veins: an opaque lit PBR surface with a disintegration
// mask (TCH2) eaten by the "DisintegrationAmount" variable (DIFC.w), a glow (TCH0) and an edge ramp (TCH1), kb
// material/d61ae63a.md.
constexpr uint32_t kShaderPhazonVeins = 0xD61AE63A;
// Distortion2 (24670bf0, the Phendrana ice walls, the crater's flesh glass; kind 23): refracts a mipped copy of
// the frame by its normal map and a fresnel term, tinted by the base map and the vertex colour (kb material/24670bf0.md).
constexpr uint32_t kShaderRefractGlass = 0x24670BF0;
// The same body from the baked-probe pack (cc8afd0f, 31 materials: Phendrana ice walls and waterfalls; kind 30):
// the scene term is v3.w x scene, without the base map and vertex colour tint (kb material/cc8afd0f.md).
constexpr uint32_t kShaderHoloGlassC = 0x2F95A061;  // HoloGlass's twin: the bent room tinted by CCH1, dual-source blended
constexpr uint32_t kShaderHoloGlassB = 0x3991DA00;  // HoloGlass: scrolling indirect layers over a distorted screen copy
// E2644678, the Ore Processing pillar glass (kb material/e2644678.md): 3991da00's body, but its vertex shader scales
// the phase by CCH4.x and the screen offset by INDP.w (the draw camera's near plane) over the depth, capped at a
// shipped 0.02, and it reflects the camera's probe (ResolveReflectionProbe), never a cube of the material's own.
constexpr uint32_t kShaderHoloGlassE264 = 0xE2644678;
// 5586D988 VS000: static hologram with green-selected apex collapse and X/Z fade.
constexpr uint32_t kShaderRouteHologram = 0x5586D988;
// 9d53cf06 (ChozoLore_Mat_B, the lore statues' sheet): unlit, premultiplied, its vertices waved along the normal.
constexpr uint32_t kShaderChozoLore = 0x9D53CF06;
constexpr double kHoloGlassE264DepthCap = 0.019999999552965164;  // vp_c1[0].x, binary32 0x3ca3d70a
constexpr uint32_t kShaderRefractGlassB = 0xCC8AFD0F;
// SimpleGlass_DX11_1 (kind 43, kb material/084a8db5.md): HoloGlass (3991da00) without TCH1, TCH2 and DIFC. TCH0
// bends both the scrolled BCLR and the screen copy, whose blur follows the scrolled BCLR's alpha.
constexpr uint32_t kShaderSimpleGlass = 0x084A8DB5;
// FB2BC671 and DF3E3423 (the ids as the reports print them), the ChozoGhost's X-ray materials, static and skinned
// (kind 31, kb material/fb2bc671.md, df3e3423.md).
constexpr uint32_t kShaderXrayStatic = 0xFB2BC671;
constexpr uint32_t kShaderXraySkinned = 0xDF3E3423;
// Shaders that are verbatim copies of one the port already handles: every permutation, vertex and fragment, has the
// same code (build/mpr/twins/twins.py, exact_twins; kb topic/shader-twins.md). They take the twin's kind and mode.
struct ShaderTwin {
  uint32_t copy;
  uint32_t of;
};
constexpr ShaderTwin kShaderTwins[] = {
    {0x88E65647, kShaderIndirect},  {0x1DD39C60, kShaderRefractGlassB}, {0x94438883, kShaderXrayStatic},
    {0x7126CA03, kShaderLambertFx}, {0x0A23705E, kShaderPickUp},        {0x24F4B1FE, kShaderPickUp},
    {0x9B6E0881, kShaderIceSpreader}, {0xCD6C8057, kShaderHoloGlassB},
    // The skinned variants: identical fragment shaders; the vertex shaders add the bone-matrix fetch (vp_c5, indices in
    // v5, weights in v6) and renormalise the skinned N and T (T1 too), nothing else. The port skins on its side
    // (DolphinCSkinRules.cpp, FinishTangentFrame, incl. the second frame).
    {0xC36C5166, kShaderIceSpreader}, {0xB55635D2, 0xA978D507}, {0xEF94ACCF, kShaderLava},
    // 81b8e136 (thardus_test_shader3, Thardus and his rocks only): every one of its 57 perms, vertex and fragment,
    // is byte-identical to an a978d507 perm, with the same bindings; only the lightmap perms are left out, which
    // its actor-only users never select.
    {0x81B8E136, 0xA978D507},
    // 1ac34e37 (Core_Phazon_Mat) is 07acff46's skinned copy, as the others.
    {0x1AC34E37, kShaderPhazonPool},
    // The fx shaders of kb material/{96009286,a5bd3ed7,ed8ab6e7,937a8c35}.md routed to the kind they match (a5bd3ed7 and
    // ed8ab6e7 differ in AUVI only; 937a8c35 has no MTRL: inferred).
    {0x96009286, kShaderRefractGlassB}, {0xA5BD3ED7, kShaderPickUp}, {0xED8AB6E7, kShaderPickUp},
    {0x937A8C35, kShaderBoundaryShield},
};
uint32_t ShaderFamily(uint32_t shader) {
  for (const ShaderTwin& t : kShaderTwins)
    if (t.copy == shader) return t.of;
  return shader;
}

// These source-confirmed lightmapped shader families read TEXCOORD_1.zw, which
// the converter flattens to logical coord 3. Other families keep the established
// TEXCOORD_0.zw (logical coord 1) default. Call this with RemMaterial::family so
// exact shader twins inherit their source family's coordinate rule.
constexpr uint32_t kLightmapCoord3Families[] = {
    // Door frames tile ICAN on TEXCOORD_0.zw; their independent baked atlas UV is
    // TEXCOORD_1.zw. Reusing the glow coordinates samples unlit atlas regions.
    // 131ED7F0 is the same shader bar its alpha, and reads the same coordinates.
    0xC78982B4, 0x131ED7F0,
    0x41A12C9E, 0x72B34E42, 0x9AB899E7, 0xA978D507, 0xB9E899F3,
    0xD363D694, 0xD37039F7, 0xD6AA2A3A, 0xDF0677FF, 0xDFEC1A87,
    0xE538B757, 0xE9DF2188, 0xF22FEB5B, 0x557A7DE4, 0xF84AC32D,
};

size_t LightmapSourceCoordinate(uint32_t family) {
  for (const uint32_t coord3Family : kLightmapCoord3Families) {
    if (family == coord3Family) {
      return 3;
    }
  }
  return 1;
}

// DFC2A7DC folds to exactly DF3E3423's perm 002_0 (same registers, same outputs; kb material/dfc2a7dc.md).
constexpr uint32_t kShaderXraySkinned2 = 0xDFC2A7DC;
constexpr bool IsXraySkinned(uint32_t shader) { return shader == kShaderXraySkinned || shader == kShaderXraySkinned2; }
constexpr uint32_t kShaderXrayMap = 0x65E90E82;
// e84190e0 (kb material/e84190e0.md) is the same X-ray fresnel/depth-fade material as fb2bc671.
constexpr uint32_t kShaderXrayStatic2 = 0xE84190E0;
// 0c0b6119 (xray_ice / XRAYLava, kb material/0c0b6119.md) folds to fb2bc671's maths (rim, depth fade, (rim + ICMC) x
// c6[0], clustered GGX lights); its c6[0] is DIFC (inferred from the bind names) where fb2bc671's is ICNC.
constexpr uint32_t kShaderXrayRim = 0x0C0B6119;
constexpr bool IsXrayGhost(uint32_t shader) {
  return shader == kShaderXrayStatic || shader == kShaderXrayStatic2 || shader == kShaderXrayRim ||
         IsXraySkinned(shader);
}
// The decal cutouts (kind 24; kb material/e538b757.md, 42fe2ed0.md): a bfb300b6 surface cut by a smoothstep of an
// alpha A about CCH0.x = c (0.5 on all of them): discard where t²(3-2t) < 0.25, t = (A - 0.5 + c) / 2c; output
// alpha 1, no vertex colour. e538b757 cuts on BCLR.a², 42fe2ed0 on TCH0.r (its alpha map, in the emissive slot).
constexpr uint32_t kShaderDecalCut = 0xE538B757;
// A shader with parameters of its own (TCHn, CCHn) reads the vertex colour as
// it likes: masks for its extra maps, a colour seen through ice. These are the
// ones read that multiply the albedo by it, as the standard shader does
// (992941B7 is ColorUnlit: a door shield's blue is all vertex colour).
constexpr uint32_t kShaderTints[] = {0x9EFE0D2E, 0xCA10C453, 0x17E458CD, 0xE9DF2188,
                                     0x41A12C9E, 0xD6AA2A3A, 0x992941B7, kShaderPhazonVeins};

// Which of a Remastered material's parameters feed the four maps, and its two
// strengths. Later parameters replace earlier ones, as in the reference.
// The names of the shader constants and lists a shader is in ("-" for none), for the materials report.
std::string ShaderRole(uint32_t shader) {
  std::string out;
  auto add = [&](bool in, const char* name) {
    if (in) {
      out += (out.empty() ? "" : "+") + std::string(name);
    }
  };
  auto in = [&](const auto& list) { return std::find(std::begin(list), std::end(list), shader) != std::end(list); };
  add(shader == kShaderHeightBlend, "height-blend");
  add(shader == kShaderUpLayer, "up-layer");
  add(shader == kShaderProjBlend, "projected-blend");
  add(shader == kShaderDecalBlend, "decal-alpha");
  add(shader == kShaderDetail || shader == kShaderDetailTinted, "detail");
  add(shader == kShaderVertexBlend, "vertex-blend");
  add(shader == kShaderLava, "lava");
  add(IsWaterfall(shader), "waterfall");
  add(shader == kShaderParallax, "parallax");
  add(shader == kShaderGlass, "glass");
  add(in(kShaderLavaPool), "lava-pool");
  add(in(kShaderGunGlow), "gun-glow");
  add(shader == kShaderFrozenShell, "frozen-shell");
  add(shader == kShaderIceSpreader, "ice");
  add(shader == kShaderMatcapShell, "matcap-shell");
  add(in(kShaderInverseExposure), "inverse-exposure");
  add(in(kShaderIncanMaskSquared), "incan-mask-squared");
  add(in(kShaderGunBody), "gun-body");
  add(shader == kShaderGunPanel, "gun-panel");
  add(in(kShaderPremulGlass), "premul-glass");
  add(shader == kShaderBoundaryShield || shader == kShaderForceField, "boundary-shield");
  add(shader == kShaderPickUp, "pickup");
  add(shader == kShaderHolo, "holo");
  add(shader == kShaderHoloRefl, "holo-refl");
  add(shader == kShaderHologram, "hologram");
  add(shader == kShaderEnergyField, "energy-field");
  add(shader == kShaderChozoLore, "chozo-lore");
  add(shader == kShaderLambertFx, "lambert-fx");
  add(shader == kShaderGunFx, "gun-fx");
  add(IsEyeGloss(shader), "eye-gloss");
  add(shader == kShaderBallGlass, "ball-glass");
  add(shader == kShaderColorUnlit, "color-unlit");
  add(shader == kShaderPhazon, "phazon");
  add(shader == kShaderPhazonB, "phazon-b");
  add(shader == kShaderPhazonPool, "phazon-pool");
  add(shader == kShaderPhazonVeins, "phazon-veins");
  add(shader == kShaderRefractGlass || shader == kShaderRefractGlassB, "refract-glass");
  add(shader == kShaderHoloGlassB || shader == kShaderHoloGlassC || shader == kShaderHoloGlassE264, "holo-glass-b");
  add(shader == kShaderDualGlass, "dual-glass");
  add(shader == kShaderSimpleGlass, "simple-glass");
  add(shader == kShaderInventoryGlass, "inventory-glass");
  add(shader == kShaderInventoryAdd, "inventory-add");
  add(IsXrayGhost(shader), "xray-ghost");
  add(shader == kShaderXrayMap, "xray-map");
  add(shader == kShaderDecalCut || shader == kShaderDecalAlphaMap, "decal-cut");
  add(in(kShaderTints), "tinted");
  return out.empty() ? "-" : out;
}

const char* KindName(int kind) {
  switch (kind) {
  case 0: return "standard";
  case 1: return "up-layer";
  case 2: return "detail";
  case 3: return "lava";
  case 4: return "parallax";
  case 6: return "lava-pool";
  case 7: return "waterfall";
  case 8: return "glass";
  case 9: return "gun-glow";
  case 10: return "premul-glass";
  case 11: return "holo-glass";
  case 12: return "frozen-shell";
  case 13: return "matcap-shell";
  case 14: return "boundary-shield";
  case 15: return "pickup";
  case 16: return "holo";
  case 17: return "holo-refl";
  case 18: return "hologram";
  case 19: return "gun-fx";
  case 20: return "vertex-blend";
  case 21: return "phazon";
  case 22: return "phazon-b";
  case 23: return "refract-glass";
  case 24: return "decal-cut";
  case 25: return "eye-gloss";
  case 26: return "projected-blend";
  case 27: return "decal-alpha";
  case 28: return "ice";
  case 29: return "holo-glass-b";
  case 30: return "refract-glass-b";
  case 31: return "xray-ghost";
  case 32: return "phazon-pool";
  case 33: return "xray-map";
  case 34: return "dual-glass";
  case 35: return "phazon-veins";
  case 36: return "inventory-glass";
  case 37: return "inventory-add";
  case 38: return "unlit-cube";
  case 39: return "unlit-vertex";
  case 40: return "unlit-matcap";
  case 41: return "unlit-add";
  case 42: return "ball-glass";
  case 43: return "simple-glass";
  case 44: return "energy-field";
  case 45: return "route-hologram";
  case 46: return "chozo-lore";
  default: return "kind?";
  }
}

// A shader's DIFC, ICNC and ICMC constants (white, black and black where the material has none).
struct ShaderColors {
  double difc[4] = {1.0, 1.0, 1.0, 1.0};
  double icnc[3] = {0.0, 0.0, 0.0};
  double icmc[3] = {0.0, 0.0, 0.0};
};
ShaderColors ReadShaderColors(const ModelMaterial& mat) {
  ShaderColors c;
  for (const ModelMaterialData& d : mat.data) {
    if (d.kind != ModelMaterialData::Kind::Color) {
      continue;
    }
    if (d.usage == FourCC('D', 'I', 'F', 'C')) {
      for (int i = 0; i < 4; ++i) {
        c.difc[i] = ShortestDouble(d.color[i]);
      }
    } else if (d.usage == FourCC('I', 'C', 'N', 'C') || d.usage == FourCC('I', 'C', 'M', 'C')) {
      double* row = d.usage == FourCC('I', 'C', 'N', 'C') ? c.icnc : c.icmc;
      for (int i = 0; i < 3; ++i) {
        row[i] = ShortestDouble(d.color[i]);
      }
    }
  }
  return c;
}

RemMaterial ReadMaterial(const ModelMaterial& mat, const ConvertOptions& opt) {
  RemMaterial out;
  out.name = mat.name;
  out.flags = mat.unk1;
  out.cutout = (mat.unk1 & kCutoutFlag) != 0;
  out.blended = (mat.unk1 & kTransparentFlag) != 0 && !out.cutout;
  uint8_t sid[4];
  std::memcpy(sid, &mat.shaderId, 4);
  const uint32_t rawShader = port::ReadBE32(sid);
  const uint32_t shader = ShaderFamily(rawShader);
  out.shader = rawShader;
  out.family = shader;
  out.role = ShaderRole(shader);
  out.shell = shader == kShaderMatcapShell;
  out.shield = shader == kShaderBoundaryShield || shader == kShaderForceField || shader == kShaderPickUp || shader == kShaderHolo ||
               shader == kShaderHoloRefl || shader == kShaderHologram || shader == kShaderRouteHologram || shader == kShaderGunFx || IsEyeGloss(shader) ||
               IsXrayGhost(shader) || shader == kShaderXrayMap;
  bool custom = false;
  for (const ModelMaterialData& d : mat.data) {
    const uint32_t family = d.usage & 0xFFFFFF00u;
    custom = custom || family == (FourCC('T', 'C', 'H', '0') & 0xFFFFFF00u) ||
             family == (FourCC('C', 'C', 'H', '0') & 0xFFFFFF00u);
  }
  // Premultiplied glass multiplies its albedo by the vertex colour and its alpha by the
  // vertex alpha, which is all the opacity a pane has (its base map is a flat grey, alpha 1).
  const bool premulGlass =
      std::find(std::begin(kShaderPremulGlass), std::end(kShaderPremulGlass), shader) != std::end(kShaderPremulGlass);
  // Plain MFVC shaders (882014EE, F22FEB5B, the layered ones) multiply only the diffuse;
  // every kShaderTints shader multiplies the albedo, so its F0 is tinted too.
  out.tintF0 = std::find(std::begin(kShaderTints), std::end(kShaderTints), shader) != std::end(kShaderTints);
  out.tinted = (mat.unk1 & kVertexColorFlag) != 0 && (!custom || premulGlass || out.tintF0);
  // What the alpha a shader writes is made of is only in its code. Every one
  // read writes the base map's alpha squared, times the vertex alpha, bar these:
  // with the mask flag the base alpha scales the glow instead and the vertex
  // alpha alone is the opacity, a material in no lit pass (screens, holograms)
  // is its own colour and glow, a framebuffer one (ice, glass) writes alpha 1
  // and refracts what is behind it (the port does that for glass, kind 8), and
  // the snow shader cuts its edge by height.
  out.maskSquared = std::binary_search(std::begin(kShaderIncanMaskSquared), std::end(kShaderIncanMaskSquared), shader);
  out.mask = (mat.unk1 & kIncanMaskFlag) != 0 || out.maskSquared;
  out.unlit = mat.types.empty();
  bool lit = false, framebuffer = false;
  for (uint32_t type : mat.types) {
    lit = lit || type == FourCC('R', 'L', 'T', 'G');
    framebuffer = framebuffer || type == FourCC('R', 'F', 'B', 'P');
  }
  if (out.mask) {
    out.blended = out.blended && out.tinted;
  }
  if (framebuffer && lit) {
    out.blended = false;
  }
  if (shader == kShaderHeightBlend) {
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', '0')) {
        out.height = std::clamp(ShortestDouble(d.color[0]), 0.01, 4.0);
      }
    }
  }
  auto set = [&](int k, const ModelTextureRef& t, MapRef* into = nullptr) {
    if (!t.hasUsage) {
      return;
    }
    MapRef& m = into ? *into : out.maps[k];
    m.has = true;
    // A model stores the id with its first three groups little endian; a pak,
    // and the printed form, have them the other way round.
    m.id = t.id;
    std::swap(m.id[0], m.id[3]);
    std::swap(m.id[1], m.id[2]);
    std::swap(m.id[4], m.id[5]);
    std::swap(m.id[6], m.id[7]);
    m.coord = t.texCoord;
    m.wrap[0] = t.wrapX;
    m.wrap[1] = t.wrapY;
    m.src = opt.texturePrefix + IdToString(m.id) + opt.textureSuffix;
  };
  bool bclr = false;
  bool refl = false;
  MapRef incanMask;
  const ModelMaterialData* icnc = nullptr;
  const ModelMaterialData* bklt = nullptr;
  const ModelMaterialData* bkla = nullptr;
  for (const ModelMaterialData& d : mat.data) {
    if (shader == kShaderIndirect && d.usage == FourCC('I', 'N', 'D', 'S')) {
      out.indStrength = d.kind == ModelMaterialData::Kind::Scalar ? d.scalar : d.color[0];
    }
    if (shader == kShaderIndirect && d.usage == FourCC('I', 'N', 'D', 'I') && d.kind == ModelMaterialData::Kind::Texture &&
        d.texture.hasUsage) {
      // The offset map is data (its xy are the offset), on its own texcoord set.
      set(kMr, d.texture);
      out.maps[kMr].raw = true;
      out.indirect = out.unlit;
    }
    const bool texture = d.kind == ModelMaterialData::Kind::Texture;
    const bool layered = d.kind == ModelMaterialData::Kind::LayeredTexture;
    switch (d.usage) {
    case FourCC('D', 'I', 'F', 'T'):
      // A material with a base colour map keeps a legacy diffuse slot beside
      // it, holding a 1x1 white default: the base colour map is the albedo.
      if (texture && !bclr) {
        set(kBase, d.texture);
      }
      break;
    case FourCC('B', 'C', 'L', 'R'):
      if (texture) {
        set(kBase, d.texture);
        bclr = d.texture.hasUsage;
      }
      break;
    case FourCC('R', 'E', 'F', 'L'):
      refl = refl || texture;
      if (texture && (shader == kShaderIceSpreader || shader == kShaderHoloRefl || shader == kShaderInventoryGlass ||
                      shader == kShaderInventoryAdd || shader == kShaderUnlitCube || shader == kShaderUnlitCubeNormal)) {
        set(kBase, d.texture, &out.refl);
        if (Lower(IdToString(out.refl.id)).rfind(kDefaultRefl, 0) == 0) {
          out.refl = MapRef{};
        }
      } else if (texture && std::find(std::begin(kShaderGunBody), std::end(kShaderGunBody), shader) != std::end(kShaderGunBody)) {
        set(kBase, d.texture, &out.refl);
        if (Lower(IdToString(out.refl.id)).rfind(kDefaultRefl, 0) == 0) {
          out.refl = MapRef{};
        }
        // The metalness keeps its full range, whether the metal shows its own cube or
        // the room's: under the ceiling the pale albedo was lit as diffuse by the bright
        // ambient around the gun and read washed-out teal, where Remastered's is charcoal.
        out.maps[kMr].metalMax = 1.0;
      }
      break;
    case FourCC('R', 'E', 'F', 'S'):
    case FourCC('R', 'E', 'F', 'V'):
      // 98F0556D's sphere map and reflectivity map: the second layer's base and MR (kind 19). 664ee5ca's (kind 40)
      // the same; which of its two textures is REFS is inferred from the matcap coordinate (kb), not verified.
      if (texture && (shader == kShaderGunFx || IsEyeGloss(shader) || shader == kShaderBallGlass || shader == kShaderUnlitMatcap)) {
        const bool sphere = d.usage == FourCC('R', 'E', 'F', 'S');
        MapRef& m = sphere ? out.layer[kBase] : out.layer[kMr];
        set(sphere ? kBase : kMr, d.texture, &m);
        m.raw = true;
      }
      break;
    case FourCC('B', 'C', 'R', 'L'):
      if (layered) {
        set(kBase, d.layeredTextures[0]);
        set(kBase, d.layeredTextures[1], &out.layer[kBase]);
      }
      break;
    case FourCC('L', 'I', 'T', 'S'):
      if (d.kind == ModelMaterialData::Kind::Scalar) {
        out.hasLits = true;
        out.lits = ShortestDouble(d.scalar);
      }
      break;
    case FourCC('B', 'L', 'S', 'M'):
      if (d.kind == ModelMaterialData::Kind::Scalar) {
        out.layerSmooth = ShortestDouble(d.scalar);
      }
      break;
    case FourCC('B', 'S', 'A', 'O'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        for (int i = 0; i < 4; ++i) {
          out.layerHeight[i] = ShortestDouble(d.color[i]);
        }
      }
      break;
    case FourCC('M', 'E', 'T', 'L'):
      if (texture) {
        set(kMr, d.texture);
      }
      break;
    case FourCC('M', 'T', 'L', 'L'):
      if (layered) {
        set(kMr, d.layeredTextures[0]);
        set(kMr, d.layeredTextures[1], &out.layer[kMr]);
      }
      break;
    case FourCC('N', 'M', 'A', 'P'):
      if (texture) {
        set(kNormal, d.texture);
      }
      break;
    case FourCC('M', 'N', 'M', 'P'):
      if (texture && shader == kShaderLayeredMacro) {
        set(kNormal, d.texture, &out.macroMap);
        out.macroLayered = out.macroMap.has;
      } else if (texture && std::find(std::begin(kShaderMacroNormal), std::end(kShaderMacroNormal), shader) !=
                                std::end(kShaderMacroNormal)) {
        set(kNormal, d.texture, &out.layer[kNormal]);
        out.macro = out.layer[kNormal].has;
      }
      break;
    case FourCC('N', 'R', 'M', 'L'):
      if (layered) {
        set(kNormal, d.layeredTextures[0]);
        set(kNormal, d.layeredTextures[1], &out.layer[kNormal]);
      }
      break;
    case FourCC('I', 'C', 'A', 'N'):
      if (texture) {
        set(kEmissive, d.texture);
      }
      break;
    case FourCC('T', 'C', 'H', '0'):
      if (texture && std::find(std::begin(kShaderGunBody), std::end(kShaderGunBody), shader) != std::end(kShaderGunBody)) {
        set(kBase, d.texture, &incanMask);
      }
      break;
    case FourCC('I', 'C', 'N', 'C'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        icnc = &d;
      }
      break;
    case FourCC('B', 'K', 'L', 'T'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        bklt = &d;
      }
      break;
    case FourCC('B', 'K', 'L', 'A'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        bkla = &d;
      }
      break;
    default:
      break;
    }
  }
  out.noRefl = !refl;
  // The strength is ICNC, the incandescence colour (grey in every material
  // seen, 0.15 to 400), times the shader's INCI parameter where it has one. A
  // render type row names the colour that holds a parameter and the component:
  // INCI / CCH2 / flag 0 is CCH2.x.
  double s = 1.0;
  if (icnc) {
    s = std::max({ShortestDouble(icnc->color[0]), ShortestDouble(icnc->color[1]), ShortestDouble(icnc->color[2])});
  }
  const ModelRenderType* inci = nullptr;
  for (const ModelRenderType& r : mat.renderTypes) {
    if (r.dataId == FourCC('I', 'N', 'C', 'I')) {
      inci = &r;
    }
  }
  if (inci && inci->flag2 < 4) {
    const ModelMaterialData* colour = nullptr;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == inci->dataType) {
        colour = &d;
      }
    }
    if (colour) {
      s *= ShortestDouble(colour->color[inci->flag2]);
    }
  }
  out.emissive = s;
  if (out.maps[kEmissive].has &&
      std::find(std::begin(kShaderGunBody), std::end(kShaderGunBody), shader) != std::end(kShaderGunBody)) {
    // The arm cannon's lit stripes. Remastered reads the ramp (dark, red, orange,
    // yellow along U; the stripe's soft edges down V) at a texcoord the model's ANUV
    // animates: the set's U plus a 0..0.5 hump, looping every 1.033 s, so the stripes
    // pulse along the ramp (the converter turns the curve into a texture matrix). Both
    // stripe shaders (917f1415, f495b260) multiply that glow by c4[0].z in every perm,
    // so it is inverse-exposed: ICAN x INCI (2) on screen, whatever the room. The old
    // compressed strength (0.1 x sqrt(s / 0.1), 0.45) left them dim orange-brown where
    // Remastered shows them bright yellow.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "gun-body list: stripes ramp at inverse exposure; ";
    // Compiled with USE_CUSTOM_INCAN_MASK, the glow is also multiplied by TCH0's red
    // ("IncanMask"), read at the base map's texcoord. On the third-person barrel the
    // ramp (206819a9) repeats and scrolls over the whole surface and the mask (cc7657ad,
    // 8% of it bright) keeps it to a few strips: unmasked, the barrel glowed all over at
    // 5x in a pattern that never held still.
    if (incanMask.has && out.maps[kBase].has && incanMask.coord == out.maps[kBase].coord) {
      out.maps[kBase].mask = {incanMask};
      out.mask = true;
      out.reason += "TCH0 incan mask as base alpha; ";
    }
  } else if (out.maps[kEmissive].has && shader == kShaderGunPanel) {
    // The beam panels: ICAN x ICNC x base alpha with no exposure factor (only the lit
    // part is exposed), so like the inverse-exposed glows it is kept linear.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "gun-panel: glow at inverse exposure; ";
  } else if (out.maps[kEmissive].has &&
             std::binary_search(std::begin(kShaderInverseExposure),
                                std::end(kShaderInverseExposure), shader)) {
    // Inverse-exposed: on screen the glow is ICAN x INCI with no exposure factor, and
    // the maps are dim (the Metroid body's peaks at 33/255), so a room's exposure would
    // leave them unlit in a dark room. Nor is it compressed: it is already what the
    // screen shows, not an HDR value for bloom.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "inverse-exposure list: glow at inverse exposure; ";
  }
  // The shaders of their own. ICNC is 1 in every lava material and the strength
  // is CCH0.x instead.
  const ModelMaterialData* tch[4] = {nullptr, nullptr, nullptr, nullptr};
  const ModelMaterialData* cch[7] = {};
  for (const ModelMaterialData& d : mat.data) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (d.kind == ModelMaterialData::Kind::Texture && d.usage == FourCC('T', 'C', 'H', char('0' + i))) {
        tch[i] = &d;
      }
    }
    for (uint32_t i = 0; i < 7; ++i) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', char('0' + i))) {
        cch[i] = &d;
      }
    }
  }
  if (shader == kShaderUpLayer && tch[0] && cch[0]) {
    out.kind = 1;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderProjBlend && tch[0] && cch[0]) {
    // 9EFE0D2E's blend (the up-facing weight of the bottom layer's normal, the heights in the alphas), whose TCH0-2
    // are read on a texcoord set of their own. Its alpha is the plain lerp of the two base alphas (kind 26).
    out.kind = 26;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderDecalBlend && tch[0] && cch[0] && !out.maps[kEmissive].has) {
    // The alpha map TCH0 rides in the emissive slot as kind 24's does; CCH0.x is the edge width (kind 27).
    out.kind = 27;
    set(kEmissive, tch[0]->texture);
    out.maps[kEmissive].detail = out.maps[kEmissive].has;
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderVertexBlend && tch[0] && cch[0]) {
    // Bottom layer BCLR/METL/NMAP, top layer TCH0-2 by the vertex alpha (CCH0.x the edge). TCH3 multiplies
    // the blended albedo: it rides in the emissive slot, which this shader has no use for.
    out.kind = 20;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    if (tch[3] && !out.maps[kEmissive].has) {
      set(kEmissive, tch[3]->texture);
      out.maps[kEmissive].detail = out.maps[kEmissive].has;
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (out.cutout && cch[0] && cch[0]->color[0] == 0.5 &&
             (shader == kShaderDecalCut || (shader == kShaderDecalAlphaMap && tch[0] && !out.maps[kEmissive].has))) {
    // c = 0.5 (t = A) is the only value they use, and the one the shader assumes.
    out.kind = 24;
    if (shader == kShaderDecalAlphaMap) {
      set(kEmissive, tch[0]->texture);
      out.maps[kEmissive].detail = out.maps[kEmissive].has;
    }
  } else if ((shader == kShaderDetail || shader == kShaderDetailTinted) && tch[0]) {
    out.kind = 2;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
  } else if (shader == kShaderLava && cch[0]) {
    out.kind = 3;
    out.vcolor = true;
    out.emissive = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderParallax && tch[0] && cch[0]) {
    out.kind = 4;
    out.vcolor = true;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    for (int i = 0; i < 4; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
    out.kindStrength = cch[1] ? ShortestDouble(cch[1]->color[0]) : 0.0;
  } else if (std::find(std::begin(kShaderLavaPool), std::end(kShaderLavaPool), shader) != std::end(kShaderLavaPool) &&
             out.maps[kBase].has && tch[0] && tch[1] && tch[2] && cch[0] && cch[1]) {
    out.kind = 6;
    out.vcolor = true;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      // The shader reads every map on the first texcoord, whatever the material says.
      out.layer[slot[i]].coord = 0;
    }
    out.maps[kBase].coord = 0;
    // The pool's LavaRenderVolume overrides CCH0 and CCH1 (x, y, z of each).
    double c[6];
    for (int i = 0; i < 6; ++i) {
      c[i] = opt.hasLava ? ShortestDouble(opt.lava[i]) : ShortestDouble(cch[i / 3]->color[i % 3]);
    }
    const double period = c[1];
    out.kindParam[0] = period > 1e-3 ? 1.0 / period : 0.0;  // the game multiplies it by the time
    out.kindParam[1] = c[0];
    out.kindParam[2] = c[3];
    out.kindParam[3] = c[5];
    out.layerHeight[0] = c[4];  // the noise map's scale
    out.layerHeight[1] = 2.4;   // the heat's gain, a literal of the shader
    out.layerHeight[2] = period;  // takes the phase back to seconds for the shimmer
    out.kindStrength = c[2];
  } else if (IsWaterfall(shader) && tch[0] && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 7;
    out.vcolor = true;
    set(kBase, tch[1]->texture);
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.maps[kBase].coord = out.layer[kBase].coord = 0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
    const double rate = ShortestDouble(cch[0]->color[0]);
    for (int i = 0; i < 4; ++i) {
      out.layerHeight[i] = rate * ShortestDouble(cch[1]->color[i]);
    }
    out.layerSmooth = rate * ShortestDouble(cch[2]->color[0]);
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.kindParam[1] = rate * ShortestDouble(cch[2]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[0]->color[1]);
    out.kindParam[3] = ShortestDouble(cch[0]->color[2]);
    out.kindStrength = ShortestDouble(cch[0]->color[3]);
    // The fold's output is ramp x c5[0] + c5[1]: c5 follows the CCH block (c4[0..2] are CCH0-2, CCH3 is never read),
    // so c5[0] = DIFC and c5[1] = ICMC. DIFC.w = 1 and ICMC.rgb = 0 on all 20 materials, so only the rgb tint is carried.
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.tint[i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderGlass && out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] && cch[2] &&
             cch[3] && cch[4]) {
    out.kind = 8;
    out.vcolor = true;
    // The mask and the noise are bound as the second layer's base and MR. CCH4.w
    // (8 in every glass material) is no uv scale the maps are read with.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    const double glow = ShortestDouble(cch[4]->color[1]);
    for (int i = 0; i < 3; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[2]->color[i]) * glow;
      out.tint[i] = ShortestDouble(cch[1]->color[i]) * ShortestDouble(cch[3]->color[i]) *
                    ShortestDouble(cch[4]->color[0]) * ShortestDouble(cch[4]->color[2]);
    }
    out.layerHeight[3] = ShortestDouble(cch[1]->color[3]);
    for (int i = 0; i < 3; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
  } else if (shader == kShaderHoloGlass && out.maps[kBase].has && tch[2] && cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 11;
    out.vcolor = true;
    // The distortion map is bound as the second layer's base; it scrolls CCH3.y a second
    // down and a quarter of that across. The scene is tinted by CCH1 (where the second
    // output lets the room through), and BCLR, the vertex colour and the reflection's
    // fresnel per vertex alpha are weighted by CCH0.w, CCH2.y and CCH0.y.
    set(kBase, tch[2]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.kindParam[1] = ShortestDouble(cch[3]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[3]->color[0]);
    out.kindParam[3] = ShortestDouble(cch[3]->color[2]);
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = ShortestDouble(cch[1]->color[i]);
    }
    out.layerHeight[0] = ShortestDouble(cch[0]->color[3]);
    out.layerHeight[1] = ShortestDouble(cch[2]->color[1]);
    out.layerHeight[2] = ShortestDouble(cch[0]->color[1]);
    out.layerHeight[3] = 0.0;
    // DIFC (row 7) scales the lit and bent terms and o1 (the fold's c6[0]); the PBR8 trailer carries it.
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderFrozenShell && out.maps[kBase].has && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 12;
    // The dissolve noise is bound as the second layer's base. The edge is
    // clamp(TCH1 - charge (CCH1.z + 2) DIFC.w + 1); the shell shows where it is under
    // about 0.9 (or where the base alpha squared times DIFC.w lets it), and the edge
    // glows CCH1.x x CCH2 at inverse exposure, so linear. The runtime puts the charge
    // in the first parameter. The incandescence is CCH0.w (IINT), not INCI.
    set(kBase, tch[1]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    double difc = 1.0;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        difc = ShortestDouble(d.color[3]);
      }
    }
    out.kindParam[0] = 0.0;
    out.kindParam[1] = (ShortestDouble(cch[1]->color[2]) + 2.0) * difc;
    out.kindParam[2] = difc;
    // The rim: F0 x AO x (1 - n.v)^CCH0.z on faces turned up, times CCH0.y x LINT
    // (CCH0.x, which nothing in the exe sets at run time), at inverse exposure.
    out.kindParam[3] = ShortestDouble(cch[0]->color[1]) * ShortestDouble(cch[0]->color[0]);
    out.kindStrength = ShortestDouble(cch[1]->color[0]);
    for (int i = 0; i < 3; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[2]->color[i]);
    }
    out.layerHeight[3] = ShortestDouble(cch[0]->color[2]);
    // ICAN x CCH0.w (IINT), exposed like other glows: drawn linear, the stored 10 turned
    // the shell a saturated cyan, where Remastered's white is the white base under the
    // probe light and the rim. CIceBeamMP1::PreRenderGunFx sets IINT to 3 + 14 v, v
    // going 0..0.5..0 once a second; the runtime pulses the stored peak to match.
    out.emissive = s * ShortestDouble(cch[0]->color[3]);
  } else if (shader == kShaderMatcapShell && out.maps[kNormal].has && tch[0] && cch[0] && cch[1]) {
    out.kind = 13;
    out.vcolor = true;
    // The matcap is bound as the second layer's base (colour: the shader decodes it, then
    // scales it by CCH0.z). CCH0 is rim power, rim gain, albedo scale, lit-normal tilt;
    // CCH1 the matcap normal's tilt, the rim's bias and the probe reflection's scale. DIFC
    // tints the albedo and its alpha scales the vertex alpha.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].coord = 0;
    out.kindParam[0] = ShortestDouble(cch[0]->color[0]);
    out.kindParam[1] = ShortestDouble(cch[0]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[1]->color[0]);
    out.kindParam[3] = ShortestDouble(cch[1]->color[1]);
    out.kindStrength = std::max(ShortestDouble(cch[0]->color[2]), 0.0);
    out.layerHeight[2] = ShortestDouble(cch[1]->color[2]);
    out.layerHeight[1] = 1.0;
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.tint[i] = ShortestDouble(d.color[i]);
        }
        out.layerHeight[1] = ShortestDouble(d.color[3]);
      }
    }
  } else if (shader == kShaderPhazon && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 21;
    out.vcolor = true;
    // BCLR, METL and NMAP are maps 0-2 as in the standard shader. TCH0 (the gradient ramp, read at
    // a computed coordinate) and TCH1 (the pulse's mask, read at uv x CCH0.w) are bound as the
    // second layer's base and MR; the sampler modes they were authored with are kept. The shader's
    // constants go whole to the runtime in GXSetPBRShield's rows: CCH0..CCH3 in rows 0-3, DIFC in
    // row 7 (the game multiplies CCH2.x by the time).
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderPhazonB && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 22;
    out.vcolor = true;
    // As kind 21: TCH0 (the ramp) and TCH1 (the mask, here read at the base UV) are the second layer's
    // base and MR, CCH0..CCH3 are rows 0-3. This shader has no DIFC (the output alpha is 1).
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
  } else if (shader == kShaderPhazonPool && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1]) {
    out.kind = 32;
    // TCH0 (the noise volume, stacked into one 2D atlas by the importer) and TCH1 (the emission ramp) are the second
    // layer's base and MR, on TCH0's texcoord set. CCH0 (fresnel power, gain, noise scale along the view, emission
    // gain) and CCH1 (noise speed, depth along BCLR.a) are rows 0 and 1, ICMC row 6 and DIFC row 7.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.layer[kBase].volume = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[24 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderPhazonVeins && out.maps[kBase].has && out.maps[kMr].has && out.maps[kNormal].has &&
             tch[0] && tch[1] && tch[2] && cch[0]) {
    out.kind = 35;
    // TCH0 (the glow) is the emissive map, scaled by CCH0.x; TCH1 (the edge ramp, read at (1 - m, 0)) and TCH2 (the
    // disintegration mask) are the second layer's base and MR, raw. CCH0 (glow gain, ramp gain, amount scale, vertex
    // alpha weight) is shield row 0, ICMC row 6 and DIFC row 7. The amount is "DisintegrationAmount" (DIFC.w).
    set(kEmissive, tch[0]->texture);
    set(kBase, tch[1]->texture, &out.layer[kBase]);
    set(kMr, tch[2]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.emissive = ShortestDouble(cch[0]->color[0]);
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[i] = ShortestDouble(cch[0]->color[i]);
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[24 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
    out.kindParam[0] = out.shieldRows[31];  // the runtime may replace it with the live variable
  } else if (shader == kShaderXrayMap && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && cch[0] && cch[1]) {
    out.kind = 33;
    // 65e90e82, the X-ray visor's map material: CCH0/CCH1 are rows 0 and 1 as kind 31's, DIFC row 2 (rgba, default 1),
    // ICMC row 3 (rgb, default 0). TCH0 (subtracted twice from the colour and averaged into the alpha) rides in the MR slot.
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[8 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[8 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[12 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
    out.shieldRows[15] = 1.0;
    set(kMr, tch[0]->texture);
    out.maps[kMr].raw = true;
  } else if ((IsXrayGhost(shader)) && out.maps[kBase].has &&
             out.maps[kNormal].has && cch[0] && cch[1]) {
    out.kind = 31;
    // CCH0 (fresnel power, ICAN scale, alpha at the near and far depth) and CCH1 (the depth range, the alpha-by-fresnel
    // and rim switches) are rows 0 and 1, ICNC row 2 (rgba), ICMC row 3 with w = 1 where the shader reads ICAN (the
    // skinned one).
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[8 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == (shader == kShaderXrayRim ? FourCC('D', 'I', 'F', 'C') : FourCC('I', 'C', 'N', 'C'))) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[8 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[12 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
    // The skinned shader and e84190e0 (ICAN x ICOF x fresnel when MUXI = CCH1.w) read ICAN; fb2bc671 and 0c0b6119 do not.
    out.shieldRows[15] = (IsXraySkinned(shader) || shader == kShaderXrayStatic2) && out.maps[kEmissive].has ? 1.0 : 0.0;
  } else if ((shader == kShaderRefractGlass || shader == kShaderRefractGlassB) && out.maps[kBase].has &&
             out.maps[kNormal].has && cch[0]) {
    out.kind = shader == kShaderRefractGlassB ? 30 : 23;
    out.vcolor = true;
    // BCLR (alpha = roughness, squared in the shader) and NMAP are maps 0 and 2 as in the standard
    // shader. CCH0 (the refraction, the normal's share, the fresnel power and scale) goes whole to
    // kindParam. REFP and BLCM are the room's, which the runtime already has.
    for (int i = 0; i < 4; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
  } else if ((shader == kShaderHoloGlassB || shader == kShaderHoloGlassC || (shader == kShaderHoloGlassE264 && cch[4])) &&
             out.maps[kBase].has && tch[0] && tch[1] && tch[2] && cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 29;
    out.vcolor = true;
    // BCLR is the base map (its alpha the roughness); TCH0 and TCH2 are indirect offsets (raw) and TCH1
    // a colour whose alpha is the scene blur's level, bound as the second layer's base, MR and normal on
    // their own AUVI texcoords. All three are raw: TCH1 decodes as its source format says and keeps its
    // alpha, as the shader samples it, never an MR map's metal clamp.
    // The shader's constants go to the runtime whole (rows 0-3 CCH0..CCH3, row 7 DIFC); row 4 x is the
    // per-object phase the game fills, row 6 w says whether the material has a cube of its own.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    set(kNormal, tch[2]->texture, &out.layer[kNormal]);
    out.layer[kBase].raw = out.layer[kMr].raw = out.layer[kNormal].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    out.shieldRows[20] = shader == kShaderHoloGlassC ? 1.0 : 0.0;  // row 5 x: tint and dual-source terms
    out.shieldRows[27] = out.refl.has ? 1.0 : 0.0;
    if (shader == kShaderHoloGlassE264) {
      // Row 4: y the near plane (the game's), z the depth factor's cap, w CCH4.x, the phase's scale; row 5 y
      // marks E2644678. Its cube is the camera's probe, which the game binds (row 6 w), not the material's.
      out.shieldRows[18] = kHoloGlassE264DepthCap;
      out.shieldRows[19] = ShortestDouble(cch[4]->color[0]);
      out.shieldRows[21] = 1.0;
      out.shieldRows[27] = 0.0;
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if (shader == kShaderSimpleGlass && out.maps[kBase].has && tch[0] && cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 43;
    out.vcolor = true;
    // Kind 29's layout less its TCH1/TCH2 layers and DIFC: BCLR is the base map (its alpha the roughness and the
    // screen blur), TCH0 the indirect offset, raw, bound as the second layer's base. The fold's samplers are texA
    // (TCH0, read at -0.5 as an offset), tex8 (BCLR), tex10 (screen), texE (read at (N.V, BCLR.a): the BRDF LUT, as
    // 3991da00's tex12; the kb page's "TCH0 -> texE" is unverified) and texC (REFL). Rows 0-3 CCH0..CCH3
    // (c5[n] = CCHn, unverified), row 4 x the per-object phase (the fold's v4.y, taken as 3991da00's v6.y), row 6 w
    // whether the material has a cube of its own.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    out.shieldRows[27] = out.refl.has ? 1.0 : 0.0;
  } else if ((shader == kShaderBoundaryShield || shader == kShaderForceField) && out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] &&
             cch[2] && cch[3] && cch[4] && cch[5] && cch[6]) {
    out.kind = 14;
    out.vcolor = true;
    // BCLR is the base map; TCH0 (the glow's mask) and TCH1 (the noise that bends everything) are
    // bound as the second layer's base and MR, each on the texcoord its own AUVI entry names. The
    // shader's constants go to the runtime whole (rows 0-6 CCH0..CCH6, row 7 DIFC): the scroll
    // speeds, pulse rates, colours and the screen bend are all in them.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 7; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if (shader == kShaderPickUp && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && cch[0] &&
             cch[1] && cch[2] && cch[3]) {
    out.kind = 15;
    out.vcolor = true;
    // BCLR (the base map) is a three-channel mask and NMAP (the normal map) bends the rim's
    // normal. TCH0, the gradient, is bound as the second layer's base; it is read at the world
    // position, so its texcoord is unused. The constants go to the runtime whole.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].coord = 0;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if (shader == kShaderEnergyField && out.maps[kBase].has && tch[0] && cch[0] && cch[1]) {
    // Ore Processing EnergyField: 000/001 fragments are identical. BCLR.r/g
    // are the two counter-scrolling threshold masks; TCH0.r is the UV stripe.
    // VS emits world position, view normal, UV0 and raw vertex RGBA.
    out.kind = 44;
    out.shield = true;
    out.vcolor = true;
    out.unlit = true;
    out.maps[kBase].raw = true;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    out.kindParam[0] = 1.0;
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) out.shieldRows[28 + i] = 1.0;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
      }
    }
  } else if (shader == kShaderChozoLore && out.maps[kBase].has && tch[0] && cch[0] && cch[1] && cch[2]) {
    // ChozoLore 9d53cf06 (kb material/9d53cf06.md), unlit and premultiplied: BCLR (tex8) is the base map, TCH0
    // (texA, the offset that bends it) is bound raw as the second layer's base (stage 4). Rows 0-2 CCH0..CCH2
    // (FS c5[0], c5[1], VS/FS CCH2: the wave), row 7 DIFC. The VS waves the vertices along the normal by the
    // vertex alpha and maps the sheet from the model-space y and z.
    out.kind = 46;
    out.shield = true;
    out.vcolor = true;
    out.unlit = true;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    for (int r = 0; r < 3; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) out.shieldRows[28 + i] = 1.0;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
      }
    }
  } else if ((shader == kShaderHolo || shader == kShaderHoloRefl || shader == kShaderHologram || shader == kShaderRouteHologram) &&
             out.maps[kBase].has && ((shader != kShaderHologram && shader != kShaderRouteHologram) || (cch[0] && cch[1] && cch[2] && cch[3]))) {
    out.kind = shader == kShaderHolo ? 16 : shader == kShaderHoloRefl ? 17 : shader == kShaderRouteHologram ? 45 : 18;
    out.vcolor = out.kind == 18 || out.kind == 45;
    // DIFT (or BCLR for the hologram) is the base map. The constants go to the runtime in the PBR8
    // trailer: row 7 DIFC; row 6 the incandescence ICNC + ICMC (summed, rgb) for 16 and 17, with w
    // the gain of the material's own cube (17); ICMC alone for 18, whose rows 0-3 are CCH0..CCH3 and
    // rows 4-5 are filled at run time (world x and y).
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    double incan[3] = {0.0, 0.0, 0.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C') || (d.usage == FourCC('I', 'C', 'N', 'C') && out.kind != 18 && out.kind != 45)) {
        for (int i = 0; i < 3; ++i) {
          incan[i] += ShortestDouble(d.color[i]);
        }
      }
    }
    if (out.kind == 18 || out.kind == 45) {
      for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = incan[i];
    }
    // The cube gain: only a cube of the material's own (not the black default) adds anything.
    out.shieldRows[27] = out.kind == 17 && out.refl.has ? 1.0 : 0.0;
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  }
  const bool eyeGloss = IsEyeGloss(shader);
  const bool ballGlass = shader == kShaderBallGlass;
  if ((shader == kShaderGunFx || eyeGloss || ballGlass) && out.maps[kBase].has && out.layer[kBase].has && out.layer[kMr].has) {
    out.kind = ballGlass ? 42 : eyeGloss ? 25 : 19;
    // Lit: REFV (map 5) x REFS (map 4, read at the view-space normal) x luminance(L) + DIFT x DIFC x L
    // + ICNC + ICMC. Row 6 = ICNC + ICMC, row 7 = DIFC; Remastered's mesh has no colour stream.
    // Kind 42 adds ICAN (map 3) x ICNC: row 5 = ICNC, row 6 = ICMC alone.
    out.layer[kMr].coord = out.layer[kBase].coord = 0;
    out.tint[0] = out.tint[1] = out.tint[2] = 1.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    double incan[3] = {0.0, 0.0, 0.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'N', 'C') && ballGlass) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[20 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C') || d.usage == FourCC('I', 'C', 'N', 'C')) {
        for (int i = 0; i < 3; ++i) {
          incan[i] += ShortestDouble(d.color[i]);
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = incan[i];
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  }
  if (std::find(std::begin(kShaderGunGlow), std::end(kShaderGunGlow), shader) != std::end(kShaderGunGlow) &&
      out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 9;
    out.vcolor = true;
    // The noise and the ramp are bound as the second layer's base and MR. The ramp is
    // read where the noise says, so its own texcoord is never used.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.layer[kMr].coord = out.layer[kBase].coord;
    for (int i = 0; i < 4; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[1]->color[i]);
    }
    out.kindParam[0] = ShortestDouble(cch[0]->color[1]);  // the game multiplies it by the time
    out.kindParam[1] = ShortestDouble(cch[2]->color[0]);
    out.kindParam[2] = ShortestDouble(cch[2]->color[1]);
    out.kindParam[3] = ShortestDouble(cch[0]->color[3]);
    // The ramp times CCH0.z (10-15) at inverse exposure (c4[0].z in both shaders), so like
    // the inverse-exposed glows it is kept linear. The square root left the Wave gun's
    // lines a dull violet.
    out.kindStrength = std::max(ShortestDouble(cch[0]->color[2]), 0.0);
  }
  if (std::find(std::begin(kShaderPremulGlass), std::end(kShaderPremulGlass), shader) !=
          std::end(kShaderPremulGlass) &&
      out.blended && !out.cutout) {
    out.kind = 10;
  }
  if (shader == kShaderDualGlass && out.blended && !out.cutout) {
    out.kind = 34;
  }
  if (shader == kShaderInventoryGlass && !out.cutout) {
    out.kind = 36;
  }
  if (shader == kShaderInventoryAdd && !out.cutout) {
    out.kind = 37;
  }
  if ((shader == kShaderUnlitCube || shader == kShaderUnlitCubeNormal) && out.maps[kBase].has) {
    // Unlit: A x BCLR x (1 - M) x DIFC + F0 x REFL + ICNC + ICMC, METL = (A, rough, M) (kind 38). Row 6 = ICNC + ICMC,
    // row 7 = DIFC; no exposure factor (mode bit 131072).
    out.kind = 38;
    out.unlit = true;
    out.tint[0] = out.tint[1] = out.tint[2] = 1.0;
    const ShaderColors c = ReadShaderColors(mat);
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = c.icnc[i] + c.icmc[i];
    }
    out.shieldRows[27] = 1.0;
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = c.difc[i];
    }
  }
  if (shader == kShaderUnlitVertex && out.maps[kBase].has) {
    // Unlit (perm 003): DIFT x DIFC x vertex colour + ICNC + ICMC, alpha DIFT.a^2 x vertex alpha x DIFC.a, no
    // exposure (kind 39). Row 6 = ICNC + ICMC, row 7 = DIFC, as kind 16. DIFT is the base map, on its one uv set.
    out.kind = 39;
    out.vcolor = true;
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    const ShaderColors c = ReadShaderColors(mat);
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = c.icnc[i] + c.icmc[i];
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = c.difc[i];
    }
  }
  if (shader == kShaderUnlitMatcap && out.maps[kBase].has && out.layer[kBase].has && out.layer[kMr].has) {
    // Unlit (perm 002 assumed): DIFT x DIFC + REFS(view-space normal x 0.5 + 0.5) x REFV + ICNC + ICMC, alpha
    // DIFT.a^2 x DIFC.a, no exposure (kind 40). One uv set. Rows as kind 19: 6 = ICNC + ICMC, 7 = DIFC.
    out.kind = 40;
    out.layer[kMr].coord = out.layer[kBase].coord = 0;
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    const ShaderColors c = ReadShaderColors(mat);
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = c.icnc[i] + c.icmc[i];
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = c.difc[i];
    }
  }
  if (shader == kShaderUnlitAdd && out.maps[kBase].has && out.maps[kMr].has) {
    // Unlit (perm 002 assumed): METL.x x BCLR x (1 - METL.z) x DIFC + ICAN x ICNC + ICMC, alpha BCLR.a^2 x DIFC.a, no
    // exposure (kind 41). Row 5 = ICMC, row 6 = ICNC (apart: only ICNC scales ICAN), row 7 = DIFC. ICAN keeps the
    // texcoord its reference names (the kb guesses it is the sampler on uv set 1; unverified).
    out.kind = 41;
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    const ShaderColors c = ReadShaderColors(mat);
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[20 + i] = c.icmc[i];
      out.shieldRows[24 + i] = c.icnc[i];
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = c.difc[i];
    }
  }
  if (shader == kShaderIceSpreader && tch[0] && !out.maps[kEmissive].has && cch[0] && cch[1] && cch[2] && cch[3]) {
    // Ice (088e025e, kind 28): TCH0 is the frost, shifted by a parallax along the view and drawn
    // over the lit surface with a rim. Rows: CCH0 (normal strength z, parallax scale w), CCH1
    // (frost gain x, rim gain y, rim power z), CCH2 rgb with CCH5.x in w (cube tint and gain),
    // CCH3 (rim colour).
    set(kEmissive, tch[0]->texture);
    out.kind = 28;
    const ModelMaterialData* rows[4] = {cch[0], cch[1], cch[2], cch[3]};
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(rows[r]->color[i]);
      }
    }
    out.shieldRows[11] = cch[5] ? ShortestDouble(cch[5]->color[0]) : 1.0;
    out.emissive = 1.0;
    out.reason += "ice: kind 28, own cube; ";
  }
  // The texture matrix a scroll loads has no scale, so CCH1.yz (1 on every door
  // shield seen) is not drawn.
  if (shader == kShaderColorUnlit && cch[0]) {
    out.scroll[0] = ShortestDouble(cch[0]->color[1]);
    out.scroll[1] = -ShortestDouble(cch[0]->color[2]);
  }
  // CharacterBacklight (shader ae819893, build/mpr/mtrl-tr/ae819893/NOTES.md): BKLT is a
  // parameter vector, x the strength of the light from behind and y of the one from above.
  // BKLA is the cubic of the fade along the model's y (t 0 to 1 over its bounds), x t^3 + y t^2 + z t + w; every
  // material seen has a single term of 1 (t^3 in 358 of 375), so its power is kept and the
  // largest term picks it otherwise. No BKLA is t^3, the common case.
  out.backlight = bklt ? ShortestDouble(bklt->color[0]) : 0.0;
  out.backlightTop = bklt ? ShortestDouble(bklt->color[1]) : 0.0;
  out.backlightFalloff = bklt ? 4.0 : 0.0;
  if (bklt && bkla) {
    int term = -1;
    for (int i = 0; i < 4; ++i) {
      if (std::abs(bkla->color[i]) > 1e-6f && (term < 0 || std::abs(bkla->color[i]) > std::abs(bkla->color[term]))) {
        term = i;
      }
    }
    out.backlightFalloff = term < 0 ? 0.0 : 4.0 - term;
  }
  if (shader == kShaderColorUnlit) {
    out.colorUnlit = true;
    out.backlight = (cch[0] ? ShortestDouble(cch[0]->color[0]) : 1.0) * (cch[1] ? ShortestDouble(cch[1]->color[0]) : 1.0);
  }
  // Both layers' base maps and an edge width make a blend; the shader's own
  // floor on the width is not known, so a zero one is the thinnest edge.
  out.layered = out.maps[kBase].has && out.layer[kBase].has && !out.cutout && !out.blended;
  if (out.kind != 7) {
    out.layerSmooth = std::clamp(out.layerSmooth, 1e-3, 16.0);
  }
  if (out.kind == 6) {
    // Its own colour, and the extra maps are no second layer but must be bound like one.
    out.layered = out.unlit = true;
    out.cutout = out.blended = out.tinted = out.mask = false;
    out.height = 0.0;
  }
  if (out.kind == 7) {
    // The ramp's colour and alpha are all of it; the sheets' map is bound as a second layer.
    out.layered = out.unlit = out.blended = true;
    out.cutout = out.tinted = out.mask = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 8) {
    // Lit, but only by its reflection: what shows through is the frame behind it.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 11) {
    // All of it is the shader's, and it covers what is behind it with the screen copy.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 14 || out.kind == 29 || out.kind == 43) {
    // Unlit and alpha blended; every colour is the shader's, and it reads the screen copy.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 15) {
    // Unlit and blended; every colour is the shader's, and the normal map is kept (it bends the rim).
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if ((out.kind >= 16 && out.kind <= 18) || out.kind == 45) {
    // Unlit and additive; every colour is the shader's, and the base map's alpha is its own.
    // No second layer: the one map is the base.
    out.blended = true;
    out.layered = false;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 44) {
    // Every shipped EnergyField carries the transparent flag, which leaves it unlayered; its TCH0
    // stripe is bound as the second layer's base map (stage 4) all the same, and its class blends it.
    out.layered = true;
    out.cutout = out.tinted = out.mask = false;
    out.height = 0.0;
  }
  if (out.kind == 46) {
    // The lore sheet's TCH0 is the second layer's base (stage 4); it blends One / InvSrcAlpha (PbrBlend).
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 28) {
    // Lit, with the frost map in the emissive slot and its own opacity (the base alpha).
    out.tinted = out.mask = out.maskSquared = out.unlit = false;
    out.height = 0.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 33) {
    // As kind 31, but TCH0 stays in the MR slot and the ICAN map is always read.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 31) {
    // Alpha blended and lit by the lights' specular alone; the shader makes the rest (ICAN is kept for the skinned one).
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = false;
    if (out.shieldRows[15] == 0.0) {
      out.maps[kEmissive].has = false;
    }
  }
  if (out.kind == 25 || out.kind == 42) {
    // As kind 19, but the normal map, MR and the light's spec stay: only the reflection is the matcap.
    // Kind 42 keeps ICAN (map 3), which its shader scales by ICNC.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kEmissive].has = out.maps[kEmissive].has && out.kind == 42;
  }
  if (out.kind == 19) {
    // Lit and opaque (Remastered's mesh class 0; a fade is the model flags'). Maps 4 and 5 are
    // REFS and REFV; the lit base is DIFT alone.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 40) {
    // As kind 19 (maps 4 and 5 are REFS and REFV, the base DIFT alone), but unlit in the shader and with its class's blend.
    out.layered = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 13) {
    // Lit by the standard path and alpha blended; the shader makes its own albedo and rim.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kEmissive].has = false;
  }
  if (out.kind == 12) {
    // Opaque and lit; the shader discards what the dissolve leaves out.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
  }
  if (out.kind == 23 || out.kind == 30) {
    // Opaque: the shader writes alpha 1, and what is behind it is its own copy of the frame. The
    // vertex alpha is the transmission weight and the base alpha the roughness, neither an opacity.
    out.layered = true; // keeps the kind (the demotion below), drawn opaque all the same
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 21 || out.kind == 22 || out.kind == 32) {
    // Opaque and lit; the base alpha is a weight in the shader and no opacity, the glow is the
    // shader's (ICNC is 0 on every material) and the vertex alpha picks no layer.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.maps[kEmissive].has = false;
  }
  if (out.kind == 35) {
    // Opaque and lit; the base alpha is a weight in the dissolve and no opacity, and the vertex alpha picks no layer.
    // The vertex colour tints it (kShaderTints) and the glow map stays: CCH0.x is its gain.
    out.layered = true;
    out.blended = out.cutout = out.mask = out.unlit = false;
    out.height = 0.0;
  }
  if (out.kind == 9) {
    // The glow is all the shader's; the vertex alpha picks the ramp's row and is no
    // opacity, and there is no edge between layers.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
  }
  // The kind the shader asked for: a role that names a kind but left it 0 lacked its maps or colours.
  if (out.kind == 0) {
    static const char* const kKindRoles[] = {"up-layer", "detail", "lava", "parallax", "waterfall",
                                             "glass",    "lava-pool", "gun-glow", "premul-glass", "holo-glass",
                                             "frozen-shell", "matcap-shell", "boundary-shield", "pickup",
                                             "holo",         "holo-refl",    "hologram",     "gun-fx", "vertex-blend", "phazon", "projected-blend", "decal-alpha", "phazon-pool", "phazon-veins",
                                             "inventory-glass", "inventory-add", "unlit-cube", "unlit-vertex", "unlit-matcap", "unlit-add",
                                             "simple-glass"};
    for (const char* name : kKindRoles) {
      if (out.role.find(name) != std::string::npos) {
        out.reason += std::string("fallback: ") + name + " shader without its maps/params (or blend); ";
      }
    }
  } else {
    out.reason += std::string("kind ") + KindName(out.kind) + " from the shader (" + out.role + "); ";
  }
  // All but lava, premultiplied glass, the holograms (kinds 16-18, one map), the decal cutouts, the unlit cube
  // surface (38) and the unlit vertex-coloured fx (39) draw with the second layer's maps.
  // Ice's fragment shader (088e025e) uses the standard single-layer PBR slots 0-2
  // plus its frost map in slot 3. Keep its kind only when those source maps exist;
  // otherwise the shader would bind neutral placeholders. Its CCH0-CCH3 row guard
  // is above, where kind 28 is assigned. Layered ice retains its historical path.
  const bool iceSingleLayerMapsBound = out.kind == 28 && !out.layered && out.maps[kBase].has && out.maps[kMr].has &&
                                       out.maps[kNormal].has && out.maps[kEmissive].has;
  if (out.kind != 3 && out.kind != 10 && !(out.kind >= 16 && out.kind <= 18) && out.kind != 45 && out.kind != 24 && out.kind != 27 &&
      out.kind != 34 && out.kind != 36 && out.kind != 37 && out.kind != 38 && out.kind != 39 && out.kind != 41 && !out.layered && !iceSingleLayerMapsBound) {
    if (out.kind != 0) {
      out.reason += std::string("demoted ") + KindName(out.kind) + " to standard: not layered; ";
    }
    if (out.maps[kEmissive].detail) {
      out.maps[kEmissive] = MapRef();
    }
    out.kind = 0;
    out.vcolor = false;
  }
  // A frost shell without its dissolve would cover the gun for good.
  out.hidden = shader == kShaderFrozenShell && out.kind != 12;
  if (out.hidden) {
    out.reason += "frozen-shell: hidden; ";
  }
  if (out.shell) {
    out.reason += "matcap-shell; ";
  }
  if (out.maskSquared) {
    out.reason += "incan-mask-squared: glow masked by base alpha squared; ";
  } else if (out.mask) {
    out.reason += "incan-mask flag: glow masked by base alpha; ";
  }
  if (out.colorUnlit) {
    out.reason += "color-unlit shader; ";
  }
  if (out.kind == 10) {
    out.reason += "premul-glass list: lit premultiplied; ";
  }
  // A material with an AUVI names, per UV output channel, which of the model's
  // texcoord sets feeds it, so a map's texcoord no longer names a set directly.
  // port_remastered_uv.h holds the rule and what the tests drive.
  //
  // Lava pools (kind 6) and waterfalls (kind 7) keep their existing forced UV0.
  // No surveyed material of either kind has AUVI; combined behavior is unverified.
  if (out.kind == 32) {
    // 07acff46 reads AUVI per sampler slot, not by the authored texcoord: x feeds BCLR, y the noise volume, z METL and
    // w NMAP (kb material/07acff46.md), so the set a map uses is its slot's component whatever `@k` it named.
    if (const int32_t* auvi = Auvi(mat)) {
      MapRef* slots[] = {&out.maps[kBase], &out.layer[kBase], &out.maps[kMr], &out.maps[kNormal]};
      for (size_t i = 0; i < std::size(slots); ++i) {
        slots[i]->authored = slots[i]->coord;
        if (auvi[i] >= 0) {
          slots[i]->coord = uint32_t(auvi[i]);
        }
      }
    }
  } else if (out.kind != 6 && out.kind != 7) {
    MapRef* mapRefs[] = {&out.maps[kBase], &out.maps[kMr], &out.maps[kNormal], &out.maps[kEmissive],
                         &out.layer[kBase], &out.layer[kMr], &out.layer[kNormal]};
    uint32_t coords[std::size(mapRefs)];
    for (size_t i = 0; i < std::size(mapRefs); ++i) {
      coords[i] = mapRefs[i]->coord;
      mapRefs[i]->authored = coords[i];
    }
    ApplyAuvi(mat, coords, std::size(coords));
    for (size_t i = 0; i < std::size(mapRefs); ++i) {
      mapRefs[i]->coord = coords[i];
    }
  }
  return out;
}

}  // namespace ConvertDetail

using namespace ConvertDetail;

int MaterialKind(const ModelMaterial& mat) { return ReadMaterial(mat, ConvertOptions{}).kind; }

int MaterialDriverSlot(const ModelMaterial& mat, uint32_t variable) {
  const RemMaterial rem = ReadMaterial(mat, ConvertOptions{});
  if (rem.kind != 7) {
    return -1;
  }
  // The material binds the variable to a component of CCH0 (renderTypes: variable, CCH0, 1, component);
  // the converter carries CCH0.z in kindParam[3] and nothing else a driver could move.
  for (const ModelRenderType& r : mat.renderTypes) {
    if (r.dataId == variable && r.dataType == FourCC('C', 'C', 'H', '0') && r.flag2 == 2) {
      return 3;
    }
  }
  return -1;
}

}  // namespace PortRemastered
