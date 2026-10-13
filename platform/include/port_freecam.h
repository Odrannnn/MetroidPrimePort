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

struct Pose {
  float x, y, z;
  float yaw;   // degrees about world up, 0 looking along +Y
  float pitch; // degrees, up positive
  float roll = 0.f; // degrees about the view axis; movement ignores it
};
Pose GetPose();
void SetPose(const Pose& pose);

// A vertical field of view in degrees that replaces the game camera's while the camera is on
// (photo mode); 0 leaves the game's. FovFor widens it to the game's aspect like the camera does.
float FovOverride();
void SetFovOverride(float degrees);

// Touch (photo mode on a phone): a stick in [-1, 1] added to the left stick until changed
// (y is ahead), and a turn in degrees taken at the next tick.
void SetTouchMove(float x, float y);
void AddTouchTurn(float yaw, float pitch);

// Once per tick with the pad's input: left stick moves, C stick turns, R is fast,
// L and Z (or the d-pad's up and down) go down and up. Returns true when the input was
// taken and the player must not see it.
bool Input(const CFinalInput& input);
// The view the world is drawn from; `game` when the camera is off.
CTransform4f View(const CTransform4f& game);

} // namespace PortFreeCam
