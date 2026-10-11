#ifndef _CCUBEMODEL
#define _CCUBEMODEL

#include "CCubeSurface.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/TToken.hpp"
#include <rstl/vector.hpp>
#ifdef TARGET_PC
#include <vector>
#endif

class IObjectStore;
class CTexture;
class CTransform4f;
class CCubeSurface;
class CStopwatch;

enum ESurfaceSelection {
  kSS_Unsorted,
  kSS_Sorted,
  kSS_All,
};

class CCubeModel {
public:
  class ModelInstance {
  public:
    ModelInstance(rstl::vector< void* >& surfaces, const void* materialData, const void* positions,
                  const void* normals, const void* colors, const void* uvs,
                  const void* packedTexCoords, uint positionsSize = 0, uint normalsSize = 0,
                  uint colorsSize = 0, uint texCoordsSize = 0, uint packedTexCoordsSize = 0)
    : x0_surfacePtrs(surfaces)
    , x4_materialData(materialData)
    , x8_positions(positions)
    , xc_normals(normals)
    , x10_colors(colors)
    , x14_texCoords(uvs)
    , x18_packedTexCoords(packedTexCoords)
    , x1c_positionsSize(positionsSize)
    , x20_normalsSize(normalsSize)
    , x24_colorsSize(colorsSize)
    , x28_texCoordsSize(texCoordsSize)
    , x2c_packedTexCoordsSize(packedTexCoordsSize) {}

    rstl::vector< void* >& Surfaces() { return x0_surfacePtrs; }
    const rstl::vector< void* >& GetSurfaces() const { return x0_surfacePtrs; }
    const void* GetMaterialPointer() const { return x4_materialData; }
    void SetMaterialPointer(const void* mat) { x4_materialData = mat; }
    const void* GetVertexPointer() const { return x8_positions; }
    const void* GetNormalPointer() const { return xc_normals; }
    const void* GetColorPointer() const { return x10_colors; }
    const void* GetTCPointer() const { return x14_texCoords; }
    const void* GetPackedTCPointer() const { return x18_packedTexCoords; }
    uint GetVertexSize() const { return x1c_positionsSize; }
    uint GetNormalSize() const { return x20_normalsSize; }
    uint GetColorSize() const { return x24_colorsSize; }
    uint GetTCSize() const { return x28_texCoordsSize; }
    uint GetPackedTCSize() const { return x2c_packedTexCoordsSize; }

  private:
    rstl::vector< void* >& x0_surfacePtrs;
    const void* x4_materialData;
    const void* x8_positions;
    const void* xc_normals;
    const void* x10_colors;
    const void* x14_texCoords;
    const void* x18_packedTexCoords;
    uint x1c_positionsSize;
    uint x20_normalsSize;
    uint x24_colorsSize;
    uint x28_texCoordsSize;
    uint x2c_packedTexCoordsSize;
  };
  CCubeModel(rstl::vector< void* >* surfaces, rstl::vector< TCachedToken< CTexture > >* textures,
             const void* materialData, const void* positions, const void* normals,
             const void* colors, const void* uvs, const void* compressedUvs, const CAABox& bounds,
             uchar visorFlags, bool texturesLoaded, uint idx, uint positionsSize = 0,
             uint normalsSize = 0, uint colorsSize = 0, uint texCoordsSize = 0,
             uint packedTexCoordsSize = 0);
  static void SetRenderModelBlack(bool v);
  static void SetModelWireframe(bool v);
  void UnlockTextures() const;
  void RemapMaterialData(const void* data, rstl::vector< TCachedToken< CTexture > >* texture);
  void DrawNormal(const float* positions, const float* normals, ESurfaceSelection which) const;
  static void DisableShadowMaps();
  static void EnableShadowMaps(const CTexture*, const CTransform4f&, unsigned char, unsigned char);
  static void SetNewPlayerPositionAndTime(const CVector3f&, const CStopwatch&);
  static void SetDrawingOccluders(bool);
  static void MakeTexturesFromMats(const void* data,
                                   rstl::vector< TCachedToken< CTexture > >& textures,
                                   IObjectStore& store, bool cache);

  const ModelInstance& GetModelInstance() const { return x0_instance; }
  bool AreTexturesLoaded() const { return !x40_24_loadTextures; }

