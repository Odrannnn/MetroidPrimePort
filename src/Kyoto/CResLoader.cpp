#include "Kyoto/CResLoader.hpp"
#include "Kyoto/CPakFile.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CZipInputStream.hpp"
#include "rstl/StringExtras.hpp"

#ifdef TARGET_PC
#include "Kyoto/CDvdRequest.hpp"
#include "port_ap_world.h"
#include "port_custom_res.h"
#include "port_disc.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_prompts.h"
#include "port_skip_cutscenes.h"

#include <dolphin/ar.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#endif

static inline int align_size(const int size) { return (size + 31) & ~31; }

#ifdef TARGET_PC
namespace {
// A custom resource is already in memory, so its "read" is done at once.
class CPortReadyDvdRequest : public CDvdRequest {
public:
  void WaitUntilComplete() override {}
  bool IsComplete() override { return true; }
  void PostCancelRequest() override {}
  int GetMediaType() const override { return 1; }
};

void PortCopyCustom(const PortCustomRes::Resource& custom, int offset, int length, void* dest) {
  if (length <= 0)
    return;
  // Past the end reads as zeros, like the padding of a 32-byte aligned read.
  const int size = static_cast< int >(custom.data.size());
  const int start = offset < 0 || offset > size ? size : offset;
  const int copied = length < size - start ? length : size - start;
  memcpy(dest, custom.data.data() + start, copied);
  memset(static_cast< char* >(dest) + copied, 0, length - copied);
}

// A resource's bytes as stored in a PAK, decompressed into `out`.
bool PortInflate(const char* buf, int len, bool compressed, std::vector< uint8_t >& out) {
  if (!compressed) {
    out.assign(buf, buf + len);
    return true;
  }
  if (len < 4)
    return false;
  // Compressed resources start with their inflated size.
  const uchar* b = reinterpret_cast< const uchar* >(buf);
  out.resize((uint(b[0]) << 24) | (uint(b[1]) << 16) | (uint(b[2]) << 8) | b[3]);
  try {
    CZipInputStream zip(rstl::auto_ptr< CInputStream >(
        rs_new CMemoryInStream(buf + 4, len - 4, CMemoryInStream::kOS_NotOwned)));
    zip.Get(out.data(), out.size());
  } catch (...) {
    return false; // truncated or corrupt stream
  }
  return true;
}

struct PortDiscResource {
  s32 entry;
  uint type, offset, size;
  bool compressed;
};

std::mutex sDiscIndexMutex;
std::map< uint, PortDiscResource > sDiscIndex;
bool sDiscIndexBuilt = false;

// Disc reads PortCustomResource keeps for the run (until PortForgetDiscReads).
std::mutex sCopiesMutex;
std::map< uint, std::unique_ptr< PortCustomRes::Resource > > sCopies;

// Where each resource sits in the disc's PAKs, loaded or not; built on first use.
const std::map< uint, PortDiscResource >& PortDiscIndex() {
  std::lock_guard< std::mutex > lock(sDiscIndexMutex);
  std::map< uint, PortDiscResource >& index = sDiscIndex;
  if (!sDiscIndexBuilt) {
    sDiscIndexBuilt = true;
    for (const std::pair< int32_t, std::string >& pak : PortMods::DiscPaks()) {
      DVDFileInfo file;
      if (!DVDFastOpen(pak.first, &file))
        continue;
      const size_t length = file.length;
      std::vector< uint8_t > header;
      PortMods::PakTable table;
      for (size_t want = 64 * 1024;;) {
        want = want < length ? want : length;
        header.resize(want);
        if (DVDReadPrio(&file, header.data(), s32(want), 0, 2) != s32(want))
          break;
        size_t needed = 0;
        if (PortMods::ParsePakTable(header.data(), header.size(), table, needed)) {
          for (size_t i = 0; i < table.resources.size(); ++i) {
            const PortMods::PakResource& res = table.resources[i];
            if (res.offset <= length && res.size <= length - res.offset)
              index.insert(std::make_pair(res.id, PortDiscResource{pak.first, res.type, res.offset,
                                                                   res.size, res.compressed != 0}));
          }
          break;
        }
        if (needed == 0 || needed > length || want >= length)
          break;
        want = needed > want * 2 ? needed : want * 2;
      }
      DVDClose(&file);
    }
  }
  return index;
}

// The disc's PAKs include the mods' (extra PAKs, resources added to world PAKs), so a mod reload
// changes them: what was read or indexed before may be gone (a Remastered effect's child, once
// Remastered is off) or moved.
void PortForgetDiscReads() {
  {
    std::lock_guard< std::mutex > lock(sDiscIndexMutex);
    sDiscIndex.clear();
    sDiscIndexBuilt = false;
  }
  std::lock_guard< std::mutex > lock(sCopiesMutex);
  sCopies.clear();
}

// A resource read straight from its PAK on the disc, decompressed.
bool PortReadDisc(uint id, std::vector< uint8_t >& out, uint* type = nullptr) {
  const std::map< uint, PortDiscResource >& index = PortDiscIndex();
  const std::map< uint, PortDiscResource >::const_iterator found = index.find(id);
  if (found == index.end())
    return false;
  const PortDiscResource& res = found->second;
  DVDFileInfo file;
  if (!DVDFastOpen(res.entry, &file))
    return false;
  std::vector< char > raw(res.size);
  const bool read = res.size == 0 ||
                    DVDReadPrio(&file, raw.data(), s32(res.size), s32(res.offset), 2) == s32(res.size);
  DVDClose(&file);
  if (!read || !PortInflate(raw.data(), int(raw.size()), res.compressed, out))
    return false;
  if (type)
    *type = res.type;
  return true;
}
} // namespace

