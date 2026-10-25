// Internal to the Remastered import (port_remastered_import.h): the two readers it imports
// from, the retail disc (port_remastered_import_retail.cpp) and the Remastered image
// (port_remastered_import_remastered.cpp), and what they share with the import itself
// (port_remastered_import.cpp), in PortRemastered::ImportDetail. Nothing outside those
// files should include this.

#pragma once

#include "port_env.h"
#include "port_log.h"
#include "port_strings.h"
#include "port_hash.h"
#include "port_bytes.h"
#include "port_remastered_import.h"
#include "port_remastered_storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <list>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <aurora/dvd.h>
#include <aurora/gfx.h>

#include "port_build_info.h"
#include "port_disc.h"
#include "port_gallery.h"
#include "port_synthetic_effect.h"
#include "port_map_icons.h"
#include "port_model_variant.h"
#include "port_mods.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_font.h"
#include "port_remastered_image.h"
#include "port_remastered_hud.h"
#include "port_remastered_map.h"
#include "port_remastered_movie.h"
#include "port_remastered_nsp.h"
#include "port_remastered_pak.h"
#include "port_remastered_report.h"
#include "port_remastered_room.h"
#include "port_remastered_table.h"
#include "port_remastered_text.h"
#include "port_remastered_txtr.h"
#include "port_room_geo.h"
#include "port_ws.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

namespace PortRemastered {
namespace ImportDetail {

namespace fs = std::filesystem;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kSMDL = 0x534D444C;
constexpr uint32_t kWMDL = 0x574D444C;  // a liquid's surface
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kCINF = 0x43494E46;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMREA = 0x4D524541;
constexpr uint32_t kSTRG = 0x53545247;
constexpr uint32_t kMSBT = 0x4D534254;
constexpr uint32_t kFONT = 0x464F4E54;
constexpr uint32_t kGUIF = 0x47554946;
constexpr uint32_t kLDTA = 0x4C445441;
constexpr uint32_t kCMAP = 0x434D4150;
constexpr uint32_t kMAPA = 0x4D415041;
constexpr uint32_t kMAPW = 0x4D415057;
constexpr uint32_t kFRME = 0x46524D45;
constexpr uint32_t kFMV0 = 0x464D5630;
constexpr uint32_t kGENP = 0x47454E50;  // a particle effect
constexpr uint32_t kSWSH = 0x53575348;  // a standalone swoosh effect
constexpr uint32_t kCRSM = 0x4352534D;  // a projectile collision response
constexpr uint32_t kELSM = 0x454C534D;  // a standalone electric effect (retail ELSC)
constexpr uint32_t kELC2 = 0x454C4332;  // the same, in the newer form
constexpr uint32_t kMATI = 0x4D415449;  // a material instance
constexpr uint32_t kVECF = 0x56454346;  // a vector field (curl noise)
constexpr uint32_t kWPSM = 0x5750534D;  // a projectile weapon (Remastered's WPSC)
constexpr uint32_t kCHPR = 0x43485052;  // a character (retail ANCS), id the ANCS's
constexpr uint32_t kWPSC = 0x57505343;  // the disc's projectile weapon
constexpr uint32_t kCRSC = 0x43525343;  // the disc's projectile collision response
constexpr uint32_t kPART = 0x50415254;  // a disc particle effect

// The import's progress and its cancel flag (port_remastered_import.cpp), which the readers
// report to.
extern std::atomic<bool> sCancel;
// The import's state and thread, shared with the Dread Samus import (port_dread_samus_import.cpp).
extern std::mutex sStateMutex;
extern ImportState sState;
extern std::thread sThread;
void Finish(bool ok, const std::string& message);
int DefaultThreads();
// A finished Dread Samus import still staged, moved into <mods>/dread-samus (port_dread_samus_import.cpp).
bool ApplyPendingDreadImport();
void SetMessage(const std::string& message);
void SetStepProgress(size_t done, size_t total);
void AddLine(const std::string& line);
uint64_t Hash64(const std::string& text);

// --- The retail disc ----------------------------------------------------------

// The CMDL, CSKR, TXTR, MLVL, MREA, STRG, FRME, MAPA and MAPW resources of the unmodded disc, and every id on it.
class Retail {
public:
  ~Retail();

  bool Index(std::string& error);

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  bool HasUniqueResource(uint32_t type, uint32_t id) const;
  // Whether any pak has a resource of this type and id (it may sit in several).
  bool HasResource(uint32_t type, uint32_t id) const { return m_resourceCounts.count(Key(type, id)) != 0; }

  // The ids of every resource of `type` it reads, in no particular order.
  std::vector<uint32_t> Ids(uint32_t type) const;

  // Which disc this is, as far as the import reads it: every resource's place and size, in
  // any order, and the ids of the rest.
  uint64_t Fingerprint() const;

  // The id of the disc's projectile weapon (WPSC) of this name.
  bool WpscId(const std::string& name, uint32_t& id) const;

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out);

private:
  struct Where {
    int32_t entry;
    uint32_t offset;
    uint32_t size;
    bool compressed;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return uint64_t(type) << 32 | id; }

  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size);

  std::mutex m_mutex;
  std::map<int32_t, void*> m_handles;
  std::unordered_map<uint64_t, Where> m_resources;
  std::unordered_set<uint32_t> m_ids;
  std::unordered_map<uint64_t, Where> m_wpsc;                 // the projectile weapons and skeletons, read on demand
  std::unordered_map<std::string, uint32_t> m_wpscNames;      // WPSC id by name
  std::unordered_set<std::string> m_ambiguousWpscNames;
  std::unordered_map<uint64_t, size_t> m_resourceCounts;      // typed ids, including PART/CRSC
};

