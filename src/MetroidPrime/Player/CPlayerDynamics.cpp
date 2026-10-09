#include "MetroidPrime/Player/CPlayer.hpp"
#include "port_debug.h"
#include "port_mouse.h"

#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Cameras/CBallCamera.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CRumbleManager.hpp"
#include "MetroidPrime/CFluidPlaneCPU.hpp"
#include "MetroidPrime/SFX/MiscSamus.h"
#include "MetroidPrime/ScriptObjects/CScriptWater.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlayerHint.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Tweaks/CTweakBall.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"

#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "Kyoto/Math/CAbsAngle.hpp"
#include <float.h>

#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "Kyoto/Math/CPlane.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Input/CFinalInput.hpp"

#include "rstl/algorithm.hpp"

static const float skTransitionFilterTime = .95f;

#ifdef TARGET_PC
// Fast Morph transitions (CPlayer::IsFastMorphTransition) keep the player's
// velocity (capped at walking speed when unmorphing on the ground) instead of
// stopping. sFastMorphAirCarry is set by an unmorph in the air: the jump's
// horizontal speed is not clamped to walking speed until the player lands or
// morphs again.
static bool sFastMorphAirCarry = false;
// Remastered movement under direct aim (PortDebug::RemasteredMovement), set by
// ComputeMovement: one force along the stick, and GetDampedClampedVelocityWR
// slows the planar speed as a whole, with Remastered's air drag in every air
// state. Retail skips the friction while a jump rises, so the speed dips at
// takeoff and recovers at the apex (issue #46); Remastered and PrimeHack don't.
static bool sPlanarMovement = false;
// Set while airborne once an orbit was held (a scan dash or strafe jump): the
// rest of that airtime moves as retail, whose rising jump keeps the dash's
// speed (Remastered's drag took a dash from 20 to 13 u/s), so dash tricks
// such as Space Jump early still carry. Cleared on landing.
static bool sOrbitAirCarry = false;
#endif

static const CMaterialList BallTransitionInclude = CMaterialList(kMT_Solid);
static const CMaterialList BallTransitionExclude =
    CMaterialList(kMT_ProjectilePassthrough, kMT_Player, kMT_Character, kMT_CameraPassthrough);
static const CMaterialFilter BallTransitionCollide =
    CMaterialFilter::MakeIncludeExclude(BallTransitionInclude, BallTransitionExclude);

static const float skStrafeDistances[] = {11.8f, 11.8f, 11.8f, 5.f, 6.f, 5.f, 5.f, 6.f};
static const float skDashStrafeDistances[] = {11.8f, 30.f, 22.6f, 10.f, 10.f, 10.f, 10.f, 10.f};
static const float skOrbitForwardDistances[] = {11.8f, 11.8f, 11.8f, 5.f, 6.f, 5.f, 5.f, 6.f};

#ifdef TARGET_PC
// Keeps the velocity's direction but holds it level and at most walking speed.
void CPlayer::ClampFastMorphGroundVelocity(float dt) {
  CVector3f velocity = GetVelocityWR();
  velocity.SetZ(0.f);
  const float maxSpeed = GetActualFirstPersonMaxVelocity(dt);
  if (velocity.Magnitude() > maxSpeed) {
    velocity = velocity * (maxSpeed / velocity.Magnitude());
  }
  SetVelocityWR(velocity);
}
#endif

CVector3f CPlayer::GetDampedClampedVelocityWR() const {
#ifdef TARGET_PC
  if (sFastMorphAirCarry) {
    if (x258_movementState != NPlayer::kMS_OnGround) {
      return GetVelocityWR();
    }
    sFastMorphAirCarry = false;
  }
  if (sPlanarMovement && x304_orbitState == kOS_NoOrbit) {
    CVector3f planarVelocity = GetTransform().TransposeRotate(GetVelocityWR());
    const float speed = std::hypot(planarVelocity.GetX(), planarVelocity.GetY());
    // Like an orbit, a grapple jump's flight is only clamped.
    const bool grappleJump =
        x258_movementState != NPlayer::kMS_OnGround && x3d8_grappleJumpTimeout > 0.f;
    if (!grappleJump && speed > 1e-14f) {
      const float tickScale = PortDebug::TickPeriod() * 60.f;
      float newSpeed;
      if (GetSurfaceRestraint() == kSR_Air) {
        // Remastered's air drag, 3.5|v|/mass a tick, compounded over the step.
        newSpeed = speed * CMath::PowF(CMath::Max(0.f, 1.f - 3.5f / GetMass()), tickScale);
      } else {
        newSpeed = CMath::Max(
            0.f, speed - gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint()) *
                             tickScale);
      }
      planarVelocity.SetX(planarVelocity.GetX() * (newSpeed / speed));
      planarVelocity.SetY(planarVelocity.GetY() * (newSpeed / speed));
    }
    // Remastered clamps the planar speed; only forward is, as retail, so a scan
    // dash's sideways momentum still carries once the lock breaks (issue #30).
    planarVelocity.SetY(CMath::Limit(
        planarVelocity.GetY(), gpTweakPlayer->GetPlayerTranslationMaxSpeed(GetSurfaceRestraint())));
    if (x258_movementState == NPlayer::kMS_OnGround) {
      planarVelocity.SetZ(0.f);
    }
    return GetTransform().Rotate(planarVelocity);
  }
#endif
  CVector3f localVelocity = GetTransform().TransposeRotate(GetVelocityWR());
  if ((x258_movementState != NPlayer::kMS_ApplyJump ||
       (x258_movementState == NPlayer::kMS_ApplyJump && GetSurfaceRestraint() != kSR_Air)) &&
      x304_orbitState == kOS_NoOrbit) {
    // The tweak friction is authored per 60 Hz tick. Scale it by the actual
    // simulation step or higher rates decelerate proportionally faster (which
    // is why walking, but not the friction-free jump, slowed down).
    const float friction = gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint()) *
                           (PortDebug::TickPeriod() * 60.f);
    if (localVelocity.GetY() > 0.f) {
      localVelocity.SetY(CMath::Max(0.f, localVelocity.GetY() - friction));
    } else {
      localVelocity.SetY(CMath::Min(0.f, localVelocity.GetY() + friction));
    }
    if (localVelocity.GetX() > 0.f) {
      localVelocity.SetX(CMath::Max(0.f, localVelocity.GetX() - friction));
    } else {
      localVelocity.SetX(CMath::Min(0.f, localVelocity.GetX() + friction));
    }
  }
  // Only forward speed is capped, as retail, under mouse aim too: sideways
  // momentum past walking speed (a scan dash whose lock broke mid-air) must
  // carry (issue #30). Free strafing stays within the cap through its force law.
  const float maxSpeed = gpTweakPlayer->GetPlayerTranslationMaxSpeed(GetSurfaceRestraint());
  localVelocity.SetY(CMath::Limit(localVelocity.GetY(), maxSpeed));
  if (x258_movementState == NPlayer::kMS_OnGround) {
    localVelocity.SetZ(0.f);
  }
  return GetTransform().Rotate(localVelocity);
}

float CPlayer::GetAverageSpeed() const {
  if (x4a4_moveSpeedAvg.GetAverage()) {
    return *x4a4_moveSpeedAvg.GetAverage();
  }
  return x4f8_moveSpeed;
}

float CPlayer::GetAcceleration() const {
  if (x2d0_curAcceleration >= x2b4_accelerationTable.size()) {
    return x2b4_accelerationTable.back();
  }
  return x2b4_accelerationTable[x2d0_curAcceleration];
}

float CPlayer::GetGravity() const {
  if (!gpGameState->GetPlayerState()->HasPowerUp(CPlayerState::kIT_GravitySuit) &&
      CheckSubmerged()) {
    return gpTweakPlayer->GetFluidGravAccel();
  }
  if (x37c_sidewaysDashing) {
    return -100.f;
  }
  return gpTweakPlayer->GetNormalGravAccel();
}

float CPlayer::GetWeight() const { return GetMass() * -GetGravity(); }

void CPlayer::UpdateBombJumpStuff() {
  if (x9d0_bombJumpCount == 0) {
    return;
  }
  if (--x9d4_bombJumpCheckDelayFrames > 0) {
    return;
  }
  CVector3f flatVelocity = GetVelocityWR();
  flatVelocity.SetZ(0.f);
  if (x258_movementState == NPlayer::kMS_OnGround ||
      (flatVelocity.CanBeNormalized() && flatVelocity.Magnitude() > 6.f)) {
    x9d0_bombJumpCount = 0;
  }
}

void CPlayer::UpdateStepCameraZBias(float dt) {
  float newBias = GetTranslation()[kDZ] + GetUnbiasedEyeHeight();
  if (x258_movementState == NPlayer::kMS_OnGround && !IsMorphBallTransitioning()) {
    const float oldBias = newBias;
    if (!x9c5_31_stepCameraZBiasDirty) {
      const float delta = newBias - x9cc_stepCameraZBias;
      const float verticalStep = dt * GetVelocityWR().GetZ();
      float newDelta = 5.f * dt;
      if (delta > 0.f) {
        if (delta > verticalStep && delta > newDelta) {
          if (delta > GetStepUpHeight()) {
            newDelta += delta - GetStepUpHeight();
          }
          newBias = x9cc_stepCameraZBias + newDelta;
        }
      } else if (delta < verticalStep && delta < -newDelta) {
        if (delta < -GetStepDownHeight()) {
          newDelta += -delta - GetStepDownHeight();
        }
        newBias = x9cc_stepCameraZBias - newDelta;
      }
    }
    SetEyeZBias(newBias - oldBias);
  } else {
    SetEyeZBias(0.f);
  }
  x9cc_stepCameraZBias = newBias;
  x9c5_31_stepCameraZBiasDirty = false;
}

bool CPlayer::SidewaysDashAllowed(float strafeInput, float forwardInput, const CFinalInput& input,
                                  CStateManager& mgr) const {
  if (x9c5_28_slidingOnWall || x9c5_29_hitWall || x304_orbitState != kOS_OrbitObject) {
    return false;
  }
  if (gpTweakPlayer->GetDashOnButtonRelease()) {
    if (x304_orbitState != kOS_NoOrbit && gpTweakPlayer->GetDashEnabled() &&
        x288_startingJumpTimeout > 0.f &&
        !ControlMapper::GetDigitalInput(ControlMapper::kC_JumpOrBoost, input) &&
        x388_dashButtonHoldTime < gpTweakPlayer->GetDashButtonHoldCancelTime() &&
        CMath::AbsF(strafeInput) >= CMath::AbsF(forwardInput) &&
        CMath::AbsF(strafeInput) > gpTweakPlayer->GetDashStrafeInputThreshold()) {
      return true;
    }
  } else if (x304_orbitState != kOS_NoOrbit && gpTweakPlayer->GetDashEnabled() &&
             ControlMapper::GetPressInput(ControlMapper::kC_JumpOrBoost, input) &&
             x288_startingJumpTimeout > 0.f &&
             CMath::AbsF(strafeInput) >= CMath::AbsF(forwardInput) &&
             CMath::AbsF(strafeInput) > 0.01f) {
    const CVector3f stickEdge = CalculateLeftStickEdgePosition(strafeInput, forwardInput);
    const float inputMagnitude =
        CMath::SqrtF(strafeInput * strafeInput + forwardInput * forwardInput);
    const float threshold = inputMagnitude / stickEdge.Magnitude();
    if (threshold >= gpTweakPlayer->GetDashStrafeInputThreshold()) {
      return true;
    }
  }
  return false;
}

void CPlayer::FinishSidewaysDash() {
  if (x37c_sidewaysDashing) {
    x38c_doneSidewaysDashing = true;
  }
  x37c_sidewaysDashing = false;
  x380_strafeInputAtDash = 0.f;
  x384_dashTimer = 0.f;
}

