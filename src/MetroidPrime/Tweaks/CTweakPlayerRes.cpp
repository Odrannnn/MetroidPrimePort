#include "MetroidPrime/Tweaks/CTweakPlayerRes.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

#include "rstl/string.hpp"

#include <string.h>

static inline CAssetId get_asset_id_from_name(const char* name) {
  CAssetId id = gpResourceFactory->GetResourceIdByName(name)->GetId();
  return id;
}

#ifdef TARGET_PC
// PAL's tweak lists nine map icons instead of 1.00's seven: letters (TXTR_IconA, E, G, M,
// R, S, T) and TXTR_MapArrowUp/Down, so they can't be read by position. Skip the extras
// here (the stick list starts with "LStick") and pick 1.00's icons by name afterwards.
static bool sPalMapIcons = false;

static rstl::reserved_vector< CAssetId, 9 > read_lstick_ids(CInputStream& in) {
  rstl::reserved_vector< CAssetId, 9 > ids;
  rstl::string name(in);
  sPalMapIcons = false;
  while (strncmp(name.c_str(), "TXTR_", 5) == 0) {
    sPalMapIcons = true;
    name = rstl::string(in);
  }
  ids.push_back(get_asset_id_from_name(name.c_str()));
  for (int i = 1; i < 9; ++i) {
    ids.push_back(get_asset_id_from_name(rstl::string(in).c_str()));
  }
  return ids;
}
#endif

template < int N >
inline rstl::reserved_vector< CAssetId, N > read_asset_ids(CInputStream& in) {
  rstl::reserved_vector< CAssetId, N > ids;
  for (int i = 0; i < N; ++i) {
    ids.push_back(get_asset_id_from_name(rstl::string(in).c_str()));
  }
  return ids;
}

CTweakPlayerRes::CTweakPlayerRes(CInputStream& in)
: x4_saveStationIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, x8_missileStationIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, xc_elevatorIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, x10_minesBreakFirstTopIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, x14_minesBreakFirstBottomIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, x18_minesBreakSecondTopIcon(get_asset_id_from_name(rstl::string(in).c_str()))
, x1c_minesBreakSecondBottomIcon(get_asset_id_from_name(rstl::string(in).c_str()))
#ifdef TARGET_PC
, x20_lStick(read_lstick_ids(in))
#else
, x20_lStick(read_asset_ids< 9 >(in))
#endif
, x48_cStick(read_asset_ids< 9 >(in))
, x70_lTrigger(read_asset_ids< 2 >(in))
, x7c_rTrigger(read_asset_ids< 2 >(in))
, x88_startButton(read_asset_ids< 2 >(in))
, x94_aButton(read_asset_ids< 2 >(in))
, xa0_bButton(read_asset_ids< 2 >(in))
, xac_xButton(read_asset_ids< 2 >(in))
, xb8_yButton(read_asset_ids< 2 >(in))
, xc4_ballTransitionsANCS(0)
, xf0_cinematicMoveOutofIntoPlayerDistance(5.f) {
  memset(xc8_ballTransitions, 0, sizeof(xc8_ballTransitions));
  memset(xdc_cineGun, 0, sizeof(xdc_cineGun));

  const rstl::string ballTransitions(in);
  xc4_ballTransitionsANCS = get_asset_id_from_name(ballTransitions.c_str());
  for (int i = 0; i < 5; ++i) {
    const rstl::string name(in);
    xc8_ballTransitions[i] = get_asset_id_from_name(name.c_str());
  }
  for (int i = 0; i < 5; ++i) {
    const rstl::string name(in);
    xdc_cineGun[i] = get_asset_id_from_name(name.c_str());
  }
  xf0_cinematicMoveOutofIntoPlayerDistance = in.ReadFloat();

#ifdef TARGET_PC
  if (sPalMapIcons) {
    // Same images as 1.00's (S, M, E); the mines arrows are 1.00's FirstTop (up) and
    // FirstBottom (down), and the Second pair is unused.
    x4_saveStationIcon = get_asset_id_from_name("TXTR_IconS");
    x8_missileStationIcon = get_asset_id_from_name("TXTR_IconM");
    xc_elevatorIcon = get_asset_id_from_name("TXTR_IconE");
    x10_minesBreakFirstTopIcon = get_asset_id_from_name("TXTR_MapArrowUp");
    x14_minesBreakFirstBottomIcon = get_asset_id_from_name("TXTR_MapArrowDown");
    x18_minesBreakSecondTopIcon = x10_minesBreakFirstTopIcon;
    x1c_minesBreakSecondBottomIcon = x14_minesBreakFirstBottomIcon;
  }
#endif
}

CAssetId CTweakPlayerRes::GetBallTransitionBeamResId(CPlayerState::EBeamId id) const {
  if (id < CPlayerState::kBI_Power || id > CPlayerState::kBI_Phazon) {
    return xc8_ballTransitions[0];
  }
  return xc8_ballTransitions[id];
}

CAssetId CTweakPlayerRes::GetCinematicBeamResId(CPlayerState::EBeamId id) const {
  if (id < CPlayerState::kBI_Power || id > CPlayerState::kBI_Phazon) {
    return xdc_cineGun[0];
  }
  return xdc_cineGun[id];
}
