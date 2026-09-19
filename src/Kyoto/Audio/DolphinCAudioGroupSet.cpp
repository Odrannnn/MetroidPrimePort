#include "Kyoto/Audio/CAudioGroupSet.hpp"
#include "Kyoto/Basics/CBasics.hpp"

#include "Kyoto/Audio/CAudioSys.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include "musyx/synthdata.h"
#include "rstl/auto_ptr.hpp"
#include "rstl/vector.hpp"
#include <stdint.h>
#include <string.h>

#ifdef TARGET_PC
namespace {
// On PC MusyX has no ARAM copy: it reads samples and macros straight out of
// `x0_data`/`x8_groupData`, and the MusyX 2.0.0 `sndPopGroup` path does not
// reliably stop every voice that still references them. Retaining these buffers
// for the session keeps those pointers valid instead of leaving the audio thread
// reading freed (unmapped) memory.
rstl::vector< void* >& RetainedAudioGroupBuffers() {
  static rstl::vector< void* >* sBuffers = nullptr;
  if (sBuffers == nullptr) {
    sBuffers = rs_new rstl::vector< void* >();
  }
  return *sBuffers;
}

void RetainAudioGroupBuffer(void* buffer) {
  if (buffer != nullptr) {
    RetainedAudioGroupBuffers().push_back(buffer);
  }
}
} // namespace
#endif