void CPlayer::ComputeDash(const CFinalInput& input, float dt, CStateManager& mgr) {
  const float strafeInput = StrafeInput(input);
  const float forwardInput = ForwardInput(input, TurnInput(input));
  CVector3f orbitPoint = x314_orbitPoint;
  orbitPoint.SetZ(GetTranslation().GetZ());
  const CVector3f orbitToPlayer = GetTranslation() - orbitPoint;
  if (!orbitToPlayer.CanBeNormalized()) {
    return;
  }
  CVector3f useOrbitToPlayer = orbitToPlayer;
  float strafeVelocity = dt * skStrafeDistances[GetSurfaceRestraint()];
  if (ControlMapper::GetDigitalInput(ControlMapper::kC_JumpOrBoost, input)) {
    x388_dashButtonHoldTime += dt;
  }
  if (!x37c_sidewaysDashing) {
    if (SidewaysDashAllowed(strafeInput, forwardInput, input, mgr)) {
      x37c_sidewaysDashing = true;
      x380_strafeInputAtDash = strafeInput;
      x38c_doneSidewaysDashing = true;
      x384_dashTimer = 0.f;
      CVector3f velocity = GetVelocityWR();
      if (velocity.GetZ() > 0.f) {
        velocity[kDZ] *= 0.1f;
        if (!GetPlayerIsSlidingOnWall()) {
          SetVelocityWR(velocity);
          x778_dashSfx = CSfxManager::SfxStart(SFXsam_b_jump_03, 127, 64, true);
          DoSfxEffects(x778_dashSfx);
          mgr.GetRumbleManager()->Rumble(mgr, kRFX_PlayerBump, 0.24375f, kRP_One);
        }
      }
    }
    strafeVelocity *= strafeInput;
  } else {
    x384_dashTimer += dt;
    if (x258_movementState == NPlayer::kMS_OnGround || x384_dashTimer >= x3a0_dashDuration ||
        GetPlayerIsSlidingOnWall() || x9c5_29_hitWall || x304_orbitState != kOS_OrbitObject) {
      FinishSidewaysDash();
      strafeVelocity *= strafeInput;
      CSfxManager::RemoveEmitter(x778_dashSfx);
    } else {
      const int outOfWaterTicks = x2b0_outOfWaterTicks;
      if (x39c_noStrafeDashBlend) {
        const ESurfaceRestraints restraint =
            outOfWaterTicks == 2 ? GetCurrentSurfaceRestraint() : kSR_Water;
        strafeVelocity = dt * (x398_dashSpeedMultiplier * skDashStrafeDistances[restraint]);
      } else {
        float blend = CMath::Limit(x384_dashTimer / x3a4_strafeDashBlendDuration, 1.f);
        blend = 1.f - blend;
        const float dashDifference =
            skDashStrafeDistances[GetSurfaceRestraint()] - skStrafeDistances[GetSurfaceRestraint()];
        strafeVelocity = dt * (x398_dashSpeedMultiplier *
                               (dashDifference * blend + skStrafeDistances[GetSurfaceRestraint()]));
      }
      if (x380_strafeInputAtDash < 0.f) {
        strafeVelocity = -strafeVelocity;
      }
    }
  }

  const float angle = strafeVelocity / orbitToPlayer.Magnitude();
  float maxAngle = M_PIF * 2.f / 3.f;
  if (x37c_sidewaysDashing) {
    maxAngle = M_PIF;
  }
  const float limitedAngle = CMath::Limit(angle, maxAngle * dt);
  const CQuaternion rotation = CQuaternion::AxisAngle(
      CUnitVector3f(0.f, 0.f, 1.f, CUnitVector3f::kN_Yes), CRelAngle(limitedAngle));
  useOrbitToPlayer = rotation.Transform(orbitToPlayer);
  orbitPoint += useOrbitToPlayer;
  if (!ControlMapper::GetDigitalInput(ControlMapper::kC_JumpOrBoost, input)) {
    x388_dashButtonHoldTime = 0.f;
  }

  strafeVelocity = dt * (forwardInput * skOrbitForwardDistances[GetSurfaceRestraint()]);
  orbitPoint += strafeVelocity * -useOrbitToPlayer.AsNormalized();
  const CVector2f flatVelocity(GetVelocityWR().GetX(), GetVelocityWR().GetY());
  const float flatVelocityY = flatVelocity.GetY();
  CVector3f newVelocity = (orbitPoint - GetTranslation()) / dt;
  newVelocity.SetZ(GetVelocityWR().GetZ());
  CVector3f velocityDelta = newVelocity - CVector3f(flatVelocity.GetX(), flatVelocityY, 0.f);
  velocityDelta.SetZ(0.f);
  const float deltaMagnitude = velocityDelta.Magnitude();
  if (deltaMagnitude > FLT_EPSILON) {
    const float acceleration = dt * GetAcceleration();
    const float accelerationBlend = CMath::Limit(deltaMagnitude / acceleration, 1.f);
    newVelocity =
        GetVelocityWR() + accelerationBlend * (acceleration * (velocityDelta / deltaMagnitude));
    if (!GetPlayerIsSlidingOnWall()) {
      SetVelocityWR(newVelocity);
    }
  }
}

void CPlayer::ComputeMovement(const CFinalInput& input, CStateManager& mgr, float dt) {
  const float jumpInput = JumpInput(input, mgr);
  const bool mouseMovement = MouseControlsAllowed(mgr) && x304_orbitState == kOS_NoOrbit;
#ifdef TARGET_PC
  if (x258_movementState == NPlayer::kMS_OnGround) {
    sOrbitAirCarry = false;
  } else if (x304_orbitState != kOS_NoOrbit) {
    sOrbitAirCarry = true;
  }
  sPlanarMovement =
      MouseControlsAllowed(mgr) && PortDebug::RemasteredMovement() && !sOrbitAirCarry;
#endif
  float turnInput = mouseMovement ? 0.f : TurnInput(input);
  float forwardInput = ForwardInput(input, turnInput);
  float strafeInput = 0.f;
  if (mouseMovement) {
    strafeInput = ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeRight, input) -
                  ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeLeft, input);
    const auto inputAxes = PortMouse::ClampPlanar({strafeInput, forwardInput}, 1.f);
    strafeInput = inputAxes.right;
    forwardInput = inputAxes.forward;
    SetAngularVelocityOR(CAxisAngle::Identity());
  }
#ifdef TARGET_PC
  // A fast morph carries its velocity through without friction (gravity still
  // applies through JumpInput). A fast unmorph is instant.
  if (!IsMorphBallTransitioning() || !IsFastMorphTransition())
#endif
  SetVelocityWR(GetDampedClampedVelocityWR());
  float turnSpeedMultiplier = gpTweakPlayer->GetTurnSpeedMultiplier();
  if (gpTweakPlayer->GetFreeLookTurnsPlayer()) {
    if (!gpTweakPlayer->GetHoldButtonsForFreeLook() ||
        (gpTweakPlayer->GetHoldButtonsForFreeLook() && x3dd_lookButtonHeld)) {
      turnSpeedMultiplier = gpTweakPlayer->GetFreeLookTurnSpeedMultiplier();
    }
  }
  if (x304_orbitState == kOS_NoOrbit ||
      (x3dd_lookButtonHeld && x304_orbitState != kOS_OrbitObject &&
       x304_orbitState != kOS_Grapple)) {
    if (close_enough(turnInput, 0.f)) {
      const float friction = gpTweakPlayer->GetPlayerRotationFriction(GetSurfaceRestraint());
      // Per-tick decay factor; raise it to the step (in 60 Hz ticks) to match.
      const float damped =
          CMath::PowF(friction, PortDebug::TickPeriod() * 60.f);
      SetAngularVelocityOR(
          CAxisAngle(CVector3f(0.f, 0.f, damped * GetAngularVelocityOR().GetVector().GetZ())));
    }
    if (GetAngularVelocityOR().GetVector().GetZ() >
        turnSpeedMultiplier * gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint())) {
      SetAngularVelocityOR(CAxisAngle(CVector3f(
          0.f, 0.f,
          turnSpeedMultiplier * gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint()))));
    } else if (-GetAngularVelocityOR().GetVector().GetZ() >
               turnSpeedMultiplier *
                   gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint())) {
      SetAngularVelocityOR(CAxisAngle(CVector3f(
          0.f, 0.f,
          turnSpeedMultiplier * -gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint()))));
    }
  }
  float angularVelocityDelta =
      turnSpeedMultiplier *
      (turnInput * gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint()));
  angularVelocityDelta -= GetAngularVelocityOR().GetVector().GetZ();
  const float turnFraction = CMath::Clamp(
      0.f,
      CMath::AbsF(angularVelocityDelta) /
          (turnSpeedMultiplier * gpTweakPlayer->GetPlayerRotationMaxSpeed(GetSurfaceRestraint())),
      1.f);
  if (angularVelocityDelta < 0.f) {
    turnInput = turnFraction * -gpTweakPlayer->GetMaxRotationalAcceleration(GetSurfaceRestraint());
  } else {
    turnInput = turnFraction * gpTweakPlayer->GetMaxRotationalAcceleration(GetSurfaceRestraint());
  }
#ifdef TARGET_PC
  // Off 60 Hz the force law is corrected to the 60 Hz one (PortMouse::ForceLawAtRate),
  // where GetDampedClampedVelocityWR subtracts the friction (and so shifts the top speed).
  const float tickScale = PortDebug::TickPeriod() * 60.f;
  const bool frictionApplied =
      (x258_movementState != NPlayer::kMS_ApplyJump || GetSurfaceRestraint() != kSR_Air) &&
      x304_orbitState == kOS_NoOrbit;
  // Direct aim moves along the stick as retail moves forward. The friction is
  // subtracted per body axis, so each axis's force law offsets it in full; while
  // none is (a jump's rise), the offset goes along the stick's unit direction,
  // or a diagonal would head for more than retail's top speed, and faster.
  float strafeDirection = 0.f;
  float forwardDirection = forwardInput > 0.f ? 1.f : -1.f;
  if (mouseMovement && !frictionApplied) {
    const float length = std::hypot(strafeInput, forwardInput);
    if (length > 0.f) {
      strafeDirection = strafeInput / length;
      forwardDirection = forwardInput / length;
    }
  }
#endif
  float forwardForce;
  if (!close_enough(0.f, forwardInput)) {
    const float maxSpeed = gpTweakPlayer->GetPlayerTranslationMaxSpeed(GetSurfaceRestraint());
    const float friction = gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint());
    const float mass = GetMass();
    const float acceleration =
        gpTweakPlayer->GetMaxTranslationalAcceleration(GetSurfaceRestraint());
    float frictionSpeed = friction * mass / (dt * acceleration);
    frictionSpeed *= maxSpeed;
#ifdef TARGET_PC
    PortMouse::ForceLaw law{frictionSpeed, 0.f, 1.f};
    if (tickScale != 1.f) {
      law = PortMouse::ForceLawAtRate(tickScale, friction, mass, maxSpeed, dt, acceleration,
                                      frictionApplied);
      frictionSpeed = law.frictionSpeed;
    }
#endif
    float desiredSpeed = forwardInput * (maxSpeed - frictionSpeed);
#ifdef TARGET_PC
    desiredSpeed += (frictionSpeed + law.shift) * forwardDirection;
#else
    desiredSpeed += frictionSpeed * (forwardInput > 0.f ? 1.f : -1.f);
#endif
    const float forwardFraction = CMath::Clamp(
        -1.f, (desiredSpeed - GetTransform().TransposeRotate(GetVelocityWR()).GetY()) / maxSpeed,
        1.f);
    forwardForce =
        forwardFraction * gpTweakPlayer->GetMaxTranslationalAcceleration(GetSurfaceRestraint());
