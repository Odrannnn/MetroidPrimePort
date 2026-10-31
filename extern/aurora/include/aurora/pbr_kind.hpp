#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aurora {

// A Remastered material's special surface kind: the converter's RemMaterial::kind, the PBR record's
// values[13], GXSetPBRMaterial's pbr_layer.y and, built into the shader, ShaderConfig::pbrKind.
// Each kind is one Remastered shader family (or a few that draw alike) that the port's PBR shader
// draws its own way; 0 is the plain lit material. The numbers are stored in imported records and
// in the pipeline key, so they never change.
enum class PbrKind : uint8_t {
  Standard = 0,        // plain lit material (and the wind-swayed foliage)
  UpLayer = 1,         // second layer blended in by the surface's up-facing (9EFE0D2E)
  Detail = 2,          // detail-mapped material
  Lava = 3,            // lava rock: glow through the emissive map
  Parallax = 4,        // parallax-mapped material (2F3FB02B)
  LavaPool = 6,        // lava pool: forced UV0, keeps the retail hot pass
  Waterfall = 7,       // falling water (50412FE7, 963E469B): scrolls with the frame phase
  Glass = 8,           // glass: refracts the screen copy
  GunGlow = 9,         // the beam glow on the arm cannon
  PremulGlass = 10,    // premultiplied-alpha glass
  HoloGlass = 11,      // holo glass: refracts the screen copy, has constants
  FrozenShell = 12,    // the Ice Beam cannon's frost shell (charge and IINT pulse)
  MatcapShell = 13,    // matcap-lit shell
  BoundaryShield = 14, // force fields and boundary shields: refract the screen copy
  Pickup = 15,         // pickups: gradient at the world position
  Holo = 16,           // hologram surface
  HoloRefl = 17,       // reflective hologram surface
  Hologram = 18,       // hologram: texture at the world position
  GunFx = 19,          // gun particle-model fx
  VertexBlend = 20,    // second layer weighed by the vertex colour
  Phazon = 21,         // Phazon (the Phazon3 stone): keeps the retail hot pass
  PhazonB = 22,        // the second Phazon shader
  RefractGlass = 23,   // refracting glass: blurred (mipped) screen copy
  DecalCut = 24,       // alpha-cut decal
  EyeGloss = 25,       // creatures' eye gloss
  ProjectedBlend = 26, // two base layers blended by a plain alpha lerp
  DecalAlpha = 27,     // alpha-blended decal
  Ice = 28,            // ice: its own cube, darkened by the room's occlusion
  HoloGlassB = 29,     // the HoloGlass with per-object phase: blurred screen copy
  RefractGlassB = 30,  // the second refracting glass: blurred screen copy
  XrayGhost = 31,      // X-ray visor ghost
  PhazonPool = 32,     // Phazon pool: keeps the retail hot pass
  XrayMap = 33,        // X-ray visor map surface
  DualGlass = 34,      // two-layer glass: refracts the screen copy
  PhazonVeins = 35,    // the Phazon Beam's veins (DisintegrationAmount): keeps the retail hot pass
  InventoryGlass = 36, // the pause visor's glass: its own cube, own screen copy
  InventoryAdd = 37,   // the pause visor's additive layer
  UnlitCube = 38,      // unlit cube-reflecting surface
  UnlitVertex = 39,    // unlit vertex-coloured fx
  UnlitMatcap = 40,    // unlit matcap
  UnlitAdd = 41,       // unlit METL-weighted additive fx
  BallGlass = 42,      // the spider ball's glass
  SimpleGlass = 43,    // simple glass: per-object phase, blurred screen copy
  EnergyField = 44,    // energy field: Remastered world X/Y rows
  RouteHologram = 45,  // route hologram: DIFC from the draw's colour
  ChozoLore = 46,      // Chozo Lore
  Icy = 47,            // the frozen actor overlay (CCubeModel's RenderIcy path, no record)
  Crystal = 48,        // 54b4021f, Monitor Station's crystals: unlit layers and a blurred screen copy
};

// The kind a float (pbr_layer.y, a record's values[13]) holds, as gx.cpp builds it into the key.
inline PbrKind PbrKindOf(float kind) { return static_cast<PbrKind>(std::clamp(std::lround(kind), 0L, 255L)); }

// A plain number (ShaderConfig::pbrKind, the converter's int kind) compares with a kind directly.
constexpr bool operator==(int kind, PbrKind k) { return kind == static_cast<int>(k); }

