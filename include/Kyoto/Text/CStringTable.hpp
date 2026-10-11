#ifndef _CSTRINGTABLE
#define _CSTRINGTABLE

#include <stdint.h>

#include "types.h"

#include <rstl/single_ptr.hpp>
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
#include "rstl/vector.hpp"
#endif

#include <Kyoto/CFactoryFnReturn.hpp>

#ifdef TARGET_PC
#include <string>
#include <vector>
#endif

class CInputStream;
class CStringTable {
  int x0_stringCount;
  rstl::single_ptr< uchar[] > x4_data;
#if TARGET_LITTLE_ENDIAN || WCHAR_MAX > 0xffff
  rstl::vector< rstl::vector< wchar_t > > mNativeStrings;
#endif
#ifdef TARGET_PC
  // The other language sections, decoded too, so the language can change
  // while the game runs (mNativeStrings holds the first, English).
  struct PortSection {
    uint language;
    rstl::vector< rstl::vector< wchar_t > > strings;
  };
  std::vector< PortSection > mPortSections;
  // The section PortDebug::TextLanguage() names, else English.
  const rstl::vector< rstl::vector< wchar_t > >& PortStrings() const;
  // A randomizer STRG (Artifact Temple totem, randomized pickup scan): its
  // last string follows PortHints, whose text can arrive (or change) after
  // the table loaded.
  uint mPortWatchedId = 0;
  std::u16string mPortWatchedText;
  void PortRefreshWatched();
#endif

public:
  CStringTable(CInputStream& in);
  const wchar_t* GetString(int idx) const;
#ifdef TARGET_PC
  // Replaces string idx with UTF-16 text (no terminator).
  void PortSetString(int idx, const unsigned short* text, int length);
  // Makes the table hold `count` empty strings.
  void PortSetCount(int count);
  // Makes this table the watched STRG strgId (see mPortWatchedId).
  void PortWatch(uint strgId);
  // Rebuilds the table so string i is old string from[i] in every language;
  // a negative entry -(k + 1) takes string k of `extra` (same language).
  void PortRemap(const std::vector< int >& from, const CStringTable* extra);
  // Takes the languages of `other` (another region's version of this table, in
  // this one's order) that this table lacks, and fills the strings a
  // Remastered language section left in English from the matching one.
  void PortAddLanguages(const CStringTable& other);
  // Rewrites imported PAL-only TXTR ids in every decoded language section.
  void PortRemapImportedImageTextureIds();
#endif
  int GetStringCount() const { return x0_stringCount; }
};

extern CStringTable* gpStringTable;

const CFactoryFnReturn FStringTableFactory(const SObjectTag& tag, CInputStream& in,
                                     const CVParamTransfer& xfer);

#endif // _CSTRINGTABLE