#ifdef TARGET_PC
    forwardForce *= law.gain;
#endif
  } else {
    forwardForce = 0.f;
  }
  float strafeForce = 0.f;
  if (mouseMovement) {
    const float acceleration = gpTweakPlayer->GetMaxTranslationalAcceleration(GetSurfaceRestraint());
    const float maxSpeed = gpTweakPlayer->GetPlayerTranslationMaxSpeed(GetSurfaceRestraint());
    const float sideVelocity = GetTransform().TransposeRotate(GetVelocityWR()).GetX();
    strafeForce = PortMouse::AxisForce(strafeInput, sideVelocity, maxSpeed,
                                      gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint()),
                                      GetMass(), dt, acceleration, tickScale, frictionApplied,
                                      strafeDirection);
    // Retail has no sideways force, so strafing the way a dash carries the
    // player must not brake its momentum back to walking speed.
    if (CMath::AbsF(sideVelocity) > maxSpeed && sideVelocity * strafeInput > 0.f) {
      strafeForce = 0.f;
    }
    const auto force = PortMouse::ClampPlanar({strafeForce, forwardForce}, acceleration);
    strafeForce = force.right;
    forwardForce = force.forward;
#ifdef TARGET_PC
    if (sPlanarMovement) {
      // Remastered: the retail force law once, along the stick, with the stick's
      // magnitude as the input and the velocity along it as the speed. The
      // magnitude is radial, scaled as retail scales forward alone (full at 0.8),
      // so a diagonal is as fast as straight ahead; retail leaves strafe unscaled.
      strafeForce = forwardForce = 0.f;
      const float stickStrafe =
          ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeRight, input) -
          ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeLeft, input);
      const float stickForward =
          forwardInput == 0.f
              ? 0.f
              : ControlMapper::GetAnalogInput(ControlMapper::kC_Forward, input) -
                    ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
      const float stickMagnitude = std::hypot(stickStrafe, stickForward);
      if (stickMagnitude > 1e-5f) {
        const float directionX = stickStrafe / stickMagnitude;
        const float directionY = stickForward / stickMagnitude;
        const float magnitude = CMath::Min(stickMagnitude / 0.8f, 1.f);
        const CVector3f localVelocity = GetTransform().TransposeRotate(GetVelocityWR());
        const float along =
            directionX * localVelocity.GetX() + directionY * localVelocity.GetY();
        float planarForce = PortMouse::AxisForce(
            magnitude, along, maxSpeed,
            gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint()), GetMass(), dt,
            acceleration, tickScale);
        // As above: momentum past the cap the way the stick points isn't braked.
        if (along > maxSpeed && planarForce < 0.f) {
          planarForce = 0.f;
        }
        strafeForce = planarForce * directionX;
        forwardForce = planarForce * directionY;
      }
    }
#endif
  }
  if (x304_orbitState != kOS_NoOrbit && gkFreeLookPreventsOrbitMovement && x3dd_lookButtonHeld) {
    forwardForce = 0.f;
  }
  if (x304_orbitState == kOS_NoOrbit || x3dd_lookButtonHeld) {
    const CVector3f force(strafeForce, forwardForce, jumpInput);
    ApplyForceOR(force, CAxisAngle::Identity());
    if (turnInput != 0.f) {
      ApplyForceOR(CVector3f::Zero(),
                   CAxisAngle(CUnitVector3f(0.f, 0.f, 1.f, CUnitVector3f::kN_Yes), turnInput));
    }
    if (x37c_sidewaysDashing) {
      x38c_doneSidewaysDashing = true;
    }
    x37c_sidewaysDashing = false;
    x380_strafeInputAtDash = 0.f;
    x384_dashTimer = 0.f;
  } else {
    switch (x304_orbitState) {
    case kOS_OrbitObject:
    case kOS_OrbitPoint:
    case kOS_OrbitCarcass:
    case kOS_ForcedOrbitObject: {
      bool canDash = true;
      if (CheckPostGrapple()) {
        canDash = false;
      }
      if (canDash) {
        ComputeDash(input, dt, mgr);
      }
    } break;
    case kOS_Grapple:
      break;
    default:
      break;
    }
    const CVector3f force(0.f, 0.f, jumpInput);
    ApplyForceOR(force, CAxisAngle::Identity());
  }
  if (x3dc_inFreeLook || x3dd_lookButtonHeld) {
    if (!GetPlayerIsSlidingOnWall() && x258_movementState == NPlayer::kMS_OnGround) {
      const CVector3f reverseVelocity =
          CVector3f::Zero() - CVector3f(GetVelocityWR().GetX(), GetVelocityWR().GetY(), 0.f);
      const float magnitude = reverseVelocity.Magnitude();
      if (magnitude > FLT_EPSILON) {
        const float acceleration = 0.2f * (dt * GetAcceleration());
        const float damping = CMath::Limit(magnitude / acceleration, 1.f);
        const CVector3f newVelocity =
            GetVelocityWR() + damping * (acceleration * (reverseVelocity / magnitude));
        SetVelocityWR(newVelocity);
      }
    }
  }
  x9c5_29_hitWall = false;
  if (x2d4_accelerationChangeTimer > 0.f) {
    x2d0_curAcceleration = 0;
  } else {
    ++x2d0_curAcceleration;
  }
  x2d4_accelerationChangeTimer -= dt;
  x2d4_accelerationChangeTimer = rstl::max_val(0.f, x2d4_accelerationChangeTimer);
}

float CPlayer::ForwardInput(const CFinalInput& input, float turnInput) const {
  float forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Forward, input);
  float backward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
  if (x2f8_morphBallState != kMS_Unmorphed || CheckPostGrapple()) {
    backward = 0.f;
  }
  if (x2f8_morphBallState == kMS_Morphing && x584_ballTransitionAnim == 2) {
    forward = 0.f;
  }
  if (x2f8_morphBallState == kMS_Unmorphing && x584_ballTransitionAnim == 5) {
    forward = 0.f;
  }
  if (!(forward < 0.001f)) {
    forward = CMath::Limit(forward / 0.8f, 1.f);
    if (CMath::AbsF(atan2f(CMath::AbsF(turnInput), forward)) <
        CRelAngle::FromDegrees(50.f).AsRadians()) {
      const CVector3f stick(CMath::AbsF(turnInput), forward, 0.f);
      if (stick.CanBeNormalized()) {
        forward = stick.Magnitude();
      }
    }
  }
  if (!(backward < 0.001f)) {
    backward = CMath::Limit(backward / 0.8f, 1.f);
    if (CMath::AbsF(atan2f(CMath::AbsF(turnInput), backward)) <
        CRelAngle::FromDegrees(50.f).AsRadians()) {
      const CVector3f stick(CMath::AbsF(turnInput), backward, 0.f);
      if (stick.CanBeNormalized()) {
        backward = stick.Magnitude();
      }
    }
  }
  if (!gpTweakPlayer->GetMoveDuringFreeLook()) {
    CVector3f flatVelocity = GetVelocityWR();
    flatVelocity.SetZ(0.f);
    if (x3dc_inFreeLook || x3dd_lookButtonHeld) {
      if (x258_movementState == NPlayer::kMS_OnGround ||
          close_enough(flatVelocity.Magnitude(), 0.f)) {
        return 0.f;
      }
    }
  }
  return CMath::Limit(forward - backward * gpTweakPlayer->GetBackwardsForceMultiplier(), 1.f);
}

float CPlayer::StrafeInput(const CFinalInput& input) const {
  if (IsMorphBallTransitioning() || x304_orbitState == kOS_NoOrbit) {
    return 0.f;
  }
  return ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeRight, input) -
         ControlMapper::GetAnalogInput(ControlMapper::kC_StrafeLeft, input);
}

float CPlayer::TurnInput(const CFinalInput& input) const {
  if (x304_orbitState == kOS_OrbitObject || x304_orbitState == kOS_Grapple) {
    return 0.f;
  }
  if (IsMorphBallTransitioning()) {
    return 0.f;
  }
  float left = ControlMapper::GetAnalogInput(ControlMapper::kC_TurnLeft, input);
  float right = ControlMapper::GetAnalogInput(ControlMapper::kC_TurnRight, input);
  if (gpTweakPlayer->GetFreeLookTurnsPlayer()) {
    if (!gpTweakPlayer->GetHoldButtonsForFreeLook() ||
        (gpTweakPlayer->GetHoldButtonsForFreeLook() && x3dd_lookButtonHeld)) {
      if (left < 0.01f && right < 0.01f) {
        left = ControlMapper::GetAnalogInput(ControlMapper::kC_LookLeft, input);
        right = ControlMapper::GetAnalogInput(ControlMapper::kC_LookRight, input);
      }
    }
  } else if (!gpTweakPlayer->GetHoldButtonsForFreeLook() ||
             (gpTweakPlayer->GetHoldButtonsForFreeLook() && x3dd_lookButtonHeld)) {
    const float lookLeft = ControlMapper::GetAnalogInput(ControlMapper::kC_LookLeft, input);
    const float lookRight = ControlMapper::GetAnalogInput(ControlMapper::kC_LookRight, input);
    if (lookLeft > 0.01f || lookRight > 0.01f) {
      return 0.f;
    }
  }
  float turn = left - right;
  if (x32c_orbitModeTimer > 0.f) {
    turn *= 1.f -
            0.5f * CMath::Clamp(0.f, x32c_orbitModeTimer / gpTweakPlayer->GetOrbitModeTimer(), 1.f);
  }
  return CMath::Limit(turn, 1.f);
}

float CPlayer::JumpInput(const CFinalInput& input, CStateManager& mgr) {
  if (IsMorphBallTransitioning()) {
    return GetGravity() * GetMass();
  }
  float jumpFactor = 1.f;
  if (!mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_GravitySuit)) {
    switch (GetSurfaceRestraint()) {
    case kSR_Water:
      jumpFactor = gpTweakPlayer->GetWaterJumpFactor();
      break;
    case kSR_Lava:
      jumpFactor = gpTweakPlayer->GetLavaJumpFactor();
      break;
    case kSR_Phazon:
      jumpFactor = gpTweakPlayer->GetPhazonJumpFactor();
      break;
    default:
      break;
    }
  }
  const float verticalJumpAccel = gpTweakPlayer->GetVerticalJumpAccel();
  const float horizontalJumpAccel = gpTweakPlayer->GetHorizontalJumpAccel();
  float doubleJumpImpulse = gpTweakPlayer->GetDoubleJumpImpulse();
  float verticalDoubleJumpAccel = gpTweakPlayer->GetVerticalDoubleJumpAccel();
  float horizontalDoubleJumpAccel = gpTweakPlayer->GetHorizontalDoubleJumpAccel();
  if (x37c_sidewaysDashing) {
    doubleJumpImpulse = gpTweakPlayer->GetSidewaysDoubleJumpImpulse();
    verticalDoubleJumpAccel = gpTweakPlayer->GetSidewaysVerticalDoubleJumpAccel();
    horizontalDoubleJumpAccel = gpTweakPlayer->GetSidewaysHorizontalDoubleJumpAccel();
  }
  const bool submerged = x828_distanceUnderWater >= 0.8f * GetEyeHeight();
  if (submerged) {
    doubleJumpImpulse *= jumpFactor;
  }
  if (x258_movementState == NPlayer::kMS_ApplyJump) {
    if (gpTweakPlayer->GetMaxDoubleJumpWindow() - gpTweakPlayer->GetMinDoubleJumpWindow() >=
            x28c_sjTimer &&
        0.f < x28c_sjTimer && ControlMapper::GetPressInput(ControlMapper::kC_JumpOrBoost, input)) {
      SetMoveState(NPlayer::kMS_Jump, mgr);
      x384_dashTimer = 0.f;
      x380_strafeInputAtDash = StrafeInput(input);
      if (gpTweakPlayer->GetImpulseDoubleJump()) {
        const CVector3f impulse(0.f, 0.f, (doubleJumpImpulse - GetVelocityWR().GetZ()) * GetMass());
        ApplyImpulseWR(impulse, CAxisAngle::Identity());
      }
      float forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Forward, input);
      const float backward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
      if (forward < backward) {
        forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
      }
      return jumpFactor * ((verticalDoubleJumpAccel -
                            forward * (verticalDoubleJumpAccel - horizontalDoubleJumpAccel)) *
                           GetMass());
    }
    return GetGravity() * GetMass();
  }
  if (ControlMapper::GetDigitalInput(ControlMapper::kC_JumpOrBoost, input) ||
      (x258_movementState == NPlayer::kMS_Jump &&
       x290_minJumpTimeout <= x288_startingJumpTimeout)) {
    if (x258_movementState != NPlayer::kMS_Jump) {
      if (ControlMapper::GetPressInput(ControlMapper::kC_JumpOrBoost, input)) {
        SetMoveState(NPlayer::kMS_Jump, mgr);
        return jumpFactor * (verticalJumpAccel * GetMass());
      }
      return 0.f;
    }
    float forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Forward, input);
    const float backward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
    if (forward < backward) {
      forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
    }
    return jumpFactor *
           ((verticalJumpAccel - forward * (verticalJumpAccel - horizontalJumpAccel)) * GetMass());
  }
  if (x258_movementState == NPlayer::kMS_Jump) {
    SetMoveState(NPlayer::kMS_ApplyJump, mgr);
  }
  return 0.f;
}

