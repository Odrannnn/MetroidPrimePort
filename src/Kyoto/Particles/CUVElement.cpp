#include "Kyoto/Particles/CUVElement.hpp"

#include "Kyoto/Graphics/CTexture.hpp"

#include "rstl/math.hpp"

#ifdef TARGET_PC
#include "Kyoto/Particles/CParticleGlobals.hpp"
#endif

CUVEConstant::CUVEConstant(TToken< CTexture > tex) : x4_tex(tex) {}

CUVEConstant::~CUVEConstant() {}

void CUVEConstant::GetValueUV(int frame, SUVElementSet& valOut) const {
  valOut.xMin = 0.f;
  valOut.yMin = 0.f;
  valOut.xMax = 1.f;
  valOut.yMax = 1.f;
}

TLockedToken< CTexture > CUVEConstant::GetValueTexture(int frame) const { return x4_tex; }

CUVEAnimTexture::CUVEAnimTexture(TToken< CTexture > tex, CIntElement* tileW, CIntElement* tileH,
                                 CIntElement* strideW, CIntElement* strideH,
                                 CIntElement* cycleFrames, const bool loop)
: x4_tex(tex), x24_loop(loop) {
  int result = 0;
  tileW->GetValue(0, result);
  x10_tileW = result;
  delete tileW;

  tileH->GetValue(0, result);
  x14_tileH = result;
  delete tileH;

  strideW->GetValue(0, result);
  x18_strideW = result;
  delete strideW;

  strideH->GetValue(0, result);
  x1c_strideH = result;
  delete strideH;

  x28_cycleFrames = cycleFrames;

  const int width = x4_tex->GetWidth();
  const int height = x4_tex->GetHeight();
#ifdef TARGET_PC
  // Port: a 0 stride from particle data is a SIGFPE on x86 (silent on PPC); one tile on that axis.
  const int xTiles = x18_strideW != 0 ? rstl::max_val(1, width / x18_strideW) : 1;
  const int yTiles = x1c_strideH != 0 ? rstl::max_val(1, height / x1c_strideH) : 1;
#else
  const int xTiles = rstl::max_val(1, width / x18_strideW);
  const int yTiles = rstl::max_val(1, height / x1c_strideH);
#endif

  x20_tiles = xTiles * yTiles;
  x2c_uvElems.reserve(xTiles * yTiles);

  int x;
  int y;
  for (y = yTiles - 1; y >= 0; --y) {
    for (x = 0; x < xTiles; ++x) {
      SUVElementSet uvs;
      uvs.xMin = static_cast< float >(x18_strideW * x) / static_cast< float >(width);
      uvs.yMin = static_cast< float >(x1c_strideH * y) / static_cast< float >(height);
      uvs.xMax = static_cast< float >((x18_strideW * x) + x10_tileW) / static_cast< float >(width);
      uvs.yMax = static_cast< float >((x1c_strideH * y) + x14_tileH) / static_cast< float >(height);
      x2c_uvElems.push_back(uvs);
    }
  }
}

CUVEAnimTexture::~CUVEAnimTexture() { delete x28_cycleFrames; }

void CUVEAnimTexture::GetValueUV(int frame, SUVElementSet& valOut) const {
  int cv = 1;
  x28_cycleFrames->GetValue(frame, cv);
  float cvf =
      static_cast< float >(frame) / (static_cast< float >(cv) / static_cast< float >(x20_tiles));

  int tile;
  if (x24_loop) {
    tile = rstl::max_val(static_cast< int >(cvf), 0);
    if (tile >= x20_tiles) {
      tile = tile % x20_tiles;
    }
  } else {
    tile = static_cast< int >(cvf);
    if (static_cast< int >(cvf) >= x20_tiles) {
      tile = x20_tiles - 1;
    }
  }

  valOut = x2c_uvElems[tile];
}

TLockedToken< CTexture > CUVEAnimTexture::GetValueTexture(int frame) const { return x4_tex; }

#ifdef TARGET_PC
static int PortEvalInt(CIntElement* elem, int def) {
  int result = def;
  if (elem != nullptr) {
    elem->GetValue(0, result);
    delete elem;
  }
  return result;
}

CUVEAtlasTexture::CUVEAtlasTexture(TToken< CTexture > tex, CIntElement* cols, CIntElement* rows,
                                   CIntElement* count, CIntElement* mode, CIntElement* flipX)
: x4_tex(tex) {
  x10_cols = rstl::max_val(1, PortEvalInt(cols, 1));
  x14_rows = rstl::max_val(1, PortEvalInt(rows, 1));
  x18_count = rstl::max_val(1, PortEvalInt(count, 1));
  x1c_mode = PortEvalInt(mode, 0);
  x20_flipX = PortEvalInt(flipX, 0) != 0;
}

CUVEAtlasTexture::~CUVEAtlasTexture() {}

int CUVEAtlasTexture::SelectTile(uint seed, int frame, int lifeFrames) const {
  if (x1c_mode == 1) {
    float life = static_cast< float >(frame) / static_cast< float >(rstl::max_val(1, lifeFrames));
    life = rstl::min_val(1.f, rstl::max_val(0.f, life));
    const int last = rstl::min_val(x18_count, x10_cols * x14_rows) - 1;
    return rstl::min_val(last, static_cast< int >(life * static_cast< float >(x18_count)));
  }
  return static_cast< int >((seed % static_cast< uint >(x18_count)) %
                            static_cast< uint >(x10_cols * x14_rows));
}

void CUVEAtlasTexture::TileUV(int tile, bool flip, SUVElementSet& valOut) const {
  // v counts from texel row 0, like ATEX's yMin; the quads put yMax on their top edge.
  const int col = tile % x10_cols;
  const int row = tile / x10_cols;
  const float x0 = static_cast< float >(col) / static_cast< float >(x10_cols);
  const float x1 = static_cast< float >(col + 1) / static_cast< float >(x10_cols);
  valOut.xMin = flip ? x1 : x0;
  valOut.xMax = flip ? x0 : x1;
  valOut.yMin = static_cast< float >(row) / static_cast< float >(x14_rows);
  valOut.yMax = static_cast< float >(row + 1) / static_cast< float >(x14_rows);
}

void CUVEAtlasTexture::GetValueUV(int frame, SUVElementSet& valOut) const {
  const CElementGen::CParticle* particle = CParticleGlobals::xPortUVParticle;
  if (particle == nullptr) {
    TileUV(0, false, valOut);
    return;
  }
  TileUV(SelectTile(particle->xPortSeed, frame, particle->x0_endFrame - particle->x28_startFrame),
         FlipFor(particle->xPortSeed), valOut);
}

TLockedToken< CTexture > CUVEAtlasTexture::GetValueTexture(int frame) const { return x4_tex; }
#endif
