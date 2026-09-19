#include "Kyoto/Audio/CSfxHandle.hpp"
#include <Kyoto/Audio/CMidiManager.hpp>

#include "Kyoto/Audio/CAudioSys.hpp"

#include <Kyoto/Basics/CBasics.hpp>
#include <Kyoto/CFactoryFnReturn.hpp>
#include <Kyoto/Streams/CInputStream.hpp>

#include <stdint.h>
#include <string.h>

#if TARGET_LITTLE_ENDIAN
// MusyX song (ARR) data is authored big-endian; convert it in place so the
// host-side sequencer can consume it through its native struct types.
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

void ConvertTrackEntries(uchar* data, uint size, uint offset, uint& maxPattern) {
  for (uint guard = 0; guard < size / 0xc; ++guard) {
    if (offset + 0xc > size) {
      return;
    }
    uchar* entry = data + offset;
    WriteU32(entry, ReadBigU32(entry));
    const u16 pattern = ReadBigU16(entry + 8);
    WriteU16(entry + 8, pattern);
    if (pattern == 0xfffe) {
      WriteU16(entry + 0xa, ReadBigU16(entry + 0xa));
    } else if (pattern == 0xffff) {
      return;
    } else if (pattern > maxPattern) {
      maxPattern = pattern;
    }
    offset += 0xc;
  }
}

void ConvertNoteStream(uchar* data, uint size, uint offset, uint end) {
  if (end > size) {
    end = size;
  }
  while (offset + 4 <= end) {
    uchar* entry = data + offset;
    WriteU16(entry, ReadBigU16(entry));
    const uchar key = entry[2];
    const uchar velocity = entry[3];
    if (key == 0xff && velocity == 0xff) {
      return;
    }
    if ((key & 0x80) != 0 || (key | velocity) == 0) {
      offset += 4;
      continue;
    }
    if (offset + 6 > end) {
      return;
    }
    WriteU16(data + offset + 4, ReadBigU16(data + offset + 4));
    offset += 6;
  }
}

void ConvertArrData(uchar* data, uint size) {
  if (size < 0x14) {
    return;
  }

  const u32 tTab = ReadBigU32(data + 0x0);
  const u32 pTab = ReadBigU32(data + 0x4);
  const u32 tmTab = ReadBigU32(data + 0x8);
  const u32 mTrack = ReadBigU32(data + 0xc);
  const u32 info = ReadBigU32(data + 0x10);
  WriteU32(data + 0x0, tTab);
  WriteU32(data + 0x4, pTab);
  WriteU32(data + 0x8, tmTab);
  WriteU32(data + 0xc, mTrack);
  WriteU32(data + 0x10, info);

  uint maxPattern = 0;
  bool hasTracks = false;
  if (tTab + 64 * 4 <= size) {
    for (uint i = 0; i < 64; ++i) {
      const u32 trackOffset = ReadBigU32(data + tTab + 4 * i);
      WriteU32(data + tTab + 4 * i, trackOffset);
      if (trackOffset != 0 && trackOffset + 0xc <= size) {
        ConvertTrackEntries(data, size, trackOffset, maxPattern);
        hasTracks = true;
      }
    }
  }

  if (mTrack + 8 <= size) {
    uint offset = mTrack;
    for (uint guard = 0; guard < size / 8; ++guard) {
      if (offset + 8 > size) {
        break;
      }
      const u32 time = ReadBigU32(data + offset);
      WriteU32(data + offset, time);
      if (time == 0xffffffff) {
        break;
      }
      WriteU32(data + offset + 4, ReadBigU32(data + offset + 4));
      offset += 8;
    }
  }

  if (hasTracks && pTab + 4 * (maxPattern + 1) <= size) {
    for (uint i = 0; i <= maxPattern; ++i) {
      const u32 patternOffset = ReadBigU32(data + pTab + 4 * i);
      WriteU32(data + pTab + 4 * i, patternOffset);
      if (patternOffset == 0 || patternOffset + 0xc > size) {
        continue;
      }
      WriteU32(data + patternOffset, ReadBigU32(data + patternOffset));
      WriteU32(data + patternOffset + 4, ReadBigU32(data + patternOffset + 4));
      WriteU32(data + patternOffset + 8, ReadBigU32(data + patternOffset + 8));
      const uint end = i < maxPattern ? ReadBigU32(data + pTab + 4 * (i + 1)) : tmTab;
      ConvertNoteStream(data, size, patternOffset + 0xc, end);
    }
  }
}
} // namespace
#endif

rstl::reserved_vector< CMidiManager::CMidiWrapper, 3 > CMidiManager::mMidiWrappers;

CMidiManager::CMidiWrapper::CMidiWrapper() : x0_sysHandle(0), xa_available(true) {}

