// The room environment's script-driven state: fog region transitions, dynamic lights and the
// grade, backlight and fog hints the area's scripts turn on and off. See
// port_room_env_internal.h.

#include "port_room_env_internal.h"

namespace PortRoomEnv {
namespace EnvDetail {

// What `sender` drives on `state` among a kind of hints (grades or backlights).
template <class Hint>
void DriveHints(uint32_t mrea, const char* what, const std::vector<Hint>& hints,
                std::vector<Area::GradeRequest>& requests, uint64_t& orderCounter, uint32_t sender, int state) {
  for (size_t i = 0; i < requests.size() && i < hints.size(); ++i) {
    for (const GradeLink& link : hints[i].links) {
      if (link.sender != sender || link.state != state) {
        continue;
      }
      Area::GradeRequest& request = requests[i];
      const bool on = link.action == PortRoomGeo::kShow ? true
                      : link.action == PortRoomGeo::kHide ? false
                      : link.action == PortRoomGeo::kToggle ? !request.on
                                                             : request.on;
      if (on && !request.on) {
        // Above every file-order start (StartGrades).
        request.order = (uint64_t(1) << 32) + ++orderCounter;
      }
      if (on != request.on) {
        PortLog::Write("room env: %08X %s %zu %s by %08X state %d\n", mrea, what, i, on ? "on" : "off", sender, state);
      }
      request.on = on;
    }
  }
}

// OnAction_Start: plays on from where it is, from the region's state now (GetTransitionState:
// distance and transmittance always, colour and cap unsigned and only when the region has them).
void StartTransition(Area& area, size_t i) {
  Area::Transition& t = area.transitions[i];
  const FogTransition& file = area.file.transitions[i];
  t.playing = true;
  t.hasRegion = file.region < area.live.size();
  t.hasColor = false;
  t.hasCap = false;
  if (!t.hasRegion) {
    return;
  }
  const FogRegion& region = area.live[file.region];
  t.distance = region.distance;
  t.transmittance = region.transmittance;
  t.hasColor = region.hasColor;
  for (int c = 0; c < 4; ++c) {
    t.color[c] = std::fabs(region.color[c]);
  }
  t.hasCap = region.hasCap;
  t.cap = std::fabs(region.cap);
}

// The transitions `sender` drives on `state` (AcceptScriptMsg).
void DriveTransitions(uint32_t mrea, Area& area, uint32_t sender, int state) {
  for (size_t i = 0; i < area.transitions.size() && i < area.file.transitions.size(); ++i) {
    Area::Transition& t = area.transitions[i];
    for (const GradeLink& link : area.file.transitions[i].links) {
      if (link.sender != sender || link.state != state || t.dead) {
        continue;
      }
      switch (link.action) {
      case PortRoomGeo::kShow:
      case PortRoomGeo::kHide:
        t.active = link.action == PortRoomGeo::kShow;
        break;
      case kTransitionStart:
        if (t.active) {
          StartTransition(area, i);
        }
        break;
      case kTransitionRestart:
        if (t.active) {
          t.time = 0.;
          StartTransition(area, i);
        }
        break;
      case kTransitionStop:
        if (t.active) {
          t.playing = false;
        }
        break;
      case kTransitionDelete:
        t.dead = true;
        t.playing = false;
        break;
      default:
        continue;
      }
      PortLog::Write("room env: %08X fog transition %zu action %d by %08X state %d\n", mrea, i, int(link.action),
                     sender, state);
    }
  }
}

// CTimePlaybackManager's time step: to 1/60000 s.
double QuantiseTime(double t) { return std::floor(t * 60000. + 0.5) / 60000.; }

// Think: one frame of every playing transition, which leaves its region in the new state.
void StepTransitions(Area& area, float dt) {
  for (size_t i = 0; i < area.transitions.size() && i < area.file.transitions.size(); ++i) {
    Area::Transition& t = area.transitions[i];
    const FogTransition& file = area.file.transitions[i];
    if (!t.playing || !t.active || t.dead || file.region >= area.live.size()) {
      continue;
    }
    FogRegion& region = area.live[file.region];
    if (region.layer >= 0 && region.layer < 64 && (area.layers >> region.layer & 1) == 0) {
      continue;
    }
    const double first = file.phase.FirstTime();
    const double last = file.phase.LastTime();
    if (file.loop) {
      t.time += dt;
      if (t.time > last) {
        t.time -= last - first;
      }
      t.time = QuantiseTime(t.time);
    } else {
      t.time = QuantiseTime(t.time + dt);
      if (t.time >= last) {
        t.time = last;
        t.playing = false;
      }
    }
    const float p = std::clamp(file.phase.Eval(float(t.time)), 0.f, 1.f);
    const auto lerp = [p](float a, float b) { return a + (b - a) * p; };
    // A field the start and the target both have blends; one only either has is that one.
    const bool targetD = (file.select & 1) != 0;
    const bool targetT = (file.select & 2) != 0;
    const bool targetColor = (file.select & 4) != 0;
    const bool targetCap = (file.select & 8) != 0;
    const bool hasD = t.hasRegion || targetD;
    const bool hasT = t.hasRegion || targetT;
    const float d = t.hasRegion && targetD ? lerp(t.distance, file.distance) : targetD ? file.distance : t.distance;
    const float tr = t.hasRegion && targetT ? lerp(t.transmittance, file.transmittance)
                     : targetT                 ? file.transmittance
                                               : t.transmittance;
    // SetTransitionState: the signs come back from the region's mode.
    const float s = region.subtract ? -1.f : 1.f;
    // (A transmittance of 0 or a distance of 0 would make it infinite: kept as it was.)
    if (hasD && hasT && std::isfinite(-std::log(tr) / d)) {
      region.distance = d;
      region.transmittance = tr;
      region.density = s * (-std::log(tr) / d);
    }
    if (t.hasColor || targetColor) {
      for (int c = 0; c < 4; ++c) {
        const float v = t.hasColor && targetColor ? lerp(t.color[c], file.color[c])
                        : targetColor             ? file.color[c]
                                                  : t.color[c];
        region.color[c] = c < 3 ? v * s : v;
      }
      region.hasColor = true;
    }
    if (t.hasCap || targetCap) {
      const float cap = t.hasCap && targetCap ? lerp(t.cap, file.cap) : targetCap ? file.cap : t.cap;
      region.cap = s * cap;
      region.hasCap = true;
    }
  }
}

// The lights `sender` drives on `state` (AcceptScriptMsg).
void DriveLights(uint32_t mrea, Area& area, uint32_t sender, int state) {
  for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
    Area::Light& l = area.lights[i];
    for (const GradeLink& link : area.file.lights[i].links) {
      if (link.sender != sender || link.state != state) {
        continue;
      }
      switch (link.action) {
      case PortRoomGeo::kShow:
      case PortRoomGeo::kHide:
        l.active = link.action == PortRoomGeo::kShow;
        break;
      case PortRoomGeo::kToggle:
        l.active = !l.active;
        break;
      case kLightStart:
      case kLightStop:
        if (l.active) {
          l.playing = link.action == kLightStart;
        }
        break;
      case kLightReset:
        if (l.active) {
          l.time = 0.;
        }
        break;
      case kLightForward:
      case kLightBackward:
        if (l.active) {
          l.forward = link.action == kLightForward;
        }
        break;
      case kLightReverse:
        if (l.active) {
          l.forward = !l.forward;
        }
        break;
      default:
        continue;
      }
      PortLog::Write("room env: %08X light %zu action %d by %08X state %d\n", mrea, i, int(link.action), sender,
                     state);
    }
  }
}

// Think: the script groups' word on each light, then one frame of every playing timeline
// (CTimePlaybackManager::Update). One that ends without a loop stops there, and switches its
// light off when its finished event is linked to do so.
void StepLights(uint32_t mrea, Area& area, float dt) {
  for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
    Area::Light& l = area.lights[i];
    const PointLight& file = area.file.lights[i];
    if (file.group != PortRoomGeo::kNoGroup) {
      const int shown = PortRoomGeo::GroupShown(mrea, file.group);
      if (shown >= 0 && shown != l.group) {
        l.active = shown == 1;
      }
      l.group = shown;
    }
    if (!file.animated || !l.playing || !l.active ||
        (file.layer >= 0 && file.layer < 64 && (area.layers >> file.layer & 1) == 0)) {
      continue;
    }
    const double length = file.length;
    bool ended = false;
    if (l.forward) {
      l.time = QuantiseTime(l.time + dt);
      if (l.time > length) {
        if (file.loop && length > 0.) {
          l.time = QuantiseTime(std::fmod(l.time, length));
        } else {
          l.time = length;
          ended = true;
        }
      } else if (l.time == length && !file.loop) {
        ended = true;
      }
    } else {
      l.time = QuantiseTime(l.time - dt);
      if (l.time < 0.) {
        if (file.loop && length > 0.) {
          l.time = QuantiseTime(length - std::fmod(-l.time, length));
        } else {
          l.time = 0.;
          ended = true;
        }
      } else if (l.time == 0. && !file.loop) {
        ended = true;
      }
    }
    if (ended) {
      l.playing = false;
      // Only the backward end (t = 0) sends the event these links answer; the forward end's is unlinked.
      if (file.offAtStart && !l.forward) {
        l.active = false;
      }
    }
  }
}

