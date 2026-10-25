// The Remastered import's reader of the Remastered image. See
// port_remastered_import_internal.h.

#include "port_remastered_import_internal.h"

namespace PortRemastered {
namespace ImportDetail {

bool Remastered::Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
  if (!m_nsp.Open(nspPath, keysPath, error)) {
    return false;
  }
  std::vector<const RomfsFile*> files;
  for (const RomfsFile& file : m_nsp.Files()) {
    if (file.path.size() > 4 && file.path.compare(file.path.size() - 4, 4, ".pak") == 0) {
      files.push_back(&file);
    }
  }
  for (size_t i = 0; i < files.size(); ++i) {
    if (sCancel) {
      error = "cancelled";
      return false;
    }
    SetMessage("Reading the paks (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");
    SetStepProgress(i, files.size());
    const RomfsFile* file = files[i];
    auto pak = std::make_unique<Pak>();
    std::string pakError;
    // Nsp::Read is safe from the workers side by side.
    const ReadFn read = [this, file](uint64_t offset, void* out, size_t size) {
      std::string ignored;
      return m_nsp.Read(*file, offset, out, size, ignored);
    };
    if (!pak->Open(read, file->size, pakError)) {
      AddLine(file->path + ": " + pakError);
      continue;
    }
    const std::vector<PakAsset>& assets = pak->Assets();
    for (size_t a = 0; a < assets.size(); ++a) {
      const uint32_t type = assets[a].type;
      if (type == kCMDL || type == kSMDL || type == kWMDL) {
        m_models.emplace(assets[a].id, Where{m_paks.size(), a});
        for (const std::string& name : assets[a].names) {
          m_modelNames.emplace(FrameKey(name), Where{m_paks.size(), a});
        }
      } else if (type == kTXTR) {
        m_textures.emplace(assets[a].id, Where{m_paks.size(), a});
        for (const std::string& name : assets[a].names) {
          m_textureNames.emplace(FrameKey(name), Where{m_paks.size(), a});
        }
      } else if (type == kCHPR) {
        m_characters.emplace(assets[a].id, Where{m_paks.size(), a});
      } else if (type == kMSBT) {
        m_texts.emplace(assets[a].id, Where{m_paks.size(), a});
      } else if (type == kFONT) {
        m_fonts.emplace(assets[a].id, Where{m_paks.size(), a});
      } else if (type == kFMV0) {
        m_movies.emplace(IdToString(assets[a].id), Where{m_paks.size(), a});
      } else if (type == kGENP) {
        if (!m_effects.emplace(assets[a].id, Where{m_paks.size(), a}).second) {
          m_effectDuplicates.insert(assets[a].id);
        }
        for (const std::string& name : assets[a].names) {
          m_effectNames.emplace(FrameKey(name), assets[a].id);
          m_effectIdNames.emplace(assets[a].id, FrameKey(name));
        }
      } else if (type == kSWSH) {
        m_swooshes.emplace(assets[a].id, Where{m_paks.size(), a});
        for (const std::string& name : assets[a].names) {
          m_effectNames.emplace(FrameKey(name), assets[a].id);
          m_effectIdNames.emplace(assets[a].id, FrameKey(name));
        }
      } else if (type == kELSM || type == kELC2) {
        m_electrics.emplace(assets[a].id, Where{m_paks.size(), a});
        m_electricTypes.emplace(assets[a].id, type);
        for (const std::string& name : assets[a].names) {
          m_effectNames.emplace(FrameKey(name), assets[a].id);
        }
      } else if (type == kWPSM) {
        for (const std::string& name : assets[a].names) {
          const std::string key = FrameKey(name);
          const auto [found, inserted] = m_projectiles.emplace(key, Where{m_paks.size(), a});
          if (!inserted && (found->second.pak != m_paks.size() || found->second.asset != a)) {
            m_ambiguousProjectiles.insert(key);
          }
        }
      } else if (type == kCRSM) {
        if (!m_collisions.emplace(assets[a].id, Where{m_paks.size(), a}).second) {
          m_collisionDuplicates.insert(assets[a].id);
        }
      } else if (type == kMATI) {
        m_materials.emplace(assets[a].id, Where{m_paks.size(), a});
      } else if (type == kVECF) {
        m_vectorFields.emplace(assets[a].id, Where{m_paks.size(), a});
      } else if (type == kGUIF) {
        for (const std::string& name : assets[a].names) {
          m_frames.emplace(FrameKey(name), Where{m_paks.size(), a});
        }
      } else if (type == kCMAP) {
        for (const std::string& name : assets[a].names) {
          m_maps.emplace(FrameKey(name), Where{m_paks.size(), a});
        }
      } else if (type == kLDTA) {
        for (const std::string& name : assets[a].names) {
          m_tweaks.emplace(FrameKey(name), Where{m_paks.size(), a});
        }
      }
    }
    m_paks.push_back(std::move(pak));
    m_paths.push_back(file->path);
  }
  if (m_models.empty() && m_movies.empty()) {
    error = "no models in this image; is it Metroid Prime Remastered?";
    return false;
  }
  return true;
}

bool Remastered::ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(m_models, id, out, error);
}

bool Remastered::ReadCharacter(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(m_characters, id, out, error);
}

bool Remastered::ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(m_textures, id, out, error);
}

