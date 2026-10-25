# Save-state entity schema (generated)

Written by `tools/gen_entity_schema.py`; do not edit. The table itself is
`platform/generated/port_entity_schema.inc`, its types `platform/include/port_entity_schema.h`.

159 classes (CEntity and every subclass a header defines), 3392 own fields; 2648 of them plain data (`statedump` writes those under `fields` when the object's `kTagFields` block saves them, else under `fields_other`).

| kind | fields |
|---|---|
| bool | 112 |
| int | 516 |
| float | 951 |
| bitfield | 642 |
| math | 271 |
| uid | 156 |
| pod | 199 |
| pointer | 1 |
| smart_ptr | 205 |
| container | 185 |
| state_machine | 1 |
| body_controller | 1 |
| anim_data | 14 |
| path_search | 19 |
| other | 119 |

## `other`: types without a codec yet (by field count)

- 23 `CSfxHandle`
- 20 `CProjectileInfo`
- 10 `CBoneTracking`
- 8 `CCameraShakeData`
- 6 `CCollidableAABox`
- 5 `CCollidableSphere`
- 3 `CPortNestedLights`
- 3 `CBurstFire`
- 2 `CCameraSpline`
- 2 `CActorParameters`
- 2 `CPoisonInfo`
- 2 `rstl::single_ptr<CElementGen>[4]`
- 2 `TReservedAverage<CTransform4f, 4>`
- 2 `CFrustumPlanes`
- 1 `CElitePirateData`
- 1 `SPositionHistory`
- 1 `CFlaahgraData`
- 1 `CFlameWarp`
- 1 `CFlyingPirate::CFlyingPirateData`
- 1 `CProjectileWeapon`
- 1 `CMarkerGrid`
- 1 `CMetroidData`
- 1 `CMetroidPrimeData`
- 1 `rstl::single_ptr<CElementGen>[5]`
- 1 `CSkinnedModel`
- 1 `CKnockBackMgr`
- 1 `PortTickPair<CPlasmaProjectile::SPortBeam>`
- 1 `CPlayerEnergyDrain`
- 1 `TReservedAverage<CVector3f, 10>`
- 1 `TReservedAverage<float, 20>`
- 1 `TReservedAverage<float, 4>`
- 1 `SPortDeferredCarry[4]`
- 1 `CRidleyData`
- 1 `CCameraOverrideInfo`
- 1 `CFluidPlaneDoor`
- 1 `TReservedAverage<float, 8>`
- 1 `std::vector<SPortExternalVar>`
- 1 `TAverage<float>`
- 1 `PortQueenBarrier::State`
- 1 `CVisorFlare`
- 1 `CSpacePirate::CSpacePirateData`
- 1 `CThardusSomething[4]`
- 1 `PortTickPair<CWaveBuster::SPortSpiral>`

## `pod`: trivially copyable, pointer-free structs and arrays (raw bytes)

`CDamageInfo` 42, `CColor` 25, `CHealthInfo` 19, `CDamageVulnerability` 19, `CSegId` 16, `CSpindleCameraInterpolant` 15, `CCameraSpring` 6, `CSteeringBehaviors` 4, `TAreaId` 3, `CMaterialList` 3, `CChozoGhost::CBehaveChance` 3, `CAnimRes` 3, `CRandom16` 3, `TEditorId` 2, `CMaterialFilter` 2, `int[4]` 2, `CGrappleParameters` 2, `CModelFlags` 1, `CBabygothData` 1, `uchar[7]` 1, `CBouncyGrenadeData` 1, `CLight` 1, `CEPGrenadeLauncherData` 1, `CIceSheegothData` 1, `CFlameInfo` 1, `CPatternedInfo` 1, `CMetroidBetaData` 1, `CMetroidPrimeIceAttack` 1, `float[4]` 1, `CMotionState` 1, `CPlayer::CVisorSteam` 1, `PortRemasteredGround::RetainedResult` 1, `PortRemasteredGroundRider::RiderController` 1, `CBeamInfo` 1, `CScriptGunTurretData` 1, `CHUDMemoParms` 1, `EffectReverbHiInfo` 1, `EffectChorusInfo` 1, `EffectReverbStdInfo` 1, `EffectDelayInfo` 1, `CAnimationParameters` 1, `CShockWaveInfo` 1, `uchar[4]` 1, `CIkChain` 1, `CTeamAiMgr::CUnknown` 1, `CCollisionSurface` 1, `u16[2]` 1

## `smart_ptr` / `container` templates

`rstl::single_ptr` 106, `rstl::reserved_vector` 79, `rstl::vector` 59, `TLockedToken` 48, `rstl::optional_object` 38, `rstl::auto_ptr` 22, `TToken` 21, `TCachedToken` 6, `rstl::basic_string` 5, `rstl::list` 4, `rstl::ncrc_ptr` 2

## Not in the table

- `CFlaahgraProjectile`: defined in `src/MetroidPrime/Enemies/CFlaahgra.cpp` only
