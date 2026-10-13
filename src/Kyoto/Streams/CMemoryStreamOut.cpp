#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "Kyoto/Alloc/CMemory.hpp"

#include <string.h>

#ifdef TARGET_PC
#include "dolphin/os.h"
#endif

CMemoryStreamOut::CMemoryStreamOut(void* buffer, size_t len, EOwnerShip ownerShip, int blockLen)
: COutputStream(blockLen)
, mOutPtr(buffer)
, mOutLength(len)
, mCurrentPosition(0)
, mBufferOwned(ownerShip == kOS_Owned) {}

CMemoryStreamOut::~CMemoryStreamOut() {
  COutputStream::Flush();

  if (mBufferOwned) {
    delete[] static_cast<uchar*>(mOutPtr);
  }
}


void CMemoryStreamOut::Write(const void* ptr, size_t len) {
#ifdef TARGET_PC
  static bool sReported = false;
  if (!sReported && len > mOutLength - mCurrentPosition) {
    sReported = true;
    OSReport("CMemoryStreamOut: write truncated (%u bytes into a %u byte buffer)\n",
             static_cast<unsigned>(len), static_cast<unsigned>(mOutLength));
  }
#endif
  len = (mOutLength - mCurrentPosition) < len ? (mOutLength - mCurrentPosition) : len;
  if (len != 0) {
    memcpy(static_cast<uchar*>(mOutPtr) + mCurrentPosition, ptr, len);
    mCurrentPosition += len;
  }
}