const PortCustomRes::Resource* CResLoader::PortCustomResource(const CAssetId asset, const bool load) {
  const bool custom = PortCustomRes::IsCustomId(asset);
  if ((!custom && !load && !PortSkipCutscenes::IsPickupDependency(asset) &&
       !PortApWorld::IsDoorDependency(asset)) ||
      PortPakResourceExists(asset))
    return nullptr;
  if (custom) {
    // Sources come from a loaded PAK when one has them, else from the disc.
    return PortCustomRes::Find(asset, [this](uint32_t id, std::vector< uint8_t >& out) {
      if (!PortPakResourceExists(id))
        return PortReadDisc(id, out);
      const CPakFile::SResInfo* info = x50_cachedResInfo;
      const bool compressed = info->IsCompressed();
      char* buf = nullptr;
      int len = 0;
      LoadMemResourceSync(SObjectTag(info->GetType(), id), &buf, &len);
      const bool ok = PortInflate(buf, len, compressed, out);
      delete[] buf;
      return ok;
    });
  }

  // A pickup model's texture, skin or animation, or a door type's shield, from another world's PAK
  // (randomprime copies these into the room's PAK instead). Also any load of an id no loaded PAK
  // has, which retail never asks for but a mod's resource can (an imported Remastered effect's
  // child from another world): it crashed on a null PAK. Kept for the run; a later load of that
  // PAK is found first.
  std::lock_guard< std::mutex > lock(sCopiesMutex);
  std::map< uint, std::unique_ptr< PortCustomRes::Resource > >::iterator found = sCopies.find(asset);
  if (found != sCopies.end())
    return found->second.get();
  std::unique_ptr< PortCustomRes::Resource > copy(new PortCustomRes::Resource);
  if (!PortReadDisc(asset, copy->data, &copy->type))
    copy.reset();
  return (sCopies[asset] = std::move(copy)).get();
}
#endif

CResLoader::CResLoader()
: x48_curPak(x18_pakLoadedList.end())
, x4c_cachedResId(kInvalidAssetId)
, x50_cachedResInfo(nullptr)
, x54_forwardSeek(false) {}

CResLoader::~CResLoader() {
  for (AUTO(it, x30_pakLoadingList.begin()); it != x30_pakLoadingList.end(); ++it) {
    CPakFile* pak = it->get();
    while (!pak->IsCompletelyLoaded()) {
      pak->AsyncIdle();
    }
  }
}

void CResLoader::MoveToCorrectLoadedList(const rstl::auto_ptr< CPakFile >& pak) {
  if (pak->IsARAMPak()) {
    x0_aramList.push_back(pak);
  } else {
    x18_pakLoadedList.push_back(pak);
  }
}

