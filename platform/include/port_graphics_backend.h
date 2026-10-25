#ifndef METROID_PRIME_PORT_PORT_GRAPHICS_BACKEND_H
#define METROID_PRIME_PORT_PORT_GRAPHICS_BACKEND_H

#include "aurora/aurora.h"
#include "port_env.h"
#include "port_debug.h"

#include <span>

// The renderer choice (issue #135), in one place: the mapping from the saved
// selection to Aurora's backends, which choices the build offers, the per-run
// MP_GRAPHICS_BACKEND override, and what the F1 Video page and the startup log say
// about all of it. main.cpp asks Aurora for ToAurora(Resolve(...).backend) and never
// maps a selection itself; the Video page draws Status. The functions are pure so
// tests/port_graphics_backend.cpp can drive every case without a window, a GPU or a
// settings file.
namespace PortGraphicsBackend {

// The choices this build offers, in the order the F1 Video page lists them. An API
// with no Aurora backend here is not offered (no Direct3D outside Windows; OpenGL on
// desktop Linux through EGL, on Windows through Dawn's EGL-on-WGL shim); Android keeps
// its `opengles` toggle and offers nothing from this setting.
inline std::span<const PortDebug::EGraphicsBackend> Choices() {
#if defined(_WIN32)
  static constexpr PortDebug::EGraphicsBackend kChoices[] = {
      PortDebug::kGraphicsBackend_Auto, PortDebug::kGraphicsBackend_D3D12,
      PortDebug::kGraphicsBackend_Vulkan, PortDebug::kGraphicsBackend_D3D11,
      PortDebug::kGraphicsBackend_OpenGL};
#elif defined(__linux__) && !defined(__ANDROID__)
  static constexpr PortDebug::EGraphicsBackend kChoices[] = {
      PortDebug::kGraphicsBackend_Auto, PortDebug::kGraphicsBackend_Vulkan,
      PortDebug::kGraphicsBackend_OpenGL};
#else
  static constexpr PortDebug::EGraphicsBackend kChoices[] = {PortDebug::kGraphicsBackend_Auto};
#endif
  return kChoices;
}

// What this run asked Aurora for: the saved selection, or MP_GRAPHICS_BACKEND when it
// names one. `env` is that variable's value (nullptr when unset). Only `choices` (this
// build's, by default) count: an override naming another platform's API is rejected,
// and a saved one (a settings file from another platform) asks for Auto, which is
// also what the Video page shows for it.
struct Request {
  PortDebug::EGraphicsBackend backend = PortDebug::kGraphicsBackend_Auto;
  // The environment named it: the setting is not what starts this run.
  bool overridden = false;
  // The environment was set but names nothing: `backend` is the saved selection, which
  // the caller warns about.
  bool rejected = false;
};

// MP_GRAPHICS_BACKEND, read here only, so the start-up choice and the Video page
// describe the same request.
inline const char* EnvRequest() { return port::EnvString("MP_GRAPHICS_BACKEND"); }

inline Request Resolve(PortDebug::EGraphicsBackend saved, const char* env,
                       std::span<const PortDebug::EGraphicsBackend> choices = Choices()) {
  const auto offered = [choices](PortDebug::EGraphicsBackend backend) {
    for (const PortDebug::EGraphicsBackend choice : choices) {
      if (choice == backend) {
        return true;
      }
    }
    return false;
  };
  Request request;
  request.backend = offered(saved) ? saved : PortDebug::kGraphicsBackend_Auto;
  if (env != nullptr) {
    PortDebug::EGraphicsBackend parsed = saved;
    if (PortDebug::ParseGraphicsBackend(env, parsed) && offered(parsed)) {
      request.backend = parsed;
      request.overridden = true;
    } else {
      request.rejected = true;
    }
  }
  return request;
}

// What a selection asks Aurora for. Auto asks for nothing in particular, so Aurora
// takes its own order: Direct3D 12, then Vulkan, then Direct3D 11 on Windows; Vulkan,
// then OpenGL ES on Linux.
inline AuroraBackend ToAurora(PortDebug::EGraphicsBackend selection) {
  switch (selection) {
  case PortDebug::kGraphicsBackend_D3D12:
    return BACKEND_D3D12;
  case PortDebug::kGraphicsBackend_Vulkan:
    return BACKEND_VULKAN;
  case PortDebug::kGraphicsBackend_D3D11:
    return BACKEND_D3D11;
  case PortDebug::kGraphicsBackend_OpenGL:
    return BACKEND_OPENGL;
  default:
    return BACKEND_AUTO;
  }
}

// The selection an actual backend is, for comparing against what was asked for:
// kGraphicsBackend_Auto for anything the list doesn't name (Metal, WebGPU, OpenGL ES,
// or none at all, BACKEND_NULL). That also means an unnamed backend can never be read
// as "the request started".
inline PortDebug::EGraphicsBackend FromAurora(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_D3D12:
    return PortDebug::kGraphicsBackend_D3D12;
  case BACKEND_VULKAN:
    return PortDebug::kGraphicsBackend_Vulkan;
  case BACKEND_D3D11:
    return PortDebug::kGraphicsBackend_D3D11;
  case BACKEND_OPENGL:
    return PortDebug::kGraphicsBackend_OpenGL;
  default:
    return PortDebug::kGraphicsBackend_Auto;
  }
}

// A backend's name for the page and the log. BACKEND_NULL is what Aurora ends on when
// no device started at all, and nothing draws.
inline const char* DisplayName(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_D3D12:
    return "Direct3D 12";
  case BACKEND_D3D11:
    return "Direct3D 11";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGL ES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "no graphics device";
  default:
    return "Aurora's automatic choice";
  }
}

// What the Video page says about the choice. `saved` is the setting now, `savedAtStart`
// what this run read from the file, `request` how this run resolved it (override and
// all), and `actual` what Aurora started.
struct Status {
  // The saved selection differs from this run's: the next start uses it. Compared on
  // the saved values only, so setting the choice back clears it. An override doesn't
  // enter into it; it outranks the file on the next start too.
  bool restartPending = false;
  // MP_GRAPHICS_BACKEND named this run's API, so a change to the setting does not
  // affect this run.
  bool overridden = false;
  // This run asked for an API and another one started: Aurora fell back, and the
  // setting is kept as it was. False for Auto, which asks for nothing in particular.
  bool fellBack = false;
  // What this run asked for (over a rejected MP_GRAPHICS_BACKEND: the saved selection).
  PortDebug::EGraphicsBackend requested = PortDebug::kGraphicsBackend_Auto;
  // What started, as FromAurora reads it (Auto when the list doesn't name it).
  PortDebug::EGraphicsBackend actual = PortDebug::kGraphicsBackend_Auto;
};

inline Status Describe(PortDebug::EGraphicsBackend saved, PortDebug::EGraphicsBackend savedAtStart,
                       const Request& request, AuroraBackend actual) {
  Status status;
  status.restartPending = saved != savedAtStart;
  status.overridden = request.overridden;
  status.requested = request.backend;
  status.actual = FromAurora(actual);
  status.fellBack = request.backend != PortDebug::kGraphicsBackend_Auto && status.actual != request.backend;
  return status;
}

} // namespace PortGraphicsBackend

#endif // METROID_PRIME_PORT_PORT_GRAPHICS_BACKEND_H
