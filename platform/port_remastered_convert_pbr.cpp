// The Remastered model converter's PBR material record (CCubeModel::PortSetPBRMaterial). See
// port_remastered_convert_internal.h.

#include "port_remastered_convert_internal.h"

namespace PortRemastered {
namespace ConvertDetail {

// The port's material record (CCubeModel::PortSetPBRMaterial), appended to a
// PBR material: emissive multiplier rgb, backlight weight rgb, 'PBRM'; or,
// for a material with a height blend or no lighting, those, the blend's
// threshold, the mode (1 unlit, 2 glow masked by the base alpha, 4 tinted by
// the vertex colour, summed), and 'PBR2'; or, for a layered one, those, the blend's edge width, the scale and offset of
// each layer's height, and 'PBR3'; or, for a shader of its own, those, the kind, its strength
// (linear like the emissive one) and its four parameters, and 'PBR4'; or, where a
// map clamps or mirrors, all nineteen, the maps' wrap modes (one word) and 'PBR5'; or, for the
// back copy of a material with a LITS, those and the diffuse and F0 factors, and 'PBR6'; or,
// for a material with a reflection cube of its own (State::Cube), all of that, the cube's id
// and 'PBR7'. The boundary shield (kind 14) adds a trailer after any of these: its shader's
// constants, CCH0..CCH6 then DIFC (32 floats), and 'PBR8'.
// Glow strengths are stored as Remastered has them, linear and uncapped (they were a capped
// square root while the port had no exposure or bloom to take HDR values). The runtime
// applies the room's exposure to the ones flagged in the mode word (32, 64); an
// inverse-exposed glow (RemMaterial::glowLinear) is what Remastered's screen shows.
// A ColorUnlit surface drawn by its own rule (mode 8): one whose vertex colour
// the port keeps and lights nothing with.
// Remastered's lit PBR shader (60989bc1, USE_TWO_SIDED_MATERIAL) takes s = LITS on a back
// face and multiplies the normal frame by sign(s), the diffuse light by |s| and F0 by 0 where
// s > 0, the frame being the mesh's own. A back copy of a lit material with a positive LITS
// is therefore its own primitive and material, diffuse |s| and F0 0 (record 'PBR6'). Its
// vertices keep the older back copy's turned normals, so the TEV fallback and every other
// reader light it as before; the F0 factor 0 is the marker by which the PBR shader turns
// the normal back (and keeps the front's tangent frame, see shader.cpp). Only the
// ordinary lit material qualifies: unlit, ColorUnlit and the special shader kinds are other
// shaders. LITS of -1 (the flipped normal of the older back copy, factors 1 and 1), no LITS
// and the zero or other negatives the survey did not find keep the older copy.
bool BackLightScale(const RemMaterial& m) {
  return m.hasLits && std::isfinite(m.lits) && m.lits > 0.0 && m.kind == 0 && !m.unlit &&
         !m.colorUnlit;
}

bool ColorUnlitDraw(const RemMaterial& m) { return m.colorUnlit && m.unlit && m.tinted; }

// The PBR record's mode word: 1 unlit, 2 glow masked by the base alpha, 4 tinted by the vertex
// colour, 8 ColorUnlit's draw, 32 a glow the runtime exposes (ExposedGlow), 64 a kind's
// glow strength it exposes (ExposedStrength), 128 the vertex colour tints F0 too.
int PbrMode(const RemMaterial& m);

// Whether the glow is Remastered's bare ICAN x ICNC in scene radiance, which the frame's
// exposure multiplies: the port's runtime scales it (bit 32). Not the inverse-exposed
// glows (glowLinear: they are drawn as the screen shows them), nor a liquid
// or glass, whose first three floats are a tint.
bool ExposedGlow(const RemMaterial& m) {
  return !m.glowLinear && m.maps[kEmissive].has && m.kind != 7 && m.kind != 20 &&
         m.kind != 8 && m.kind != 11 && m.kind != 14 && m.kind != 21 && m.kind != 24;
}

// A glow strength of a kind below 5 (the parallax's inside), exposed at run time like the
// emissive map's (mode bit 64).
bool ExposedStrength(const RemMaterial& m) { return m.kind > 0 && m.kind < 5 && m.kindStrength > 0.0; }

// A lit plain (kind 0) surface without REFL reflects nothing around it: every Remastered
// shader that samples a cube has REFL, and no other does (mode bit 256).
// VFX_Model_Base (29D9FDFB, and 1982DB27 without the vertex colour) and LambertFx (4BC890C1) are
// pure Lambert: no specular lobe and no cube (kb material/{29d9fdfb,1982db27,4bc890c1}.md).
bool VfxBase(const RemMaterial& m) {
  return (m.shader == kShaderVfxBase || m.shader == kShaderVfxBase2) && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0;
}
bool PureLambert(const RemMaterial& m) {
  return (VfxBase(m) || (m.family == kShaderLambertFx && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0));
}
// LambertFx's fragment perms (042-057) sample no lightmap, probe volume or BLCM: the light sum starts
// from the flat ambient constant alone (mode bit 262144; kb material/4bc890c1.md, perms 000/001 have no
// fragment shader, they are the depth pass).
bool FlatAmbient(const RemMaterial& m) { return m.family == kShaderLambertFx && PureLambert(m); }
bool NoEnvSpec(const RemMaterial& m) { return (m.noRefl || PureLambert(m)) && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0; }

bool SurfaceUnlit(const RemMaterial& m) {
  return (m.unlit && m.kind == 0 && !ColorUnlitDraw(m) &&
          std::find(std::begin(kShaderSurfaceUnlit), std::end(kShaderSurfaceUnlit), m.shader) != std::end(kShaderSurfaceUnlit)) ||
         BareUnlitKind(m.kind);
}

int PbrMode(const RemMaterial& m) {
  return (m.unlit ? 1 : 0) + (m.mask ? 2 : 0) + (m.tinted ? 4 : 0) + (ColorUnlitDraw(m) ? 8 : 0) +
         (ExposedGlow(m) ? 32 : 0) + (ExposedStrength(m) ? 64 : 0) +
         (m.tinted && m.tintF0 && !ColorUnlitDraw(m) ? 128 : 0) + (NoEnvSpec(m) ? 256 : 0) +
         (m.cutExact ? 512 : 0) + (PureLambert(m) ? 1024 : 0) + (VfxBase(m) ? 2048 + 4096 : 0) +
         (m.indirect ? 8192 : 0) + (m.macro || m.macroLayered ? 16384 : 0) +
         (m.wind ? 32768 : 0) + (m.wind && m.shader == kShaderVertexAlpha ? 65536 : 0) +
         (SurfaceUnlit(m) ? 131072 : 0) + (FlatAmbient(m) ? 262144 : 0) +
         (std::find(std::begin(kShaderRawVertex), std::end(kShaderRawVertex), m.shader) != std::end(kShaderRawVertex) ? 524288 : 0);
}

void PbrRecord(Blob& b, const RemMaterial& m, uint32_t wrap, uint32_t cube) {
  const double e = std::max(m.emissive, 0.0);
  // ColorUnlit's backlight is its gain, and no backlight at all where drawn otherwise. A lit
  // surface's is the strength from behind, from above, and the fade's power plus 1.
  double k[3] = {0.0, 0.0, 0.0};
  if (ColorUnlitDraw(m)) {
    k[0] = k[1] = k[2] = std::max(m.backlight, 0.0);
  } else if (!m.colorUnlit && !m.unlit && (m.backlight > 0.0 || m.backlightTop > 0.0)) {
    k[0] = std::max(m.backlight, 0.0);
    k[1] = std::max(m.backlightTop, 0.0);
    k[2] = m.backlightFalloff;
  } else if (m.indirect) {
    k[0] = m.indStrength;  // INDS: an unlit surface has no use for the backlight
  }
  std::vector<double> f;
  for (int i = 0; i < 3; ++i) {
    // A liquid has no glow of its own, and its colour goes where the glow's would.
    f.push_back(m.kind == 7 || m.kind == 8 || m.kind == 11 || m.kind == 13 ? m.tint[i] : e);
  }
  for (int i = 0; i < 3; ++i) {
    f.push_back(k[i]);
  }
  const char* tag = "PBRM";
  if (m.height > 0.0 || m.unlit || m.mask || m.layered || m.tinted || m.kind || ExposedGlow(m) || NoEnvSpec(m) || m.cutExact || m.indirect || m.macro || m.macroLayered || m.wind) {
    f.push_back(m.height);
    f.push_back(double(PbrMode(m)));
    tag = "PBR2";
    if (m.layered || m.kind) {
      // Only a blend of two layers has an edge.
      f.push_back(m.kind == 7 || m.kind == 20 || m.kind == 26 || m.kind == 27 || (m.layered && m.kind <= 1) ? m.layerSmooth : 0.0);
      for (double h : m.layerHeight) {
        f.push_back(h);
      }
      tag = "PBR3";
      if (m.kind) {
        f.push_back(double(m.kind));
        f.push_back(m.kind >= 5 ? m.kindStrength : std::max(m.kindStrength, 0.0));
        for (double v : m.kindParam) {
          f.push_back(v);
        }
        tag = "PBR4";
      }
    }
  }
  // Only a material with a map that does not repeat needs the long form: every
  // field, the ones the short form leaves out at the reader's neutral 0.
  const bool scaled = m.lightScale[0] != 1.0 || m.lightScale[1] != 1.0;
  const bool wraps = wrap != 0x55555555u || scaled || cube != 0;
  if (wraps) {
    f.resize(19, 0.0);
    tag = cube != 0 ? "PBR7" : scaled ? "PBR6" : "PBR5";
  }
  for (double v : f) {
    PF(b, v);
  }
  if (wraps) {
    P32(b, wrap);
  }
  if (scaled || cube != 0) {
    PF(b, m.lightScale[0]);
    PF(b, m.lightScale[1]);
  }
  if (cube != 0) {
    P32(b, cube);
  }
  b.insert(b.end(), tag, tag + 4);
  if ((m.kind >= 14 && m.kind <= 19) || m.kind == 21 || m.kind == 22 || m.kind == 32 || m.kind == 35 || m.kind == 25 || m.kind == 28 || m.kind == 29 || m.kind == 11 || m.kind == 31 || m.kind == 33 || (m.kind >= 38 && m.kind <= 45) || m.wind) {
    // The boundary shield's (or pickup's) constants follow the record, as a trailer the reader strips first.
    // A swaying leaf's is the model's SWindSet (v1, v2, rate, b, c) in the first nine.
    for (double v : m.shieldRows) {
      PF(b, v);
    }
    static constexpr char kPbr8[] = "PBR8";
    b.insert(b.end(), kPbr8, kPbr8 + 4);
  }
}

// The matcap shell (kind 13) is drawn premultiplied, GX_BL_ONE and GX_BL_INVSRCALPHA: its
// shader scales the diffuse by the opacity, and the rim and reflection are added unscaled.
// The boundary shield (kind 14) and cracked glass (kind 8, class 1 with the scene in rgb) are plainly alpha blended, GX_BL_SRCALPHA and GX_BL_INVSRCALPHA.
// The pickup (kind 15) is SrcAlpha with the mesh class's destination: One where it is additive.
// 4BC890C1 (a lit Lambert) is opaque, as Remastered's mesh class 0 draws it.
// The pause visor's glass (kind 36) is SrcAlpha / InvSrcAlpha (mesh class 1), its lit layer (kind 37)
// SrcAlpha / One (mesh class 3).
void PbrBlend(const RemMaterial& rem, const RetailMaterial& pm, uint32_t& src, uint32_t& dst) {
  const bool lambertFx = rem.family == kShaderLambertFx;
  // Otherwise as its mesh class: a retail model's material from the class table, a standalone
  // one from the synthetic retail material its class picked (premultiplied glass among them).
  uint32_t classSrc = pm.blendSrc;
  uint32_t classDst = pm.blendDst;
  if (rem.classBlend) {
    ClassBlend(rem.blendClass, classSrc, classDst);
  }
  dst = rem.kind == 8 || rem.kind == 13 || rem.kind == 14 || rem.kind == 29 || rem.kind == 34 || rem.kind == 36 || rem.kind == 43 ? 5 : rem.kind == 37 ? 1 : (rem.kind >= 15 && rem.kind <= 18) || rem.kind == 31 || rem.kind == 33 ? (rem.additive ? 1 : 5) : rem.kind == 19 ? (GunFxParticle(pm) ? 5 : 0) : lambertFx ? 0 : classDst;
  src = rem.kind == 8 || rem.kind == 14 || rem.kind == 29 || rem.kind == 34 || rem.kind == 36 || rem.kind == 37 || rem.kind == 43 || (rem.kind >= 15 && rem.kind <= 18) || rem.kind == 31 || rem.kind == 33 ? 4 : rem.kind == 19 ? (GunFxParticle(pm) ? 4 : 1) : rem.kind == 13 || lambertFx ? 1 : classSrc;
}

// A retail material rebuilt for the port's PBR path: maps 0-3 are base, MR,
// normal and emissive, each on the texcoord its Remastered map used, lit
// channel 0 kept from retail. The TEV is the fallback the black, shadow,
// thermal and blended paths still draw with (base x lighting + emissive), and
// it samples every map so all are bound. A layered material has three more,
// maps 4-6: the second layer's base, MR and normal.
Blob PbrMaterial(const RetailMaterial& pm, uint32_t vtx, const uint32_t* texIdx, uint32_t group,
                 const uint32_t* coords, const RemMaterial& rem, uint32_t wrap, uint32_t cube,
                 const uint32_t* authored, const AnuvEntry* anim, AnuvCounts& counts) {
  const int nmaps = rem.macroLayered ? kMacroMaps : rem.layered || rem.macro ? kLayeredMaps : kMaps;
  Blob b;
  // ColorUnlit's colour, out of linear light, is (2 x gain)^(1/2.2) times the vertex
  // colour times the base map, both as stored: a stage multiplies by half that
  // (konst 0) and doubles. Exact bar the tone curve, which TEV has none of.
  const bool colorUnlit = ColorUnlitDraw(rem);
  // The emissive map is stored as authored (the PBR path scales it at run time), but the
  // fallback adds it as it is: it took the 0.10 the converter used to bake in, as a konst
  // (in the gamma domain TEV works in, that is 0.10^(1/2.2) of the byte).
  const bool emissiveKonst = rem.maps[kEmissive].has && !rem.maps[kEmissive].raw &&
                             !rem.maps[kEmissive].detail;
  const uint32_t emissiveSel = colorUnlit ? 0x0Du : 0x0Cu;  // GX_TEV_KCSEL_K1 / K0
  // A retail model's material sorts and writes depth as its mesh class does: classes 1 and 3
  // are Remastered's sorted pass, depth test only; 0 and 2 the unsorted one, depth written
  // (kb topic/mesh-blend-class.md).
  uint32_t passFlags = pm.flags & (0x10 | 0x80);
  if (rem.classBlend && rem.blendClass >= 0) {
    passFlags = rem.blendClass == 1 || rem.blendClass == 3 ? 0x10u : 0x80u;
  }
  const uint32_t flags = (pm.flags & 0xFFFF & ~uint32_t(0x8 | 0x10 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800 | 0x2000)) |
                         passFlags | 0xF0000 | kPbrFlag | (colorUnlit || emissiveKonst ? 0x8u : 0u);
  P32(b, flags);
  P32(b, uint32_t(nmaps));
  for (int i = 0; i < nmaps; ++i) {
    P32(b, texIdx[i]);
  }
  // The vertex descriptor is retail's with a texcoord slot for every Remastered set: it
  // fixes how many texcoord attributes, and so how many indices per vertex, the display
  // list carries.
  P32(b, vtx);
  // The material's cache id: CCubeMaterial::SetCurrent skips the vertex layout
  // and TEV when it matches the previous draw's, so it must differ from retail's.
  P32(b, group);
  if (colorUnlit || emissiveKonst) {
    P32(b, (colorUnlit ? 1u : 0u) + (emissiveKonst ? 1u : 0u));
    if (colorUnlit) {
      const double half = 0.5 * std::pow(2.0 * std::max(rem.backlight, 0.0), 1.0 / 2.2);
      const uint32_t k = uint32_t(std::lround(std::clamp(half, 0.0, 1.0) * 255.0));
      P32(b, k << 24 | k << 16 | k << 8 | 0xFF);
    }
    if (emissiveKonst) {
      const uint32_t k = uint32_t(std::lround(std::pow(kPbrEmissive, 1.0 / 2.2) * 255.0));
      P32(b, k << 24 | k << 16 | k << 8 | 0xFF);
    }
  }
  uint32_t blendSrc = 0;
  uint32_t blendDst = 0;
  PbrBlend(rem, pm, blendSrc, blendDst);
  P16(b, blendDst);
  P16(b, blendSrc);
  // An unlit surface coloured by its vertices (a door shield) keeps that in the
  // fallback too, which is what draws it whenever the model is not opaque: channel
  // 0 unlit with the vertex colour as its material colour (bit 2), and the alpha
  // taken from it as well.
  const bool vertexGlow = rem.unlit && rem.tinted;
  P32(b, vertexGlow ? 1u : uint32_t(pm.chans.size()));
  if (vertexGlow) {
    P32(b, 0x4);
  } else {
    for (uint32_t c : pm.chans) {
      P32(b, c);
    }
  }
  static const uint32_t tev[kLayeredMaps][3] = {
      {0x7A14F, 0x21CE7, 4},  // ZERO, RASC, TEXC, ZERO: base x channel 0; alpha = base
      {0x3D0F, 0x1CE7, 255},  // ZERO, TEXC, ZERO, CPREV: MR, sampled only
      {0x3D0F, 0x1CE7, 255},  // normal, sampled only
      {0x310F, 0x1CE7, 255},  // ZERO, TEXC, ONE, CPREV: + emissive
      {0x3D0F, 0x1CE7, 255},  // the second layer, sampled only
      {0x3D0F, 0x1CE7, 255},
      {0x3D0F, 0x1CE7, 255},
  };
  // Glass samples the frame behind it, which the game copies into map 7 (the
  // spare buffer's) before it draws one: a stage of its own binds it.
  const int nstages = rem.kind == 8 || rem.kind == 11 || rem.kind == 14 || rem.kind == 23 || rem.kind == 29 || rem.kind == 30 || rem.kind == 34 || rem.kind == 36 || rem.kind == 43 ? nmaps + 1 : nmaps;
  P32(b, uint32_t(nstages));
  for (int i = 0; i < nstages; ++i) {
    // Kind 20's map 3 is a detail map, sampled only (the table's stage adds it as glow).
    const uint32_t* t = tev[rem.maps[kEmissive].detail && i == kEmissive ? 2 : std::min(i, kLayeredMaps - 1)];
    const bool gain = colorUnlit && i == 1;
    const bool emissiveStage = emissiveKonst && i == kEmissive;
    P32(b, gain ? 0x7B80Fu : emissiveStage ? 0x390Fu : t[0]);  // gain: ZERO, CPREV, KONST, ZERO, then x2; emissive: ZERO, TEXC, KONST, CPREV
    P32(b, i == 0 && vertexGlow ? 0x39487u : t[1]);  // ZERO, TEXA, RASA, ZERO: base x vertex alpha
    P32(b, gain ? 0x140u : 0x100u);
    P32(b, 0x100);
    P8(b, 0);
    P8(b, 0);
    P8(b, gain ? 0x0C : emissiveStage ? emissiveSel : 0);  // GX_TEV_KCSEL_K0 (K1)
    P8(b, uint8_t(t[2]));
  }
  for (int i = 0; i < nstages; ++i) {
    P8(b, 0);
    P8(b, 0);
    // A macro-normal material has no second layer: its maps 4 and 5 are only placeholders, sampled
    // from map 0 so that the shader sees map 6 (the macro normal) without a layer base.
    P8(b, uint8_t(rem.macro && !rem.layered && (i == 4 || i == 5) ? 0 : i < nmaps ? i : 7));
    P8(b, uint8_t(i < nmaps ? coords[i] : 0));
  }
  // One texgen per texcoord the maps use. Each reads its own texcoord through the
  // identity (GX_TG_MTX3x4, TEXi, GX_IDENTITY, GX_PTIDENTITY): texgen word 0 would be
  // position through GX_TEXMTX0, a projection by whatever matrix the last animated
  // material loaded, which slid Magmoor's room rock along with the lava. Where
  // retail's texgen for the coord reads that same texcoord it is kept, with retail's
  // UV animations, so an animated coordinate still drives the maps; one over position
  // or normal (an env map) says nothing about a Remastered texcoord.
  const uint32_t n = 1 + *std::max_element(coords, coords + nmaps);
  std::vector<uint32_t> gens(n);
  for (uint32_t i = 0; i < n; ++i) {
    gens[i] = 0x1EBC00 | ((4 + i) << 4);
  }
  // A scroll of Remastered's own replaces retail's animations: the base map's
  // texcoord goes through GX_TEXMTX0, which a UV scroll (mode 2: offset, then
  // speed per second) loads.
  // The model's own ANUV animations win over both: each texcoord slot a map reads
  // through a transform that moves goes through a matrix of its own (GX_TEXMTXn, n
  // the animation's place in the list, as SetCurrent loads them), from the set the
  // AUVI picked. `authored` is the texcoord index the material named, which is what
  // picks the entry's transform.
  std::vector<const AnuvTransform*> moving;
  std::vector<int> matrixOf(n, -1);
  for (int i = 0; anim != nullptr && i < nmaps; ++i) {
    const uint32_t a = authored[i];
    if (a >= 3 || anim->xf[a].Identity()) {
      continue;
    }
    if (matrixOf[coords[i]] < 0) {
      matrixOf[coords[i]] = int(moving.size());
      moving.push_back(&anim->xf[a]);
    } else if (moving[size_t(matrixOf[coords[i]])] != &anim->xf[a]) {
      ++counts.sharedSlots;  // two transforms on one slot: the first drives both
    }
  }
  const bool animated = !moving.empty();
  counts.animated += animated ? 1 : 0;
  const bool scroll = !animated && (rem.scroll[0] != 0.0 || rem.scroll[1] != 0.0);
  counts.scrollOverridden += animated && (rem.scroll[0] != 0.0 || rem.scroll[1] != 0.0) ? 1 : 0;
  bool retailGens = false;
  for (int i = 0; i < nmaps && !scroll && !animated; ++i) {
    const uint32_t c = coords[i];
    if (c < pm.texgen.size() && ((pm.texgen[c] >> 4) & 31) == 4 + c) {
      gens[c] = pm.texgen[c];
      retailGens = true;
    }
  }
  if (scroll) {
    gens[coords[kBase]] = 0x1E8000 | ((4 + coords[kBase]) << 4);  // GX_TG_MTX3x4, TEXc, GX_TEXMTX0, GX_PTIDENTITY
  }
  for (uint32_t c = 0; c < n; ++c) {
    if (matrixOf[c] >= 0) {
      gens[c] = 0x1E8000 | (uint32_t(3 * matrixOf[c]) << 9) | ((4 + c) << 4);  // TEXMTX0 + 3 per matrix
    }
  }
  P32(b, n);
  for (uint32_t g : gens) {
    P32(b, g);
  }
  if (animated) {
    std::vector<uint32_t> words;
    for (const AnuvTransform* xf : moving) {
      const std::vector<uint32_t> w = AnuvAnimWords(*xf);
      words.insert(words.end(), w.begin(), w.end());
    }
    P32(b, uint32_t(2 + words.size()) * 4);  // the section's size, the count, then the animations
    P32(b, uint32_t(moving.size()));
    for (uint32_t w : words) {
      P32(b, w);
    }
  } else if (scroll) {
    P32(b, 24);  // the count and one animation of five words
    P32(b, 1);
    P32(b, 2);
    PF(b, 0.0);
    PF(b, 0.0);
    PF(b, rem.scroll[0]);
    PF(b, rem.scroll[1]);
  } else if (retailGens && !pm.uvAnim.empty()) {
    b.insert(b.end(), pm.uvAnim.begin(), pm.uvAnim.end());
  } else {
    P32(b, 4);  // no UV animations
    P32(b, 0);
  }
  PbrRecord(b, rem, wrap, cube);
  return b;
}

}  // namespace ConvertDetail

}  // namespace PortRemastered