  const void* GetPositions() const { return x0_instance.GetVertexPointer(); }
  const void* GetNormals() const { return x0_instance.GetNormalPointer(); }

  const CAABox& GetBoundingBox() const { return x20_bounds; }
  const CCubeSurface& GetNormalSurfaces() const { return x38_firstUnsorted; }
  const CCubeSurface& GetAlphaSurfaces() const { return x3c_firstSorted; }
  bool GetShouldDrawWorldFlag() const { return x40_25_visible; }
  void SetShouldDrawWorldFlag(bool shouldDraw) { x40_25_visible = shouldDraw; }
  uchar GetModelFlags() const { return x41_visorFlags; }
  // CMDL flag 0x8 (the port's converted models): each normal entry is N, B, T (9 floats), kept in
  // x41_visorFlags' bit 7. Drawn through GX_VTXFMT3 (DL opcode 0x93).
  static constexpr uchar kNbtNormals = 0x80;
  bool HasNbtNormals() const { return (x41_visorFlags & kNbtNormals) != 0; }
  // x41_visorFlags' bit 6, on top of kNbtNormals: fifteen floats (N, B0, T0, B1, T1), the
  // second frame being TANGENT_1 of the two-tangent layered shaders. Drawn through
  // GX_VTXFMT4 (DL opcode 0x94).
  static constexpr uchar kNbtNormals5 = 0x40;
  bool HasNbtNormals5() const { return (x41_visorFlags & kNbtNormals5) != 0; }
  // Vectors per normal entry: N, then B and T (NBT), then B1 and T1 (NBT5).
  uint NormalVecs() const { return HasNbtNormals5() ? 5 : HasNbtNormals() ? 3 : 1; }
  int GetModelIndex() const { return x44_idx; } // TODO: name

  CCubeMaterial GetMaterialByIndex(const int idx) const;
#ifdef TARGET_PC
  // Sends the material's PBR record and the draw's fade (see GXSetPBRLightScale); returns
  // the record's surface kind (glass is 8). cube, when given, gets the file id of the
  // material's own reflection cube ('PBR7'; see PortRoomEnv::MaterialCube), 0 without one.
  // frameExposed: GlowScale does not scale this draw (see PortRoomEnv::GlowGain).
  float PortSetPBRMaterial(const int idx, const float fade, const bool fadeReplaces,
                           const bool frameExposed, uint* cube = nullptr) const;
  // The material's record (see the definition) with the neutral values where it has
  // none; returns how many floats the record holds, 0 without one. wrap, when given, gets
  // the maps' sampler modes: map i's S mode in bits 4i..4i+1, its T mode in 4i+2..4i+3
  // (CTexture::EClampMode); all repeat without a 'PBR5' record.
  // lightScale, when given, gets the diffuse and F0 factors of a back-facing copy ('PBR6'):
  // 1, 1 without one. cube, when given, gets the reflection cube's file id ('PBR7'), 0
  // without one. shield, when given, gets the 32 floats of a kind 14 material's 'PBR8' trailer
  // (all zero without one).
  int PortReadPBRMaterial(const int idx, float values[19], uint* wrap = nullptr,
                          float lightScale[2] = nullptr, uint* cube = nullptr, float* shield = nullptr) const;
  uint PortMaterialCount() const;
  // The vertex texcoord slot of the material's lightmap UV (its 'LMUV' trailer), or -1.
  int PortLightmapSlot(const int idx) const;
  // For a PBR material drawn by its embedded TEV: its emissive konst follows the room's
  // exposure as the PBR path's glow does (the converter bakes a fixed 0.10 there).
  void PortSetFallbackGlow(const CCubeMaterial& material, int idx, bool frameExposed) const;
  // Debugging: draws a model's material with values[field] replaced, until cleared. The
  // caller must clear before the model goes.
  static void PortOverridePBR(const CCubeModel* model, int material, int field, float value);
  static void PortClearPBROverrides();
  // Until called again with null: every PBR material drawn glows in this colour, which stands
  // in for the emissive strength Remastered gave the material (ICNC), as a ColorModulateMP1
  // in its incandescence mode does (PortRoomGeo::Instance::glow).
  static void PortSetGlow(const float* rgb);
  // Until called again with null: every PBR material drawn is a sky's, unlit and its colour
  // multiplied by this (a Remastered Skybox's colour and intensity, exposed; CWorld::DrawSky).
  static void PortSetSky(const float* rgb);
  // The beam's charge, 0 to 1, which the Ice Beam cannon's frost shell (kind 12) dissolves
  // with: Remastered's DisintegrationAmount. At 0 the shell is not drawn.
  static void PortSetChargeShell(float amount);
  // The Remastered Phazon Beam's DisintegrationAmount, 0 to 1 (CPhazonBeamMP1::Update), which
  // the veins material (kind 35) eats its mask with. Reset to 0 when the beam unloads.
  static void PortSetDisintegration(float amount);
  // The CMDL this model was loaded from (0 for an area's models), for diagnostics.
  void PortSetAssetId(uint id) { xPort_assetId = id; }
  uint PortAssetId() const { return xPort_assetId; }