// The grades and backlights `sender` drives on `state`.
void DriveGrades(uint32_t mrea, Area& area, uint32_t sender, int state) {
  DriveHints(mrea, "grade", area.file.grades, area.grades, sGradeOrder, sender, state);
  DriveHints(mrea, "backlight", area.file.backlights, area.backlights, sBacklightOrder, sender, state);
  DriveHints(mrea, "fog", area.file.fogs, area.fogs, sFogOrder, sender, state);
  DriveHints(mrea, "fog region", area.file.regions, area.regions, sRegionOrder, sender, state);
  DriveTransitions(mrea, area, sender, state);
  DriveLights(mrea, area, sender, state);
  if (area.hints.Drive(area.file.fluidHints, sender, state)) {
    PortLog::Write("room env: %08X exposure hints driven by %08X state %d, pick %d\n", mrea, sender, state,
                   area.hints.Pick(area.file.fluidHints.size()));
  }
}

void DriveCameraWaterItems(uint32_t mrea, Area& area, uint64_t items) {
  for (uint32_t k = 0; k < kCameraWaterItems; ++k) {
    DriveGrades(mrea, area, kSenderCameraWaterItem + k, (items >> k & 1) != 0 ? 0 : 1);
  }
}

// Every grade and backlight as it starts, then the fluids the player and camera are in already.
void StartGrades(uint32_t mrea, Area& area, bool playerFluid, bool cameraWater) {
  // The exposure hints' entities as they load (their registrations follow).
  if (area.hints.on.size() == area.file.fluidHints.size()) {
    bool changed = false;
    for (size_t i = 0; i < area.hints.on.size(); ++i) {
      const uint8_t on = area.file.fluidHints[i].on ? 1 : 0;
      changed |= area.hints.on[i] != on;
      area.hints.on[i] = on;
    }
    if (changed) {
      area.hints.Update(area.file.fluidHints, false);
    }
  }
  area.grades.assign(area.file.grades.size(), {});
  for (size_t i = 0; i < area.grades.size(); ++i) {
    area.grades[i].on = area.file.grades[i].on;
    // In file order, below anything turned on later: the last of equals wins, as before.
    area.grades[i].order = i;
  }
  area.backlights.assign(area.file.backlights.size(), {});
  for (size_t i = 0; i < area.backlights.size(); ++i) {
    area.backlights[i].on = area.file.backlights[i].on;
    area.backlights[i].order = i;
  }
  area.fogs.assign(area.file.fogs.size(), {});
  for (size_t i = 0; i < area.fogs.size(); ++i) {
    area.fogs[i].on = area.file.fogs[i].on;
    area.fogs[i].order = i;
  }
  area.regions.assign(area.file.regions.size(), {});
  for (size_t i = 0; i < area.regions.size(); ++i) {
    area.regions[i].on = area.file.regions[i].on;
  }
  area.live = area.file.regions;
  area.lights.assign(area.file.lights.size(), {});
  for (size_t i = 0; i < area.lights.size(); ++i) {
    area.lights[i].active = area.file.lights[i].on;
    area.lights[i].playing = area.file.lights[i].playing;
  }
  area.transitions.assign(area.file.transitions.size(), {});
  for (size_t i = 0; i < area.transitions.size(); ++i) {
    area.transitions[i].active = area.file.transitions[i].on;
    if (area.file.transitions[i].autoStart) {
      StartTransition(area, i);
    }
  }
  if (playerFluid) {
    DriveGrades(mrea, area, kSenderPlayerFluid, 0);
  }
  if (cameraWater) {
    DriveGrades(mrea, area, kSenderCameraWater, 0);
    DriveCameraWaterItems(mrea, area, sItems);
  }
}

// A file that loads takes the world volume table from its file (a new one, from another world,
// starts the camera outside every volume), then hears the volumes entered and left so far.
void ReplayWorldVolumes(uint32_t mrea, Area& area) {
  // A file before version 22 has none: an old room of another world still drops this one's.
  if (!SameWorldVolumes(area.file.worldVolumes, sWorldVolumes)) {
    sWorldVolumes = area.file.worldVolumes;
    sWorldVolumeState.Clear();
    return;
  }
  std::vector<WorldVolumeState::Event> events;
  sWorldVolumeState.Replay(events);
  for (const WorldVolumeState::Event& e : events) {
    DriveGrades(mrea, area, kSenderWorldVolume + e.volume, e.state);
  }
}

}  // namespace EnvDetail
}  // namespace PortRoomEnv
