#pragma once

// Aurora folds the ARAM queue API into dolphin/ar.h (types, macros, ARQInit,
// ARQPostRequest, ...). The decompiled code includes the separate arq.h the
// original SDK shipped, so re-export it here.
#include <dolphin/ar.h>
