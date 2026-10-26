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
_aurora_dependency_version(AURORA_DAWN_VERSION "v20261026.185535" "Dawn prebuilt version tag (https://github.com/Odrannnn/dawn/releases, branch mpp)"
        "v20260523.201736" "v20260603.191052" "v20260618.032059" "v20260807.225922" "v20261025.182847" "v20261025.204802" "v20261025.224144" "v20261026.114446") # Previous versions
_aurora_dependency_version(AURORA_DAWN_REF "fef22023bf097ca371a9af17e2134cb631fe8912" "Dawn commit ref (https://github.com/Odrannnn/dawn)"
        "a04c63840d530f771de754a6991da392e109e0a6" "9aa45f938d4b36626722bbfdc2f18447179337e6" "13abc3bc8ea2d3c2050f9e77a12d012108ceee24" "266c1cf8de969a364afa4fa49311631fc99a881e" "1155e0ed531126f33a1279afa029349651ca1c93" "738a656da3a19d51a1d553814febebe9468a2757" "f3a9fa72535b5a333766bafcd48187bc4c055b2b" "4195093a6be1134c4e5873752e60d0b997b4bd78") # Previous versions
_aurora_dependency_version(AURORA_SDL3_VERSION "3.4.10" "SDL3 prebuilt version tag (https://github.com/libsdl-org/SDL/releases)")
_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10" "SDL3 commit ref (https://github.com/libsdl-org/SDL)")
_aurora_dependency_version(AURORA_NOD_VERSION "v2.0.0-alpha.12" "nod version tag (https://github.com/encounter/nod/releases)"
        "v2.0.0-alpha.10" "v2.0.0-alpha.11") # Previous versions

# SHA-256 of the default downloads, keyed by version (or ref) and platform, so a
# changed or replaced archive fails the configure instead of being linked in.
# A version without an entry here downloads unverified, with a warning; a custom
# package URL can name its own digest with AURORA_<DEP>_PACKAGE_SHA256.
set(_aurora_dawn_sha256_v20261026.185535_windows-amd64 50c47de32e3a5f15902a17ae16d84e0160f007b65a62f8b0b7c120ff958ff839)
set(_aurora_dawn_sha256_v20261026.185535_windows-arm64 2922a9128d0e5294eb71c4b6c70c6cae9be1637c83efacb402ec306219a1b17a)
set(_aurora_dawn_sha256_v20261026.185535_linux-x86_64 d85e6a4baf583e60a5c2fe06ac10893b5834a65de208a2d38d34bd16f157d08d)
set(_aurora_dawn_sha256_v20261026.185535_linux-aarch64 34246b4578637bdc5a3e552531e80be3bc2a15b7d1a6620a4ec10a12e6d0998b)
set(_aurora_dawn_sha256_v20261026.185535_darwin-arm64 98b864165d28e49fd60df14650cbdb0f629b1134075befdce56f10294a3a780c)
set(_aurora_dawn_sha256_v20261026.185535_darwin-x86_64 547b6c6734aea45b77d46f5ba43eba0e72799d2851ec8bffabe46bcdb375b4a0)
set(_aurora_dawn_sha256_v20261026.185535_ios-arm64 061f30b1b6d509f106a42f3f6ff74b05c8530afd229e674bd349e4844db2a953)
set(_aurora_dawn_sha256_v20261026.185535_android-aarch64 6a3ac06c29e08ec1ea1d7862bd0d45d2f1f40d546d522fe7e3b0d70caebbe94d)
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
