// Keeps the binary startable on older glibc.
//
// Recent glibc gives the float math functions and the C23 strtol/scanf family
// new symbol versions, so a binary built against it refuses to start on an
// older distribution even though the functions themselves are ancient. Only a
// couple of translation units here end up needing them, but the result is a
// hard requirement on the build host's glibc.
//
// Defining the new names in terms of the long-standing double-precision ones
// (and the pre-C23 parsing functions) puts the requirement back to what the
// code actually uses. The C23 variants differ only in accepting 0b prefixes and
// binary-exponent hex floats, which nothing here parses.
#include <math.h>



#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// glibc only; other libcs (and Windows) do not version these symbols, and the
// double-precision names this relies on are glibc's too.
#if defined(__linux__) && defined(__GLIBC__)

// The double-precision functions keep their original symbol versions.
float acosf(float x) { return (float)acos(x); }
float acoshf(float x) { return (float)acosh(x); }
float asinf(float x) { return (float)asin(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float atanhf(float x) { return (float)atanh(x); }
float coshf(float x) { return (float)cosh(x); }
float log10f(float x) { return (float)log10(x); }
float sinhf(float x) { return (float)sinh(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }

// remainder() and hypot() are versioned too. remainder() is rebuilt from
// nearbyint(), which is not; hypot() is bound to its original version, since
// the later one only changed how it rounds.
double remainder(double x, double y) { return x - nearbyint(x / y) * y; }

// Both halves matter, or hypot() spins forever calling itself. No
// __asm__("hypot") label, unlike the parsers below: that names the wrapper
// itself. And the wrapper stays hidden: exported, it interposes libm, and the
// dynamic linker resolves the versioned reference right back to it.
// The original version is each architecture's glibc baseline.
extern double glibc_hypot(double, double);
#if defined(__aarch64__)
__asm__(".symver glibc_hypot,hypot@GLIBC_2.17");
#else
__asm__(".symver glibc_hypot,hypot@GLIBC_2.2.5");
#endif
__attribute__((visibility("hidden"))) double hypot(double x, double y) { return glibc_hypot(x, y); }

// Pre-C23 parsing, referenced by its original name so this does not recurse
// into the wrapper it is defining.
extern long glibc_strtol(const char*, char**, int) __asm__("strtol");
extern unsigned long glibc_strtoul(const char*, char**, int) __asm__("strtoul");
extern long long glibc_strtoll(const char*, char**, int) __asm__("strtoll");
extern unsigned long long glibc_strtoull(const char*, char**, int) __asm__("strtoull");
extern long glibc_wcstol(const wchar_t*, wchar_t**, int) __asm__("wcstol");
extern int glibc_vsscanf(const char*, const char*, va_list) __asm__("vsscanf");
extern int glibc_vfscanf(void*, const char*, va_list) __asm__("vfscanf");

long __isoc23_strtol(const char* s, char** end, int base) { return glibc_strtol(s, end, base); }
unsigned long __isoc23_strtoul(const char* s, char** end, int base) { return glibc_strtoul(s, end, base); }
long long __isoc23_strtoll(const char* s, char** end, int base) { return glibc_strtoll(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return glibc_strtoull(s, end, base); }
long __isoc23_wcstol(const wchar_t* s, wchar_t** end, int base) { return glibc_wcstol(s, end, base); }

int __isoc23_vsscanf(const char* s, const char* format, va_list args) { return glibc_vsscanf(s, format, args); }

int __isoc23_sscanf(const char* s, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = glibc_vsscanf(s, format, args);
  va_end(args);
  return result;
}

int __isoc23_fscanf(void* stream, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = glibc_vfscanf(stream, format, args);
  va_end(args);
  return result;
}

// strlcpy/strlcat and their wide versions arrived in glibc 2.38. These follow
// the usual BSD semantics: NUL-terminate when there is room and return the
// length the result would have had.
size_t strlcpy(char* dst, const char* src, size_t size) {
  const size_t length = strlen(src);
  if (size != 0) {
    const size_t copy = length < size - 1 ? length : size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return length;
}

size_t strlcat(char* dst, const char* src, size_t size) {
  size_t used = 0;
  while (used < size && dst[used] != '\0') {
    ++used;
  }
  const size_t length = strlen(src);
  if (used == size) {
    return size + length;
  }
  const size_t space = size - used - 1;
  const size_t copy = length < space ? length : space;
  memcpy(dst + used, src, copy);
  dst[used + copy] = '\0';
  return used + length;
}

size_t wcslcpy(wchar_t* dst, const wchar_t* src, size_t size) {
  const size_t length = wcslen(src);
  if (size != 0) {
    const size_t copy = length < size - 1 ? length : size - 1;
    wmemcpy(dst, src, copy);
    dst[copy] = L'\0';
  }
  return length;
}

size_t wcslcat(wchar_t* dst, const wchar_t* src, size_t size) {
  size_t used = 0;
  while (used < size && dst[used] != L'\0') {
    ++used;
  }
  const size_t length = wcslen(src);
  if (used == size) {
    return size + length;
  }
  const size_t space = size - used - 1;
  const size_t copy = length < space ? length : space;
  wmemcpy(dst + used, src, copy);
  dst[used + copy] = L'\0';
  return used + length;
}

#endif // __linux__ && __GLIBC__
