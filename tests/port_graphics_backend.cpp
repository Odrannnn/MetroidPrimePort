// The renderer choice (issue #135): the names the settings file and
// MP_GRAPHICS_BACKEND carry, which choices this build offers, the per-run override that
// outranks the file, and the status the F1 Video page draws from them.
// port_graphics_backend.h owns the mapping main.cpp starts from and the page describes,
// so this drives the same code both do.
//
// What is checked here:
//   * every choice's stored name reads back as that choice, and anything else is
//     refused with the caller's value left alone (so a hand-edited or older file
//     keeps the saved selection instead of silently changing it);
//   * the choices this platform offers, since the Video page's combo index is a
//     position in that list, never an enum value;
//   * MP_GRAPHICS_BACKEND precedence: it wins when it names a choice, a value that
//     names nothing is rejected and leaves the saved selection in charge, and an
//     unset variable changes nothing;
//   * the status cases the page gets wrong if the request is taken from the setting
//     rather than from what this run resolved: a successful override is not a
//     fallback, and a fallback from an overridden or saved request is reported;
//   * restart tracking follows the SAVED selection only, so setting the choice back
//     clears it, and an override does not hide a pending change.

#include "port_graphics_backend.h"
#include "port_test.h"

#include <cstdio>
#include <span>
#include <string>

namespace {

using PortDebug::EGraphicsBackend;
using PortGraphicsBackend::Request;
using PortGraphicsBackend::Status;

// Every choice, for the cases that check resolution itself rather than one platform's list.
constexpr EGraphicsBackend kAll[] = {PortDebug::kGraphicsBackend_Auto, PortDebug::kGraphicsBackend_D3D12,
                                     PortDebug::kGraphicsBackend_Vulkan, PortDebug::kGraphicsBackend_D3D11,
                                     PortDebug::kGraphicsBackend_OpenGL};

const char* Name(EGraphicsBackend backend) { return PortDebug::GraphicsBackendConfigName(backend); }

// A name is accepted and means that choice again.
void CheckRoundTrip(EGraphicsBackend backend) {
  EGraphicsBackend parsed = PortDebug::kGraphicsBackend_Auto;
  if (!PortDebug::ParseGraphicsBackend(Name(backend), parsed)) {
    std::fprintf(stderr, "FAIL: %s is not accepted\n", Name(backend));
    port_test::CountFailure();
    return;
  }
  if (parsed != backend) {
    std::fprintf(stderr, "FAIL: %s reads back as %s\n", Name(backend), Name(parsed));
    port_test::CountFailure();
  }
}

// A value nothing names is refused and leaves the caller's choice as it was.
void CheckRefused(const char* value) {
  EGraphicsBackend untouched = PortDebug::kGraphicsBackend_D3D11;
  if (PortDebug::ParseGraphicsBackend(value, untouched)) {
    std::fprintf(stderr, "FAIL: \"%s\" should not be accepted\n", value);
    port_test::CountFailure();
    return;
  }
  if (untouched != PortDebug::kGraphicsBackend_D3D11) {
    std::fprintf(stderr, "FAIL: \"%s\" changed the choice it refused\n", value);
    port_test::CountFailure();
  }
}

// One resolution case: the saved selection, the environment, and what must come out.
void CheckResolve(EGraphicsBackend saved, const char* env, EGraphicsBackend expectBackend, bool expectOverridden,
                  bool expectRejected, const char* what) {
  const Request request = PortGraphicsBackend::Resolve(saved, env, kAll);
  if (request.backend != expectBackend || request.overridden != expectOverridden ||
      request.rejected != expectRejected) {
    std::fprintf(stderr, "FAIL: %s: resolved %s overridden=%d rejected=%d\n", what, Name(request.backend),
                 request.overridden, request.rejected);
    port_test::CountFailure();
  }
}

// One status case: what this run asked for, what started, and what the page must say.
void CheckStatus(EGraphicsBackend saved, EGraphicsBackend savedAtStart, const char* env, AuroraBackend actual,
                 bool expectRestartPending, bool expectOverridden, bool expectFellBack, const char* what) {
  const Request request = PortGraphicsBackend::Resolve(savedAtStart, env, kAll);
  const Status status = PortGraphicsBackend::Describe(saved, savedAtStart, request, actual);
  if (status.restartPending != expectRestartPending || status.overridden != expectOverridden ||
      status.fellBack != expectFellBack) {
    std::fprintf(stderr, "FAIL: %s: restartPending=%d overridden=%d fellBack=%d\n", what, status.restartPending,
                 status.overridden, status.fellBack);
    port_test::CountFailure();
  }
  if (status.requested != request.backend || status.actual != PortGraphicsBackend::FromAurora(actual)) {
    std::fprintf(stderr, "FAIL: %s: requested %s, actual %s\n", what, Name(status.requested), Name(status.actual));
    port_test::CountFailure();
  }
}

} // namespace

