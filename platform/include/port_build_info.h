#ifndef METROID_PRIME_PORT_PORT_BUILD_INFO_H
#define METROID_PRIME_PORT_PORT_BUILD_INFO_H

// The commit and release version the binary was built from. They're defined in
// a generated source (cmake/BuildRevision.cmake) rather than a generated header,
// so a new commit or a dirty tree recompiles one tiny file instead of every
// file that shows the version.
extern const char kMpBuildRevision[];
extern const char kMpBuildVersion[];

#define MP_BUILD_REVISION kMpBuildRevision
#define MP_BUILD_VERSION kMpBuildVersion

#endif
