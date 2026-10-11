#ifndef _CCUBEMATERIAL
#define _CCUBEMATERIAL

#include "types.h"
#include "Kyoto/Basics/CBasics.hpp"

#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CVector3f.hpp"

enum EStateFlags {
  kStateFlag_KonstValues = (1 << 3),
  kStateFlag_DepthSorting = (1 << 4),
  kStateFlag_AlphaTest = (1 << 5),
  kStateFlag_Reflection = (1 << 6),
  kStateFlag_DepthWrite = (1 << 7),
  kStateFlag_ReflectionSurfaceEye = (1 << 8),
  kStateFlag_ShadowOccluderMesh = (1 << 9),
  kStateFlag_ReflectionIndirectTexture = (1 << 10),
  kStateFlag_Lightmap = (1 << 11),
  kStateFlag_LightmapUvArray = (1 << 13),
#ifdef TARGET_PC
  // Port: unused by retail data. Converted mod materials set it to be shaded
  // with Aurora's PBR path (maps 0-3 = base, ORM, normal, emissive).
  kStateFlag_PortPBR = (1 << 14),
  // Port: unused by retail data. A converted HUD picture whose Remastered shader also adds
  // its base map, unscaled by the widget colour (3853595c: rgb = T*v1 + T*ICNC, ICNC white).
  // Drawn once, with the widget's own blend, and TEV stage 0 as T + T*c (clamped at 1, as the
  // shader output is into the HUD's UNORM target after tone mapping).
  kStateFlag_PortHudGlow = (1 << 15),
  // Port: above the texture slot mask (bits 16..23, one per GX texmap), unused by retail and
  // converted data. A converted HUD picture whose Remastered shader squares the filtered
  // alpha it samples (ca1106b0 with unk1&1, 3853595c, 2d606234).
  kStateFlag_PortHudSquare = (1 << 24),
  // Port: as kStateFlag_PortHudSquare. A converted HUD picture drawn with Remastered's
  // UI_Interference (ad2c208c); bits 26..28 index PortRemastered::kHudInterferenceRows.
  kStateFlag_PortHudInterference = (1 << 25),
  kStateFlag_PortHudInterferenceShift = 26,
#endif
  kStateFlag_TextureSlotMask = static_cast< uint >(~kStateFlag_LightmapUvArray),
};

class CCubeSurface;
class CCubeModel;
class CCubeMaterial {
public:
  explicit CCubeMaterial(const void* data) : x0_data(data) {}
  static void ResetCachedMaterials();
  static void EnsureViewDepStateCached(const CCubeSurface* surface);
  static void EnsureTevsDirect();
  static void KillCachedViewDepState();

  inline const uchar* GetData() const { return static_cast< const uchar* >(x0_data); }
  uint GetFlags() const { return CBasics::SwapBytes(*reinterpret_cast< const uint* >(GetData())); }
  bool IsFlagSet(const EStateFlags flag) const { return (GetFlags() & flag) != 0; }
  void SetCurrent(const CModelFlags& flags, const CCubeSurface& surface,
                  const CCubeModel& mode) const;
  void SetCurrentBlack() const;
#ifdef TARGET_PC
  // Port: whether a kStateFlag_PortPBR material may use the PBR path for this
  // draw. The black, shadow-map, thermal and blended paths keep the TEV.
  static bool PortPBRAllowed(const CModelFlags& flags);
  // Port: whether a glass material's screen stage (map 7) is drawn: the projected shadow
  // takes map 7, and the thermal visor draws only the first stage.
  static bool PortScreenCopyUsed();
  // Port: the PBR reflection probe. The weight is 1 once CStateManager has filled all six
  // faces, and the draw count tells it whether anything would reflect the probe.
  static float sPortPBRProbeWeight;
  // 0 off, 1 on, 2 mirror, 3 window; -1 takes it from MP_PBR_PROBE on first use.
  static int sPortPBRProbeMode;
  static uint sPortPBRDraws;
  // Port: the converted Remastered HUD's DYIN (x = 1 - the HUD's fade-in alpha, y = random;
  // CSamusHud::Draw sets it while the HUD fades in, zero otherwise).
  static float sPortHudDyin[2];
  // The PBR draws outside the probe's own capture that reflected the live probe rather than a
  // room environment's cube; while none do, the capture is skipped.
  static uint sPortPBRProbeDraws;
  static bool sPortCapturingProbe;
  // Port: a draw that keeps PBR under the thermal visor, as the fluid planes keep their own
  // shader there (port_room_liquid.cpp); additive is the hot pass's blend.
  enum EPortPBRThermal { kPT_None, kPT_Cold, kPT_Additive };
  static EPortPBRThermal sPortPBRThermal;
  // Port: the light slots holding area lights (CActorLights::ActivateLights; CGraphics::
  // LoadLight clears a slot it loads). A PBR draw lit by a room's baked light leaves them
  // out, as Remastered lights actors by the bake and runtime lights alone.
  static uint sPortAreaLights;
  // Port: whether the material draws differently once its model's vertices are moved into
  // world space and drawn with an identity model matrix (port_room_geo.cpp's merged props):
  // reflections and UV animation mode 6 place the texture by the model matrix itself.
  bool PortNeedsModelMatrix() const;
  // Port: how many TEV konst colours the material loads (0 without kStateFlag_KonstValues).
  uint PortKonstCount() const;
#endif
  uint GetTextureCount() const {
    return CBasics::SwapBytes(*reinterpret_cast< const uint* >(GetData() + 4));
  }
  uint GetVertexDesc() const {
    return CBasics::SwapBytes(*reinterpret_cast< const uint* >(
        GetData() + (GetTextureCount() * sizeof(uint) + sizeof(uint) * 2)));
  }
  
  // TODO: Figure out wtf is going on here
  uint GetVertexDescLwzx() const {
    return CBasics::SwapBytes(static_cast< const uint* >(x0_data)[GetTextureCount() + 2]);
  }

  uint GetCompressedBlend() const;

  static const CVector3f& GetViewingReflection() { return sViewingFrom; }

private:
  static void SetupBlendMode(uint blendFactors, const CModelFlags& flags, bool alphaTest);
  static uint HandleReflection(bool usesTevReg2, GXTexMapID indTexSlot, int indMtxScaleExp,
                               uint tevCount, uint texCount, uint tcgCount,
                               uint finalKColorCount, uint& finalCCFlags, uint& finalACFlags);

  static const CCubeModel* sLastModelCached;
  static const CCubeModel* sRenderingModel;
  static CVector3f sViewingFrom;
  const void* x0_data;
};

#endif // _CCUBEMATERIAL
