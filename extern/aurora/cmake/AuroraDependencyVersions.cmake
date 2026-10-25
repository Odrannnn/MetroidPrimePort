include_guard(GLOBAL)

# Specifies a cache string and previous values to forcibly migrate from
macro(_aurora_dependency_version name value doc)
  set(_aurora_old_defaults ${ARGN})
  if (DEFINED CACHE{${name}} AND ${name} IN_LIST _aurora_old_defaults)
    message(STATUS "aurora: Migrating ${name} from old default ${${name}} to ${value}")
    set(${name} "${value}" CACHE STRING "${doc}" FORCE)
  else ()
    set(${name} "${value}" CACHE STRING "${doc}")
  endif ()
  unset(_aurora_old_defaults)
endmacro()

# Dependency versions
_aurora_dependency_version(AURORA_DAWN_VERSION "v20261025.204802" "Dawn prebuilt version tag (https://github.com/Odrannnn/dawn/releases, branch mpp)"
        "v20260523.201736" "v20260603.191052" "v20260618.032059" "v20260807.225922" "v20261025.182847") # Previous versions
_aurora_dependency_version(AURORA_DAWN_REF "f3a9fa72535b5a333766bafcd48187bc4c055b2b" "Dawn commit ref (https://github.com/Odrannnn/dawn)"
        "9aa45f938d4b36626722bbfdc2f18447179337e6" "13abc3bc8ea2d3c2050f9e77a12d012108ceee24" "266c1cf8de969a364afa4fa49311631fc99a881e" "1155e0ed531126f33a1279afa029349651ca1c93" "738a656da3a19d51a1d553814febebe9468a2757") # Previous versions
_aurora_dependency_version(AURORA_SDL3_VERSION "3.4.10" "SDL3 prebuilt version tag (https://github.com/libsdl-org/SDL/releases)")
_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10" "SDL3 commit ref (https://github.com/libsdl-org/SDL)")
_aurora_dependency_version(AURORA_NOD_VERSION "v2.0.0-alpha.12" "nod version tag (https://github.com/encounter/nod/releases)"
        "v2.0.0-alpha.10" "v2.0.0-alpha.11") # Previous versions

# SHA-256 of the default downloads, keyed by version (or ref) and platform, so a
# changed or replaced archive fails the configure instead of being linked in.
# A version without an entry here downloads unverified, with a warning; a custom
# package URL can name its own digest with AURORA_<DEP>_PACKAGE_SHA256.
set(_aurora_dawn_sha256_v20261025.204802_windows-amd64 e6da9c1022d58ea696cbd5e7682ab5d72f969bfd01e299187f8632ca9bc99ee2)
set(_aurora_dawn_sha256_v20261025.204802_windows-arm64 9889302008a4f88a19818a70907581a359be9aa678eef954f6eaa2871f3e55f7)
set(_aurora_dawn_sha256_v20261025.204802_linux-x86_64 1cf637190ae9c69309dc308fdd7f98733ff139506890fb0b8e766d51f1c8df3c)
set(_aurora_dawn_sha256_v20261025.204802_linux-aarch64 b72ed506fea55dbffa40fae4843beaeaceb6b819b8e5a8946ce1fc81df86bbe8)
set(_aurora_dawn_sha256_v20261025.204802_darwin-arm64 6f6e7dfe5e07496ee069398254d85b6ed390f95e09f88dcccc5f6ae021db6573)
set(_aurora_dawn_sha256_v20261025.204802_darwin-x86_64 ae5205721669cd502e603a5753b9a8603b59044b69137be876c3b673a64d30f1)
set(_aurora_dawn_sha256_v20261025.204802_ios-arm64 87f92d18619209c14e65ec48b80475422b8798ac73ecccbcc70e3a09d237d0b9)
set(_aurora_dawn_sha256_v20261025.204802_android-aarch64 3446446833e398ddcf4a85fcfcca99f45bde4af03e8bb4e01d7ba10540a24346)
set(_aurora_nod_sha256_v2.0.0-alpha.12_windows-x86_64 c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881)
set(_aurora_nod_sha256_v2.0.0-alpha.12_windows-arm64 24f3a5d2aea299415438917d0078b48731af66f95ff17a6cf6a47ea6993b6c0f)
set(_aurora_nod_sha256_v2.0.0-alpha.12_linux-x86_64 920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f)
set(_aurora_nod_sha256_v2.0.0-alpha.12_linux-aarch64 6872dd3d45d9e688012db30f0a75b10702b8520b5b550ea6b50e9f6d458b05b3)
set(_aurora_nod_sha256_v2.0.0-alpha.12_macos-arm64 00c0625f0061b1d5c2211504bc971bd0740092d4018098f541f1941f2ac38b7d)
set(_aurora_nod_sha256_v2.0.0-alpha.12_macos-x86_64 50f28fdf7488c5f81e359440dd698abb2e12fa0b3ea377fc1262d76da9ff2aa7)
set(_aurora_sdl3_sha256_refs/tags/release-3.4.10_source 0dc11d980ba17250200718fa4e28011da293f27ed92f92203afffe396811f307)

# Sets out_var to "URL_HASH;SHA256=<digest>" for FetchContent_Declare, from an
# explicit digest if one is given, else from the table above; empty if neither.
function(aurora_url_hash out_var dep key explicit)
  if (explicit)
    set(${out_var} URL_HASH "SHA256=${explicit}" PARENT_SCOPE)
  elseif (DEFINED _aurora_${dep}_sha256_${key})
    set(${out_var} URL_HASH "SHA256=${_aurora_${dep}_sha256_${key}}" PARENT_SCOPE)
  else ()
    message(WARNING "aurora: no SHA-256 pinned for ${dep} ${key}; downloading it unverified")
    set(${out_var} "" PARENT_SCOPE)
  endif ()
endfunction()
