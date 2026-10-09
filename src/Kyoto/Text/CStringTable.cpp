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
#include "port_disc.h"
#include "port_hints.h"
#include "port_pal_languages.h"

#include <Kyoto/CResFactory.hpp>
#include <rstl/auto_ptr.hpp>

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
// Each Remastered language code, and the PAL disc's section for it.
static const FourCC kPalSections[][2] = {
    {'EUFR', 'FREN'}, {'USFR', 'FREN'}, {'EUGE', 'GERM'},
    {'EUSP', 'SPAN'}, {'USSP', 'SPAN'}, {'EUIT', 'ITAL'},
};

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

void CStringTable::PortRemap(const std::vector< int >& from, const CStringTable* extra) {
  const int count = static_cast< int >(from.size());
  struct Remapper {
    const std::vector< int >& from;
    const CStringTable* extra;
    rstl::vector< rstl::vector< wchar_t > > Apply(const rstl::vector< rstl::vector< wchar_t > >& src,
                                                  uint language) const {
      const rstl::vector< rstl::vector< wchar_t > >* other = NULL;
      if (extra != NULL) {
        other = &extra->mNativeStrings;
        for (size_t i = 0; i < extra->mPortSections.size(); ++i) {
          if (extra->mPortSections[i].language == language) {
            other = &extra->mPortSections[i].strings;
          }
        }
      }
      rstl::vector< rstl::vector< wchar_t > > out;
      out.reserve(from.size());
      for (size_t i = 0; i < from.size(); ++i) {
        const int f = from[i];
        if (f >= 0 && f < static_cast< int >(src.size())) {
          out.push_back(src[f]);
        } else if (f < 0 && other != NULL && -f - 1 < static_cast< int >(other->size())) {
          out.push_back((*other)[-f - 1]);
        } else {
          out.push_back(rstl::vector< wchar_t >(1, L'\0'));
        }
      }
      return out;
    }
  };
  const Remapper remap = {from, extra};
  mNativeStrings = remap.Apply(mNativeStrings, 'ENGL');
  for (size_t i = 0; i < mPortSections.size(); ++i) {
    mPortSections[i].strings = remap.Apply(mPortSections[i].strings, mPortSections[i].language);
  }
  x0_stringCount = count;
}

void CStringTable::PortAddLanguages(const CStringTable& other) {
  // A remapped PAL table may keep strings past 1.00's end (STRG_PauseScreen's
  // instruction panes), which a 1.00 disc doesn't use.
  if (other.x0_stringCount < x0_stringCount) {
    return;
  }
  for (size_t i = 0; i < other.mPortSections.size(); ++i) {
    bool have = false;
    for (size_t j = 0; j < mPortSections.size(); ++j) {
      have = have || mPortSections[j].language == other.mPortSections[i].language;
    }
    if (!have) {
      mPortSections.push_back(other.mPortSections[i]);
      mPortSections.back().strings.resize(x0_stringCount);
    }
  }
  // The Remastered import leaves a string it has no translation for as the
  // disc's (English) one.
  for (size_t i = 0; i < mPortSections.size(); ++i) {
    const rstl::vector< rstl::vector< wchar_t > >* source = NULL;
    for (size_t k = 0; k < sizeof(kPalSections) / sizeof(kPalSections[0]); ++k) {
      if (kPalSections[k][0] != mPortSections[i].language) {
        continue;
      }
      for (size_t j = 0; j < other.mPortSections.size(); ++j) {
        if (other.mPortSections[j].language == kPalSections[k][1]) {
          source = &other.mPortSections[j].strings;
        }
      }
    }
    if (source == NULL) {
      continue;
    }
    rstl::vector< rstl::vector< wchar_t > >& strings = mPortSections[i].strings;
    for (int s = 0; s < x0_stringCount; ++s) {
      const rstl::vector< wchar_t >& native = mNativeStrings[s];
      if (strings[s].size() == native.size() &&
          std::equal(native.begin(), native.end(), strings[s].begin())) {
        strings[s] = (*source)[s];
      }
    }
  }
}