void CPlayer::SetMoveState(NPlayer::EPlayerMovementState state, CStateManager& mgr) {
  switch (state) {
  case NPlayer::kMS_Jump:
    if (x258_movementState == NPlayer::kMS_ApplyJump) {
      DoSfxEffects(CSfxManager::SfxStart(SFXsam_b_jump_00, 127, 64, true));
      mgr.GetRumbleManager()->Rumble(mgr, kRFX_PlayerBump, 0.2015f, kRP_One);
      x288_startingJumpTimeout = gpTweakPlayer->GetAllowedDoubleJumpTime();
      x290_minJumpTimeout =
          gpTweakPlayer->GetAllowedDoubleJumpTime() - gpTweakPlayer->GetMinDoubleJumpTime();
      x28c_sjTimer = 0.f;
    } else if (x258_movementState != NPlayer::kMS_Jump) {
      DoSfxEffects(CSfxManager::SfxStart(SFXsam_b_jump_01, 127, 64, true));
      x2a0_ = 0.01f;
      x288_startingJumpTimeout = gpTweakPlayer->GetAllowedJumpTime();
      x290_minJumpTimeout = gpTweakPlayer->GetAllowedJumpTime() - gpTweakPlayer->GetMinJumpTime();
      if (mgr.GetPlayerState()->GetItemAmount(CPlayerState::kIT_SpaceJumpBoots) != 0) {
        x28c_sjTimer = gpTweakPlayer->GetMaxDoubleJumpWindow();
      } else {
        x28c_sjTimer = 0.f;
      }
      if (x294_jumpCameraTimer <= 0.f && x29c_fallCameraTimer <= 0.f && !x3dc_inFreeLook &&
          !x3dd_lookButtonHeld) {
        x294_jumpCameraTimer = 0.01f;
        x2a4_cancelCameraPitch = false;
      }
    }
    x258_movementState = NPlayer::kMS_Jump;
    x2ac_surfaceRestraint = kSR_Air;
    x2a8_timeSinceJump = 0.f;
    break;
  case NPlayer::kMS_Falling:
    if (x258_movementState == NPlayer::kMS_OnGround) {
      x288_startingJumpTimeout = gpTweakPlayer->GetAllowedLedgeTime();
      x258_movementState = NPlayer::kMS_Falling;
      x2a0_ = 0.01f;
      if (gpTweakPlayer->GetFallingDoubleJump()) {
        x28c_sjTimer = gpTweakPlayer->GetMaxDoubleJumpWindow();
      } else {
        x28c_sjTimer = 0.f;
      }
    }
    break;
  case NPlayer::kMS_FallingMorphed:
    x258_movementState = NPlayer::kMS_FallingMorphed;
    x2ac_surfaceRestraint = kSR_Normal;
    break;
  case NPlayer::kMS_OnGround:
    x300_fallingTime = 0.f;
    x258_movementState = NPlayer::kMS_OnGround;
    x288_startingJumpTimeout = 0.f;
    x28c_sjTimer = 0.f;
    x2ac_surfaceRestraint = kSR_Normal;
    if (x2f8_morphBallState != kMS_Morphed) {
      AddMaterial(kMT_GroundCollider, mgr);
    }
    x294_jumpCameraTimer = 0.f;
    x29c_fallCameraTimer = 0.f;
    x2a4_cancelCameraPitch = false;
    x298_jumpPresses = 0;
    break;
  case NPlayer::kMS_ApplyJump:
    x288_startingJumpTimeout = 0.f;
    if (x258_movementState != NPlayer::kMS_ApplyJump) {
      x258_movementState = NPlayer::kMS_ApplyJump;
      if (x294_jumpCameraTimer <= 0.f && x29c_fallCameraTimer <= 0.f && !x3dc_inFreeLook &&
          !x3dd_lookButtonHeld) {
        x29c_fallCameraTimer = 0.01f;
        x2a4_cancelCameraPitch = false;
      }
    }
    x2ac_surfaceRestraint = kSR_Air;
    break;
  }
}

void CPlayer::CalculatePlayerMovementDirection(float dt) {
  if (x2f8_morphBallState == kMS_Morphing || x2f8_morphBallState == kMS_Unmorphing) {
    return;
  }
  const CVector3f delta = GetTranslation() - x524_lastPosForDirCalc;
  if (delta.CanBeNormalized() && delta.Magnitude() > 0.02f) {
    x53c_timeMoving += dt;
    x4f8_moveSpeed = CMath::AbsF(delta.Magnitude() / dt);
    x500_lookDir = delta.AsNormalized();
    CVector3f flatDelta = delta;
    flatDelta.SetZ(0.f);
    if (flatDelta.CanBeNormalized()) {
      x4fc_flatMoveSpeed = CMath::AbsF(flatDelta.Magnitude() / dt);
      flatDelta.Normalize();
      switch (x2f8_morphBallState) {
      case kMS_Morphed:
        if (x4fc_flatMoveSpeed > 0.25f) {
          x50c_moveDir = flatDelta;
        }
        x530_gunDir = x50c_moveDir;
        x524_lastPosForDirCalc = GetTranslation();
        break;
      case kMS_Unmorphed:
      case kMS_Morphing:
      case kMS_Unmorphing:
        x500_lookDir = GetTransform().GetForward();
        x50c_moveDir = x500_lookDir;
        x50c_moveDir.SetZ(0.f);
        if (x50c_moveDir.CanBeNormalized()) {
          x50c_moveDir.Normalize();
        }
        x530_gunDir = x50c_moveDir;
        x524_lastPosForDirCalc = GetTranslation();
        break;
      }
    } else {
      if (x2f8_morphBallState != kMS_Morphed) {
        x500_lookDir = GetTransform().GetForward();
        x50c_moveDir = x500_lookDir;
        x50c_moveDir.SetZ(0.f);
        if (x50c_moveDir.CanBeNormalized()) {
          x50c_moveDir.Normalize();
        }
        x530_gunDir = x50c_moveDir;
        x524_lastPosForDirCalc = GetTranslation();
      }
      x4fc_flatMoveSpeed = 0.f;
    }
  } else {
    x53c_timeMoving = 0.f;
    switch (x2f8_morphBallState) {
    case kMS_Morphed:
    case kMS_Morphing:
    case kMS_Unmorphing:
      x500_lookDir = x50c_moveDir;
      break;
    default:
      x500_lookDir = GetTransform().GetForward();
      x50c_moveDir = x500_lookDir;
      x50c_moveDir.SetZ(0.f);
      if (x50c_moveDir.CanBeNormalized()) {
        x50c_moveDir.Normalize();
      }
      x530_gunDir = x50c_moveDir;
      x524_lastPosForDirCalc = GetTranslation();
      break;
    }
    x4f8_moveSpeed = 0.f;
    x4fc_flatMoveSpeed = 0.f;
  }
  x50c_moveDir.SetZ(0.f);
  if (x50c_moveDir.CanBeNormalized()) {
    x500_lookDir.Normalize();
  }
}

void CPlayer::UpdatePlayerControlDirection(float dt, CStateManager& mgr) {
  const CVector3f oldDirection = x540_controlDir;
  const CVector3f oldFlatDirection = x54c_controlDirFlat;
  CalculatePlayerControlDirection(mgr);
  if (x9c6_25_interpolatingControlDir && x2f8_morphBallState == kMS_Morphed) {
    x9f8_controlDirInterpTime += dt;
    if (x9f8_controlDirInterpTime > x9fc_controlDirInterpDur) {
      x9f8_controlDirInterpTime = x9fc_controlDirInterpDur;
      ResetControlDirectionInterpolation();
    }
    const float blend = CMath::Limit(x9f8_controlDirInterpTime / x9fc_controlDirInterpDur, 1.f);
    x540_controlDir = CVector3f::Lerp(oldDirection, x540_controlDir, blend);
    x54c_controlDirFlat = CVector3f::Lerp(oldFlatDirection, x540_controlDir, blend);
  }
}

void CPlayer::CalculatePlayerControlDirection(CStateManager& mgr) {
  if (x9c4_30_controlDirOverride) {
    if (x9d8_controlDirOverrideDir.CanBeNormalized()) {
      x540_controlDir = x9d8_controlDirOverrideDir.AsNormalized();
      x54c_controlDirFlat = x9d8_controlDirOverrideDir;
      x54c_controlDirFlat.SetZ(0.f);
      if (x54c_controlDirFlat.CanBeNormalized()) {
        x54c_controlDirFlat.Normalize();
      } else {
        x540_controlDir = CVector3f(0.f, 1.f, 0.f);
        x54c_controlDirFlat = CVector3f(0.f, 1.f, 0.f);
      }
    } else {
      x540_controlDir = CVector3f(0.f, 1.f, 0.f);
      x54c_controlDirFlat = CVector3f(0.f, 1.f, 0.f);
    }
  } else {
    const CVector3f cameraToPlayer =
        GetTranslation() - mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTranslation();
    if (!cameraToPlayer.CanBeNormalized()) {
      x540_controlDir = CVector3f(0.f, 1.f, 0.f);
      x54c_controlDirFlat = CVector3f(0.f, 1.f, 0.f);
    } else {
      CVector3f flatDirection = cameraToPlayer;
      flatDirection.SetZ(0.f);
      if (flatDirection.CanBeNormalized()) {
        if (flatDirection.Magnitude() > gpTweakBall->GetBallCameraControlDistance()) {
          x540_controlDir = cameraToPlayer.AsNormalized();
          if (flatDirection.CanBeNormalized()) {
            flatDirection.Normalize();
            switch (x2f8_morphBallState) {
            case kMS_Morphed:
              x54c_controlDirFlat = flatDirection;
              break;
            case kMS_Unmorphed:
            case kMS_Morphing:
            case kMS_Unmorphing:
              x540_controlDir = GetTransform().GetForward();
              x54c_controlDirFlat = x540_controlDir;
              x54c_controlDirFlat.SetZ(0.f);
              if (x54c_controlDirFlat.CanBeNormalized()) {
                x54c_controlDirFlat.Normalize();
              }
              break;
            }
          } else if (x2f8_morphBallState != kMS_Morphed) {
            x540_controlDir = GetTransform().GetForward();
            x54c_controlDirFlat = x540_controlDir;
            x54c_controlDirFlat.SetZ(0.f);
            if (x54c_controlDirFlat.CanBeNormalized()) {
              x54c_controlDirFlat.Normalize();
            }
          }
        } else {
          if (x4fc_flatMoveSpeed < 0.25f) {
            x540_controlDir = cameraToPlayer;
            x54c_controlDirFlat = flatDirection;
          } else if (x2f8_morphBallState != kMS_Morphed) {
            x540_controlDir = GetTransform().GetForward();
            x54c_controlDirFlat = x540_controlDir;
            x54c_controlDirFlat.SetZ(0.f);
            if (x54c_controlDirFlat.CanBeNormalized()) {
              x54c_controlDirFlat.Normalize();
            }
          }
        }
      }
    }
  }
}

