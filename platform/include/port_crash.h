#ifndef METROID_PRIME_PORT_PORT_CRASH_H
#define METROID_PRIME_PORT_PORT_CRASH_H

// Writes a crash report into the log: what went wrong, where (module + offset,
// which a symbolizer turns back into a line of code), the build, and the stack.
// Nothing else changes about the crash, so the system still gets it afterwards:
// Windows Error Reporting (Event Viewer), a core dump on Linux, Android's
// tombstone. Windows also writes <user folder>/metroid_prime_port.dmp, and its
// report has function names and lines when metroid_prime_port.pdb is beside the
// program (the release zip has it).
//
// MP_CRASH_TEST=segv|abort crashes right after Install, to check the report.
namespace PortCrash {

void Install();

// Linux and Android: asks the thread with kernel id `threadId` (gettid; macOS: its pthread_t) to write its
// current stack into the log as "watchdog:" lines (needs Install), then carry on.
// False when the request could not be sent, and always on Windows.
bool RequestStack(long threadId);

// Call from a catch block: logs the exception's type and what(), then aborts, so the
// crash report follows.
[[noreturn]] void AbortOnException(const char* where);

} // namespace PortCrash

#endif // METROID_PRIME_PORT_PORT_CRASH_H
