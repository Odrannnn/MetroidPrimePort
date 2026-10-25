#pragma once

#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

// UI_HUDMPR1.ROOM inventory lighting. These are lighting parameters, not textures.
// Scene consumers: 0xfa1428; direction: 0x2e0744 (entity local +Z).
namespace PortPauseLighting {
inline bool active = false;
inline bool doll = false;
inline bool phazon = false;

// Remastered queues CGuiTextPane to StoreVisorHUDCommand (0xbde370),
// consumed after pause post and helmet by RenderHUD (0xfa0dac).
class TextPass {
  inline static TextPass* current = nullptr;
  TextPass* previous;
  std::vector<std::function<void()>> draws;
public:
  explicit TextPass(bool enabled) : previous(current) { current = enabled ? this : nullptr; }
  ~TextPass() { current = previous; }
  TextPass(const TextPass&) = delete;
  TextPass& operator=(const TextPass&) = delete;
  static bool Deferring() { return current != nullptr; }
  static void Queue(std::function<void()> draw) { current->draws.push_back(std::move(draw)); }
  void Flush() {
    // End collection as well as preventing recursive deferral: later save/UI
    // draws in this frame must render immediately after this one-shot flush.
    current = nullptr;
    auto pending = std::move(draws);
    draws.clear();
    for (const auto& draw : pending) draw();
  }
};

struct Scope {
  bool previous;
  bool& flag;
  explicit Scope(bool& value, bool on = true) : previous(value), flag(value) { flag = on; }
  ~Scope() { flag = previous; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline float FloatBits(unsigned bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void Bloom(float tints[5][3]) {
  constexpr unsigned bits[5] = {0x3ed0d0d1, 0x3f3bbbbc, 0x3f800000, 0x3f800000, 0x3f05c3f9};
  for (int i = 0; i < 5; ++i)
    for (int c = 0; c < 3; ++c)
      tints[i][c] = FloatBits(bits[i]);
}

// LightGather uses the UI perspective camera, not GetViewToWorldPauseScreenModel
// (0xfa1428 -> 0x3ad3cc). Its only CameraHint behavior copies entity rotation
// (0x4ec828); UI_HUDMPR1 #111 is Rz(180) * Ry(7.016477463194512e-15) * Rx(180).
// CreateNativeWorldToView (0x12df54) takes -column0, +column1, -column2.
// Express those rows in the port's (-x,z,y) world basis. Keep the authored
// angles rather than substituting an idealized identity in native space.
inline void LightView(float out[3][4]) {
  constexpr double pi = 3.14159265358979323846;
  const double x = 180. * pi / 180., y = 7.016477463194512e-15 * pi / 180.,
               z = 180. * pi / 180.;
  const double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y),
               sz = std::sin(z), cz = std::cos(z);
  const double columns[3][3] = {
      {cz * cy, sz * cy, -sy},
      {cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx},
      {cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx}};
  for (int row = 0; row < 3; ++row) {
    const double sign = row == 1 ? 1. : -1.;
    out[row][0] = float(-sign * columns[row][0]);
    out[row][1] = float(sign * columns[row][2]);
    out[row][2] = float(sign * columns[row][1]);
    out[row][3] = 0.f;
  }
}

// Three 16-float GX room-light records. The quaternion/entity convention is
// Rz * Ry * Rx; convert Remastered axes to retail (-x,z,y), then to view.
inline void Lights(const float view[3][4], bool phazonSuit, float out[48]) {
  constexpr float angles[3][3] = {
      {-161.19400024414062f, 59.63199996948242f, 175.1790008544922f},
      {-41.573001861572266f, -19.958999633789062f, 106.84100341796875f},
      {-22.805999755859375f, -81.56700134277344f, 0.f}};
  constexpr unsigned colors[3][3] = {
      {0x3f800000, 0x3f7e74b1, 0x3f7481bd},
      {0x3f800000, 0x3e2b50a6, 0x3c2436cb},
      {0x3f1a946e, 0x3f753751, 0x3f800000}};
  const float intensity[3] = {phazonSuit ? 2.f : 2.5f, 10.f, 0.25f};
  constexpr double pi = 3.14159265358979323846;
  std::memset(out, 0, 48 * sizeof(float));
  for (int i = 0; i < 3; ++i) {
    const double x = angles[i][0] * pi / 180., y = angles[i][1] * pi / 180.,
                 z = angles[i][2] * pi / 180.;
    const double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y),
                 sz = std::sin(z), cz = std::cos(z);
    // Negative local +Z, with the basis conversion applied.
    const double toLight[3] = {cz * sy * cx + sz * sx, -cy * cx,
                              -sz * sy * cx + cz * sx};
    for (int c = 0; c < 3; ++c) {
      out[i * 16 + 4 + c] = FloatBits(colors[i][c]) * intensity[i] / float(pi);
      out[i * 16 + 8 + c] = float(view[c][0] * toLight[0] + view[c][1] * toLight[1] +
                                  view[c][2] * toLight[2]);
    }
    out[i * 16 + 14] = 2.f;
  }
}
} // namespace PortPauseLighting