void CPlayer::CalculateLeaveMorphBallDirection(const CFinalInput& input) {
  if (x2f8_morphBallState != kMS_Morphed) {
    x518_leaveMorphDir = x50c_moveDir;
  } else {
    const float forward = ControlMapper::GetAnalogInput(ControlMapper::kC_Forward, input);
    const float backward = ControlMapper::GetAnalogInput(ControlMapper::kC_Backward, input);
    const float left = ControlMapper::GetAnalogInput(ControlMapper::kC_TurnLeft, input);
    const float right = ControlMapper::GetAnalogInput(ControlMapper::kC_TurnRight, input);
    if (forward > 0.3f || backward > 0.3f || left > 0.3f || right > 0.3f) {
      if (GetVelocityWR().Magnitude() > 0.5f) {
        x518_leaveMorphDir = x50c_moveDir;
      }
    }
  }
}

float CPlayer::GetBallMaxVelocity() const {
  return gpTweakBall->GetBallTranslationMaxSpeed(GetSurfaceRestraint());
}

float CPlayer::GetActualFirstPersonMaxVelocity(float dt) const {
  const float friction = gpTweakPlayer->GetPlayerTranslationFriction(GetSurfaceRestraint());
  const float frictionForce = friction * GetMass();
  const float maxSpeed = gpTweakPlayer->GetPlayerTranslationMaxSpeed(GetSurfaceRestraint());
  const float acceleration = gpTweakPlayer->GetMaxTranslationalAcceleration(GetSurfaceRestraint());
  return -(frictionForce * maxSpeed / (acceleration * dt) - maxSpeed - friction);
}

float CPlayer::GetActualBallMaxVelocity(float dt) const {
  const float friction = gpTweakBall->GetBallTranslationFriction(GetSurfaceRestraint());
  const float frictionForce = friction * GetMass();
  const float maxSpeed = gpTweakBall->GetBallTranslationMaxSpeed(GetSurfaceRestraint());
  const float acceleration = gpTweakBall->GetMaxBallTranslationAcceleration(GetSurfaceRestraint());
  return -(frictionForce * maxSpeed / (acceleration * dt) - maxSpeed - friction);
}

void CPlayer::CollidedWith(const TUniqueId& id, const CCollisionInfoList& list,
                           CStateManager& mgr) {
  if (x2f8_morphBallState != kMS_Unmorphed) {
    x768_morphball->CollidedWith(id, list, mgr);
  }
}

CTransform4f CPlayer::GetPrimitiveTransform() const {
  return CPhysicsActor::GetPrimitiveTransform();
}

const CCollidableSphere* CPlayer::GetCollidableSphere() const {
  return &x768_morphball->GetCollidableSphere();
}

const CCollisionPrimitive* CPlayer::GetCollisionPrimitive() const {
  switch (x2f8_morphBallState) {
  case kMS_Morphed:
    return GetCollidableSphere();
  case kMS_Unmorphed:
    return CPhysicsActor::GetCollisionPrimitive();
  case kMS_Morphing:
  case kMS_Unmorphing:
    return CPhysicsActor::GetCollisionPrimitive();
  default:
    return CPhysicsActor::GetCollisionPrimitive();
  }
}

CTransform4f CPlayer::CreateTransformFromMovementDirection() const {
  CVector3f direction = x50c_moveDir;
  if (direction.CanBeNormalized()) {
    direction.Normalize();
  } else {
    direction = CVector3f(0.f, 1.f, 0.f);
  }
  const CVector3f right(direction.GetY(), -direction.GetX(), 0.f);
  return CTransform4f::FromColumns(right, direction, CVector3f::Up(), GetTranslation());
}

void CPlayer::BombJump(const CVector3f& position, CStateManager& mgr) {
  if (x2f8_morphBallState == kMS_Morphed &&
      x768_morphball->GetBombJumpState() != CMorphBall::kBJS_BombJumpDisabled) {
    const float extent = gpTweakPlayer->GetPlayerBallHalfExtent();
    const CVector3f toBall =
        GetTranslation() + CVector3f(0.f, 0.f, extent) - position;
    const float maxDistance = gpTweakPlayer->GetBombJumpHeight();
    if (toBall.MagSquared() < maxDistance * maxDistance &&
        CVector3f::Dot(CVector3f(0.f, 0.f, 1.f), toBall) >= -extent) {
      float velocity = sqrt(2.0 * fabs(gpTweakPlayer->GetNormalGravAccel()) *
                            gpTweakPlayer->GetBombJumpRadius());
      mgr.GetRumbleManager()->Rumble(mgr, kRFX_PlayerBump, 0.3f, kRP_One);
      x2a0_ = 0.01f;
      switch (GetSurfaceRestraint()) {
      case kSR_Water:
        velocity *= gpTweakPlayer->GetWaterBallJumpFactor();
        break;
      case kSR_Lava:
        velocity *= gpTweakPlayer->GetLavaBallJumpFactor();
        break;
      case kSR_Phazon:
        velocity *= gpTweakPlayer->GetPhazonBallJumpFactor();
        break;
      default:
        break;
      }
      const CVector3f newVelocity(0.f, 0.f, velocity);
      SetVelocityWR(newVelocity);
      x768_morphball->SetDamageTimer(0.1f);
      x768_morphball->CancelBoosting();
      if (x9d0_bombJumpCount > 0) {
        if (x9d0_bombJumpCount > 2) {
          x9d0_bombJumpCount = 0;
          x9d4_bombJumpCheckDelayFrames = 0;
        } else {
          ++x9d0_bombJumpCount;
        }
      } else {
        const CBallCamera* camera = mgr.GetCameraManager()->GetBallCamera();
        if (camera->GetTooCloseActorId() != kInvalidUniqueId &&
            camera->GetTooCloseActorDistance() < 5.f) {
          x9d0_bombJumpCount = 1;
          x9d4_bombJumpCheckDelayFrames = 2;
        }
      }
      DoSfxEffects(CSfxManager::AddEmitter(SFXsam_b_bombjump_00, GetTranslation(),
                                           CVector3f::Zero(), false, false));
    }
  }
}

void CPlayer::Teleport(const CTransform4f& transform, CStateManager& mgr,
                       const bool resetBallCamera) {
  CVector3f direction = transform.GetForward();
  direction.SetZ(0.f);
  CPhysicsActor::Stop();
  if (direction.CanBeNormalized()) {
    direction.Normalize();
    SetTransform(CTransform4f::LookAt(CVector3f::Zero(), direction, CVector3f::Up()));
    SetTranslation(transform.GetTranslation());
    x500_lookDir = direction;
    x50c_moveDir = direction;
    x530_gunDir = direction;
    x524_lastPosForDirCalc = transform.GetTranslation();
    x4f8_moveSpeed = 0.f;
    x4fc_flatMoveSpeed = 0.f;
    x53c_timeMoving = 0.f;
    x4a4_moveSpeedAvg.clear();
    x540_controlDir = direction;
    x54c_controlDirFlat = direction;
  } else {
    SetTranslation(transform.GetTranslation());
  }
  x9c5_31_stepCameraZBiasDirty = true;
  SetEyeZBias(0.f);
  SetLastNonCollidingState(GetMotionState());
  SetMoveState(NPlayer::kMS_OnGround, mgr);
  CTransform4f eyeTransform = GetTransform();
  eyeTransform.SetTranslation(GetEyePosition());
  mgr.GetCameraManager()->FirstPersonCamera()->Reset(eyeTransform, mgr);
  if (resetBallCamera) {
    mgr.GetCameraManager()->BallCamera()->Reset(eyeTransform, mgr);
  }
  ForceGunOrientation(GetTransform(), mgr);
  BreakOrbit(kOB_Respawn, mgr);
}

bool CPlayer::CheckSubmerged() const {
  if (!IsInFluid()) {
    return false;
  }
  const float ballHeight = 2.f * gpTweakPlayer->GetPlayerBallHalfExtent();
  const float eyeHeight = 0.5f * GetEyeHeight();
  float height = eyeHeight;
  if (x2f8_morphBallState == kMS_Morphed) {
    height = ballHeight;
  }
  return x828_distanceUnderWater >= height;
}

void CPlayer::UpdateSubmerged(const CStateManager& mgr) {
  x82c_inLava = false;
  x828_distanceUnderWater = 0.f;
  if (!IsInFluid()) {
    return;
  }
  if (const CScriptWater* water = TCastToConstPtr< CScriptWater >(mgr.GetObjectById(InFluidId()))) {
    x828_distanceUnderWater = -CPlane(water->GetTriggerBoundsWR().GetMaxPoint().GetZ(),
                                      CUnitVector3f(0.f, 0.f, 1.f, CUnitVector3f::kN_Yes))
                                   .GetHeight(GetTranslation());
    bool lava = true;
    const CFluidPlane::EFluidType fluidType = water->GetFluidPlane().GetFluidType();
    if (fluidType != CFluidPlane::kFT_Lava && fluidType != CFluidPlane::kFT_ThickLava) {
      lava = false;
    }
    x82c_inLava = lava;
    CheckSubmerged();
  }
}

float CPlayer::GetStepDownHeight() const {
  if (x258_movementState == NPlayer::kMS_Jump) {
    return -1.f;
  }
  if (x258_movementState == NPlayer::kMS_ApplyJump) {
    return 0.1f;
  }
  return CPhysicsActor::GetStepDownHeight();
}

float CPlayer::GetStepUpHeight() const {
  if (x258_movementState == NPlayer::kMS_Jump || x258_movementState == NPlayer::kMS_ApplyJump) {
    return 0.3f;
  }
  return CPhysicsActor::GetStepUpHeight();
}

float CPlayer::GetUnbiasedEyeHeight() const {
  return x2d8_fpBounds.GetPointD().GetZ() - gpTweakPlayer->GetEyeOffset();
}

float CPlayer::GetEyeHeight() const {
  return x9c8_eyeZBias + (x2d8_fpBounds.GetPointD().GetZ() - gpTweakPlayer->GetEyeOffset());
}

CVector3f CPlayer::GetEyePosition() const {
  return GetTranslation() + CVector3f(0.f, 0.f, GetEyeHeight());
}

CVector3f CPlayer::GetBallPosition() const {
  return GetTranslation() + CVector3f(0.f, 0.f, gpTweakPlayer->GetPlayerBallHalfExtent());
}

void CPlayer::ResetPlayerHintState(CStateManager& mgr) {
  x9c4_26_ = true;
  x9c4_27_canEnterMorphBall = true;
  x9c4_28_canLeaveMorphBall = true;
  x9c4_30_controlDirOverride = false;
  x9c6_24_extendTargetDistance = false;
  x9c6_26_outOfBallLookAtHint = false;
  x9c4_29_spiderBallControlXY = false;
  x9c6_29_disableInput = false;
  x9c7_25_outOfBallLookAtHintActor = false;
  x768_morphball->SetBoostEnabled(true);
  ResetControlDirectionInterpolation();
}

