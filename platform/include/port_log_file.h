#ifndef METROID_PRIME_PORT_PORT_LOG_FILE_H
#define METROID_PRIME_PORT_PORT_LOG_FILE_H
#include <string>

// The "write the log to a file" setting (F1 > System > Log, MP_LOG_FILE). Everything
// the process prints to stdout and stderr - the port's messages, Aurora's, SDL's,
// and the FATAL line Aurora prints right before it aborts - is also written to
// <user folder>/metroid_prime_port.log, so a crash on a machine without a
// terminal leaves a log to send. The previous run's log is kept as
// metroid_prime_port.old.log, so restarting after a crash does not lose it.
//
// The log is on by default (the logging=0 setting turns it off). Since it is meant to be
// attached to bug reports, the file has the user's home folder and account name
// taken out of it (port_log_redact.h).
//
// On Linux a small forked process copies a pipe to both the terminal and the file:
// it outlives an abort and drains everything already written. On Windows the same
// is done by a second copy of the program (RunCopy), to the file and to wherever
// stderr went before (console or redirect). On Android the logcat writers (PortLog, Aurora's
// callback, SDL) also call Write, each line straight to the file, and stdout and
// stderr are copied into logcat and the file; the log goes to the app's external
// folder (Android/data/org.metroidprime.port/files) unless the data was moved to
// shared storage, and a copy goes to
// Documents/MetroidPrime/metroid_prime_port.log, which the phone's file manager
// can open (it cannot browse Android/data since Android 13).
namespace PortLogFile {

// Starts the file log (once per run); true when it is running.
bool Start();
bool Active();
// Android: appends "<tag>: <text>" as one line while the log runs. Elsewhere a
// no-op, since stdout and stderr already reach the file.
void Write(const char* tag, const char* text);
#if !defined(_WIN32)
// Async-signal-safe, for the crash report (port_crash.h): appends the bytes to the
// log as they are. On Android straight to the files, elsewhere to stderr.
void WriteRaw(const char* data, size_t size);
#endif
// The log's path, empty without a user folder.
std::string Path();
// Android: the shared copy's path (Documents/MetroidPrime), empty elsewhere or
// when the data folder was moved to shared storage. Opening it can still fail
// (Android 9-10 without the storage permission); the log's first lines say so.
std::string SharedPath();
#if defined(_WIN32)
// The copying process Start launches on Windows (`--log-copy <pipe> <file>
// <terminal> <ready>`, the handles' values, 0 for no terminal): signals the ready
// event, then copies the pipe into the file, redacted, and into the terminal as it
// is, until the game has gone.
int RunCopy(const char* pipe, const char* file, const char* terminal, const char* ready);
// The program is a GUI one (no console window of its own). Started from a
// terminal, with nothing redirected, it prints to that terminal instead.
void AttachParentConsole();
#endif

} // namespace PortLogFile

#endif // METROID_PRIME_PORT_PORT_LOG_FILE_H
