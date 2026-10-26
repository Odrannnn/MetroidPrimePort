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
_aurora_dependency_version(AURORA_DAWN_VERSION "v20261026.114446" "Dawn prebuilt version tag (https://github.com/Odrannnn/dawn/releases, branch mpp)"
        "v20260523.201736" "v20260603.191052" "v20260618.032059" "v20260807.225922" "v20261025.182847" "v20261025.204802" "v20261025.224144") # Previous versions
_aurora_dependency_version(AURORA_DAWN_REF "4195093a6be1134c4e5873752e60d0b997b4bd78" "Dawn commit ref (https://github.com/Odrannnn/dawn)"
        "a04c63840d530f771de754a6991da392e109e0a6" "9aa45f938d4b36626722bbfdc2f18447179337e6" "13abc3bc8ea2d3c2050f9e77a12d012108ceee24" "266c1cf8de969a364afa4fa49311631fc99a881e" "1155e0ed531126f33a1279afa029349651ca1c93" "738a656da3a19d51a1d553814febebe9468a2757" "f3a9fa72535b5a333766bafcd48187bc4c055b2b") # Previous versions
_aurora_dependency_version(AURORA_SDL3_VERSION "3.4.10" "SDL3 prebuilt version tag (https://github.com/libsdl-org/SDL/releases)")
_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10" "SDL3 commit ref (https://github.com/libsdl-org/SDL)")
_aurora_dependency_version(AURORA_NOD_VERSION "v2.0.0-alpha.12" "nod version tag (https://github.com/encounter/nod/releases)"
        "v2.0.0-alpha.10" "v2.0.0-alpha.11") # Previous versions

# SHA-256 of the default downloads, keyed by version (or ref) and platform, so a
# changed or replaced archive fails the configure instead of being linked in.
# A version without an entry here downloads unverified, with a warning; a custom
# package URL can name its own digest with AURORA_<DEP>_PACKAGE_SHA256.
set(_aurora_dawn_sha256_v20261026.114446_windows-amd64 00a6b2e72e02b40067d6b2c34aeaaf1de421cefcc98b503552a8761407284f8f)
set(_aurora_dawn_sha256_v20261026.114446_windows-arm64 e681e6a56b1c4f13d20d04dabcf878e2d7f575b347492f5a2feb8d7faab351dc)
set(_aurora_dawn_sha256_v20261026.114446_linux-x86_64 3aa23a7049608ff8291c30251e0f8f15f49c315ac667184eca9d6f428d8c3bf5)
set(_aurora_dawn_sha256_v20261026.114446_linux-aarch64 86e248181546bf4b971206fdc82334cb8ae7456fa907da70f8585d56199dfafb)
set(_aurora_dawn_sha256_v20261026.114446_darwin-arm64 1ce99b663f5581607a2624a965f6d3559e4de7183c291675cb543013db4d4489)
set(_aurora_dawn_sha256_v20261026.114446_darwin-x86_64 9aade1594b8243ee6efd23220ad3e5b5266f88624a6b7c7abeb5a294f02f395a)
set(_aurora_dawn_sha256_v20261026.114446_ios-arm64 b04bb53f2f58d3cb8d8eef816273dc58a35bd964ec863c395fdc534ac0a3b682)
set(_aurora_dawn_sha256_v20261026.114446_android-aarch64 e0da65de4e09b94ef91c38c15abda85041dc80aa0c1abd773fbeaa19d6fcd830)
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