const bool CPlayer::SetAreaPlayerHint(const CScriptPlayerHint& hint, CStateManager& mgr) {
  x9c4_26_ = (hint.GetOverrideFlags() & 0x1) != 0;
  x9c4_27_canEnterMorphBall = !(hint.GetOverrideFlags() & 0x40);
  x9c4_28_canLeaveMorphBall = !(hint.GetOverrideFlags() & 0x20);
  x9c4_30_controlDirOverride = (hint.GetOverrideFlags() & 0x2) != 0;
  if (x9c4_30_controlDirOverride) {
    x9d8_controlDirOverrideDir = hint.GetTransform().GetForward();
  }
  x9c6_24_extendTargetDistance = (hint.GetOverrideFlags() & 0x4) != 0;
  x9c6_26_outOfBallLookAtHint = (hint.GetOverrideFlags() & 0x8) != 0;
  x9c4_29_spiderBallControlXY = (hint.GetOverrideFlags() & 0x10) != 0;
  x9c6_29_disableInput = (hint.GetOverrideFlags() & 0x80) != 0;
  x9c7_25_outOfBallLookAtHintActor = (hint.GetOverrideFlags() & 0x4000) != 0;
  x768_morphball->SetBoostEnabled(!(hint.GetOverrideFlags() & 0x100));
  bool switchedVisor = false;
  if ((hint.GetOverrideFlags() & 0x200) != 0) {
    if (mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_CombatVisor)) {
      mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_Combat);
    }
    switchedVisor = true;
  }
  if ((hint.GetOverrideFlags() & 0x400) != 0) {
    if (mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_ScanVisor)) {
      mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_Scan);
    }
    switchedVisor = true;
  }
  if ((hint.GetOverrideFlags() & 0x800) != 0) {
    if (mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_ThermalVisor)) {
      mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_Thermal);
    }
    switchedVisor = true;
  }
  if ((hint.GetOverrideFlags() & 0x1000) != 0) {
    if (mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_XRayVisor)) {
      mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_XRay);
    }
    switchedVisor = true;
  }
  return switchedVisor;
}

void CPlayer::UpdatePlayerHints(CStateManager& mgr) {
  bool removedHint = false;
  for (AUTO(it, x838_playerHints.begin()); it != x838_playerHints.end();) {
    if (!TCastToConstPtr< CScriptPlayerHint >(mgr.ObjectById(it->second))) {
      it = x838_playerHints.erase(it);
      removedHint = true;
    } else {
      ++it;
    }
  }
  bool needsNewHint = false;
  if (!x93c_playerHintsToRemove.empty()) {
    for (AUTO(id, x93c_playerHintsToRemove.begin()); id != x93c_playerHintsToRemove.end(); ++id) {
      TUniqueId uid = *id;
      const CScriptPlayerHint* hint = TCastToConstPtr< CScriptPlayerHint >(mgr.GetObjectById(uid));
      if (hint && (hint->GetObjectCount() == 0 || hint->GetDeactivated())) {
        for (AUTO(it, x838_playerHints.begin()); it != x838_playerHints.end(); ++it) {
          if (it->second == uid) {
            x838_playerHints.erase(it);
            if (uid == x830_playerHint) {
              needsNewHint = true;
            }
            break;
          }
        }
      }
    }
    x93c_playerHintsToRemove.clear();
  }
  bool addedHint = false;
  if (!x980_playerHintsToAdd.empty()) {
    for (AUTO(id, x980_playerHintsToAdd.begin()); id != x980_playerHintsToAdd.end(); ++id) {
      TUniqueId uid = *id;
      const CScriptPlayerHint* const hint = TCastToConstPtr< CScriptPlayerHint >(mgr.GetObjectById(uid));
      if (hint) {
        bool exists = false;
        for (rstl::reserved_vector< rstl::pair< int, TUniqueId >, 32 >::const_iterator it =
                 x838_playerHints.begin();
             it != x838_playerHints.end(); ++it) {
          if (it->second == uid) {
            exists = true;
            break;
          }
        }
        if (!exists) {
          x838_playerHints.push_back(rstl::pair< int, TUniqueId >(hint->GetPriority(), uid));
          addedHint = true;
        }
      }
    }
    x980_playerHintsToAdd.clear();
  }
  if (needsNewHint || addedHint || removedHint) {
    rstl::less< int > less;
    rstl::pair_sorter_finder< rstl::pair< int, TUniqueId >, rstl::less< int > > sorter(less);
    rstl::sort(x838_playerHints.begin(), x838_playerHints.end(), sorter);
    if ((needsNewHint || removedHint) && x838_playerHints.empty()) {
      x830_playerHint = kInvalidUniqueId;
      x834_playerHintPriority = 1000;
      ResetPlayerHintState(mgr);
      return;
    }
    CScriptPlayerHint* bestHint = nullptr;
    bool foundInArea = false;
    for (AUTO(it, x838_playerHints.begin()); it != x838_playerHints.end(); ++it) {
      bestHint = TCastToPtr< CScriptPlayerHint >(mgr.ObjectById(it->second));
      if (bestHint && bestHint->GetCurrentAreaId() == mgr.GetNextAreaId()) {
        foundInArea = true;
        break;
      }
    }
    if (!foundInArea) {
      x830_playerHint = kInvalidUniqueId;
      x834_playerHintPriority = 1000;
      ResetPlayerHintState(mgr);
    }
    if (bestHint && foundInArea && x830_playerHint != bestHint->GetUniqueId()) {
      x830_playerHint = bestHint->GetUniqueId();
      x834_playerHintPriority = bestHint->GetPriority();
      if (SetAreaPlayerHint(*bestHint, mgr)) {
        DeactivatePlayerHint(x830_playerHint, mgr);
      }
    }
  }
}

void CPlayer::AddToPlayerHintAddList(TUniqueId id, CStateManager& mgr) {
  if (TCastToPtr< CScriptPlayerHint >(mgr.ObjectById(id))) {
    for (AUTO(it, x980_playerHintsToAdd.begin()); it != x980_playerHintsToAdd.end(); ++it) {
      if (*it == id) {
        return;
      }
    }
    if (x838_playerHints.size() == 32 || x980_playerHintsToAdd.size() == 32) {
      return;
    }
    x980_playerHintsToAdd.push_back(id);
  }
}

void CPlayer::DeactivatePlayerHint(TUniqueId id, CStateManager& mgr) {
  if (CScriptPlayerHint* hint = TCastToPtr< CScriptPlayerHint >(mgr.ObjectById(id))) {
    AUTO(found, rstl::find(x93c_playerHintsToRemove.begin(), x93c_playerHintsToRemove.end(), id));
    if (found == x93c_playerHintsToRemove.end() && x93c_playerHintsToRemove.size() != 32) {
      x93c_playerHintsToRemove.push_back(id);
      hint->ClearObjectList();
      hint->SetDeactivated();
    }
  }
}

void CPlayer::AddToPlayerHintRemoveList(TUniqueId id, CStateManager& mgr) {
  if (TCastToPtr< CScriptPlayerHint >(mgr.ObjectById(id))) {
    AUTO(found, rstl::find(x93c_playerHintsToRemove.begin(), x93c_playerHintsToRemove.end(), id));
    if (found == x93c_playerHintsToRemove.end() && x93c_playerHintsToRemove.size() != 32) {
      x93c_playerHintsToRemove.push_back(id);
    }
  }
}

void CPlayer::SetEyeZBias(float bias) { x9c8_eyeZBias = bias; }

float CPlayer::UpdateCameraBob(float dt, CStateManager& mgr) {
  float magnitude = 0.f;
  CPlayerCameraBob::ECameraBobState state;
  const CVector3f velocity = GetVelocityWR();
  if (x304_orbitState == kOS_NoOrbit) {
    const float forwardSpeed = CVector3f::Dot(velocity, GetTransform().GetForward());
    state = CPlayerCameraBob::kCBS_Walk;
    magnitude = CMath::AbsF(forwardSpeed / GetActualFirstPersonMaxVelocity(dt));
    if (magnitude < 0.01f) {
      state = CPlayerCameraBob::kCBS_WalkNoBob;
      magnitude = 0.f;
    }
  } else {
    state = CPlayerCameraBob::kCBS_Orbit;
    const float rightSpeed = CVector3f::Dot(velocity, GetTransform().GetRight());
    const float forwardSpeed = CVector3f::Dot(velocity, GetTransform().GetForward());
    const float maxSpeed = GetActualFirstPersonMaxVelocity(dt);
    const float strafeSpeed = skStrafeDistances[GetSurfaceRestraint()];
    const float maxMagnitude = CMath::SqrtF(strafeSpeed * strafeSpeed + maxSpeed * maxSpeed);
    magnitude = CMath::SqrtF(rightSpeed * rightSpeed + forwardSpeed * forwardSpeed) / maxMagnitude;
    magnitude *= CPlayerCameraBob::GetOrbitBobScale();
    magnitude = rstl::min_val(CPlayerCameraBob::GetMaxOrbitBobScale(), magnitude);
    if (magnitude < 0.01f) {
      magnitude = 0.f;
    }
  }
  if (x258_movementState != NPlayer::kMS_OnGround) {
    state = CPlayerCameraBob::kCBS_InAir;
    magnitude = 0.f;
  } else if (magnitude < 0.01f) {
    if (x490_gun->GetFiring() != 0) {
      state = CPlayerCameraBob::kCBS_GunFireNoBob;
      magnitude = 0.f;
    } else if (CMath::AbsF(GetAngularVelocityOR().GetAngle()) > 0.1f) {
      state = CPlayerCameraBob::kCBS_TurningNoBob;
      magnitude = 0.f;
    }
  }
  if (x3dc_inFreeLook || x3dd_lookButtonHeld) {
    state = CPlayerCameraBob::kCBS_FreeLookNoBob;
    magnitude = 0.f;
  }
  if (x304_orbitState == kOS_Grapple) {
    state = CPlayerCameraBob::kCBS_GrappleNoBob;
    magnitude = 0.f;
  }
  if (x3a8_scanState == kSS_ScanComplete) {
    magnitude = 0.f;
  }
  if (x38c_doneSidewaysDashing) {
    state = CPlayerCameraBob::kCBS_FreeLookNoBob;
    magnitude *= 0.1f;
    if (x258_movementState == NPlayer::kMS_OnGround) {
      x38c_doneSidewaysDashing = false;
    }
  }
  if (mgr.GetCameraManager()->IsInCinematicCamera()) {
    magnitude = 0.f;
  }
  magnitude *= mgr.GetCameraManager()->GetCameraBobMagnitude();
  x76c_cameraBob->SetPlayerVelocity(velocity);
  x76c_cameraBob->SetState(state, mgr);
  x76c_cameraBob->SetBobMagnitude(magnitude);
  const float timeScaleRange = 1.f - CPlayerCameraBob::GetSlowSpeedPeriodScale();
  x76c_cameraBob->SetBobTimeScale(timeScaleRange * magnitude +
                                  CPlayerCameraBob::GetSlowSpeedPeriodScale());
  x76c_cameraBob->Update(dt, mgr);
  return magnitude;
}

void CPlayer::SetIntoBallReadyAnimation(CStateManager& mgr) {
  const CAnimPlaybackParms parms(2, -1, 1.f, true);
  AnimationData()->SetAnimation(parms, false);
  AnimationData()->EnableLooping(false);
  ModelData()->AdvanceAnimation(0.f, mgr, kInvalidAreaId, true);
  AnimationData()->SetIsAnimating(false);
}