#if TARGET_LITTLE_ENDIAN
namespace {
u16 ReadBigU16(const uchar* ptr) {
  u16 value;
  memcpy(&value, ptr, sizeof(value));
  return CBasics::SwapBytes(value);
}

u32 ReadBigU32(const uchar* ptr) {
  u32 value;
  memcpy(&value, ptr, sizeof(value));
  return CBasics::SwapBytes(value);
}

void WriteU16(uchar* ptr, u16 value) { memcpy(ptr, &value, sizeof(value)); }

void WriteU32(uchar* ptr, u32 value) { memcpy(ptr, &value, sizeof(value)); }

bool RecordOffset(uint* offsets, uint& count, uint capacity, uint offset) {
  for (uint i = 0; i < count; ++i) {
    if (offsets[i] == offset) {
      return false;
    }
  }
  if (count == capacity) {
    return false;
  }
  offsets[count++] = offset;
  return true;
}

void ConvertIdList(uchar* project, uint projectSize, uint offset,
                   uint* convertedLists, uint& convertedListCount) {
  if (offset >= projectSize ||
      !RecordOffset(convertedLists, convertedListCount, 512, offset)) {
    return;
  }

  while (offset + sizeof(u16) <= projectSize) {
    const u16 value = ReadBigU16(project + offset);
    WriteU16(project + offset, value);
    offset += sizeof(u16);
    if (value == 0xffff) {
      return;
    }
  }
}

void ConvertFxData(uchar* project, uint projectSize, uint offset,
                   uint* convertedFxData, uint& convertedFxDataCount) {
  if (offset + 4 > projectSize ||
      !RecordOffset(convertedFxData, convertedFxDataCount, 128, offset)) {
    return;
  }

  const u16 count = ReadBigU16(project + offset);
  WriteU16(project + offset, count);
  WriteU16(project + offset + 2, ReadBigU16(project + offset + 2));
  offset += 4;
  const uint available = (projectSize - offset) / sizeof(FX_TAB);
  const uint convertCount = count < available ? count : available;
  for (uint i = 0; i < convertCount; ++i, offset += sizeof(FX_TAB)) {
    WriteU16(project + offset, ReadBigU16(project + offset));
    WriteU16(project + offset + 2, ReadBigU16(project + offset + 2));
  }
}

void ConvertProject(uchar* project, uint projectSize) {
  uint convertedLists[512];
  uint convertedListCount = 0;
  uint convertedFxData[128];
  uint convertedFxDataCount = 0;
  uint offset = 0;

  for (uint groupCount = 0; groupCount < projectSize / 0x28; ++groupCount) {
    if (offset + 0x28 > projectSize) {
      return;
    }
    uchar* group = project + offset;
    const u32 nextOffset = ReadBigU32(group);
    const u16 type = ReadBigU16(group + 6);
    const u32 macroOffset = ReadBigU32(group + 8);
    const u32 sampleOffset = ReadBigU32(group + 0xc);
    const u32 curveOffset = ReadBigU32(group + 0x10);
    const u32 keymapOffset = ReadBigU32(group + 0x14);
    const u32 layerOffset = ReadBigU32(group + 0x18);
    const u32 normpageOffset = ReadBigU32(group + 0x1c);

    WriteU32(group, nextOffset);
    WriteU16(group + 4, ReadBigU16(group + 4));
    WriteU16(group + 6, type);
    for (uint field = 8; field < 0x28; field += sizeof(u32)) {
      WriteU32(group + field, ReadBigU32(group + field));
    }

    if (nextOffset == 0xffffffff) {
      return;
    }

    ConvertIdList(project, projectSize, macroOffset, convertedLists, convertedListCount);
    ConvertIdList(project, projectSize, sampleOffset, convertedLists, convertedListCount);
    ConvertIdList(project, projectSize, curveOffset, convertedLists, convertedListCount);
    ConvertIdList(project, projectSize, keymapOffset, convertedLists, convertedListCount);
    ConvertIdList(project, projectSize, layerOffset, convertedLists, convertedListCount);
    if (type == 1) {
      ConvertFxData(project, projectSize, normpageOffset, convertedFxData,
                    convertedFxDataCount);
    }
    if (nextOffset == offset) {
      return;
    }
    offset = nextOffset;
  }
}

void ConvertPoolChain(uchar* pool, uint poolSize, uint offset, uint type) {
  for (uint recordCount = 0; recordCount < poolSize / 8; ++recordCount) {
    if (offset + 8 > poolSize) {
      return;
    }
    uchar* record = pool + offset;
    const u32 nextOffset = ReadBigU32(record);
    WriteU32(record, nextOffset);

    if (nextOffset == 0xffffffff) {
      return;
    }
    if (nextOffset < 8 || nextOffset > poolSize - offset) {
      return;
    }
    WriteU16(record + 4, ReadBigU16(record + 4));
    WriteU16(record + 6, ReadBigU16(record + 6));

    const uint recordEnd = offset + nextOffset;
    if (type == 0) {
      for (uint pos = offset + 8; pos + sizeof(u32) <= recordEnd; pos += sizeof(u32)) {
        WriteU32(pool + pos, ReadBigU32(pool + pos));
      }
    } else if (type == 2) {
      for (uint i = 0, pos = offset + 8;
           i < 128 && pos + sizeof(KEYMAP) <= recordEnd; ++i, pos += sizeof(KEYMAP)) {
        WriteU16(pool + pos, ReadBigU16(pool + pos));
        WriteU16(pool + pos + 4, ReadBigU16(pool + pos + 4));
      }
    } else if (type == 3 && offset + 12 <= recordEnd) {
      const u32 count = ReadBigU32(pool + offset + 8);
      WriteU32(pool + offset + 8, count);
      for (uint i = 0, pos = offset + 12;
           i < count && pos + sizeof(LAYER) <= recordEnd; ++i, pos += sizeof(LAYER)) {
        WriteU16(pool + pos, ReadBigU16(pool + pos));
        WriteU16(pool + pos + 6, ReadBigU16(pool + pos + 6));
      }
    }
    offset += nextOffset;
  }
}

void ConvertPool(uchar* pool, uint poolSize) {
  if (poolSize < sizeof(POOL_DATA)) {
    return;
  }

  u32 offsets[4];
  for (uint i = 0; i < 4; ++i) {
    offsets[i] = ReadBigU32(pool + i * sizeof(u32));
    WriteU32(pool + i * sizeof(u32), offsets[i]);
  }

  for (uint i = 0; i < 4; ++i) {
    if (offsets[i] != 0) {
      ConvertPoolChain(pool, poolSize, offsets[i], i);
    }
  }
}

uint CountSampleDirEntries(const uchar* sampleDir, uint sampleDirSize) {
  const uint available = sampleDirSize / sizeof(SDIR_DATA_INTER);
  for (uint i = 0; i < available; ++i) {
    if (ReadBigU16(sampleDir + i * sizeof(SDIR_DATA_INTER)) == 0xffff) {
      return i + 1;
    }
  }
  return available + 1;
}

// Native `SDIR_DATA` is larger than the disc's `SDIR_DATA_INTER` (its `addr` is a
// host pointer), so the ADPCM info blocks that follow the disc entries have to be
// copied and converted, and each entry's `extraData` offset rebased onto the new
// entry array.
void ConvertAdpcmInfoBlocks(uchar* sampleDir, uint blocksOffset, uint blocksSize,
                            SDIR_DATA* entries, uint count, uint discEntryBytes) {
  uint converted[256];
  uint convertedCount = 0;
  for (uint i = 0; i < count; ++i) {
    const uint oldOffset = entries[i].extraData;
    if (oldOffset < discEntryBytes) {
      continue;
    }
    const uint newOffset = blocksOffset + (oldOffset - discEntryBytes);
    if (newOffset + 0x28 > blocksOffset + blocksSize) {
      continue;
    }
    entries[i].extraData = newOffset;
    if (!RecordOffset(converted, convertedCount, 256, newOffset)) {
      continue;
    }
    uchar* info = sampleDir + newOffset;
    WriteU16(info + 0x00, ReadBigU16(info + 0x00)); // numCoef
    WriteU16(info + 0x04, ReadBigU16(info + 0x04)); // loopY0
    WriteU16(info + 0x06, ReadBigU16(info + 0x06)); // loopY1
    for (uint c = 0; c < 16; ++c) {
      const uint at = 0x08 + c * 2;
      WriteU16(info + at, ReadBigU16(info + at)); // coefTab[8][2]
    }
  }
}

void ConvertSampleDir(SDIR_DATA* dest, uint destCount, const uchar* source, uint sourceSize) {
  const uint sourceCount = sourceSize / sizeof(SDIR_DATA_INTER);
  uint i = 0;
  for (; i < sourceCount && i < destCount; ++i) {
    const uchar* entry = source + i * sizeof(SDIR_DATA_INTER);
    dest[i].id = ReadBigU16(entry);
    dest[i].ref_cnt = ReadBigU16(entry + 2);
    dest[i].offset = ReadBigU32(entry + 4);
    dest[i].addr = reinterpret_cast< void* >(static_cast< uintptr_t >(ReadBigU32(entry + 8)));
    dest[i].header.info = ReadBigU32(entry + 0xc);
    dest[i].header.length = ReadBigU32(entry + 0x10);
    dest[i].header.loopOffset = ReadBigU32(entry + 0x14);
    dest[i].header.loopLength = ReadBigU32(entry + 0x18);
    dest[i].extraData = ReadBigU32(entry + 0x1c);
    if (dest[i].id == 0xffff) {
      return;
    }
  }

  if (i < destCount) {
    memset(&dest[i], 0, sizeof(dest[i]));
    dest[i].id = 0xffff;
  }
}
} // namespace
#endif