const CSfxHandle& CMidiManager::CMidiWrapper::GetManagerHandle() const { return x4_midiHandle; }

const u32 CMidiManager::CMidiWrapper::GetAudioSysHandle() const { return x0_sysHandle; }

const bool CMidiManager::CMidiWrapper::IsAvailable() const { return xa_available; }

const short CMidiManager::CMidiWrapper::GetSongId() const { return x8_songId; }

void CMidiManager::CMidiWrapper::SetAvailable(const bool v) { xa_available = v; }

void CMidiManager::CMidiWrapper::SetAudioSysHandle(const u32 handle) { x0_sysHandle = handle; }

void CMidiManager::CMidiWrapper::SetMidiHandle(const CSfxHandle& handle) { x4_midiHandle = handle; }

void CMidiManager::CMidiWrapper::SetSongId(const short id) { x8_songId = id; }

CSfxHandle CMidiManager::Play(const CMidiData& data, unsigned short fadeTime, bool stopExisting,
                              short volume) {
  bool foundExisting = false;
  u32 sysHandle = 0;
  CSfxHandle handle = LocateHandle();
  if (!handle) {
    return CSfxHandle();
  }
  CMidiWrapper& wrapper = mMidiWrappers[handle.GetIndex()];
  wrapper.SetAvailable(false);
  wrapper.SetMidiHandle(handle);
  if (stopExisting) {
    for (int i = 0; i < mMidiWrappers.size(); ++i) {
      if (mMidiWrappers[i].IsAvailable()) {
        continue;
      }

      if (data.GetSongId() == mMidiWrappers[i].GetSongId()) {
        foundExisting = true;
        sysHandle = mMidiWrappers[i].GetAudioSysHandle();
        mMidiWrappers[i].SetAvailable(true);
      } else {
        Stop(mMidiWrappers[i].GetManagerHandle(), fadeTime);
      }
    }
  }

  if (foundExisting) {
    wrapper.SetAudioSysHandle(sysHandle);
    wrapper.SetSongId(data.GetSongId());
  } else {
    u32 sysHandle = CAudioSys::SeqPlayEx(data.GetGroupId(), data.GetSongId(), data.GetData(), nullptr, 0);
    if (fadeTime != 0) {
      CAudioSys::SeqVolume(0, 0, sysHandle, 0);
    }
    CAudioSys::SeqVolume(volume, fadeTime, sysHandle, 0);
    wrapper.SetAudioSysHandle(sysHandle);
    wrapper.SetSongId(data.GetSongId());
  }

  return handle;
}

void CMidiManager::Stop(const CSfxHandle& handle, ushort fadeTime) {
  if (!handle) {
    return;
  }

  if (handle != mMidiWrappers[handle.GetIndex()].GetManagerHandle()) {
    return;
  }

  u32 sysHandle = mMidiWrappers[handle.GetIndex()].GetAudioSysHandle();
  if (fadeTime == 0) {
    CAudioSys::SeqStop(sysHandle);
  } else {
    CAudioSys::SeqVolume(0, fadeTime, sysHandle, 1);
  }

  mMidiWrappers[handle.GetIndex()].SetAvailable(true);
}

void CMidiManager::StopAll() {
  for (int i = 0; i < mMidiWrappers.size(); ++i) {
    if (!mMidiWrappers[i].IsAvailable()) {
      Stop(mMidiWrappers[i].GetManagerHandle(), 0);
    }
  }
}

CSfxHandle CMidiManager::LocateHandle() {
  for (int i = 0; i < mMidiWrappers.size(); ++i) {
    if (mMidiWrappers[i].IsAvailable()) {
      return CSfxHandle(i);
    }
  }

  if (mMidiWrappers.size() == mMidiWrappers.capacity()) {
    return CSfxHandle();
  }

  mMidiWrappers.push_back(CMidiWrapper());
  return CSfxHandle(mMidiWrappers.size() - 1);
}

CMidiManager::CMidiData::CMidiData(CInputStream& in)
: x0_songId(-1), x2_groupId(-1), x4_agscId(-1) {
  in.ReadLong();
  x0_songId = in.ReadLong();
  x2_groupId = in.ReadLong();
  x4_agscId = in.ReadLong();
  int len = in.ReadInt32();
  x8_data = rs_new uchar[len];
  in.Get(x8_data.get(), len);
#if TARGET_LITTLE_ENDIAN
  ConvertArrData(x8_data.get(), len);
#endif
}

const CFactoryFnReturn FMidiDataFactory(const SObjectTag& tag, CInputStream& in, const CVParamTransfer&) {
  return rs_new CMidiManager::CMidiData(in);
}
