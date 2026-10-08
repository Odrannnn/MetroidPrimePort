#include "Kyoto/Text/CStringTable.hpp"

#include <Kyoto/Streams/CInputStream.hpp>
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include <string.h>
#endif

#ifdef TARGET_PC
#include "port_apclient.h"
#include "port_custom_res.h"
#include "port_debug.h"
#include "port_hints.h"

#include <algorithm>
#include <string>
#include <vector>
#endif

#include <rstl/pair.hpp>
#include <rstl/vector.hpp>

static FourCC mCurrentLanguage = 'ENGL';
static const wchar_t skInvalidString[] = L"Invalid";

#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
// Decodes one language section's strings into out; false if its offsets
// don't fit the section.
static bool DecodeSection(const rstl::vector< uchar >& data, int count,
                          rstl::vector< rstl::vector< wchar_t > >& out) {
  const uint dataLen = data.size();
  if (count < 0 || static_cast< uint >(count) > dataLen / sizeof(uint)) {
    return false;
  }

  CMemoryInStream offsets(data.data(), count * sizeof(uint));
  out.reserve(count);
  for (int i = 0; i < count; ++i) {
    uint pos = offsets.ReadLong();
    rstl::vector< wchar_t > text;
    bool terminated = false;
    while (pos <= dataLen && dataLen - pos >= sizeof(ushort)) {
      ushort unit;
      memcpy(&unit, data.data() + pos, sizeof(unit));
      uint codepoint = CBasics::SwapBytes(unit);
      pos += sizeof(unit);
      if (codepoint == 0) {
        terminated = true;
        break;
      }

      // Preserve UTF-16 on 16-bit wchar_t hosts; combine surrogate pairs on
      // hosts whose wide characters can hold a complete Unicode code point.
      if (sizeof(wchar_t) > sizeof(ushort) && codepoint >= 0xd800 && codepoint <= 0xdbff &&
          dataLen - pos >= sizeof(ushort)) {
        memcpy(&unit, data.data() + pos, sizeof(unit));
        const ushort low = CBasics::SwapBytes(unit);
        if (low >= 0xdc00 && low <= 0xdfff) {
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
          pos += sizeof(unit);
        }
      }
      text.push_back(static_cast< wchar_t >(codepoint));
    }
    if (!terminated) {
      text.clear();
      for (const wchar_t* c = skInvalidString; *c; ++c) {
        text.push_back(*c);
      }
    }
    text.push_back(0);
    out.push_back(text);
  }
  return true;
}
#endif

CStringTable::CStringTable(CInputStream& in) : x0_stringCount(0), x4_data(NULL) {
  in.ReadLong();
  in.ReadLong();
  int langCount = in.Get(TType< int >());
  x0_stringCount = in.Get(TType< uint >());
  rstl::vector< rstl::pair< FourCC, uint > > langOffsets(langCount);
  for (int i = 0; i < langCount; ++i) {
    langOffsets.push_back(in.Get(TType< rstl::pair< FourCC, uint > >()));
  }

#ifdef TARGET_PC
  // Every section is decoded, so a language change shows in tables that are
  // already loaded. The first one listed is the fallback.
  std::vector< int > order;
  for (int i = 0; i < langCount; ++i) {
    order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return langOffsets[a].second < langOffsets[b].second;
  });
  uint pos = 0;
  bool haveFirst = false;
  for (int i : order) {
    const uint offset = langOffsets[i].second;
    if (offset < pos) {
      continue;
    }
    for (; pos < offset; ++pos) {
      in.ReadChar();
    }
    const uint dataLen = in.Get(TType< uint >());
    rstl::vector< uchar > data(dataLen, uchar(0));
    in.ReadBytes(data.data(), dataLen);
    pos += sizeof(uint) + dataLen;
    if (i == 0) {
      haveFirst = DecodeSection(data, x0_stringCount, mNativeStrings);
    } else {
      PortSection section;
      section.language = langOffsets[i].first;
      if (DecodeSection(data, x0_stringCount, section.strings)) {
        mPortSections.push_back(section);
      }
    }
  }
  if (!haveFirst) {
    x0_stringCount = 0;
    mNativeStrings.clear();
    mPortSections.clear();
  }
#else
  int offset = langOffsets.front().second;
  for (int i = 0; i < langCount; ++i) {
    if (langOffsets[i].first == mCurrentLanguage) {
      offset = langOffsets[i].second;
      break;
    }
  }
  for (uint i = 0; i < offset; ++i) {
    in.ReadChar();
  }

  uint dataLen = in.Get(TType< uint >());
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
  rstl::vector< uchar > data(dataLen, uchar(0));
  in.ReadBytes(data.data(), dataLen);
  if (!DecodeSection(data, x0_stringCount, mNativeStrings)) {
    x0_stringCount = 0;
  }
#else
  x4_data = rs_new uchar[dataLen];
  in.ReadBytes(x4_data.get(), dataLen);
#endif
#endif
}

