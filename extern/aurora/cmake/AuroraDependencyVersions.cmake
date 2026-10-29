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
_aurora_dependency_version(AURORA_DAWN_VERSION "v20261029.114630" "Dawn prebuilt version tag (https://github.com/Odrannnn/dawn/releases, branch mpp)"
        "v20260523.201736" "v20260603.191052" "v20260618.032059" "v20260807.225922" "v20261025.182847" "v20261025.204802" "v20261025.224144" "v20261026.114446" "v20261026.185535" "v20261027.153325" "v20261027.212404" "v20261029.111150") # Previous versions
_aurora_dependency_version(AURORA_DAWN_REF "118436b3f8f68583eb86e811ca605dc41637e8cc" "Dawn commit ref (https://github.com/Odrannnn/dawn)"
        "a04c63840d530f771de754a6991da392e109e0a6" "9aa45f938d4b36626722bbfdc2f18447179337e6" "13abc3bc8ea2d3c2050f9e77a12d012108ceee24" "266c1cf8de969a364afa4fa49311631fc99a881e" "1155e0ed531126f33a1279afa029349651ca1c93" "738a656da3a19d51a1d553814febebe9468a2757" "f3a9fa72535b5a333766bafcd48187bc4c055b2b" "4195093a6be1134c4e5873752e60d0b997b4bd78" "fef22023bf097ca371a9af17e2134cb631fe8912" "59d485b83b95ad075b0b812e0ce33fc2012e827b" "bfa890082bf20bc10e0a3bfe122cf0bfa7f4da61" "4f56eac83322c45f5ce82dd1c8d2bc8c9cb8bb6e") # Previous versions
_aurora_dependency_version(AURORA_SDL3_VERSION "3.4.10" "SDL3 prebuilt version tag (https://github.com/libsdl-org/SDL/releases)")
_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10" "SDL3 commit ref (https://github.com/libsdl-org/SDL)")
_aurora_dependency_version(AURORA_NOD_VERSION "v2.0.0-alpha.12" "nod version tag (https://github.com/encounter/nod/releases)"
        "v2.0.0-alpha.10" "v2.0.0-alpha.11") # Previous versions

# SHA-256 of the default downloads, keyed by version (or ref) and platform, so a
# changed or replaced archive fails the configure instead of being linked in.
# A version without an entry here downloads unverified, with a warning; a custom
# package URL can name its own digest with AURORA_<DEP>_PACKAGE_SHA256.
set(_aurora_dawn_sha256_v20261029.111150_windows-amd64 440e9a43c1a7c5e7500f54f85677aaa6b2355a0514561b31216aa15ee4e7efe8)
set(_aurora_dawn_sha256_v20261029.111150_windows-arm64 65b307fc0ade18f8797b5efa7dba5f126e06e02dcc20c61af2fe3f800120f827)
set(_aurora_dawn_sha256_v20261029.111150_linux-x86_64 19577afbfb1081a7975c60e22572b43f96daf0b063dae267f62f29944d8ac7cd)
set(_aurora_dawn_sha256_v20261029.111150_linux-aarch64 cfbaa486c46566bc0d4913cee371dd58d1fce93cc584e1fc3e77025a7fb30bc1)
set(_aurora_dawn_sha256_v20261029.111150_darwin-arm64 d95a49a16d89a144a30700ca66492bf636f2106f716d88b076755164d376ef0e)
set(_aurora_dawn_sha256_v20261029.111150_darwin-x86_64 5322db091020a0978875a5ce1d160bd53e17a329db72216a8d36a07d8dd1e3b7)
set(_aurora_dawn_sha256_v20261029.111150_ios-arm64 d60411b2b4c9fecb8ca8386e5f10fbe247223d4a91e0d6311b3d25d03ec69564)
set(_aurora_dawn_sha256_v20261029.111150_android-aarch64 815622f767495ae9ff878a41785f962fb9d0c43639887cd0990bd5eb65350e14)
set(_aurora_dawn_sha256_v20261029.114630_windows-amd64 430db33fa0b51369c8acdd5a7cf9b0f40f5f1d7d1146cfa98c0faa568af37959)
set(_aurora_dawn_sha256_v20261029.114630_windows-arm64 6d46868d8bc34b3e033582ff43fc93c1089898b548c5511e7f7d617ea3e24371)
set(_aurora_dawn_sha256_v20261029.114630_linux-x86_64 ebc335ebf7d17b3a940d3f46abf726ef0531b9fa05a549be44d8c2a2dc1ba29d)
set(_aurora_dawn_sha256_v20261029.114630_linux-aarch64 cbb550709fdd519d52330b9b59206a8300bcbd8bd698af0359e0ba5bfca2fed8)
set(_aurora_dawn_sha256_v20261029.114630_darwin-arm64 e2d17fc673e38a912be2564f432f91c1412c2e6ab40916e73534e9692161a4eb)
set(_aurora_dawn_sha256_v20261029.114630_darwin-x86_64 a978f93e0bfe7a45f8150e3644059c5522cc517b4335cd7afc2637ff644e9b74)
set(_aurora_dawn_sha256_v20261029.114630_ios-arm64 5409d80fd81488ec781f726076478d362d363f71d67f7c086e847546faf9c786)
set(_aurora_dawn_sha256_v20261029.114630_android-aarch64 6d2c30358db2dd542f1ce9d5ea54a751dfb16cda924c1c58039ed9b9eda41eea)
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