CAudioGroupSet::CAudioGroupSet(const TLockedToken< CAudioGrpSetLoc >& group)
: x0_baseDir(group->GetBaseDirName())
, x10_groupSetName(group->GetGroupSetName())
, x20_groupSetTok(group) {}

CAudioGroupSet::~CAudioGroupSet() {}

void CAudioGroupSet::Reload() {}

void CAudioGroupSet::FreeSampleBuffer() { x20_groupSetTok.data()->FreeSampleBuffer(); }

CAudioGrpSetLoc::CAudioGrpSetLoc(const rstl::auto_ptr< uchar >& data, int length)
: x0_data(data.release())
, x30_aramSize(0)
, x34_pool(nullptr)
, x38_project(nullptr)
, x3c_sampleDir(nullptr)
, x40_samples(nullptr) {
  uint readPosition;
  const uint poolSize = ReadHeader(data.get(), length, readPosition);
  CAudioSys::GetVerbose();

  const uint projectOffset = readPosition + poolSize;
#if TARGET_LITTLE_ENDIAN
  const uint projectSize = CBasics::SwapBytes(*reinterpret_cast< uint* >(data.get() + projectOffset));
#else
  const uint projectSize = *reinterpret_cast< uint* >(data.get() + projectOffset);
#endif
  CAudioSys::GetVerbose();

  const uint sampOffset = 4 + projectSize + projectOffset;
#if TARGET_LITTLE_ENDIAN
  const uint sampSize = CBasics::SwapBytes(*reinterpret_cast< uint* >(data.get() + sampOffset));
#else
  const uint sampSize = *reinterpret_cast< uint* >(data.get() + sampOffset);
#endif
  CAudioSys::GetVerbose();
  x30_aramSize = sampSize;

  const uint sdirOffset = 4 + sampOffset + sampSize;
#if TARGET_LITTLE_ENDIAN
  const uint sdirSize = CBasics::SwapBytes(*reinterpret_cast< uint* >(data.get() + sdirOffset));
#else
  const uint sdirSize = *reinterpret_cast< uint* >(data.get() + sdirOffset);
#endif
  CAudioSys::GetVerbose();

  uchar* ptr = x0_data.get();
#if TARGET_LITTLE_ENDIAN
  const uint roundedPoolSize = (poolSize + 3) & ~3;
  const uint projectEnd = roundedPoolSize + projectSize;
  const uint sampleDirOffset =
      (projectEnd + sizeof(void*) - 1) & ~(static_cast< uint >(sizeof(void*)) - 1);
  const uint sampleDirCount = CountSampleDirEntries(ptr + sdirOffset + 4, sdirSize);
  const uint entryBytes = sampleDirCount * sizeof(SDIR_DATA);
  // The disc sample directory ends with a 4-byte 0xFFFFFFFF terminator rather
  // than a full entry, so the trailing ADPCM info blocks start at
  // (count - 1) * entrySize + 4. Using count * entrySize skipped the first
  // block and left samples whose info begins there (e.g. the looping charge
  // layer) reading coefficients out of the entry table.
  const uint discEntryBytes =
      sampleDirCount > 0 ? (sampleDirCount - 1) * sizeof(SDIR_DATA_INTER) + 4 : 0;
  const uint blockBytes = sdirSize > discEntryBytes ? sdirSize - discEntryBytes : 0;
  x8_groupData = rstl::auto_ptr< uchar >(static_cast< uchar* >(
      CMemory::Alloc(sampleDirOffset + entryBytes + blockBytes, IAllocator::kHI_RoundUpLen)));
#else
  x8_groupData = rstl::auto_ptr< uchar >(static_cast< uchar* >(
      CMemory::Alloc(poolSize + projectSize + sdirSize + 8, IAllocator::kHI_RoundUpLen)));
#endif
  x34_pool = x8_groupData.get();
  memcpy(x34_pool, ptr + readPosition, poolSize);

#if !TARGET_LITTLE_ENDIAN
  const uint roundedPoolSize = ((poolSize + 3) & ~3);
#endif
  x38_project = x8_groupData.get() + roundedPoolSize;
  memcpy(x38_project, ptr + (projectOffset + 4), projectSize);

#if TARGET_LITTLE_ENDIAN
  x3c_sampleDir = x8_groupData.get() + sampleDirOffset;
  ConvertPool(x34_pool, poolSize);
  ConvertProject(x38_project, projectSize);
  ConvertSampleDir(reinterpret_cast< SDIR_DATA* >(x3c_sampleDir), sampleDirCount,
                   ptr + sdirOffset + 4, sdirSize);
  if (blockBytes != 0) {
    memcpy(x3c_sampleDir + entryBytes, ptr + (sdirOffset + 4) + discEntryBytes, blockBytes);
  }
  ConvertAdpcmInfoBlocks(x3c_sampleDir, entryBytes, blockBytes,
                         reinterpret_cast< SDIR_DATA* >(x3c_sampleDir), sampleDirCount,
                         discEntryBytes);
#else
  uint roundedProjectSize = ((projectSize + 3) & ~3);
  roundedProjectSize = roundedPoolSize + roundedProjectSize;
  x3c_sampleDir = x8_groupData.get() + roundedProjectSize;
  memcpy(x3c_sampleDir, ptr + (sdirOffset + 4), sdirSize);
#endif
  x40_samples = &ptr[sampOffset + 4];
}