#ifdef TARGET_PC
namespace {
// Trilogy's RS5.pak, mounted ahead of the rest, holds Wii-remote art under the
// ids of the GameCube button textures (the PlayerRes tweak and the strings'
// &image= tags name the same ids). Looking past it for these finds the GC art
// in Strings/NoARAM/GGuiSys, which port_prompts re-icons for the active input
// as on a GC disc (#220). RS5's other assets (beam/visor select, scan rings)
// stay.
bool PortSkipRS5Button(const CPakFile& pak, const CAssetId asset) {
  if (!PortPrompts::IsPromptAsset(asset))
    return false;
  if (!PortDisc::IsTrilogy(PortDisc::Current()))
    return false;
  const char* name = pak.GetDvdFile().GetFilename().data();
  // "aram:RS5.pak"
  const char* base = strrchr(name, ':');
  base = base ? base + 1 : name;
  constexpr std::string_view rs5 = "RS5.";
  return std::strlen(base) >= rs5.size() &&
         std::equal(rs5.begin(), rs5.end(), base, [](char a, char b) {
           return a == std::toupper(static_cast<unsigned char>(b));
         });
}
} // namespace
#endif

bool CResLoader::CacheFromPak(const CPakFile& pak, const CAssetId asset) const {
#ifdef TARGET_PC
  if (PortSkipRS5Button(pak, asset))
    return false;
#endif
  const CPakFile::SResInfo* resInfo = pak.GetResInfo(asset);
  if (!resInfo) {
    return false;
  }

  x4c_cachedResId = asset;
  x50_cachedResInfo = resInfo;

  return true;
}

bool CResLoader::CacheFromPakForLoad(CPakFile& pak, const CAssetId asset) {
#ifdef TARGET_PC
  if (PortSkipRS5Button(pak, asset))
    return false;
#endif
  const CPakFile::SResInfo* resInfo = nullptr;
  if (x54_forwardSeek) {
    resInfo = pak.GetResInfoForLoadPreferForward(asset);
    x54_forwardSeek = false;
  } else {
    resInfo = pak.GetResInfoForLoadDirectionless(asset);
  }

  if (resInfo == nullptr) {
    return false;
  }

  x4c_cachedResId = asset;
  x50_cachedResInfo = resInfo;

  return true;
}

#ifdef TARGET_PC
// Retail loads only what ResourceExists found, so these never miss there. A
// miss here (a PAK closed or stashed under the load, an order retail never
// runs) would read through a null or another asset's SResInfo: stop with the
// asset named instead, and the crash handler's stack shows who asked.
static void PortCheckLoad(const CPakFile* pak, const SObjectTag& tag) {
  if (pak != nullptr)
    return;
  const uint type = tag.GetType();
  PortLog::Write("CResLoader: %c%c%c%c %08X is in no loaded PAK\n", (type >> 24) & 0xff,
                 (type >> 16) & 0xff, (type >> 8) & 0xff, type & 0xff, static_cast< uint >(tag.GetId()));
  std::abort();
}
#endif

CPakFile* CResLoader::FindResourceForLoad(const SObjectTag& tag) {
  return FindResourceForLoad(tag.GetId());
}

CPakFile* CResLoader::FindResourceForLoad(const CAssetId asset) {
  rstl::list< rstl::auto_ptr< CPakFile > >::iterator it;
  for (it = x0_aramList.begin(); it != x0_aramList.end(); ++it) {
    CPakFile* pak = it->get();
    if (CacheFromPak(*pak, asset)) {
      return pak;
    }
  }

  if (x48_curPak != x18_pakLoadedList.end()) {
    CPakFile* pak = x48_curPak->get();
    if (CacheFromPakForLoad(*pak, asset)) {
      return pak;
    }
  }

  for (it = x18_pakLoadedList.begin(); it != x18_pakLoadedList.end(); ++it) {
    CPakFile* pak = it->get();
    if (x48_curPak != it && CacheFromPakForLoad(*pak, asset)) {
      x48_curPak = it;
      return pak;
    }
  }

  return nullptr;
}