int main() {
  constexpr EGraphicsBackend kAuto = PortDebug::kGraphicsBackend_Auto;
  constexpr EGraphicsBackend kD3D12 = PortDebug::kGraphicsBackend_D3D12;
  constexpr EGraphicsBackend kVulkan = PortDebug::kGraphicsBackend_Vulkan;
  constexpr EGraphicsBackend kD3D11 = PortDebug::kGraphicsBackend_D3D11;
  constexpr EGraphicsBackend kOpenGL = PortDebug::kGraphicsBackend_OpenGL;

  // The stored names, and their order in the config file. Both are in the file:
  // reordering or renumbering them changes what a user's file means. OpenGL was
  // added last for that reason, and `opengl` only became a config name with it.
  CHECK_MSG(std::string(Name(kAuto)) == "auto", "auto's name");
  CHECK_MSG(std::string(Name(kD3D12)) == "d3d12", "d3d12's name");
  CHECK_MSG(std::string(Name(kVulkan)) == "vulkan", "vulkan's name");
  CHECK_MSG(std::string(Name(kD3D11)) == "d3d11", "d3d11's name");
  CHECK_MSG(std::string(Name(kOpenGL)) == "opengl", "opengl's name");
  CHECK_MSG(kAuto == 0, "auto is 0");
  CHECK_MSG(kD3D12 == 1, "d3d12 is 1");
  CHECK_MSG(kVulkan == 2, "vulkan is 2");
  CHECK_MSG(kD3D11 == 3, "d3d11 is 3");
  CHECK_MSG(kOpenGL == 4, "opengl is 4");
  CHECK_MSG(PortDebug::kGraphicsBackendCount == 5, "five choices");

  for (const EGraphicsBackend backend : {kAuto, kD3D12, kVulkan, kD3D11, kOpenGL}) {
    CheckRoundTrip(backend);
  }

  // The choices this build offers, Auto first. The Video page's combo index is a
  // position in this list, so its contents are pinned per platform.
  const std::span<const EGraphicsBackend> choices = PortGraphicsBackend::Choices();
  CHECK_MSG(!choices.empty() && choices.front() == kAuto, "auto is the first choice");
  for (const EGraphicsBackend backend : choices) {
    CheckRoundTrip(backend);
  }
#if defined(_WIN32)
  CHECK_MSG(choices.size() == 5, "windows offers five choices");
  CHECK_MSG(choices[1] == kD3D12 && choices[2] == kVulkan && choices[3] == kD3D11 && choices[4] == kOpenGL,
            "windows offers D3D12, Vulkan, D3D11, OpenGL");
#elif defined(__linux__) && !defined(__ANDROID__)
  CHECK_MSG(choices.size() == 3, "linux offers three choices");
  CHECK_MSG(choices[1] == kVulkan && choices[2] == kOpenGL, "linux offers Vulkan, then OpenGL");
#else
  CHECK_MSG(choices.size() == 1, "elsewhere auto is the only choice");
#endif

  // Main.cpp's MP_GRAPHICS_BACKEND and the settings file take exactly the names
  // above: lowercase, no spaces, no aliases.
  CheckRefused("");
  CheckRefused(" ");
  CheckRefused("AUTO");
  CheckRefused("Auto");
  CheckRefused("D3D12");
  CheckRefused("Vulkan");
  CheckRefused("d3d12 ");
  CheckRefused(" d3d12");
  CheckRefused("d3d12\n");
  CheckRefused("direct3d12");
  CheckRefused("direct3d 12");
  CheckRefused("d3d");
  CheckRefused("vk");
  CheckRefused("vulkan1");
  CheckRefused("opengles");
  CheckRefused("OpenGL");
  CheckRefused("gl");
  CheckRefused("none");
  CheckRefused("1");
  CheckRefused("2");
  CheckRefused("true");
  CheckRefused("yes");

  // A value from a newer build (an enum number this build doesn't know) still
  // writes something this build reads back, so a file is never left unreadable.
  CHECK_MSG(std::string(Name(static_cast< EGraphicsBackend >(5))) == "auto", "an unknown number writes auto");

  // The selection -> Aurora mapping, and back. Nothing but the named APIs can read as
  // "the request started", so an unnamed backend (Metal, WebGPU, OpenGL ES, none) can
  // never hide a fallback.
  CHECK_MSG(PortGraphicsBackend::ToAurora(kAuto) == BACKEND_AUTO, "auto asks for nothing in particular");
  CHECK_MSG(PortGraphicsBackend::ToAurora(kD3D12) == BACKEND_D3D12, "d3d12 asks for D3D12");
  CHECK_MSG(PortGraphicsBackend::ToAurora(kVulkan) == BACKEND_VULKAN, "vulkan asks for Vulkan");
  CHECK_MSG(PortGraphicsBackend::ToAurora(kD3D11) == BACKEND_D3D11, "d3d11 asks for D3D11");
  CHECK_MSG(PortGraphicsBackend::ToAurora(kOpenGL) == BACKEND_OPENGL, "opengl asks for OpenGL");
  for (const EGraphicsBackend backend : {kD3D12, kVulkan, kD3D11, kOpenGL}) {
    CHECK_MSG(PortGraphicsBackend::FromAurora(PortGraphicsBackend::ToAurora(backend)) == backend, "mapping round trip");
  }
  CHECK_MSG(PortGraphicsBackend::FromAurora(BACKEND_METAL) == kAuto, "Metal is not a named choice");
  CHECK_MSG(PortGraphicsBackend::FromAurora(BACKEND_WEBGPU) == kAuto, "WebGPU is not a named choice");
  CHECK_MSG(PortGraphicsBackend::FromAurora(BACKEND_OPENGLES) == kAuto, "OpenGL ES is not a named choice");
  CHECK_MSG(PortGraphicsBackend::FromAurora(BACKEND_NULL) == kAuto, "no device is not a named choice");
  CHECK_MSG(std::string(PortGraphicsBackend::DisplayName(BACKEND_D3D12)) == "Direct3D 12", "d3d12's display name");
  CHECK_MSG(std::string(PortGraphicsBackend::DisplayName(BACKEND_VULKAN)) == "Vulkan", "vulkan's display name");
  CHECK_MSG(std::string(PortGraphicsBackend::DisplayName(BACKEND_D3D11)) == "Direct3D 11", "d3d11's display name");
  CHECK_MSG(std::string(PortGraphicsBackend::DisplayName(BACKEND_OPENGL)) == "OpenGL", "opengl's display name");
  CHECK_MSG(std::string(PortGraphicsBackend::DisplayName(BACKEND_NULL)) == "no graphics device", "no device's name");

  // MP_GRAPHICS_BACKEND precedence: the environment wins when it names a choice; a
  // value that names nothing is refused and leaves the saved selection in charge.
  CheckResolve(kD3D12, nullptr, kD3D12, false, false, "no override");
  CheckResolve(kD3D12, "d3d11", kD3D11, true, false, "the override wins");
  CheckResolve(kAuto, "d3d11", kD3D11, true, false, "the override wins over auto");
  CheckResolve(kD3D11, "auto", kAuto, true, false, "auto is an override too");
  CheckResolve(kVulkan, "opengl", kOpenGL, true, false, "opengl is an override");
  CheckResolve(kD3D11, "bogus", kD3D11, false, true, "an unnamed override keeps the selection");
  CheckResolve(kAuto, "bogus", kAuto, false, true, "an unnamed override keeps auto");
  CheckResolve(kAuto, "", kAuto, false, true, "an empty override keeps auto");

  // Only the offered choices resolve: another platform's API is rejected as an override,
  // and a saved one asks for Auto (what the Video page shows for it).
  {
    constexpr EGraphicsBackend kOffered[] = {kAuto, kVulkan, kOpenGL};
    const Request env = PortGraphicsBackend::Resolve(kVulkan, "d3d12", kOffered);
    CHECK_MSG(env.rejected && !env.overridden && env.backend == kVulkan, "an unoffered override is rejected");
    const Request saved = PortGraphicsBackend::Resolve(kD3D12, nullptr, kOffered);
    CHECK_MSG(!saved.rejected && saved.backend == kAuto, "an unoffered saved choice asks for auto");
  }

  // The statuses the page draws. savedAtStart is what this run read from the file and
  // env what MP_GRAPHICS_BACKEND was at startup; `saved` is the setting right now.
  /* saved, savedAtStart, env, started, restart, overridden, fellBack, case */
  // Nothing changed, nothing overridden, the request started.
  CheckStatus(kD3D12, kD3D12, nullptr, BACKEND_D3D12, false, false, false, "d3d12 started as asked");
  CheckStatus(kAuto, kAuto, nullptr, BACKEND_VULKAN, false, false, false, "auto started Vulkan");
  // The setting was changed after startup: a restart is pending, and the API in use
  // is still the one this run asked for (no fallback to report).
  CheckStatus(kVulkan, kD3D12, nullptr, BACKEND_D3D12, true, false, false, "a changed setting needs a restart");
  // Set back to the startup choice: the notice clears.
  CheckStatus(kD3D12, kD3D12, nullptr, BACKEND_D3D12, false, false, false, "set back to the startup choice");
  // An explicit request that didn't start: report the fallback, and the setting stays.
  CheckStatus(kD3D12, kD3D12, nullptr, BACKEND_VULKAN, false, false, true, "d3d12 fell back to Vulkan");
  CheckStatus(kD3D12, kD3D12, nullptr, BACKEND_NULL, false, false, true, "d3d12 found no device");
  // OpenGL on desktop Linux is an ordinary explicit choice: it starts as asked, and its
  // failure to start is a fallback like any other.
  CheckStatus(kOpenGL, kOpenGL, nullptr, BACKEND_OPENGL, false, false, false, "opengl started as asked");
  CheckStatus(kOpenGL, kOpenGL, nullptr, BACKEND_VULKAN, false, false, true, "opengl fell back to Vulkan");
  CheckStatus(kOpenGL, kOpenGL, nullptr, BACKEND_NULL, false, false, true, "opengl found no device");
  // The reviewer's first case: saved d3d12 with MP_GRAPHICS_BACKEND=d3d11 that
  // started D3D11 is a successful override, NOT a failed Direct3D 12 start.
  CheckStatus(kD3D12, kD3D12, "d3d11", BACKEND_D3D11, false, true, false, "the override started as asked");
  // The reviewer's second case: saved auto with MP_GRAPHICS_BACKEND=d3d11 that fell
  // back still reports the fallback, even though the setting is Auto.
  CheckStatus(kAuto, kAuto, "d3d11", BACKEND_VULKAN, false, true, true, "the override fell back");
  CheckStatus(kAuto, kAuto, "d3d11", BACKEND_NULL, false, true, true, "the override found no device");
  // A rejected override leaves the saved selection asking, so its fallback is reported.
  CheckStatus(kD3D12, kD3D12, "bogus", BACKEND_VULKAN, false, false, true, "a refused override keeps d3d12 asking");
  // An override in charge does not hide a pending change to the setting.
  CheckStatus(kVulkan, kD3D12, "d3d11", BACKEND_D3D11, true, true, false, "override + changed setting");
  // Auto asks for nothing in particular, so nothing can have failed.
  CheckStatus(kAuto, kAuto, nullptr, BACKEND_NULL, false, false, false, "auto cannot fall back");
  CheckStatus(kAuto, kAuto, "auto", BACKEND_METAL, false, true, false, "an explicit auto never falls back");

  if (port_test::Failures() == 0) {
    std::printf("port_graphics_backend: OK\n");
  }
  return PORT_TEST_RESULT();
}
