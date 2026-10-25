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
_aurora_dependency_version(AURORA_DAWN_VERSION "v20261025.224144" "Dawn prebuilt version tag (https://github.com/Odrannnn/dawn/releases, branch mpp)"
        "v20260523.201736" "v20260603.191052" "v20260618.032059" "v20260807.225922" "v20261025.182847" "v20261025.204802") # Previous versions
_aurora_dependency_version(AURORA_DAWN_REF "a04c63840d530f771de754a6991da392e109e0a6" "Dawn commit ref (https://github.com/Odrannnn/dawn)"
        "9aa45f938d4b36626722bbfdc2f18447179337e6" "13abc3bc8ea2d3c2050f9e77a12d012108ceee24" "266c1cf8de969a364afa4fa49311631fc99a881e" "1155e0ed531126f33a1279afa029349651ca1c93" "738a656da3a19d51a1d553814febebe9468a2757" "f3a9fa72535b5a333766bafcd48187bc4c055b2b") # Previous versions
_aurora_dependency_version(AURORA_SDL3_VERSION "3.4.10" "SDL3 prebuilt version tag (https://github.com/libsdl-org/SDL/releases)")
_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10" "SDL3 commit ref (https://github.com/libsdl-org/SDL)")
_aurora_dependency_version(AURORA_NOD_VERSION "v2.0.0-alpha.12" "nod version tag (https://github.com/encounter/nod/releases)"
        "v2.0.0-alpha.10" "v2.0.0-alpha.11") # Previous versions

# SHA-256 of the default downloads, keyed by version (or ref) and platform, so a
# changed or replaced archive fails the configure instead of being linked in.
# A version without an entry here downloads unverified, with a warning; a custom
# package URL can name its own digest with AURORA_<DEP>_PACKAGE_SHA256.
set(_aurora_dawn_sha256_v20261025.224144_windows-amd64 d3cd82e16bb36b59a43876378006f60377ac356458165d74c2314a9559413b93)
set(_aurora_dawn_sha256_v20261025.224144_windows-arm64 1317068fa9ab83d9bb72a241d9a4f5efb5d10c84483bec9eca8cb6d9e1577e78)
set(_aurora_dawn_sha256_v20261025.224144_linux-x86_64 fd73b2399966e99c3da3383c83de907cbbd423109f4a6b438efe65077ad49a7d)
set(_aurora_dawn_sha256_v20261025.224144_linux-aarch64 b414a6ef770ebdcd8b44bc7bc1c37773f16575893575c63ebae58af3d1af116a)
set(_aurora_dawn_sha256_v20261025.224144_darwin-arm64 b824abae06d58119d68a91662d01dcffa511ac87b581d26fe493f23c707cc5f2)
set(_aurora_dawn_sha256_v20261025.224144_darwin-x86_64 ecc2c98549060f6d99992a1f6c7b04b4532ab4218952e795fdf40a7a04b45ffc)
set(_aurora_dawn_sha256_v20261025.224144_ios-arm64 68e81bd1753b80cd310d5a72be336571af4c61832a223f46f4ec640b3ebd4536)
set(_aurora_dawn_sha256_v20261025.224144_android-aarch64 df61176a146806bf47697eafc082c395f11366e064f7c3b4e5260a372f28814d)
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