CPakFile* CResLoader::FindResource(const SObjectTag& tag) {
  x54_forwardSeek = false;
  CPakFile* ret = FindResourceForLoad(tag);
  x54_forwardSeek = true;
  return ret;
}

bool CResLoader::ResourceExists(CAssetId asset) {
#ifdef TARGET_PC
  if (PortPakResourceExists(asset))
    return true;
  return PortCustomRes::IsCustomId(asset) && PortCustomResource(asset) != nullptr;
}

bool CResLoader::PortPakResourceExists(CAssetId asset) {
#endif
#ifdef TARGET_PC
  // Port: ClearCache leaves kInvalidAssetId cached with no info, so an object
  // without an X-ray model (asset -1) made right after it got a null info back
  // (issue 179: a resume regenerating Metroid Prime Lair's Metroids).
  if (x4c_cachedResId == asset && x50_cachedResInfo != nullptr) {
#else
  if (x4c_cachedResId == asset) {
#endif
    return true;
  }

  for (AUTO(it, x0_aramList.begin()); it != x0_aramList.end(); ++it) {
    if (CacheFromPak(**it, asset)) {
      return true;
    }
  }

  if (x48_curPak != x18_pakLoadedList.end()) {
    if (CacheFromPak(**x48_curPak, asset)) {
      return true;
    }
  }

  for (AUTO(it, x18_pakLoadedList.begin()); it != x18_pakLoadedList.end(); ++it) {
    if (x48_curPak != it && CacheFromPak(**it, asset)) {
      return true;
    }
  }

  return false;
}

#ifdef TARGET_PC
void CResLoader::PortReopenPaks(void (*between)()) {
  struct SPak {
    rstl::auto_ptr< CPakFile >* slot;
    rstl::string name;
    bool depList;
    bool worldPak;
  };
  std::vector< SPak > paks;
  rstl::list< rstl::auto_ptr< CPakFile > >* lists[] = {&x0_aramList, &x18_pakLoadedList,
                                                        &x30_pakLoadingList};
  ClearCache();
  for (int i = 0; i < ARRAY_SIZE(lists); ++i) {
    for (AUTO(it, lists[i]->begin()); it != lists[i]->end(); ++it) {
      const CPakFile* pak = it->get();
      const SPak entry = {&*it, pak->GetDvdFile().GetFilename(), pak->PortBuildsDepList(),
                          pak->IsWorldPak()};
      paks.push_back(entry);
      // Closes the file (the destructor finishes a table still being read).
      *it = rstl::auto_ptr< CPakFile >();
    }
  }
  between();
  PortForgetDiscReads();
  for (size_t i = 0; i < paks.size(); ++i) {
    CPakFile* pak = rs_new CPakFile(paks[i].name, paks[i].depList, paks[i].worldPak);
    while (!pak->IsCompletelyLoaded()) {
      pak->AsyncIdle();
      if (!pak->IsCompletelyLoaded()) {
        ARQPoll();
        OSYieldThread();
      }
    }
    *paks[i].slot = rstl::auto_ptr< CPakFile >(pak);
  }
  // A reload can spill mod resources into more extra PAKs than before.
  for (int i = 0; i < PortMods::ExtraPakCount(); ++i) {
    const rstl::string name(PortMods::ExtraPakName(i).c_str());
    const rstl::string file(name + ".pak");
    bool open = false;
    for (size_t j = 0; j < paks.size() && !open; ++j) {
      open = CStringExtras::CompareCaseInsensitive(paks[j].name, file) == 0;
    }
    if (!open) {
      AddPakFileAsync(name, false, false);
    }
  }
  while (!AreAllPaksLoaded()) {
    AsyncIdlePakLoading();
    ARQPoll();
    OSYieldThread();
  }
}
#endif

void CResLoader::ClearCache() {
  x48_curPak = x18_pakLoadedList.end();
  x4c_cachedResId = kInvalidAssetId;
  x50_cachedResInfo = nullptr;
}

void CResLoader::AsyncIdlePakLoading() {
  bool skipIdle = false;
  for (AUTO(it, x30_pakLoadingList.begin()); it != x30_pakLoadingList.end();) {
    CPakFile* pak = it->get();
    const bool aramPak = pak->IsARAMPak();
    if (aramPak || !skipIdle) {
      pak->AsyncIdle();
    }

    if (pak->IsCompletelyLoaded()) {
      MoveToCorrectLoadedList((*it));
      it = x30_pakLoadingList.erase(it);
    } else {
      if (!aramPak) {
        skipIdle = true;
      }
      ++it;
    }
  }
}

bool CResLoader::AreAllPaksLoaded() const { return x30_pakLoadingList.empty(); }

const SObjectTag* CResLoader::GetResourceIdByName(const char* name) const {
  for (AUTO(it, x0_aramList.begin()); it != x0_aramList.end(); ++it) {
    const SObjectTag* id = (*it)->GetResIdByName(name);
    if (id != nullptr) {
      return id;
    }
  }

  for (AUTO(it, x18_pakLoadedList.begin()); it != x18_pakLoadedList.end(); ++it) {
    const SObjectTag* id = (*it)->GetResIdByName(name);
    if (id != nullptr) {
      return id;
    }
  }

  return nullptr;
}

FourCC CResLoader::GetResourceTypeById(const CAssetId asset) const {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom =
          const_cast< CResLoader& >(*this).PortCustomResource(asset, false)) {
    return custom->type;
  }
#endif
  if (const_cast< CResLoader& >(*this).ResourceExists(asset)) {
    return x50_cachedResInfo->GetType();
  }

  return 0;
}

