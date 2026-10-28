if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

# SDL scales the Wii U GameCube adapter's axes by a range that starts at
# 128 +/- 88 and widens whenever a stick passes it, so the bytes the pad sent
# can't be recovered from the result. This sends them linearly instead
# ((raw - 128) * 256 on every axis, triggers included) and marks the joystick
# with SDL.joystick.gamecube.raw_axes once its first packet is in, so the pad
# layer can apply the console's origin and clamp to the exact values.
set(_driver "${SDL_SOURCE_DIR}/src/joystick/hidapi/SDL_hidapi_gamecube.c")
if (NOT EXISTS "${_driver}")
  message(FATAL_ERROR "SDL GameCube driver source is missing: ${_driver}")
endif ()

file(READ "${_driver}" _source)

string(FIND "${_source}" "SDL.joystick.gamecube.raw_axes" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 GameCube raw axes patch already applied")
  return()
endif ()

function(_replace_once old new)
  string(FIND "${_source}" "${old}" _first)
  string(FIND "${_source}" "${old}" _last REVERSE)
  if (_first EQUAL -1 OR NOT _first EQUAL _last)
    message(FATAL_ERROR "Failed to apply SDL3 GameCube raw axes patch: expected exactly one of:\n${old}")
  endif ()
  string(REPLACE "${old}" "${new}" _source "${_source}")
  set(_source "${_source}" PARENT_SCOPE)
endfunction()

_replace_once(
  "    Uint64 pc_rumble_sent;\n} SDL_DriverGameCube_Context;\n"
  "    Uint64 pc_rumble_sent;\n    bool raw_reported[MAX_CONTROLLERS];\n} SDL_DriverGameCube_Context;\n")

_replace_once(
  "    axis_value = (Sint16)HIDAPI_RemapVal(curSlot[off], ctx->min_axis[i * SDL_GAMEPAD_AXIS_COUNT + axis], ctx->max_axis[i * SDL_GAMEPAD_AXIS_COUNT + axis], SDL_MIN_SINT16, SDL_MAX_SINT16); \\\n"
  "    axis_value = (Sint16)((curSlot[off] - 128) * 256);                                  \\\n")

_replace_once(
  "        READ_AXIS(8, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)\n#undef READ_AXIS\n"
  "        READ_AXIS(8, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)\n#undef READ_AXIS\n        if (!ctx->raw_reported[i]) {\n            SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), \"SDL.joystick.gamecube.raw_axes\", true);\n            ctx->raw_reported[i] = true;\n        }\n")

_replace_once(
  "            joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;\n"
  "            joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;\n            ctx->raw_reported[i] = false;\n")

file(WRITE "${_driver}" "${_source}")
message(STATUS "Applied SDL3 GameCube raw axes patch")
