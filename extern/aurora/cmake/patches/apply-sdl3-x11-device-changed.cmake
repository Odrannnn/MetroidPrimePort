if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

# SDL 3.4.10 caches each XInput2 device's valuator classes on first use and
# ignores XI_DeviceChanged. A master pointer's classes follow whichever slave
# moved last (XISlaveSwitch); under Xwayland that flips between the absolute
# "xwayland-pointer" and the relative "xwayland-relative-pointer", so relative
# deltas get differenced as absolute positions and mouse aim snaps back.
# Backport of upstream 89fc9aa6b/b673d2bef (in release-3.4.18): drop the cached
# device on XI_DeviceChanged so it is queried again, and take the first
# absolute value after that as a baseline instead of a delta from zero.
set(_header "${SDL_SOURCE_DIR}/src/video/x11/SDL_x11mouse.h")
set(_xinput2 "${SDL_SOURCE_DIR}/src/video/x11/SDL_x11xinput2.c")
foreach (_file IN ITEMS "${_header}" "${_xinput2}")
  if (NOT EXISTS "${_file}")
    message(FATAL_ERROR "SDL X11 source is missing: ${_file}")
  endif ()
endforeach ()

file(READ "${_header}" _header_source)
file(READ "${_xinput2}" _xinput2_source)

string(FIND "${_header_source}" "prev_coord_valid" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 X11 XI_DeviceChanged patch already applied")
  return()
endif ()

function (_sdl_patch_replace _var _old _new _what)
  string(FIND "${${_var}}" "${_old}" _pos)
  if (_pos EQUAL -1)
    message(FATAL_ERROR "SDL3 X11 XI_DeviceChanged patch: ${_what} not found")
  endif ()
  string(REPLACE "${_old}" "${_new}" _out "${${_var}}")
  set(${_var} "${_out}" PARENT_SCOPE)
endfunction ()

_sdl_patch_replace(_header_source
  "    double prev_coords[2];\n"
  "    double prev_coords[2];\n    bool prev_coord_valid[2];\n"
  "SDL_XInput2DeviceInfo.prev_coords")

_sdl_patch_replace(_xinput2_source
  "                    processed_coords[j] = (current_val - devinfo->prev_coords[j]); // convert absolute to relative\n                    devinfo->prev_coords[j] = current_val;\n"
  "                    if (devinfo->prev_coord_valid[j]) {\n                        processed_coords[j] = (current_val - devinfo->prev_coords[j]); // convert absolute to relative\n                    }\n                    devinfo->prev_coords[j] = current_val;\n                    devinfo->prev_coord_valid[j] = true;\n"
  "absolute valuator conversion")

_sdl_patch_replace(_xinput2_source
  "    //case XI_DeviceChanged:\n\n    case XI_PropertyEvent:\n"
  "    //case XI_DeviceChanged:\n\n    case XI_DeviceChanged:\n    {\n        // The valuator classes changed (e.g. a master pointer switched slaves): query them again on next use.\n        const XIDeviceChangedEvent *dcev = (const XIDeviceChangedEvent *)cookie->data;\n        xinput2_remove_device_info(videodata, dcev->deviceid);\n    } break;\n\n    case XI_PropertyEvent:\n"
  "XI_DeviceChanged case")

file(WRITE "${_header}" "${_header_source}")
file(WRITE "${_xinput2}" "${_xinput2_source}")
message(STATUS "Applied SDL3 X11 XI_DeviceChanged patch")