std::vector<ModelUuid> Remastered::Fonts() const {
  std::vector<ModelUuid> ids;
  for (const auto& [id, where] : m_fonts) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool Remastered::ReadFont(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(m_fonts, id, out, error);
}

bool Remastered::ReadFrame(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_frames.find(FrameKey(name));
  if (found == m_frames.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::ReadTweak(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_tweaks.find(FrameKey(name));
  if (found == m_tweaks.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::ReadTextureNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_textureNames.find(FrameKey(name));
  if (found == m_textureNames.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::ReadModelNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_modelNames.find(FrameKey(name));
  if (found == m_modelNames.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::ReadMap(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_maps.find(FrameKey(name));
  if (found == m_maps.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::ReadMovie(const std::string& id, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = m_movies.find(id);
  if (found == m_movies.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

std::vector<ModelUuid> Remastered::Texts() const {
  std::vector<ModelUuid> ids;
  for (const auto& [id, where] : m_texts) {
    ids.push_back(id);
  }
  return ids;
}

bool Remastered::ReadText(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(m_texts, id, out, error);
}

std::vector<ModelUuid> Remastered::Effects() const {
  std::vector<ModelUuid> ids;
  for (const auto& [id, where] : m_effects) {
    ids.push_back(id);
  }
  for (const auto& [id, where] : m_electrics) {
    ids.push_back(id);
  }
  for (const auto& [id, where] : m_swooshes) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> Remastered::ProjectileNames() const {
  std::vector<std::string> names;
  for (const auto& [name, where] : m_projectiles) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool Remastered::ReadProjectile(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  if (m_ambiguousProjectiles.count(name) != 0) {
    error = "ambiguous asset name";
    return false;
  }
  const auto found = m_projectiles.find(name);
  if (found == m_projectiles.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

bool Remastered::HasUniqueCollision(const ModelUuid& id) const {
  return m_collisions.count(id) != 0 && m_collisionDuplicates.count(id) == 0;
}

bool Remastered::ReadCollision(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  if (!HasUniqueCollision(id)) {
    error = "missing or ambiguous collision response";
    return false;
  }
  return Read(m_collisions, id, out, error);
}

bool Remastered::HasUniqueEffect(const ModelUuid& id) const {
  return m_effects.count(id) != 0 && m_effectDuplicates.count(id) == 0;
}

bool Remastered::EffectName(const ModelUuid& id, std::string& name) const {
  const auto found = m_effectIdNames.find(id);
  if (found == m_effectIdNames.end()) {
    return false;
  }
  name = found->second;
  return true;
}

bool Remastered::EffectByName(const std::string& name, ModelUuid& id) const {
  const auto found = m_effectNames.find(name);
  if (found == m_effectNames.end()) {
    return false;
  }
  id = found->second;
  return true;
}

bool Remastered::ReadEffectAsset(uint32_t type, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  return Read(type == kGENP   ? m_effects
              : type == kSWSH ? m_swooshes
              : type == kELSM || type == kELC2 ? m_electrics
              : type == kMATI ? m_materials
              : type == kVECF ? m_vectorFields
                              : m_textures,
              id, out, error);
}

uint32_t Remastered::EffectAssetType(const ModelUuid& id) const {
  return m_textures.count(id) != 0    ? kTXTR
         : m_materials.count(id) != 0 ? kMATI
         : m_vectorFields.count(id) != 0 ? kVECF
         : m_effects.count(id) != 0   ? kGENP
         : m_swooshes.count(id) != 0  ? kSWSH
         : m_electrics.count(id) != 0 ? m_electricTypes.at(id)
         : m_models.count(id) != 0    ? kCMDL
                                      : 0;
}

void Remastered::World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const {
  const std::string folder = "/!" + dir + "/";
  for (size_t i = 0; i < m_paks.size(); ++i) {
    const std::string& path = m_paths[i];
    const size_t at = path.find(folder);
    if (at == std::string::npos) {
      continue;
    }
    const std::string name = path.substr(at + folder.size(), path.size() - at - folder.size() - 4);
    if (name == "!" + dir) {
      master = RoomPak{dir, m_paks[i].get()};
    } else if (name.find('/') == std::string::npos) {
      rooms.push_back(RoomPak{name, m_paks[i].get()});
    }
  }
}

bool Remastered::ExtractBrdf(std::vector<uint8_t>& out, std::string& error) const {
  try {
    return ExtractBrdfLut(m_nsp, out, error);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool Remastered::ExtractThermalNoiseBytes(std::vector<uint8_t>& out, std::string& error) const {
  try {
    return ExtractThermalNoise(m_nsp, out, error);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

std::vector<RoomPak> Remastered::AllPaks() const {
  std::vector<RoomPak> all;
  for (size_t i = 0; i < m_paks.size(); ++i) {
    all.push_back(RoomPak{m_paths[i], m_paks[i].get()});
  }
  return all;
}

std::string Remastered::FrameKey(const std::string& name) {
  const size_t slash = name.find_last_of("/\\");
  std::string key = name.substr(slash == std::string::npos ? 0 : slash + 1);
  key = key.substr(0, key.find('.'));
  for (char& c : key) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return key;
}

bool Remastered::Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
  const auto found = index.find(id);
  if (found == index.end()) {
    error = "not in the image";
    return false;
  }
  const Pak& pak = *m_paks[found->second.pak];
  return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
}

}  // namespace ImportDetail
}  // namespace PortRemastered