int CPlayer::ChoseTransitionToAnimation(float dt, CStateManager& mgr) const {
  if (x258_movementState == NPlayer::kMS_ApplyJump) {
    return 3;
  }
  const CVector3f localVelocity = GetTransform().TransposeRotate(GetVelocityWR());
  const CVector2f flat(localVelocity.GetX(), localVelocity.GetY());
  const CVector3f flatVelocity(flat.GetX(), flat.GetY(), 0.f);
  const float speed = flatVelocity.Magnitude();
  if (speed > 1.f) {
    float velocityAngle = atan2f(-flatVelocity.GetX(), flatVelocity.GetY());
    float twoPi = 2.f * M_PIF;
    float reciprocalTwoPi = 1.f / (2.f * M_PIF);
    if (velocityAngle > twoPi) {
      float turns = static_cast< int >(velocityAngle * reciprocalTwoPi);
      velocityAngle -= turns * twoPi;
    } else if (velocityAngle < 0.f) {
      float turns = static_cast< int >(velocityAngle * reciprocalTwoPi);
      velocityAngle = twoPi + (velocityAngle - turns * twoPi);
    }
    const float angle = CRelAngle(velocityAngle).AsDegrees();
    const float maxSpeed = GetActualFirstPersonMaxVelocity(dt);
    if (angle < 45.f || angle > 315.f) {
      if (speed < .5f * maxSpeed) {
        return 0;
      }
      return 4;
    }
    return 1;
  }
  return 2;
}

int CPlayer::GetNextBallTransitionAnim(float dt, bool& loop, CStateManager& mgr) {
  int anim = 12;
  const CVector2f flat(GetVelocityWR().GetX(), GetVelocityWR().GetY());
  const CVector3f velocity(flat.GetX(), flat.GetY(), 0.f);
  loop = false;
  if (velocity.CanBeNormalized()) {
    const float speed = velocity.Magnitude();
    const float maxSpeed = GetActualFirstPersonMaxVelocity(dt);
    if (speed > .2f * maxSpeed) {
      loop = true;
      anim = 15;
      if (speed >= maxSpeed) {
        anim = 13;
      }
      if (CVector3f::Dot(x50c_moveDir,
                         mgr.GetCameraManager()->GetBallCamera()->GetTransform().GetForward()) <
          -.5f) {
        anim = 12;
      }
    }
  }
  return anim;
}

void CPlayer::TransitionToMorphBallState(float dt, CStateManager& mgr) {
  x584_ballTransitionAnim = ChoseTransitionToAnimation(dt, mgr);
  x58c_transitionVel = GetVelocityWR().Magnitude();
  if (HasAnimation()) {
    CAnimData& animData = *AnimationData();
    const CAnimPlaybackParms& parms = CAnimPlaybackParms(x584_ballTransitionAnim, -1, 1.f, true);
    animData.SetAnimation(parms, false);
    animData.SetAnimDir(CAnimData::kAD_Forward);
  }
  ModelData()->EnableLooping(false);
  ModelData()->Touch(mgr, 0);
  SetMomentumWR(CVector3f::Zero());
#ifdef TARGET_PC
  sFastMorphAirCarry = false;
  if (IsFastMorphTransition()) {
    ClearForcesAndTorques();
    SetAngularVelocityWR(CAxisAngle::Identity());
    // The flash retail starts halfway through covers the whole fast morph.
    x768_morphball->ResetMorphBallTransitionFlash();
  } else
#endif
  Stop();
  SetMorphBallState(kMS_Morphing, mgr);
  SetCameraState(kCS_Transitioning, mgr);
  x500_lookDir = GetTransform().GetForward();
  x50c_moveDir = x500_lookDir;
  x50c_moveDir.SetZ(0.f);
  if (x50c_moveDir.CanBeNormalized()) {
    x50c_moveDir.Normalize();
  } else {
    x500_lookDir = CVector3f(0.f, 1.f, 0.f);
    x50c_moveDir = CVector3f(0.f, 1.f, 0.f);
  }
  CBallCamera* ballCamera = mgr.CameraManager()->BallCamera();
  mgr.CameraManager()->SetPlayerCamera(mgr, ballCamera->GetUniqueId());
  if (!mgr.GetCameraManager()->HasBallCameraInitialPositionHint(mgr)) {
    mgr.CameraManager()->ResetCameraHint(mgr);
    ballCamera->SetState(CBallCamera::kBCS_ToBall, mgr);
  } else {
    ballCamera->SetState(CBallCamera::kBCS_Default, mgr);
    SetCameraState(kCS_Ball, mgr);
    const CTransform4f newXf = mgr.GetCameraManager()->GetFirstPersonCamera()->GetTransform();
    ballCamera->SetTransform(newXf);
    ballCamera->TeleportCamera(newXf.GetTranslation(), mgr);
    mgr.CameraManager()->ResetCameraHint(mgr);
    ballCamera->InterpolateFOV(mgr.GetCameraManager()->GetFirstPersonCamera()->GetFov(),
                               CCameraManager::GetDefaultThirdPersonVerticalFOV(), 1.f, 0.f);
  }
  BreakOrbit(kOB_EnterMorphBall, mgr);
  x490_gun->CancelFiring(mgr);
  HolsterGun(mgr);
}

void CPlayer::TransitionFromMorphBallState(float dt, CStateManager& mgr) {
  x584_ballTransitionAnim = 14;
  const CVector2f flatVelocity(GetVelocityWR().GetX(), GetVelocityWR().GetY());
  x58c_transitionVel = flatVelocity.Magnitude();
  if (x58c_transitionVel < 1.f) {
    x584_ballTransitionAnim = 5;
  }
  if (x258_movementState != NPlayer::kMS_OnGround) {
    const CVector3f ballPos = GetBallPosition();
    if (mgr.RayCollideWorld(ballPos, ballPos + CVector3f(0.f, 0.f, -7.f), BallTransitionCollide,
                            this)) {
      x584_ballTransitionAnim = 7;
    }
  }
  if (HasAnimation()) {
    CAnimData& animData = *AnimationData();
    const CAnimPlaybackParms& parms = CAnimPlaybackParms(x584_ballTransitionAnim, -1, 1.f, true);
    animData.SetAnimation(parms, false);
    animData.SetAnimDir(CAnimData::kAD_Forward);
  }
  ModelData()->EnableLooping(false);
  ModelData()->Touch(mgr, 0);
  SetMorphBallState(kMS_Unmorphing, mgr);
  x768_morphball->LeaveMorphBallState(mgr);
  CBallCamera* ballCamera = mgr.CameraManager()->BallCamera();
  mgr.CameraManager()->SetPlayerCamera(
      mgr, TUniqueId(mgr.GetCameraManager()->GetFirstPersonCamera()->GetUniqueId()));
  CVector3f camToPlayer = GetTranslation() - ballCamera->GetTranslation();
  camToPlayer.SetZ(0.f);
  if (camToPlayer.CanBeNormalized()) {
    camToPlayer.Normalize();
    CVector3f direction = x518_leaveMorphDir;
    CVector3f lookFlat = x500_lookDir;
    lookFlat.SetZ(0.f);
    if (!lookFlat.CanBeNormalized() || lookFlat.Magnitude() < .1f) {
      direction = camToPlayer;
    }
    if (x9c6_26_outOfBallLookAtHint) {
      if (const CScriptPlayerHint* hint =
              TCastToConstPtr< CScriptPlayerHint >(mgr.GetObjectById(x830_playerHint))) {
        CVector3f delta = hint->GetTranslation() - GetTranslation();
        delta.SetZ(0.f);
        if (delta.CanBeNormalized()) {
          direction = delta.AsNormalized();
        }
      }
    }
    if (x9c7_25_outOfBallLookAtHintActor) {
      if (const CScriptPlayerHint* hint =
              TCastToConstPtr< CScriptPlayerHint >(mgr.GetObjectById(x830_playerHint))) {
        if (const CActor* actor =
                TCastToConstPtr< CActor >(mgr.GetObjectById(hint->GetActorId()))) {
          CVector3f delta = actor->GetOrbitPosition(mgr) - GetTranslation();
          delta.SetZ(0.f);
          if (delta.CanBeNormalized()) {
            direction = delta.AsNormalized();
          }
        }
      }
    }
    if (acosf(CMath::Limit(CVector3f::Dot(camToPlayer, direction), 1.f)) < M_PIF / 1.2f ||
        x9c7_25_outOfBallLookAtHintActor) {
      SetTransform(CTransform4f::LookAt(GetTranslation(), CVector3f(GetTranslation() + direction)));
    } else {
      SetTransform(
          CTransform4f::LookAt(GetTranslation(), CVector3f(GetTranslation() + camToPlayer)));
      UpdateArmAndGunTransforms(.01f, mgr);
    }
  } else {
    SetTransform(CreateTransformFromMovementDirection());
  }
  const TUniqueId closeActorId = mgr.GetCameraManager()->GetBallCamera()->GetTooCloseActorId();
  if (const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(closeActorId))) {
    if (ballCamera->GetTooCloseActorDistance() < 20.f &&
        ballCamera->GetTooCloseActorDistance() > 1.f) {
      CVector3f delta = actor->GetTranslation() - GetTranslation();
      delta.SetZ(0.f);
      CVector3f camDelta = actor->GetTranslation() - ballCamera->GetTranslation();
      camDelta.SetZ(0.f);
      if (delta.CanBeNormalized() && camDelta.CanBeNormalized()) {
        delta.Normalize();
        CVector3f cameraLook = ballCamera->GetTransform().GetForward();
        cameraLook.SetZ(0.f);
        cameraLook.Normalize();
        camDelta.Normalize();
        if (CVector3f::Dot(delta, camDelta) >= .3f && CVector3f::Dot(camDelta, cameraLook) >= .7f) {
          SetTransform(CTransform4f::LookAt(GetTranslation(), GetTranslation() + delta));
        }
      }
    }
  }
  ForceGunOrientation(GetTransform(), mgr);
  DrawGun(mgr);
  mgr.CameraManager()->BallCamera()->SetState(CBallCamera::kBCS_FromBall, mgr);
  ClearForcesAndTorques();
  SetAngularVelocityWR(CAxisAngle::Identity());
  AddMaterial(kMT_GroundCollider, mgr);
  SetMomentumWR(CVector3f::Zero());
#ifdef TARGET_PC
  if (IsFastMorphTransition()) {
    if (x258_movementState == NPlayer::kMS_OnGround) {
      // As in Metroid Prime 4: standing up on the ground drops to walking speed.
      ClampFastMorphGroundVelocity(dt);
    } else {
      // In the air the whole arc carries over. A falling ball is FallingMorphed,
      // which has ground friction; the standing player falls in ApplyJump.
      if (x258_movementState == NPlayer::kMS_FallingMorphed) {
        SetMoveState(NPlayer::kMS_ApplyJump, mgr);
      }
      sFastMorphAirCarry = true;
    }
  }
#endif
  SetCameraState(kCS_Transitioning, mgr);
  x824_transitionFilterTimer = .01f;
  x57c_ = 0;
  x580_ = 0;
#ifdef TARGET_PC
  // A fast unmorph cuts straight to first person behind the transition flash,
  // rather than flying the camera into a sped-up stand-up animation.
  const bool immediate =
      IsFastMorphTransition() ||
      !mgr.CameraManager()->BallCamera()->TransitionFromMorphBallState(mgr);
#else
  const bool immediate = !mgr.CameraManager()->BallCamera()->TransitionFromMorphBallState(mgr);
