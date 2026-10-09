#pragma once

// The decompiled code uses the POSIX <alloca.h> header. MSVC has no such
// header: alloca lives in <malloc.h> there.
#ifdef _MSC_VER
#include <malloc.h>
#ifndef alloca
#define alloca _alloca
#endif
#elif defined(__APPLE__)
#include <stdlib.h> // alloca() is declared there
#else
#include_next <alloca.h>
#endif