CAudioGrpSetLoc::~CAudioGrpSetLoc() {
#ifdef TARGET_PC
  RetainAudioGroupBuffer(x8_groupData.release());
  RetainAudioGroupBuffer(x0_data.release());
#else
  CMemory::Free(x8_groupData.release());
#endif
}

void CAudioGrpSetLoc::FreeSampleBuffer() {
#ifdef TARGET_PC
  // On PC `hwSaveSample` never copies samples into ARAM, so MusyX reads sample
  // data directly from `x0_data` through pointers stored in the pushed sample
  // directory. Releasing the buffer here would leave those pointers dangling
  // while the group is still pushed; the buffer is retained at destruction.
#else
  x0_data = nullptr;
  x40_samples = nullptr;
#endif
}

template <>
CFactoryFnReturn::CFactoryFnReturn(CAudioGrpSetLoc* ptr)
: obj(TToken< CAudioGrpSetLoc >::GetIObjObjectFor(rstl::auto_ptr< CAudioGrpSetLoc >(ptr))
          .release()) {}

const CFactoryFnReturn FAudioGroupSetLocDataFactory(const SObjectTag& tag,
                                                    const rstl::auto_ptr< uchar >& data, int length,
                                                    const CVParamTransfer& xfer) {
  return rs_new CAudioGrpSetLoc(data, length);
}