#endif
  if (immediate) {
    x824_transitionFilterTimer = .95f;
#ifdef TARGET_PC
    if (IsFastMorphTransition() && HasAnimation()) {
      // Play the skipped stand-up animation (retail's 1 s unmorph) out at once
      // for its sound events.
      UpdateAnimation(1.f, mgr, true);
    }
#endif
    LeaveMorphBallState(mgr);
  }
}

void CPlayer::ActivateMorphBallCamera(CStateManager& mgr) {
  SetCameraState(kCS_Ball, mgr);
  mgr.CameraManager()->BallCamera()->SetState(CBallCamera::kBCS_Default, mgr);
}

void CPlayer::EnterMorphBallState(CStateManager& mgr) {
  SetMorphBallState(kMS_Morphed, mgr);
  RemoveMaterial(kMT_GroundCollider, mgr);
  x730_transitionModels.clear();
  SetAngularVelocityOR(CAxisAngle::FromVector(CVector3f(
      -GetVelocityWR().Magnitude() / gpTweakPlayer->GetPlayerBallHalfExtent(), 0.f, 0.f)));
  x768_morphball->EnterMorphBallState(mgr);
  x768_morphball->TakeDamage(-1.f);
  x768_morphball->SetDamageTimer(0.f);
  mgr.PlayerState()->StartTransitionToVisor(CPlayerState::kPV_Combat);
}

void CPlayer::LeaveMorphBallState(CStateManager& mgr) {
#ifdef TARGET_PC
  // Only the end of a Fast Morph unmorph keeps velocity, not a scripted one.
  const bool keepVelocity =
      x2f8_morphBallState == kMS_Unmorphing && IsFastMorphTransition();
#endif
  x730_transitionModels.clear();
  AddMaterial(kMT_GroundCollider, mgr);
  SetMomentumWR(CVector3f::Zero());
  SetMorphBallState(kMS_Unmorphed, mgr);
  SetHudDisable(FLT_EPSILON, 0.f, 2.f);
  SetHudDisable(FLT_EPSILON, 0.f, 2.f);
  SetIntoBallReadyAnimation(mgr);
#ifdef TARGET_PC
  if (keepVelocity) {
    ClearForcesAndTorques();
    SetAngularVelocityWR(CAxisAngle::Identity());
  } else
#endif
  Stop();
  x3e4_freeLookYawAngle = 0.f;
  x3e8_horizFreeLookAngleVel = 0.f;
  x3ec_freeLookPitchAngle = 0.f;
  x3f0_vertFreeLookAngleVel = 0.f;
  x768_morphball->LeaveMorphBallState(mgr);
  mgr.CameraManager()->SetPlayerCamera(
      mgr, mgr.GetCameraManager()->GetFirstPersonCamera()->GetUniqueId());
  mgr.CameraManager()->BallCamera()->SetState(CBallCamera::kBCS_Default, mgr);
  SetCameraState(kCS_FirstPerson, mgr);
  mgr.CameraManager()->FirstPersonCamera()->DeferBallTransitionProcessing();
  mgr.CameraManager()->FirstPersonCamera()->Think(0.f, mgr);
  ForceGunOrientation(GetTransform(), mgr);
  DrawGun(mgr);
}

void CPlayer::InitialiseAnimation() {
  if (HasAnimation()) {
    AnimationData()->SetAnimation(CAnimPlaybackParms(2, -1, 1.f, true), false);
  }
}

void CPlayer::UpdateTransitionFilter(float dt, CStateManager& mgr) {
  CCameraFilterPass& filter = mgr.CameraFilterPass(CStateManager::kCFS_Eight);
  if (x824_transitionFilterTimer <= 0.f) {
    filter.DisableFilter(0.f);
    return;
  }
  x824_transitionFilterTimer += dt;
  if (x824_transitionFilterTimer > 1.25f) {
    x824_transitionFilterTimer = 0.f;
    filter.DisableFilter(0.f);
    return;
  }
  if (x824_transitionFilterTimer < .95f) {
    return;
  }
  const float time = x824_transitionFilterTimer - .95f;
  CColor color(static_cast< uchar >(255), static_cast< uchar >(223), static_cast< uchar >(137));
  if (time < .1f) {
    color = color.WithAlphaOf(.3f * time / .1f);
  } else if (time >= .15f) {
    color = color.WithAlphaOf(.3f * (1.f - CMath::Limit((time - .15f) / .15f, 1.f)));
  } else {
    color = color.WithAlphaOf(.3f);
  }
  filter.SetFilter(CCameraFilterPass::kFT_Add, CCameraFilterPass::kFS_ScanLinesEven, 0.f, color,
                   kInvalidAssetId);
}

void CPlayer::UpdateMorphBallTransition(float dt, CStateManager& mgr) {
  const EPlayerMorphBallState morphState = x2f8_morphBallState;
  if (morphState != kMS_Morphing && morphState != kMS_Unmorphing) {
    CPlayerState::EPlayerSuit suit = mgr.GetPlayerState()->GetCurrentSuitRaw();
    if (mgr.GetPlayerState()->GetIsFusionEnabled()) {
      suit = static_cast< CPlayerState::EPlayerSuit >(suit + 4);
    }
    if (x7cc_transitionSuit != suit) {
      x7cc_transitionSuit = suit;
      const bool canLoop = x7d0_animRes.CanLoop();
      const CAnimRes res(x7d0_animRes.GetId(), x7cc_transitionSuit, x7d0_animRes.GetScale(),
                         x7d0_animRes.GetDefaultAnim(), canLoop);
      SetModelData(CModelData(res));
      SetIntoBallReadyAnimation(mgr);
    }
    return;
  }
  switch (morphState) {
  case kMS_Unmorphing: {
    CAnimData& animData = *AnimationData();
    if (x584_ballTransitionAnim == 14) {
      if (animData.GetAnimTimeRemaining(rstl::string_l("Whole Body")) /
              animData.GetAnimationDuration(x584_ballTransitionAnim) <
          .5f) {
        bool loop = false;
        x584_ballTransitionAnim = GetNextBallTransitionAnim(dt, loop, mgr);
        if (HasAnimation()) {
          const CAnimPlaybackParms parms(x584_ballTransitionAnim, -1, 1.f, true);
          animData.SetAnimation(parms, false);
          animData.EnableLooping(loop);
        }
      }
    } else if (x584_ballTransitionAnim != 5 && x584_ballTransitionAnim != 7) {
      const float maxSpeed = GetActualFirstPersonMaxVelocity(dt);
      const CVector2f velocity(GetVelocityWR().GetX(), GetVelocityWR().GetY());
      const float speed = velocity.Magnitude();
      if (fabsf(x58c_transitionVel - speed) > .4f * maxSpeed || speed < 1.f) {
        bool loop = false;
        const int nextAnim = GetNextBallTransitionAnim(dt, loop, mgr);
        if (HasAnimation() && x584_ballTransitionAnim != nextAnim && x584_ballTransitionAnim != 7) {
          x584_ballTransitionAnim = nextAnim;
          const CAnimPlaybackParms parms(x584_ballTransitionAnim, -1, 1.f, true);
          animData.SetAnimation(parms, false);
          animData.EnableLooping(loop);
          x58c_transitionVel = speed;
        }
      }
    }
    break;
  }
  default:
    break;
  }
#ifdef TARGET_PC
  // A fast morph runs the (hidden) animation at the transition's pace, so its
  // sound events still play, and the kept velocity moves the player rather
  // than the animation's root motion.
  const bool fastMorph = IsFastMorphTransition();
  const CAdvancementDeltas deltas =
      UpdateAnimation(fastMorph ? dt / x578_morphDuration : dt, mgr, true);
  if (!fastMorph) {
    MoveInOneFrameOR(deltas.GetOffsetDelta(), dt);
  }
#else
  const CAdvancementDeltas deltas = UpdateAnimation(dt, mgr, true);
  MoveInOneFrameOR(deltas.GetOffsetDelta(), dt);
#endif
  RotateInOneFrameOR(deltas.GetOrientationDelta(), dt);
  x574_morphTime = rstl::min_val(x578_morphDuration, x574_morphTime + dt);
  const float morphT = x574_morphTime / x578_morphDuration;
  if ((!(morphT < .7f) || !(x574_morphTime > 2.f * dt)) && !x730_transitionModels.empty()) {
    x730_transitionModels.erase(x730_transitionModels.begin());
  }
  for (int i = 0; i < x730_transitionModels.size(); ++i) {
    x730_transitionModels[i]->AdvanceAnimation(dt, mgr, kInvalidAreaId, true);
  }
  const CGameCamera& camera = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  x588_alpha = GetTransitionAlpha(camera.GetTranslation(), camera.GetNearClipDistance());
  if (morphState == kMS_Morphing && morphT > .93f) {
    x588_alpha *= rstl::min_val(1.f - (morphT - .93f) / (1.f - .93f) + .2f, 1.f);
    SetModelFlags(CModelFlags::AlphaBlended(x588_alpha).DepthCompareUpdate(true, false));
  } else if (morphState == kMS_Unmorphing && x588_alpha < 1.f) {
    if (x588_alpha > .05f) {
      const CModelFlags& flags =
          CModelFlags::AlphaBlended(x588_alpha).DepthCompareUpdate(true, false);
      SetModelFlags(CModelFlags(flags, flags.GetOtherFlags() | CModelFlags::kF_DrawNormal));
    } else {
      SetModelFlags(CModelFlags::AlphaBlended(x588_alpha).DepthCompareUpdate(true, false));
    }
  } else {
    SetModelFlags(CModelFlags::AlphaBlended(x588_alpha).DepthCompareUpdate(true, true));
  }
  x594_transisionBeamXfs.AddValue(x7f4_gunWorldXf);
  x658_transitionModelXfs.AddValue(GetTransform());
  x71c_transitionModelAlphas.AddValue(x588_alpha);
  switch (morphState) {
  case kMS_Unmorphing: {
    const CAABox bounds = GetCollisionPrimitive()->CalculateAABox(GetPrimitiveTransform());
    bounds.GetCenterPoint();
    ClearForcesAndTorques();
    SetAngularVelocityWR(CAxisAngle::Identity());
    bool cinematic = false;
    if (mgr.GetCameraManager()->IsInCinematicCamera()) {
      cinematic = true;
    }
    if (x574_morphTime >= x578_morphDuration || cinematic) {
      x824_transitionFilterTimer =
          rstl::max_val(x824_transitionFilterTimer, skTransitionFilterTime);
      CVector3f pos = CVector3f::Zero();
      if (CanLeaveMorphBallState(mgr, pos)) {
        SetTranslation(GetTranslation() + pos);
        LeaveMorphBallState(mgr);
        SetModelFlags(CModelFlags::Normal());
      } else {
        x574_morphTime = x578_morphDuration - x574_morphTime;
        TransitionToMorphBallState(dt, mgr);
      }
    }
    break;
  }
  case kMS_Morphing: {
    ClearForcesAndTorques();
    SetAngularVelocityWR(CAxisAngle::Identity());
    bool cinematic = false;
    if (mgr.GetCameraManager()->IsInCinematicCamera()) {
      cinematic = true;
    }
    if (x574_morphTime >= x578_morphDuration || cinematic) {
      if (CanEnterMorphBallState(mgr, 1.f)) {
        ActivateMorphBallCamera(mgr);
        EnterMorphBallState(mgr);
        SetModelFlags(CModelFlags::Normal());
      } else {
        x574_morphTime = x578_morphDuration - x574_morphTime;
        TransitionFromMorphBallState(dt, mgr);
      }
    }
    if (GetMorphBallTransitionFactor() >= .5f &&
#ifdef TARGET_PC
        !IsFastMorphTransition() &&
#endif
        !x768_morphball->IsMorphBallTransitionFlashValid()) {
      x768_morphball->ResetMorphBallTransitionFlash();
    }
    break;
  }
  default:
    break;
  }
}
