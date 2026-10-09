#pragma once

// The decompiled code uses the POSIX <alloca.h> header. MSVC has no such
// header: alloca lives in <malloc.h> there.
#ifdef _MSC_VER
#include <malloc.h>
#ifndef alloca
#define alloca _alloca
#endif
#else
#include_next <alloca.h>
#ifndef alloca
#define alloca __builtin_alloca
#endif
#endif