// --- The Remastered image ------------------------------------------------------

// Every model and texture in the image's paks, by id.
class Remastered {
public:
  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error);

  bool ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;
  bool HasModel(const ModelUuid& id) const { return m_models.count(id) != 0; }
  // A character (CHPR) by id; a retail ANCS's is 10000000-0000-f000-f000-0000 and the ANCS id.
  bool ReadCharacter(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;
  bool ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;

  // The FONT assets, in id order. There are several, with different sets of characters.
  std::vector<ModelUuid> Fonts() const;
  bool ReadFont(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;

  // A GUI frame by its asset name ("FRME_CombatHud"), whatever folder and case the pak has it under.
  bool ReadFrame(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // A tweak file by its asset name ("TweakGuiColorsMP1"), as ReadFrame finds a frame.
  bool ReadTweak(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // A texture by its asset name ("TXTR_IconS"), as ReadFrame finds a frame.
  bool ReadTextureNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // The Extras gallery's pictures, in the pak's order: the textures of UI_FrontEnd at least 1000 texels high (the
  // concept art; the menu backdrops are 1600x900). `visit` gets each one's position and its TXTR file, and says
  // whether to go on.
  template <class Visit>
  void ForEachGalleryTexture(Visit&& visit) const {
    size_t position = 0;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      if (path.substr(path.rfind('/') + 1) != "UI_FrontEnd.pak") {
        continue;
      }
      const Pak& pak = *m_paks[i];
      for (const PakAsset& asset : pak.Assets()) {
        std::vector<uint8_t> raw;
        std::string error;
        TxtrImage info;
        if (asset.type != kTXTR || !pak.ReadAsset(asset, raw, error) ||
            !ReadTxtrInfo(raw.data(), raw.size(), info, error) || info.height < 1000) {
          continue;
        }
        if (!visit(position++, raw)) {
          return;
        }
      }
    }
  }

  // A model by its asset name ("CMDL_MapCompass"), as ReadFrame finds a frame.
  bool ReadModelNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // A world's map by its asset name ("CMAP_IceLevel"), as ReadFrame finds a frame.
  bool ReadMap(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // A movie by its id as IdToString prints it.
  bool ReadMovie(const std::string& id, std::vector<uint8_t>& out, std::string& error) const;

  // Every text asset, each once however many paks carry it.
  std::vector<ModelUuid> Texts() const;
  bool ReadText(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;

  // The particle effects (GENP) and what they read: material instances and textures.
  std::vector<ModelUuid> Effects() const;
  // The projectile weapons (WPSM) by lower-case name, and a GENP/SWSH by its name.
  std::vector<std::string> ProjectileNames() const;
  bool ReadProjectile(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;
  bool HasUniqueCollision(const ModelUuid& id) const;
  bool ReadCollision(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;
  bool HasUniqueEffect(const ModelUuid& id) const;
  // A GENP/SWSH's (lower-case) name; the first when it has several.
  bool EffectName(const ModelUuid& id, std::string& name) const;
  bool EffectByName(const std::string& name, ModelUuid& id) const;
  bool ReadEffectAsset(uint32_t type, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;
  uint32_t EffectAssetType(const ModelUuid& id) const;

  // The paks of one world directory ("Intro_Master") as the room writer takes
  // them, and every pak of the image for the assets rooms share.
  void World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const;
  // The environment BRDF table out of the executable. Never throws.
  bool ExtractBrdf(std::vector<uint8_t>& out, std::string& error) const;

  // The thermal visor's noise texture out of the executable. Never throws.
  bool ExtractThermalNoiseBytes(std::vector<uint8_t>& out, std::string& error) const;

  std::vector<RoomPak> AllPaks() const;

private:
  struct Where {
    size_t pak;
    size_t asset;
  };
  using Index = std::unordered_map<std::array<uint8_t, 16>, Where, PakIdHash>;

  static std::string FrameKey(const std::string& name);

  bool Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const;

  Nsp m_nsp;
  std::vector<std::unique_ptr<Pak>> m_paks;
  std::vector<std::string> m_paths;  // of m_paks, in the image
  Index m_models;
  Index m_characters;
  Index m_textures;
  Index m_texts;
  Index m_fonts;
  Index m_effects;
  std::unordered_set<ModelUuid, PakIdHash> m_effectDuplicates;
  Index m_swooshes;  // standalone SWSH effects
  Index m_electrics;  // standalone ELSM and ELC2 effects
  std::unordered_map<ModelUuid, uint32_t, PakIdHash> m_electricTypes;
  Index m_materials;
  Index m_vectorFields;
  Index m_collisions;
  std::unordered_set<ModelUuid, PakIdHash> m_collisionDuplicates;
  std::unordered_map<std::string, Where> m_projectiles;  // WPSM, by FrameKey
  std::unordered_set<std::string> m_ambiguousProjectiles;
  std::map<ModelUuid, std::string> m_effectIdNames;  // the first name of a GENP and SWSH
  std::unordered_map<std::string, ModelUuid> m_effectNames;  // the named GENP and SWSH, by FrameKey
  std::unordered_map<std::string, Where> m_frames;  // GUIF, by FrameKey
  std::unordered_map<std::string, Where> m_tweaks;  // LDTA, by FrameKey
  std::unordered_map<std::string, Where> m_textureNames;  // the named TXTR, by FrameKey
  std::unordered_map<std::string, Where> m_modelNames;    // the named CMDL, by FrameKey
  std::unordered_map<std::string, Where> m_maps;  // CMAP, by FrameKey
  std::unordered_map<std::string, Where> m_movies;  // FMV0, by IdToString
};

}  // namespace ImportDetail
}  // namespace PortRemastered
