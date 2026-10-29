#pragma once

class CFinalInput;
class CStateManager;
class CTransform4f;

// A debug camera that leaves the player where they are: the world is drawn from a
// viewpoint of its own, flown with the pad, and the HUD and the arm cannon are left out.
// F1 > Debug and the console's `freecam` both drive it.
namespace PortFreeCam {

bool Active();
// Starts at the game camera's place and direction.
void SetActive(bool on, const CStateManager* mgr);
// The game's simulation stands still while the camera flies.
bool Frozen();
void SetFrozen(bool frozen);
// Metres per second at a full stick; the right trigger multiplies it by four.
float Speed();
void SetSpeed(float speed);
// Samus's body is drawn where the player stands (on by default); the game leaves it
// out in first person. True only while the camera is on.
bool ShowPlayer();
void SetShowPlayer(bool show);
// The pad drives Samus and the camera holds still (off by default), for watching her move
// from a fixed viewpoint. True only while the camera is on.
bool Drive();
void SetDrive(bool drive);

struct Pose {
  float x, y, z;
  float yaw;   // degrees about world up, 0 looking along +Y
  float pitch; // degrees, up positive
  float roll = 0.f; // degrees about the view axis; movement ignores it
};
Pose GetPose();
// snap false: a step taken in this tick's input, blended over the tick like the
// camera's own moves; true (default): a jump.
void SetPose(const Pose& pose, bool snap = true);

// A vertical field of view in degrees that replaces the game camera's while the camera is on
// (photo mode); 0 leaves the game's. FovFor widens it to the game's aspect like the camera does.
float FovOverride();
// The override for the frame being drawn, blended between ticks like the view.
float PresentedFovOverride();
void SetFovOverride(float degrees, bool snap = true);

// Touch (photo mode on a phone): a stick in [-1, 1] added to the left stick until changed
// (y is ahead), and a turn in degrees taken at the next tick.
void SetTouchMove(float x, float y);
void AddTouchTurn(float yaw, float pitch);

// Once per tick with the pad's input: left stick moves, C stick turns, R is fast,
// L and Z (or the d-pad's up and down) go down and up. Returns true when the input was
// taken and the player must not see it.
bool Input(const CFinalInput& input);
// The view the world is drawn from, blended between ticks; `game` when the camera is off (or
// the third-person camera's view of it, port_third_person.h).
CTransform4f View(const CTransform4f& game);

} // namespace PortFreeCam
