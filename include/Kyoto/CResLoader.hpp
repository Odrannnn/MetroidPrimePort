#ifndef _CRESLOADER
#define _CRESLOADER

#include "Kyoto/CPakFile.hpp"
#include "Kyoto/SObjectTag.hpp"

#include "types.h"

#include "rstl/list.hpp"
#include "rstl/string.hpp"

class CDvdRequest;
#ifdef TARGET_PC
namespace PortCustomRes {
struct Resource;
}
#endif

class CResLoader {
public:
  enum ECompressionType {
    kCompressionType_Uncompressed,
    kCompressionType_Compressed,
  };
  CResLoader();
  ~CResLoader();
  void MoveToCorrectLoadedList(const rstl::auto_ptr< CPakFile >& pak);
  bool CacheFromPak(const CPakFile& pak, CAssetId asset) const;
  bool CacheFromPakForLoad(CPakFile& pak, CAssetId asset);
  CPakFile* FindResourceForLoad(const SObjectTag& tag);
  CPakFile* FindResourceForLoad(const CAssetId asset);
  CPakFile* FindResource(const SObjectTag& tag);
  bool ResourceExists(CAssetId asset);
  bool ResourceExists(const SObjectTag& tag) const;
  void ClearCache();
  void AsyncIdlePakLoading();
  bool AreAllPaksLoaded() const;
  const SObjectTag* GetResourceIdByName(const char* name) const;
  FourCC GetResourceTypeById(CAssetId asset) const;
  uint ResourceSize(const SObjectTag& tag) const;
  ECompressionType GetResourceCompression(const SObjectTag& tag) const;
  void AddPakFileAsync(const rstl::string&, bool, bool);
  void RemovePakFile(const rstl::string&);
  CInputStream* LoadNewResourceSync(const SObjectTag& tag, char* extBuf);
  CInputStream* LoadResourceFromMemorySync(const SObjectTag& tag, const void* extBuf);
  void LoadMemResourceSync(const SObjectTag& tag, char** extBuf, int* len);
  CInputStream* LoadNewResourcePartSync(const SObjectTag& tag, int offset, int length,
                                        char* extBuf);
  CDvdRequest* LoadResourcePartAsync(const SObjectTag& tag, int offset, int length, char* extBuf);
  CDvdRequest* LoadResourceAsync(const SObjectTag& tag, char* extBuf);

  const rstl::vector< CAssetId >* GetTagListForFile(const rstl::string& pak) const;
  rstl::vector< rstl::pair< rstl::string, SObjectTag > > GetResourceIdToNameList() const;

#ifdef TARGET_PC
  // Port: closes every PAK, calls `between`, then opens them again and reads
  // their tables anew (the mods folder changed). No load may be in flight.
  void PortReopenPaks(void (*between)());
  // Port: whether a load of asset finds it, in a loaded PAK or (copied from it) in any PAK on
  // the disc, as PortCustomResource answers loads no loaded PAK can.
  bool PortLoadable(CAssetId asset) { return PortPakResourceExists(asset) || PortCustomResource(asset) != nullptr; }
#endif

  int GetPakCount() const;
  CPakFile* GetPakFile(int idx) const;

private:
#ifdef TARGET_PC
  // Port: randomprime's custom pickup assets (platform/port_custom_res.cpp),
  // answered for their ids when no PAK has them. `load` is false for type
  // queries, which keep retail's answer for ids no loaded PAK has.
  bool PortPakResourceExists(CAssetId asset);
  const PortCustomRes::Resource* PortCustomResource(CAssetId asset, bool load = true);
#endif

  rstl::list< rstl::auto_ptr< CPakFile > > x0_aramList;
  rstl::list< rstl::auto_ptr< CPakFile > > x18_pakLoadedList;
  rstl::list< rstl::auto_ptr< CPakFile > > x30_pakLoadingList;
  rstl::list< rstl::auto_ptr< CPakFile > >::iterator x48_curPak;
  mutable CAssetId x4c_cachedResId;
  mutable const CPakFile::SResInfo* x50_cachedResInfo;
  bool x54_forwardSeek;
};

#endif // _CRESLOADER
