#include "Kyoto/Particles/CPortVfxMat.hpp"

#ifdef TARGET_PC
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "aurora/vfx.hpp"

CTexture* CreateTexture(int value);

// VMAT v2, v3 (v2 followed by opacity gain/bias), v4 (v3 followed by the colour map's
// alpha exponent) and v5 (v4 followed by a flags word, bit 0 per-vertex lighting). An unknown
// version is skipped
// (the material stays absent, so the PART draws as retail).
bool PortReadVfxMat(CPortVfxData& vfx, CInputStream& in, u32 nbytes, CSimplePool* pool) {
  CPortVfxMat& m = vfx.mat;
  m = CPortVfxMat();
  u32 consumed = 0;
  const auto skipRest = [&]() {
    for (; nbytes - consumed >= 4; consumed += 4) {
      in.ReadLong();
    }
    for (; consumed < nbytes; ++consumed) {
      in.ReadChar();
    }
  };
  if (nbytes < 16) {
    skipRest();
    return true;
  }
  const u32 version = in.ReadLong();
  const u32 features = in.ReadLong();
  const u32 blend = in.ReadLong();
  const u32 ntex = in.ReadLong();
  consumed = 16;
  // Eleven words per texture; seven slots, two ramp rows, add row, eleven original
  // three-word sources, modulate/depthSoften/spriteCenter. v3 adds two floats, v4 one more, v5 a
  // flags word.
  constexpr u32 kTexWords = 11;
  constexpr u32 kTailWords = 7 + 2 + 1 + 3 * 11 + 3;
  const u32 extraWords = version == 3 ? 2 : version == 4 ? 3 : version == 5 ? 4 : 0;
  if (version < 2 || version > 5 /* CPortVfxMat::Supported */ || ntex > 4 || blend > aurora::gfx::vfx::MaxBlend ||
      nbytes != (kTexWords * ntex + kTailWords + extraWords) * 4 + 16) {
    skipRest();
    return true;
  }
  m.version = version;
  m.features = features;
  m.blend = blend;
  for (u32 i = 0; i < ntex; ++i) {
    const CAssetId id = in.ReadLong();
    TToken< CTexture > tok = id == 0
                                 ? TToken< CTexture >(CreateTexture(-1))
                                 : TToken< CTexture >(pool->GetObj(SObjectTag('TXTR', id)));
    // Assigning into a default (null) token would release a null reference.
    CPortVfxMat::Tex t{TCachedToken< CTexture >(tok)};
    t.uvSet = in.ReadLong();
    t.wrapS = in.ReadLong();
    t.wrapT = in.ReadLong();
    t.linear = in.ReadLong();
    t.cols = in.ReadLong();
    t.rows = in.ReadLong();
    t.layers = in.ReadLong();
    t.warped = in.ReadLong();
    t.warpScale[0] = in.ReadFloat();
    t.warpScale[1] = in.ReadFloat();
    m.tex.push_back(t);
  }
  m.colorSlot = in.ReadInt32();
  m.opacitySlot = in.ReadInt32();
  m.rampSlot = in.ReadInt32();
  m.ramp2Slot = in.ReadInt32();
  m.thresholdSlot = in.ReadInt32();
  m.indirectSlot = in.ReadInt32();
  m.paletteSlot = in.ReadInt32();
  m.rampRow[0] = in.ReadInt32();
  m.rampRow[1] = in.ReadInt32();
  m.addRow = in.ReadInt32();
  for (CPortVfxMat::Src& s : m.src) {
    s.row = in.ReadInt32();
    s.comp = in.ReadInt32();
    s.value = in.ReadFloat();
  }
  m.modulate = in.ReadFloat();
  m.depthSoften = in.ReadFloat();
  m.spriteCenter = in.ReadLong();
  if (version >= 3) {
    m.opacityGain = in.ReadFloat();
    m.opacityBias = in.ReadFloat();
  }
  if (version >= 4) {
    m.alphaGamma = in.ReadFloat();
  }
  if (version >= 5) {
    m.litPerVertex = (in.ReadLong() & 1) != 0;
  }
  return true;
}
#endif