#ifdef TARGET_PC
const rstl::vector< rstl::vector< wchar_t > >& CStringTable::PortStrings() const {
  const char* code = PortDebug::TextLanguage();
  if (code[0] != '\0') {
    const FourCC language =
        uint(uchar(code[0])) << 24 | uint(uchar(code[1])) << 16 | uint(uchar(code[2])) << 8 | uint(uchar(code[3]));
    for (size_t i = 0; i < mPortSections.size(); ++i) {
      if (mPortSections[i].language == language) {
        return mPortSections[i].strings;
      }
    }
    // A PAL disc's own sections, for a language a Remastered import doesn't
    // add to this table.
    static const FourCC kPalSections[][2] = {
        {'EUFR', 'FREN'}, {'USFR', 'FREN'}, {'EUGE', 'GERM'},
        {'EUSP', 'SPAN'}, {'USSP', 'SPAN'}, {'EUIT', 'ITAL'},
    };
    for (size_t k = 0; k < sizeof(kPalSections) / sizeof(kPalSections[0]); ++k) {
      if (kPalSections[k][0] != language) {
        continue;
      }
      for (size_t i = 0; i < mPortSections.size(); ++i) {
        if (mPortSections[i].language == kPalSections[k][1]) {
          return mPortSections[i].strings;
        }
      }
    }
  }
  return mNativeStrings;
}
#endif

const wchar_t* CStringTable::GetString(int idx) const {
  if (idx < 0 || idx >= x0_stringCount) {
    return skInvalidString;
  }
#ifdef TARGET_PC
  if (mPortWatchedId != 0 && idx == x0_stringCount - 1) {
    // Tables are heap objects from the factory, never const-defined.
    const_cast< CStringTable* >(this)->PortRefreshWatched();
  }
#endif
#ifdef TARGET_PC
  return PortStrings()[idx].data();
#elif TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
  return mNativeStrings[idx].data();
#else
  int offset = *(reinterpret_cast< const int* >(x4_data.get()) + idx);
  return reinterpret_cast< const wchar_t* >(x4_data.get() + offset);
#endif
}

#ifdef TARGET_PC
void CStringTable::PortSetString(int idx, const unsigned short* text, int length) {
  if (idx < 0 || idx >= x0_stringCount) {
    return;
  }
  rstl::vector< wchar_t > native;
  for (int i = 0; i < length; ++i) {
    uint codepoint = text[i];
    // Same as the loader: whole code points where wchar_t holds them.
    if (sizeof(wchar_t) > sizeof(ushort) && codepoint >= 0xd800 && codepoint <= 0xdbff &&
        i + 1 < length && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
      codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + text[i + 1] - 0xdc00;
      ++i;
    }
    native.push_back(static_cast< wchar_t >(codepoint));
  }
  native.push_back(0);
  // Every language says the same.
  mNativeStrings[idx] = native;
  for (size_t i = 0; i < mPortSections.size(); ++i) {
    mPortSections[i].strings[idx] = native;
  }
}

void CStringTable::PortSetCount(int count) {
  mPortSections.clear();
  mNativeStrings.clear();
  mNativeStrings.reserve(count);
  for (int i = 0; i < count; ++i) {
    mNativeStrings.push_back(rstl::vector< wchar_t >(1, L'\0'));
  }
  x0_stringCount = count;
}

void CStringTable::PortWatch(uint strgId) {
  mPortWatchedId = strgId;
  PortRefreshWatched();
}

void CStringTable::PortRefreshWatched() {
  std::u16string text;
  // No text (yet, or after a disconnect) keeps whatever the string last said.
  if (x0_stringCount <= 0 || !PortHints::WatchedText(mPortWatchedId, text) ||
      text == mPortWatchedText) {
    return;
  }
  mPortWatchedText = text;
  PortSetString(x0_stringCount - 1, reinterpret_cast< const unsigned short* >(text.data()),
                static_cast< int >(text.size()));
}
#endif

const CFactoryFnReturn FStringTableFactory(const SObjectTag& tag, CInputStream& in,
                                     const CVParamTransfer& xfer) {
#ifdef TARGET_PC
  CStringTable* table = rs_new CStringTable(in);
  // The Artifact Temple totems say where a randomized seed put each artifact,
  // and a randomized pickup's scan what it holds.
  if (PortHints::IsWatched(tag.GetId())) {
    table->PortWatch(tag.GetId());
  }
  // An Archipelago seed's elevator texts and temple objective.
  std::vector< std::string > seedStrings;
  if (PortAp::SeedStrings(tag.GetId(), seedStrings)) {
    table->PortSetCount(static_cast< int >(seedStrings.size()));
    for (size_t i = 0; i < seedStrings.size(); ++i) {
      const std::u16string text = PortCustomRes::Utf16(seedStrings[i]);
      table->PortSetString(static_cast< int >(i),
                           reinterpret_cast< const unsigned short* >(text.data()),
                           static_cast< int >(text.size()));
    }
  }
  // The completion screen names the seed (STRG_CompletionScreen, string 1).
  std::string resultsLine;
  if (tag.GetId() == 0x95019A7A && PortAp::SeedResultsLine(resultsLine)) {
    const std::u16string text = PortCustomRes::Utf16(resultsLine + "\nPercentage Complete");
    table->PortSetString(1, reinterpret_cast< const unsigned short* >(text.data()),
                         static_cast< int >(text.size()));
  }
  return table;
#else
  return rs_new CStringTable(in);
#endif
}