// A PAL disc's string tables that the 1.00 code indexes by number are laid
// out differently (PAL added a language menu, moved the image gallery's
// labels into STRG_SlideShow and added inventory counters), so they're put
// back in 1.00's order. `slideShow` is the PAL STRG_SlideShow, for STRG_Main.
static void PortRemapPalTable(uint id, CStringTable& table, const CStringTable* slideShow) {
  std::vector< int > from;
  const CStringTable* extra = NULL;
  if (id == 0x0552A456 && table.GetStringCount() == 104) {  // STRG_Main
    extra = slideShow;
    for (int i = 0; i < 110; ++i) {
      if (i <= 38) {
        from.push_back(i);  // HUD, visors, dialogs, log book
      } else if (i <= 54) {
        from.push_back(i + 1);  // file select, map legend; PAL 39 = 'Select Language'
      } else if (i <= 60) {
        from.push_back(-(i - 55 + 4) - 1);  // gallery: Legend .. Reset View
      } else if (i <= 62) {
        from.push_back(56);  // Exit
      } else {
        from.push_back(i - 6);
      }
    }
  } else if (id == 0x500EC6A0 && table.GetStringCount() == 110) {  // STRG_PauseScreen
    for (int i = 0; i < 100; ++i) {
      // PAL adds the item/scan counters at 9-10 and an empty string at 27.
      from.push_back(i <= 8 ? i : i <= 24 ? i + 2 : i + 3);
    }
    // Kept past 1.00's end: NEXT, EXIT, BACK for PAL's instruction text panes
    // (CPauseScreen; 1.00 draws them as a model).
    from.push_back(105);
    from.push_back(103);
    from.push_back(104);
  } else if (id == 0x19C3F7F7 && table.GetStringCount() == 29) {  // STRG_MemoryCard
    // PAL's card-full texts for a save over an existing one are empty.
    for (int i = 0; i < 29; ++i) {
      from.push_back(i == 9 || i == 10 ? 6 : i);
    }
  } else {
    return;
  }
  table.PortRemap(from, extra);
}

static rstl::single_ptr< CStringTable > PortLoadDiscSlideShow() {
  rstl::single_ptr< CStringTable > slideShow;
  const SObjectTag slideTag('STRG', 0xBD727D06);  // STRG_SlideShow
  if (gpResourceFactory->GetResLoader().ResourceExists(slideTag)) {
    rstl::auto_ptr< CInputStream > in = gpResourceFactory->GetResLoader().LoadNewResourceSync(slideTag, NULL);
    if (in.get() != NULL) {
      slideShow = rs_new CStringTable(*in);
    }
  }
  return slideShow;
}

// A PAL disc's version of STRG `id`, imported by PortPalLanguages, for its
// languages; null when there is none.
static rstl::single_ptr< CStringTable > PortLoadLanguageTable(uint id) {
  rstl::single_ptr< CStringTable > table;
  std::vector< uint8_t > data;
  if (!PortPalLanguages::ReadTable(id, data) || data.size() < 16) {
    return table;
  }
  CMemoryInStream in(data.data(), data.size());
  table = rs_new CStringTable(in);
  return table;
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
  const uint id = tag.GetId();
  if (PortDisc::Current() == PortDisc::Version::Pal) {
    const rstl::single_ptr< CStringTable > slideShow(id == 0x0552A456 ? PortLoadDiscSlideShow()
                                                                       : rstl::single_ptr< CStringTable >());
    PortRemapPalTable(id, *table, slideShow.get());
  } else {
    // Languages added from a PAL disc.
    rstl::single_ptr< CStringTable > pal = PortLoadLanguageTable(id);
    if (pal.get() != NULL) {
      const rstl::single_ptr< CStringTable > slideShow(
          id == 0x0552A456 ? PortLoadLanguageTable(0xBD727D06) : rstl::single_ptr< CStringTable >());
      PortRemapPalTable(id, *pal, slideShow.get());
      table->PortAddLanguages(*pal);
    }
  }
  // The Artifact Temple totems say where a randomized seed put each artifact,
  // and a randomized pickup's scan what it holds.
  if (PortHints::IsWatched(id)) {
    table->PortWatch(id);
  }
  // An Archipelago seed's elevator texts and temple objective.
  std::vector< std::string > seedStrings;
  if (PortAp::SeedStrings(id, seedStrings)) {
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
  if (id == 0x95019A7A && PortAp::SeedResultsLine(resultsLine)) {
    const std::u16string text = PortCustomRes::Utf16(resultsLine + "\nPercentage Complete");
    table->PortSetString(1, reinterpret_cast< const unsigned short* >(text.data()),
                         static_cast< int >(text.size()));
  }
  return table;
#else
  return rs_new CStringTable(in);
#endif
}