bool CResLoader::ResourceExists(const SObjectTag& tag) const {
  // Port: ResourceExists returns bool; the decompiled `!= nullptr` does not
  // compile on clang.
  return const_cast< CResLoader* >(this)->ResourceExists(tag.GetId());
}

uint CResLoader::ResourceSize(const SObjectTag& tag) const {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom =
          const_cast< CResLoader& >(*this).PortCustomResource(tag.GetId())) {
    return custom->data.size();
  }
#endif
  if (const_cast< CResLoader& >(*this).ResourceExists(tag.GetId())) {
    return x50_cachedResInfo->GetSize();
  }

  return 0;
}

CResLoader::ECompressionType CResLoader::GetResourceCompression(const SObjectTag& tag) const {
#ifdef TARGET_PC
  if (const_cast< CResLoader& >(*this).PortCustomResource(tag.GetId()) != nullptr) {
    return kCompressionType_Uncompressed;
  }
#endif
  if (const_cast< CResLoader& >(*this).ResourceExists(tag.GetId())) {
    return x50_cachedResInfo->IsCompressed() ? kCompressionType_Compressed
                                             : kCompressionType_Uncompressed;
  }

  return kCompressionType_Uncompressed;
}

CDvdRequest* CResLoader::LoadResourceAsync(const SObjectTag& tag, char* extBuf) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    const int size = static_cast< int >(custom->data.size());
    PortCopyCustom(*custom, 0, align_size(size), extBuf);
    return rs_new CPortReadyDvdRequest();
  }
#endif
  CPakFile* curPak = FindResourceForLoad(tag);
#ifdef TARGET_PC
  PortCheckLoad(curPak, tag);
#endif
  const CPakFile::SResInfo* info = x50_cachedResInfo;
  return curPak->DvdFile().AsyncSeekRead(extBuf, align_size(info->GetSize()), kSO_Begin, info->GetOffset());
}

CDvdRequest* CResLoader::LoadResourcePartAsync(const SObjectTag& tag, const int offset,
                                               const int length, char* extBuf) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    PortCopyCustom(*custom, offset, length, extBuf);
    return rs_new CPortReadyDvdRequest();
  }
#endif
  CPakFile* curPak = FindResourceForLoad(tag);
#ifdef TARGET_PC
  PortCheckLoad(curPak, tag);
#endif
  const CPakFile::SResInfo* info = x50_cachedResInfo;
  return curPak->DvdFile().AsyncSeekRead(extBuf, length, kSO_Begin, info->GetOffset() + offset);
}
CInputStream* CResLoader::LoadNewResourceSync(const SObjectTag& tag, char* extBuf) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    const int size = static_cast< int >(custom->data.size());
    void* dest = extBuf ? extBuf : rs_new char[align_size(size)];
    PortCopyCustom(*custom, 0, align_size(size), dest);
    return rs_new CMemoryInStream(dest, size,
                                  extBuf == nullptr ? CMemoryInStream::kOS_Owned
                                                    : CMemoryInStream::kOS_NotOwned);
  }