constexpr const char* PbrKindName(PbrKind k) {
  switch (k) {
  case PbrKind::Standard: return "standard";
  case PbrKind::UpLayer: return "up-layer";
  case PbrKind::Detail: return "detail";
  case PbrKind::Lava: return "lava";
  case PbrKind::Parallax: return "parallax";
  case PbrKind::LavaPool: return "lava-pool";
  case PbrKind::Waterfall: return "waterfall";
  case PbrKind::Glass: return "glass";
  case PbrKind::GunGlow: return "gun-glow";
  case PbrKind::PremulGlass: return "premul-glass";
  case PbrKind::HoloGlass: return "holo-glass";
  case PbrKind::FrozenShell: return "frozen-shell";
  case PbrKind::MatcapShell: return "matcap-shell";
  case PbrKind::BoundaryShield: return "boundary-shield";
  case PbrKind::Pickup: return "pickup";
  case PbrKind::Holo: return "holo";
  case PbrKind::HoloRefl: return "holo-refl";
  case PbrKind::Hologram: return "hologram";
  case PbrKind::GunFx: return "gun-fx";
  case PbrKind::VertexBlend: return "vertex-blend";
  case PbrKind::Phazon: return "phazon";
  case PbrKind::PhazonB: return "phazon-b";
  case PbrKind::RefractGlass: return "refract-glass";
  case PbrKind::DecalCut: return "decal-cut";
  case PbrKind::EyeGloss: return "eye-gloss";
  case PbrKind::ProjectedBlend: return "projected-blend";
  case PbrKind::DecalAlpha: return "decal-alpha";
  case PbrKind::Ice: return "ice";
  case PbrKind::HoloGlassB: return "holo-glass-b";
  case PbrKind::RefractGlassB: return "refract-glass-b";
  case PbrKind::XrayGhost: return "xray-ghost";
  case PbrKind::PhazonPool: return "phazon-pool";
  case PbrKind::XrayMap: return "xray-map";
  case PbrKind::DualGlass: return "dual-glass";
  case PbrKind::PhazonVeins: return "phazon-veins";
  case PbrKind::InventoryGlass: return "inventory-glass";
  case PbrKind::InventoryAdd: return "inventory-add";
  case PbrKind::UnlitCube: return "unlit-cube";
  case PbrKind::UnlitVertex: return "unlit-vertex";
  case PbrKind::UnlitMatcap: return "unlit-matcap";
  case PbrKind::UnlitAdd: return "unlit-add";
  case PbrKind::BallGlass: return "ball-glass";
  case PbrKind::SimpleGlass: return "simple-glass";
  case PbrKind::EnergyField: return "energy-field";
  case PbrKind::RouteHologram: return "route-hologram";
  case PbrKind::ChozoLore: return "chozo-lore";
  case PbrKind::Crystal: return "crystal";
  default: return "kind?";
  }
}

// Kinds whose constants (GXSetPBRShield rows: DIFC, glow and the kind's own) the draw supplies;
// every other kind clears the last draw's.
constexpr bool IsShieldKind(PbrKind k) {
  switch (k) {
  case PbrKind::HoloGlass:
  case PbrKind::BoundaryShield:
  case PbrKind::Pickup:
  case PbrKind::Holo:
  case PbrKind::HoloRefl:
  case PbrKind::Hologram:
  case PbrKind::GunFx:
  case PbrKind::Phazon:
  case PbrKind::PhazonB:
  case PbrKind::EyeGloss:
  case PbrKind::Ice:
  case PbrKind::HoloGlassB:
  case PbrKind::XrayGhost:
  case PbrKind::PhazonPool:
  case PbrKind::XrayMap:
  case PbrKind::PhazonVeins:
  case PbrKind::UnlitCube:
  case PbrKind::UnlitVertex:
  case PbrKind::UnlitMatcap:
  case PbrKind::UnlitAdd:
  case PbrKind::BallGlass:
  case PbrKind::SimpleGlass:
  case PbrKind::EnergyField:
  case PbrKind::RouteHologram:
  case PbrKind::ChozoLore:
  case PbrKind::Crystal:
    return true;
  default:
    return false;
  }
}

// Kinds that see what is behind them: the screen so far, copied into map 7 before the draw.
constexpr bool IsScreenCopyKind(PbrKind k) {
  switch (k) {
  case PbrKind::Glass:
  case PbrKind::HoloGlass:
  case PbrKind::BoundaryShield:
  case PbrKind::RefractGlass:
  case PbrKind::HoloGlassB:
  case PbrKind::RefractGlassB:
  case PbrKind::DualGlass:
  case PbrKind::InventoryGlass:
  case PbrKind::SimpleGlass:
  case PbrKind::Crystal:
    return true;
  default:
    return false;
  }
}

// The screen-copy kinds that read a blurred copy: the copy gets mips.
constexpr bool IsScreenMipsKind(PbrKind k) {
  return k == PbrKind::RefractGlass || k == PbrKind::HoloGlassB || k == PbrKind::RefractGlassB ||
         k == PbrKind::SimpleGlass || k == PbrKind::Crystal;
}

// Kinds a standalone model sorts as glass (the converter's rmat 3): the screen-copy kinds and the
// premultiplied glass.
constexpr bool IsGlassSortKind(PbrKind k) { return k == PbrKind::PremulGlass || IsScreenCopyKind(k); }

// Kinds that keep the retail hot (thermal) pass in Remastered too.
constexpr bool KeepsRetailHotPass(PbrKind k) {
  return k == PbrKind::LavaPool || k == PbrKind::Phazon || k == PbrKind::PhazonPool || k == PbrKind::PhazonVeins;
}

} // namespace aurora