  // Draw identification (console `drawlog`, `pick`, `view drawid`). While the log or the draw id
  // view is on, every surface drawn gets a serial (1.., 24 bits), which Aurora can draw as a
  // colour and note the shader of, and a PortDraw entry for the frame. Off, a draw pays one
  // branch on a flag.
  struct PortDraw {
    uint serial;
    const CCubeModel* model;
    uint asset;       // the CMDL's file id, 0 for an area's model
    uint modelIndex;  // index in its area (area models)
    uint material;
    uint surface;     // place in the model's surface chains, unsorted first
    uint flags;       // the material's flags
    int floats;       // how many floats its record holds
    uint wrap;
    bool scaled;      // 'PBR6'
    uint cube;        // 'PBR7'
    float values[19]; // as drawn (neutral where the record has none)
    uint mode;        // values[7]
    float kind;       // values[13]
    bool pbr;         // drawn through the PBR path
  };
  static void PortSetDrawLog(bool on);
  static void PortSetDrawIds(bool on);
  static bool PortDrawLogOn();
  // The entry of a serial in the frame being drawn or the last 8 before it.
  static bool PortFindDraw(uint serial, PortDraw& out);
  // The last completed frame's entries, in draw order.
  static void PortLastFrameDraws(std::vector< PortDraw >& out);
  // A model that has drawn since the log went on, by CMDL file id (null when none has).
  static const CCubeModel* PortFindModel(uint asset);
  // The record's name: 'PBRM' (six floats) to 'PBR7', 'WRAP', or 'none'.
  static const char* PortRecordTag(int floats, uint wrap, bool scaled, uint cube);
  // Names the draws of `surface` (when numbering) and sets Aurora's serial; the caller resets it.
  uint PortBeginDraw(const CCubeSurface& surface, bool pbr) const;
#endif
  void SetStaticArraysCurrent() const;
  void SetArraysCurrent() const;
  void SetSkinningArraysCurrent(const float* positions, const float* normals) const;
  void SetUsingPackedLightmaps(const bool use) const;
  static bool IsUsingPackedLightmaps() { return sUsingPackedLightmaps; }
  void DrawSurface(const CCubeSurface& surface, const CModelFlags& modelFlags) const;
  void DrawSurfaceWireframe(const CCubeSurface& surface) const;
  void DrawFlat(const float* positions, const float* normals, ESurfaceSelection which) const;
  bool TryLockTextures() const;
  void Draw(const CModelFlags& flags) const;
  void Draw(const float* positions, const float* normals, const CModelFlags& flags) const;
  void DrawNormal(const CModelFlags& flags) const;
  void DrawAlpha(const CModelFlags& flags) const;
  void DrawSurfaces(const CModelFlags& flags) const;
  void DrawNormalSurfaces(const CModelFlags& flags) const;
  void DrawAlphaSurfaces(const CModelFlags& flags) const;

  rstl::vector< TCachedToken< CTexture > >& GetTextures() const { return *x1c_textures; };

private:
  ModelInstance x0_instance;
  rstl::vector< TCachedToken< CTexture > >* x1c_textures;
  CAABox x20_bounds;
  CCubeSurface x38_firstUnsorted;
  CCubeSurface x3c_firstSorted;
  mutable bool x40_24_loadTextures : 1;
  bool x40_25_visible : 1;
  uchar x41_visorFlags;
  int x44_idx;
#ifdef TARGET_PC
  uint xPort_assetId = 0;
#endif

  static bool sUsingPackedLightmaps;
};

#endif // _CCUBEMODEL