#endif
  CPakFile* curPak = FindResourceForLoad(tag);
#ifdef TARGET_PC
  PortCheckLoad(curPak, tag);
#endif
  const CPakFile::SResInfo* info = x50_cachedResInfo;
  uint len = align_size(info->GetSize());
  void* dest = extBuf ? extBuf : rs_new char[len];

  curPak->DvdFile().SyncSeekRead(dest, len, kSO_Begin, info->GetOffset());
  CInputStream* input = rs_new CMemoryInStream(dest, info->GetSize(),
                                               extBuf == nullptr ? CMemoryInStream::kOS_Owned
                                                                 : CMemoryInStream::kOS_NotOwned);

  if (info->IsCompressed()) {
    input->Get< uint >();
    return rs_new CZipInputStream(input);
  }

  return input;
}

CInputStream* CResLoader::LoadResourceFromMemorySync(const SObjectTag& tag, const void* extBuf) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    return rs_new CMemoryInStream(extBuf, custom->data.size());
  }
#endif
  FindResourceForLoad(tag);
  const CPakFile::SResInfo* info = x50_cachedResInfo;
#ifdef TARGET_PC
  // A miss here keeps the last lookup's info, as retail does; only none at all
  // (after ClearCache) is fatal.
  if (info == nullptr)
    PortCheckLoad(nullptr, tag);
#endif
  CInputStream* input = rs_new CMemoryInStream(extBuf, info->GetSize());

  if (info->IsCompressed()) {
    input->Get< uint >();
    return rs_new CZipInputStream(input);
  }
  return input;
}

void CResLoader::LoadMemResourceSync(const SObjectTag& tag, char** bufOut, int* lenOut) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    const int size = static_cast< int >(custom->data.size());
    char* buf = rs_new char[align_size(size)];
    PortCopyCustom(*custom, 0, align_size(size), buf);
    *bufOut = buf;
    *lenOut = size;
    return;
  }
#endif
  CPakFile* curPak = FindResourceForLoad(tag);
#ifdef TARGET_PC
  PortCheckLoad(curPak, tag);
#endif
  const CPakFile::SResInfo* info = x50_cachedResInfo;
  uint len = align_size(info->GetSize());
  char* buf = rs_new char[len];
  curPak->DvdFile().SyncSeekRead(buf, len, kSO_Begin, info->GetOffset());
  *bufOut = buf;
  *lenOut = info->GetSize();
}

CInputStream* CResLoader::LoadNewResourcePartSync(const SObjectTag& tag, int offset, int length,
                                                  char* extBuf) {
#ifdef TARGET_PC
  if (const PortCustomRes::Resource* custom = PortCustomResource(tag.GetId())) {
    void* dest = extBuf ? extBuf : rs_new char[length];
    PortCopyCustom(*custom, offset, length, dest);
    return rs_new CMemoryInStream(dest, length,
                                  extBuf == nullptr ? CMemoryInStream::kOS_Owned
                                                    : CMemoryInStream::kOS_NotOwned);
  }
#endif
  CPakFile* curPak = FindResourceForLoad(tag);
#ifdef TARGET_PC
  PortCheckLoad(curPak, tag);
#endif
  const CPakFile::SResInfo* info = x50_cachedResInfo;

  void* dest = extBuf ? extBuf : rs_new char[length];
  curPak->DvdFile().SyncSeekRead(dest, length, kSO_Begin, info->GetOffset() + offset);

  CInputStream* input = rs_new CMemoryInStream(
      dest, length, extBuf == nullptr ? CMemoryInStream::kOS_Owned : CMemoryInStream::kOS_NotOwned);
  return input;
}

void CResLoader::AddPakFileAsync(const rstl::string& filePath, const bool a, const bool b) {
  const rstl::string pathWithExt(filePath + ".pak");

  if (CDvdFile::FileExists(pathWithExt.data())) {
    x30_pakLoadingList.push_back(rs_new CPakFile(pathWithExt, a, b));
  }
}

