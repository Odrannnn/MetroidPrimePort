#pragma once

// Decision rules for presented (between-tick) transforms, kept free of game
// types so tests can pin them (docs/FRAME_INTERPOLATION.md, issue #70).

// A tick snapshot is only valid in the tick that wrote it. A tick that skips
// the writer (the camera manager's Update in a soft pause, or any pause) must
// not replay the previous pair: it would sweep prev -> cur again on every tick
// while the sim value sits still, which shows as flicker.
inline bool PortSnapshotFresh(unsigned snapshotGeneration, unsigned tickGeneration) {
  return tickGeneration != 0 && snapshotGeneration == tickGeneration;
}

enum class PortRigidBlendKind {
  Cut,           // draw at the sim transform
  TranslateOnly, // centre lerps, rotation takes the sim value
  Full,          // centre lerps, rotation slerps
};

// Shared snap rule. `dist2` = squared centre distance between the two ticks,
// `rotDot` = |dot| of their unit quaternions. A teleport (over 4 units) always
// cuts. A turn over 45 degrees cuts an ordinary actor; a pivoted body (the
// rolling morph ball) keeps interpolating its centre, because its rotation is
// its roll and spinning past 45 degrees a tick must not drop the translation.
inline PortRigidBlendKind PortClassifyRigidBlend(float dist2, float rotDot, bool pivoted) {
  if (dist2 > 16.f)
    return PortRigidBlendKind::Cut;
  if (rotDot < 0.9238795f)
    return pivoted ? PortRigidBlendKind::TranslateOnly : PortRigidBlendKind::Cut;
  return PortRigidBlendKind::Full;
}

// The blend itself, shared by CActor::PortBlendRigid and the tests. `Ops`
// supplies the math: Rigid(xf, rot) (orthonormal form of xf, rotation out),
// Translation(xf), SetTranslation(xf, v), Dot(q, q), Slerp(q, q, t) and
// Build(q, centre). `pivot` is a world offset from the actor origin to its
// visual centre; that centre lerps and the turn happens about it.
template <class Xf, class Vec, class Quat, class Ops>
bool PortBlendRigidGeneric(const Ops& ops, const Xf& from, const Xf& to, float t, Xf& blend,
                           Xf& cur, const Vec& pivot, bool pivoted) {
  const Vec prevPos = ops.Translation(from) + pivot;
  const Vec curPos = ops.Translation(to) + pivot;
  Quat prevRot, curRot;
  ops.Rigid(from, prevRot);
  cur = ops.Rigid(to, curRot);
  ops.SetTranslation(cur, curPos);
  const PortRigidBlendKind kind =
      PortClassifyRigidBlend((curPos - prevPos).MagSquared(), ops.AbsDot(prevRot, curRot), pivoted);
  if (kind == PortRigidBlendKind::Cut)
    return false;
  const Vec centre = prevPos + (curPos - prevPos) * t;
  blend = ops.Build(kind == PortRigidBlendKind::Full ? ops.Slerp(prevRot, curRot, t) : curRot,
                    centre);
  return true;
}
