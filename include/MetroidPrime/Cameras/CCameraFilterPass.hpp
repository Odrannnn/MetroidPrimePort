#ifndef _CCAMERAFILTERPASS
#define _CCAMERAFILTERPASS

#include "types.h"

#include "MetroidPrime/TGameTypes.hpp"

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/TToken.hpp"

#include "rstl/auto_ptr.hpp"

class CTexture;

class CCameraFilterPass {
public:
  enum EFilterType {
    kFT_Passthru,
    kFT_Multiply,
    kFT_Invert,
    kFT_Add,
    kFT_Subtract,
    kFT_Blend,
    kFT_Widescreen,
    kFT_SceneAdd,
    kFT_NoColor,
    kFT_InvDstMultiply,
  };
  enum EFilterShape {
    kFS_Fullscreen,
    kFS_FullscreenHalvesLeftRight,
    kFS_FullscreenHalvesTopBottom,
    kFS_FullscreenQuarters,
    kFS_CinemaBars,
    kFS_ScanLinesEven,
    kFS_ScanLinesOdd,
    kFS_RandomStatic,
    kFS_CookieCutterDepthRandomStatic,
#ifdef TARGET_PC
    // Port: Remastered's DrawFilterShape shape 0xd, DrawFullScreenTintedRadialGradient 0xfaf650.
    kFS_PortRadialGradient = 0xd,
#endif
  };

  CCameraFilterPass();

  void SetFilter(const EFilterType type, const EFilterShape shape, const float time,
                 const CColor& color, const CAssetId txtr);
  void DisableFilter(float time);
  void Update(float dt);
  void Draw() const;

  float GetT(bool invert) const;
  // Port: a filter is applied or still fading (the pass is not a settled passthru).
  bool PortIsActive() const { return x10_remTime > 0.f || x0_curType != kFT_Passthru; }
  void PortDescribe(char* out, unsigned int size) const;
  // Port: the colour of a plain fullscreen Add filter (what Draw would add), false for any other.
  bool PortAddColor(CColor& out) const;
  // Port: SetFilter with Remastered's SRadialGradientColorData (c0 apex, c1 rim, c2 the texture
  // mod colour) and the radial shape. Any other SetFilter drops the gradient.
  void PortSetRadialFilter(EFilterType type, const CColor& color, const float c0[4],
                           const float c1[4], const float c2[4]);
  // Port: the gradient of an applied radial filter, false for any other.
  bool PortRadialGradient(float c0[4], float c1[4], float c2[4]) const;

  static void DrawWideScreen(const CColor& color, const CTexture* tex, float v);
  static void DrawFilter(EFilterType type, EFilterShape shape, const CColor& color,
                         const CTexture* tex, float lod);
  static void DrawFilterShape(EFilterShape shape, const CColor& color, const CTexture* tex,
                              float lod);
  static void DrawFullScreenColoredQuad(const CColor& color);
  static void DrawFullScreenTexturedQuad(const CColor& color, const CTexture* tex, float lod);
  static void DrawFullScreenTexturedQuadQuarters(const CColor& color, const CTexture* tex,
                                                 float lod);
  static void DrawScanLines(const CColor& color, bool even);
  static void DrawRandomStatic(const CColor& color, float alpha, bool cookieCutter);

private:
  EFilterType x0_curType;
  EFilterType x4_nextType;
  EFilterShape x8_shape;
  float xc_duration;
  float x10_remTime;
  CColor x14_prevColor;
  CColor x18_curColor;
  CColor x1c_nextColor;
  CAssetId x20_nextTxtr;
  rstl::auto_ptr< TLockedToken< CTexture > > x24_texObj;
#ifdef TARGET_PC
  float xPortGradient[3][4] = {};
  bool xPortGradientEngaged = false;
  void PortDrawRadialGradient() const;
#endif
};
CHECK_SIZEOF(CCameraFilterPass, 0x2c)

#endif // _CCAMERAFILTERPASS