void CResLoader::RemovePakFile(const rstl::string& filePath) {
  rstl::string pathWithExt(filePath + ".pak");
  ClearCache();
  rstl::list< rstl::auto_ptr< CPakFile > >* lists[] = {&x0_aramList, &x18_pakLoadedList};

  for (int i = 0; i < ARRAY_SIZE(lists); ++i) {
    rstl::list< rstl::auto_ptr< CPakFile > >& list = *lists[i];
    for (AUTO(it, list.begin()); it != list.end(); ++it) {
      const CPakFile* pak = it->get();
      if (CStringExtras::CompareCaseInsensitive(pak->GetDvdFile().GetFilename(), pathWithExt) == 0) {
        list.erase(it);
        return;
      }
    }
  }

  for (AUTO(it, x30_pakLoadingList.begin()); it != x30_pakLoadingList.end(); ++it) {
    const CPakFile* pak = it->get();
    if (CStringExtras::CompareCaseInsensitive(pak->GetDvdFile().GetFilename(), pathWithExt) == 0) {
      while (!pak->IsCompletelyLoaded()) {
        AsyncIdlePakLoading();
      }
      x30_pakLoadingList.erase(it);
      return;
    }
  }
}

const rstl::vector< CAssetId >* CResLoader::GetTagListForFile(const rstl::string& filePath) const {
  rstl::string pathWithExt(filePath + ".pak");

  const rstl::list< rstl::auto_ptr< CPakFile > >* lists[] = {&x0_aramList, &x18_pakLoadedList};

  for (int i = 0; i < ARRAY_SIZE(lists); ++i) {
    const rstl::list< rstl::auto_ptr< CPakFile > >& list = *lists[i];
    for (AUTO(it, list.begin()); it != list.end(); ++it) {
      const CPakFile* pak = it->get();
      if (CStringExtras::CompareCaseInsensitive(pak->GetDvdFile().GetFilename(), pathWithExt) == 0) {
        return pak->GetDepList();
      }
    }
  }

  return nullptr;
}

rstl::vector< rstl::pair< rstl::string, SObjectTag > > CResLoader::GetResourceIdToNameList() const {
  const rstl::list< rstl::auto_ptr< CPakFile > >* lists[] = {&x0_aramList, &x18_pakLoadedList};
  int nameCount = 0;
  for (int i = 0; i < ARRAY_SIZE(lists); ++i) {
    const rstl::list< rstl::auto_ptr< CPakFile > >& list = *lists[i];
    for (AUTO(it, list.begin()); it != list.end(); ++it) {
      const CPakFile* pak = it->get();
      if (!pak->IsStashedInARAM()) {
        nameCount += pak->GetStringToObjectList().size();
      }
    }
  }

  rstl::vector< rstl::pair< rstl::string, SObjectTag > > ret(nameCount);

  for (int i = 0; i < ARRAY_SIZE(lists); ++i) {
    const rstl::list< rstl::auto_ptr< CPakFile > >& list = *lists[i];
    for (rstl::list< rstl::auto_ptr< CPakFile > >::const_iterator it = list.begin();
         it != list.end(); ++it) {
      const CPakFile* pak = it->get();
      if (!pak->IsStashedInARAM()) {
        const rstl::vector< rstl::pair< rstl::string, SObjectTag > >& tagList =
            pak->GetStringToObjectList();
        ret.insert(ret.end(), tagList.begin(), tagList.end());
      }
    }
  }

  return ret;
}
int CResLoader::GetPakCount() const { return x0_aramList.size() + x18_pakLoadedList.size(); }
CPakFile* CResLoader::GetPakFile(const int idx) const {
  int numAramPaks = x0_aramList.size();
  if (idx < numAramPaks) {
    AUTO(it, x0_aramList.begin());
    for (int i = 0; i < idx; ++it, ++i) {
    }
    return it->get();
  }

  AUTO(it, x18_pakLoadedList.begin());
  for (int i = 0; i < idx - numAramPaks; ++it, ++i) {
  }
  return it->get();
}
