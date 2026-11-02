#ifndef _CUVELEMENT
#define _CUVELEMENT

#include "types.h"

#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/TToken.hpp"

class CTexture;

class CUVEConstant : public CUVElement {
  TLockedToken< CTexture > x4_tex;

public:
  CUVEConstant(TToken< CTexture > tex);
  ~CUVEConstant() override;
  TLockedToken< CTexture > GetValueTexture(int frame) const override;
  void GetValueUV(int frame, SUVElementSet& valOut) const override;
  bool HasConstantTexture() const override { return true; }
  bool HasConstantUV() const override { return true; }
};

class CUVEAnimTexture : public CUVElement {
  TLockedToken< CTexture > x4_tex;
  int x10_tileW;
  int x14_tileH;
  int x18_strideW;
  int x1c_strideH;
  int x20_tiles;
  bool x24_loop;
  CIntElement* x28_cycleFrames;
  rstl::vector< SUVElementSet > x2c_uvElems;

public:
  CUVEAnimTexture(TToken< CTexture > tex, CIntElement* tileW, CIntElement* tileH,
                  CIntElement* strideW, CIntElement* strideH, CIntElement* cycleFrames, bool loop);
  ~CUVEAnimTexture() override;
  TLockedToken< CTexture > GetValueTexture(int frame) const override;
  void GetValueUV(int frame, SUVElementSet& valOut) const override;
  bool HasConstantTexture() const override { return true; }
  bool HasConstantUV() const override { return false; }
};

#ifdef TARGET_PC
// Port-only 'PATL': an atlas of cols x rows equal tiles, tile k in column k % cols and row
// k / cols counted from texel row 0. Mode 0 picks a tile per particle from its seed, mode 1
// flips through `count` tiles over the particle's life. flipX mirrors half the particles.
// Port-only 'PAFB' (mode 2): Remastered's ATFB, `count` tiles over a cycle of frames, an int
// element read as a real, looping or holding the last (CUVEAnimFlipBookTexture::GetValue 0x2cde08).
class CUVEAtlasTexture : public CUVElement {
  TLockedToken< CTexture > x4_tex;
  int x10_cols;
  int x14_rows;
  int x18_count;
  int x1c_mode;
  bool x20_flipX;
  bool x21_loop = false;
  CIntElement* x24_cycle = nullptr;

public:
  CUVEAtlasTexture(TToken< CTexture > tex, CIntElement* cols, CIntElement* rows, CIntElement* count,
                   CIntElement* mode, CIntElement* flipX);
  CUVEAtlasTexture(TToken< CTexture > tex, CIntElement* cols, CIntElement* rows, CIntElement* count,
                   CIntElement* loop, CIntElement* flipX, CIntElement* cycle);
  ~CUVEAtlasTexture() override;
  TLockedToken< CTexture > GetValueTexture(int frame) const override;
  void GetValueUV(int frame, SUVElementSet& valOut) const override;
  bool HasConstantTexture() const override { return true; }
  bool HasConstantUV() const override { return false; }
  // Pure tile selection, exposed for tests. `lifeFrames` is endFrame - startFrame.
  int SelectTile(uint seed, int frame, int lifeFrames) const;
  void TileUV(int tile, bool flip, SUVElementSet& valOut) const;
  bool FlipFor(uint seed) const { return x20_flipX && ((seed >> 16) & 1) != 0; }
};
#endif

#endif // _CUVELEMENT
