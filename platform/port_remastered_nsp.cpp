// Reads the RomFS of a Switch title straight out of the user's .nsp or .xci with the
// user's own key file (port_remastered_nsp.h). The container layout and key
// derivation follow hactool (ISC licence), which is the reference for both.

#include "port_remastered_nsp.h"
#include "port_strings.h"
#include "port_bytes.h"

#include "port_remastered_nso.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include <cstdlib>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(MP_HAVE_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

namespace PortRemastered {

bool SourceFile::Open(const std::string& path) {
  Close();
#if !defined(_WIN32)
  if (path.rfind("fd:", 0) == 0) {
    char* end = nullptr;
    const long fd = std::strtol(path.c_str() + 3, &end, 10);
    if (end == path.c_str() + 3 || *end != '\0' || fd < 0) {
      return false;
    }
    m_fd = dup(int(fd));
    return m_fd >= 0;
  }
  // A descriptor of its own, so that reads can run side by side with pread.
  m_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (m_fd >= 0) {
    return true;
  }
#endif
  m_stream.open(path, std::ios::binary);
  return bool(m_stream);
}

void SourceFile::Close() {
#if !defined(_WIN32)
  if (m_fd >= 0) {
    close(m_fd);
    m_fd = -1;
  }
#endif
  m_stream.close();
  m_stream.clear();
}

size_t SourceFile::ReadSome(uint64_t offset, void* out, size_t size) {
#if !defined(_WIN32)
  if (m_fd >= 0) {
    // pread leaves the position alone, which the duplicate shares with the caller's.
    size_t done = 0;
    while (done < size) {
      const ssize_t got = pread(m_fd, static_cast<char*>(out) + done, size - done, off_t(offset + done));
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }
#endif
  std::lock_guard<std::mutex> lock(m_streamMutex);
  m_stream.clear();
  m_stream.seekg(std::streamoff(offset), std::ios::beg);
  if (!m_stream) {
    return 0;
  }
  m_stream.read(static_cast<char*>(out), std::streamsize(size));
  return size_t(m_stream.gcount());
}

bool SourceFile::ReadAt(uint64_t offset, void* out, size_t size) { return ReadSome(offset, out, size) == size; }

#if defined(MP_HAVE_OPENSSL)
namespace {

constexpr uint64_t kMediaUnit = 0x200;
constexpr size_t kNcaHeaderSize = 0xC00;
// Reads are decrypted in pieces of this size through the scratch buffer.
constexpr size_t kChunk = 1u << 20;
// Far past what this title has; only here to reject garbage before allocating.
constexpr uint32_t kMaxPfs0Files = 4096;
constexpr uint32_t kMaxPfs0Strings = 1u << 20;
constexpr uint64_t kMaxRomfsTable = 256u << 20;

using port::ReadLE32;
using port::ReadLE64;

using port::HexDigit;

// Wipes key material when the scope ends, so no exit path forgets to.
struct Wipe {
  void* data;
  size_t size;
  ~Wipe() { OPENSSL_cleanse(data, size); }
};

// The NCA header's key-area-key index: which family of keys wraps its key area.
const char* const kKeyAreaNames[3] = {"key_area_key_application_", "key_area_key_ocean_", "key_area_key_system_"};

// Only the keys this title needs: the header key, and per master key
// generation a title KEK (eShop titles) and the key area keys (gamecard ones).
// Parsing keeps nothing else from the file.
struct KeySet {
  uint8_t headerKey[32] = {};
  bool hasHeader = false;
  std::map<std::string, std::vector<uint8_t>> titleKeks;
  std::map<std::string, std::vector<uint8_t>> keyAreaKeys[3];
  ~KeySet() {
    OPENSSL_cleanse(headerKey, sizeof(headerKey));
    for (auto& entry : titleKeks) {
      OPENSSL_cleanse(entry.second.data(), entry.second.size());
    }
    for (auto& family : keyAreaKeys) {
      for (auto& entry : family) {
        OPENSSL_cleanse(entry.second.data(), entry.second.size());
      }
    }
  }
};

bool LoadKeys(const std::string& path, KeySet& keys, std::string& error) {
  // A key file is a few KB of text; one that isn't is not read past this.
  std::string text(1 << 20, '\0');
  {
    SourceFile file;
    if (!file.Open(path)) {
      error = "cannot open key file";
      return false;
    }
    text.resize(file.ReadSome(0, text.data(), text.size()));
  }
  Wipe wipe{text.data(), text.size()};
  // A key file re-saved as UTF-16 (some Windows editors do) has a NUL after or
  // before every character; the names and hex digits are ASCII, so dropping
  // the NULs gives the text back.
  if (text.find('\0') != std::string::npos) {
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
  }
  std::istringstream f(text);
  std::string line;
  // Lines named by 32 hex digits are rights id = title key: a title.keys file.
  bool sawTitleKeys = false;
  while (std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    // Only name characters are kept, which also drops spaces and byte order marks.
    std::string name;
    for (size_t i = 0; i < eq; ++i) {
      char c = line[i];
      if (c >= 'A' && c <= 'Z') {
        c = char(c - 'A' + 'a');
      }
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
        name.push_back(c);
      }
    }
    bool wantHeader = name == "header_key";
    bool wantKek = name.rfind("titlekek_", 0) == 0 && name.size() == 11;
    int keyArea = -1;
    for (int i = 0; i < 3; ++i) {
      const size_t prefix = std::strlen(kKeyAreaNames[i]);
      if (name.size() == prefix + 2 && name.compare(0, prefix, kKeyAreaNames[i]) == 0) {
        keyArea = i;
      }
    }
    if (name.size() == 32 && std::all_of(name.begin(), name.end(), [](char c) { return HexDigit(c) >= 0; })) {
      sawTitleKeys = true;
    }
    if (!wantHeader && !wantKek && keyArea < 0) {
      continue;
    }
    std::vector<uint8_t> value;
    int high = -1;
    for (size_t i = eq + 1; i < line.size(); ++i) {
      int digit = HexDigit(line[i]);
      if (digit < 0) {
        continue;
      }
      if (high < 0) {
        high = digit;
      } else {
        value.push_back(uint8_t((high << 4) | digit));
        high = -1;
      }
    }
    if (wantHeader && value.size() == 32) {
      std::memcpy(keys.headerKey, value.data(), 32);
      keys.hasHeader = true;
    } else if (wantKek && value.size() == 16) {
      keys.titleKeks[name.substr(9)] = value;
    } else if (keyArea >= 0 && value.size() == 16) {
      keys.keyAreaKeys[keyArea][name.substr(name.size() - 2)] = value;
    }
    OPENSSL_cleanse(value.data(), value.size());
  }
  if (!keys.hasHeader) {
    error = sawTitleKeys ? "this is a title.keys file; pick prod.keys (the one with header_key) instead"
                         : "key file has no valid header_key (pick your console's prod.keys)";
    return false;
  }
  return true;
}

struct PackedFile {
  std::string name;
  uint64_t offset; // absolute, in the image
  uint64_t size;
};

// PFS0 (an .nsp, 0x18-byte entries) and HFS0 (a gamecard partition, 0x40-byte
// entries) share a layout: magic, file count, string table size, a reserved
// word, the entries ({offset, size, name offset, ...}), then the names. Entry
// offsets count from the end of the names.
bool ReadFileTable(SourceFile& file, uint64_t at, const char* magic, size_t entrySize, std::vector<PackedFile>& out,
                   std::string& error) {
  uint8_t head[16];
  if (!file.ReadAt(at, head, sizeof(head)) || std::memcmp(head, magic, 4) != 0) {
    error = std::string("no ") + magic + " header";
    return false;
  }
  uint32_t numFiles = ReadLE32(head + 4);
  uint32_t stringSize = ReadLE32(head + 8);
  if (numFiles == 0 || numFiles > kMaxPfs0Files || stringSize > kMaxPfs0Strings) {
    error = std::string("implausible ") + magic + " header";
    return false;
  }
  size_t tableSize = size_t(numFiles) * entrySize + stringSize;
  std::vector<uint8_t> table(tableSize);
  if (!file.ReadAt(at + 16, table.data(), tableSize)) {
    error = std::string("truncated ") + magic + " table";
    return false;
  }
  const uint64_t dataStart = at + 16 + tableSize;
  const char* strings = reinterpret_cast<const char*>(table.data() + size_t(numFiles) * entrySize);
  out.clear();
  for (uint32_t i = 0; i < numFiles; ++i) {
    const uint8_t* e = table.data() + size_t(i) * entrySize;
    uint32_t nameOffset = ReadLE32(e + 16);
    if (nameOffset >= stringSize) {
      error = std::string("bad ") + magic + " name offset";
      return false;
    }
    size_t len = strnlen(strings + nameOffset, stringSize - nameOffset);
    out.push_back({std::string(strings + nameOffset, len), dataStart + ReadLE64(e), ReadLE64(e + 8)});
  }
  return true;
}

// The files of a gamecard image's secure partition, where the game's NCAs are.
// A plain dump starts with the card header ("HEAD" at 0x100); some dumpers put
// the card's 0x1000-byte key area in front of it, which shifts everything.
bool ReadXciSecure(SourceFile& file, std::vector<PackedFile>& out, std::string& error) {
  for (uint64_t base : {uint64_t(0), uint64_t(0x1000)}) {
    uint8_t head[0x200];
    if (!file.ReadAt(base, head, sizeof(head)) || std::memcmp(head + 0x100, "HEAD", 4) != 0) {
      continue;
    }
    std::vector<PackedFile> root;
    if (!ReadFileTable(file, base + ReadLE64(head + 0x130), "HFS0", 0x40, root, error)) {
      error = "gamecard root partition: " + error;
      return false;
    }
    for (const PackedFile& partition : root) {
      if (partition.name == "secure") {
        if (!ReadFileTable(file, partition.offset, "HFS0", 0x40, out, error)) {
          error = "gamecard secure partition: " + error;
          return false;
        }
        return true;
      }
    }
    error = "the .xci has no secure partition";
    return false;
  }
  error = "not an .nsp or .xci (no PFS0 or gamecard header)";
  return false;
}

// Whether the NCA's RomFS is a BKTR patch (an update) rather than a whole RomFS.
bool HasPatchRomfs(const uint8_t* hdr) {
  for (int i = 0; i < 4; ++i) {
    const uint8_t* fs = hdr + 0x400 + i * 0x200;
    if (ReadLE32(hdr + 0x240 + i * 0x10) != 0 && fs[2] == 0 && fs[3] == 3 && fs[4] == 4) {
      return true;
    }
  }
  return false;
}

// Nintendo's XTS sector tweak is the sector number big-endian, where the
// standard (and OpenSSL's) convention is little-endian, so the IV is built by
// hand for every 0x200-byte sector.
bool DecryptHeader(const uint8_t* key, uint8_t* data, size_t size) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }
  bool ok = true;
  for (size_t sector = 0; ok && sector * 0x200 < size; ++sector) {
    uint8_t tweak[16] = {};
    for (int i = 15, shift = 0; i >= 8; --i, shift += 8) {
      tweak[i] = uint8_t(uint64_t(sector) >> shift);
    }
    uint8_t* block = data + sector * 0x200;
    int outLen = 0;
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_xts(), nullptr, key, tweak) == 1 &&
         EVP_DecryptUpdate(ctx, block, &outLen, block, 0x200) == 1 && outLen == 0x200;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool DecryptEcb(const uint8_t* key, const uint8_t* in, uint8_t* out) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }
  int outLen = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_DecryptUpdate(ctx, out, &outLen, in, 16) == 1 &&
            outLen == 16;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

// The counter is the section's 8-byte nonce (stored reversed) followed by the
// big-endian count of 16-byte blocks from the start of the NCA.
// `ctx` is the caller's, reused across the chunks of one read.
bool DecryptCtr(EVP_CIPHER_CTX* ctx, const uint8_t* key, const uint8_t* nonce, uint64_t ncaOffset, uint8_t* data,
                size_t size) {
  uint8_t iv[16];
  for (int i = 0; i < 8; ++i) {
    iv[i] = nonce[7 - i];
  }
  uint64_t block = ncaOffset >> 4;
  for (int i = 15; i >= 8; --i) {
    iv[i] = uint8_t(block);
    block >>= 8;
  }
  int outLen = 0;
  return EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) == 1 &&
         EVP_DecryptUpdate(ctx, data, &outLen, data, int(size)) == 1 && size_t(outLen) == size;
}

} // namespace

Nsp::~Nsp() { Close(); }

void Nsp::Close() {
  OPENSSL_cleanse(m_contentKey, sizeof(m_contentKey));
  m_file.Close();
  m_files.clear();
  m_open = false;
}

bool Nsp::Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
  Close();

  KeySet keys;
  if (!LoadKeys(keysPath, keys, error)) {
    return false;
  }
  if (!m_file.Open(nspPath)) {
    error = "cannot open " + nspPath;
    return false;
  }

  // --- The container: an .nsp (PFS0) or a gamecard image (.xci) --------------
  uint8_t magic[4];
  if (!m_file.ReadAt(0, magic, sizeof(magic))) {
    error = "cannot read " + nspPath;
    return false;
  }
  std::vector<PackedFile> entries;
  if (std::memcmp(magic, "PFS0", 4) == 0 ? !ReadFileTable(m_file, 0, "PFS0", 24, entries, error)
                                         : !ReadXciSecure(m_file, entries, error)) {
    return false;
  }

  // The game is the largest program NCA; the small ones are metadata, the icon
  // and the manual, and a gamecard may also carry an update (a BKTR patch).
  std::vector<const PackedFile*> ncas;
  std::vector<const PackedFile*> tickets;
  for (const PackedFile& entry : entries) {
    auto ends = [&](const char* suffix) {
      size_t n = std::strlen(suffix);
      return entry.name.size() >= n && entry.name.compare(entry.name.size() - n, n, suffix) == 0;
    };
    if (ends(".nca") && !ends(".cnmt.nca")) {
      ncas.push_back(&entry);
    } else if (ends(".tik")) {
      tickets.push_back(&entry);
    }
  }
  if (ncas.empty()) {
    const bool compressed = std::any_of(entries.begin(), entries.end(), [](const PackedFile& e) {
      return e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".ncz") == 0;
    });
    error = compressed ? "compressed images (.nsz/.xcz) are not supported; decompress it first" : "no .nca in the image";
    return false;
  }
  std::stable_sort(ncas.begin(), ncas.end(), [](const PackedFile* a, const PackedFile* b) { return a->size > b->size; });

  std::vector<uint8_t> hdr(kNcaHeaderSize);
  const PackedFile* nca = nullptr;
  std::string firstError;
  for (const PackedFile* candidate : ncas) {
    std::string why;
    if (candidate->size < kNcaHeaderSize || !m_file.ReadAt(candidate->offset, hdr.data(), kNcaHeaderSize)) {
      why = "cannot read the NCA header";
    } else if (!DecryptHeader(keys.headerKey, hdr.data(), kNcaHeaderSize)) {
      why = "header decryption failed";
    } else if (std::memcmp(hdr.data() + 0x200, "NCA3", 4) != 0) {
      why = std::memcmp(hdr.data() + 0x200, "NCA2", 4) == 0 || std::memcmp(hdr.data() + 0x200, "NCA0", 4) == 0
                ? "NCA0/NCA2 is not supported"
                : "NCA header is not valid; wrong keys?";
    } else if (hdr[0x205] != 0) {
      continue; // not a program: control, manual, data
    } else if (HasPatchRomfs(hdr.data())) {
      why = "BKTR (patch) section: only a base-game RomFS is supported";
    } else {
      nca = candidate;
      break;
    }
    if (firstError.empty()) {
      firstError = why;
    }
  }
  if (!nca) {
    error = firstError.empty() ? "no program NCA in the image" : firstError;
    return false;
  }

  uint8_t cryptoType = std::max(hdr[0x206], hdr[0x220]);
  if (cryptoType) {
    --cryptoType;
  }
  static const char kHex[] = "0123456789abcdef";
  std::string kekName = std::string(1, kHex[cryptoType >> 4]) + kHex[cryptoType & 15];

  const uint8_t* rightsId = hdr.data() + 0x230;
  bool hasRights = false;
  for (int i = 0; i < 16; ++i) {
    hasRights = hasRights || rightsId[i] != 0;
  }
  if (hasRights) {
    // --- Title-key crypto (eShop): the key is in the ticket ----------------------
    uint8_t tik[0x2C0];
    Wipe wipeTik{tik, sizeof(tik)};
    bool found = false;
    for (const PackedFile* ticket : tickets) {
      if (ticket->size >= sizeof(tik) && m_file.ReadAt(ticket->offset, tik, sizeof(tik)) &&
          std::memcmp(rightsId, tik + 0x2A0, 16) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      error = tickets.empty() ? "no .tik in the image (title-key crypto needs the ticket)"
                              : "no ticket for this NCA's rights id";
      return false;
    }
    // 0x10004 is RSA-2048 + SHA-256, the layout the offsets below assume.
    if (ReadLE32(tik) != 0x10004) {
      error = "unsupported ticket signature type";
      return false;
    }
    if (tik[0x281] != 0) {
      error = "personalized ticket (the title key is console-encrypted); only common tickets are supported";
      return false;
    }
    auto kek = keys.titleKeks.find(kekName);
    if (kek == keys.titleKeks.end()) {
      error = "key file lacks the title KEK for master key generation " + kekName;
      return false;
    }
    if (!DecryptEcb(kek->second.data(), tik + 0x180, m_contentKey)) {
      error = "title key decryption failed";
      return false;
    }
  } else {
    // --- Key-area crypto (gamecards, and .nsp files made from them) ---------------
    // The header's key area holds four wrapped keys; AES-CTR sections use the third.
    uint8_t keyIndex = hdr[0x207];
    if (keyIndex > 2) {
      error = "unknown NCA key area key index";
      return false;
    }
    auto kak = keys.keyAreaKeys[keyIndex].find(kekName);
    if (kak == keys.keyAreaKeys[keyIndex].end()) {
      error = std::string("key file lacks ") + kKeyAreaNames[keyIndex] + kekName;
      return false;
    }
    if (!DecryptEcb(kak->second.data(), hdr.data() + 0x300 + 2 * 16, m_contentKey)) {
      error = "key area decryption failed";
      return false;
    }
  }

  // --- The RomFS section -------------------------------------------------------
  int section = -1;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* entry = hdr.data() + 0x240 + i * 0x10;
    const uint8_t* fs = hdr.data() + 0x400 + i * 0x200;
    if (ReadLE32(entry) == 0) {
      continue;
    }
    uint8_t partition = fs[2];
    uint8_t fsType = fs[3];
    uint8_t crypt = fs[4];
    if (partition != 0 || fsType != 3) {
      continue;
    }
    if (crypt == 4) {
      error = "BKTR (patch) section: only a base-game RomFS is supported";
      return false;
    }
    if (crypt != 3) {
      error = "RomFS section is not AES-CTR";
      return false;
    }
    section = i;
    break;
  }
  if (section < 0) {
    error = "no RomFS section in the NCA";
    return false;
  }
  const uint8_t* entry = hdr.data() + 0x240 + section * 0x10;
  const uint8_t* fs = hdr.data() + 0x400 + section * 0x200;
  uint64_t start = uint64_t(ReadLE32(entry)) * kMediaUnit;
  uint64_t end = uint64_t(ReadLE32(entry + 4)) * kMediaUnit;
  if (end <= start || end > nca->size) {
    error = "bad section bounds";
    return false;
  }
  std::memcpy(m_ctrHigh, fs + 0x140, 8);

  const uint8_t* ivfc = fs + 8;
  uint32_t numLevels = ReadLE32(ivfc + 0xC);
  if (std::memcmp(ivfc, "IVFC", 4) != 0 || numLevels < 2 || numLevels > 7) {
    error = "RomFS section has no IVFC header";
    return false;
  }
  // The last level is the RomFS itself; the others are hash trees.
  m_romfsOffset = ReadLE64(ivfc + 0x10 + (numLevels - 2) * 0x18);

  m_sectionInNca = start;
  m_sectionBase = nca->offset + start;
  m_sectionSize = end - start;

  // --- RomFS tables -----------------------------------------------------------
  std::string readError;
  uint8_t rh[0x50];
  if (!ReadSection(m_romfsOffset, rh, sizeof(rh), readError)) {
    error = "cannot read the RomFS header: " + readError;
    Close();
    return false;
  }
  if (ReadLE64(rh) != 0x50) {
    error = "bad RomFS header (wrong keys or unsupported layout)";
    Close();
    return false;
  }
  uint64_t dirOffset = ReadLE64(rh + 0x18), dirSize = ReadLE64(rh + 0x20);
  uint64_t fileOffset = ReadLE64(rh + 0x38), fileSize = ReadLE64(rh + 0x40);
  m_dataOffset = ReadLE64(rh + 0x48);
  if (dirSize > kMaxRomfsTable || fileSize > kMaxRomfsTable) {
    error = "implausible RomFS table size";
    Close();
    return false;
  }
  std::vector<uint8_t> dirs(dirSize), files(fileSize);
  if (!ReadSection(m_romfsOffset + dirOffset, dirs.data(), dirs.size(), readError) ||
      !ReadSection(m_romfsOffset + fileOffset, files.data(), files.size(), readError)) {
    error = "cannot read the RomFS tables: " + readError;
    Close();
    return false;
  }

  constexpr uint32_t kEmpty = 0xFFFFFFFF;
  // Depth-first over the directory tree, with every entry visited at most once.
  struct Pending {
    uint32_t dir;
    std::string path;
  };
  std::vector<Pending> stack;
  stack.push_back({0, std::string()});
  size_t visited = 0;
  while (!stack.empty()) {
    Pending cur = std::move(stack.back());
    stack.pop_back();
    if (cur.dir + 24 > dirs.size() || ++visited > dirs.size() / 24 + 1) {
      error = "corrupt RomFS directory table";
      Close();
      return false;
    }
    const uint8_t* d = dirs.data() + cur.dir;
    uint32_t child = ReadLE32(d + 8), firstFile = ReadLE32(d + 12);
    for (uint32_t f = firstFile; f != kEmpty;) {
      if (uint64_t(f) + 32 > files.size()) {
        error = "corrupt RomFS file table";
        Close();
        return false;
      }
      const uint8_t* fe = files.data() + f;
      uint32_t nameSize = ReadLE32(fe + 28);
      if (uint64_t(f) + 32 + nameSize > files.size() || m_files.size() > files.size() / 32) {
        error = "corrupt RomFS file table";
        Close();
        return false;
      }
      RomfsFile file;
      file.path = cur.path + std::string(reinterpret_cast<const char*>(fe + 32), nameSize);
      file.offset = m_dataOffset + ReadLE64(fe + 8);
      file.size = ReadLE64(fe + 16);
      m_files.push_back(std::move(file));
      f = ReadLE32(fe + 4);
    }
    // Children are pushed with their full prefix, found by walking the child
    // chain here: a sibling link alone would not know the parent's prefix.
    for (uint32_t c = child; c != kEmpty;) {
      if (uint64_t(c) + 24 > dirs.size()) {
        error = "corrupt RomFS directory table";
        Close();
        return false;
      }
      const uint8_t* ce = dirs.data() + c;
      uint32_t nameSize = ReadLE32(ce + 20);
      if (uint64_t(c) + 24 + nameSize > dirs.size() || stack.size() > dirs.size() / 24 + 1) {
        error = "corrupt RomFS directory table";
        Close();
        return false;
      }
      stack.push_back({c, cur.path + std::string(reinterpret_cast<const char*>(ce + 24), nameSize) + "/"});
      c = ReadLE32(ce + 4);
    }
  }
  std::sort(m_files.begin(), m_files.end(), [](const RomfsFile& a, const RomfsFile& b) { return a.path < b.path; });

  // --- ExeFS: a PFS0 section, found softly (the import works without it) ----
  m_hasExefs = false;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* e = hdr.data() + 0x240 + i * 0x10;
    const uint8_t* f = hdr.data() + 0x400 + i * 0x200;
    if (ReadLE32(e) == 0 || f[2] != 1 || f[3] != 2 || f[4] != 3) {
      continue;
    }
    uint64_t s0 = uint64_t(ReadLE32(e)) * kMediaUnit, s1 = uint64_t(ReadLE32(e + 4)) * kMediaUnit;
    // HierarchicalSha256: master hash (0x20), block size, layer count, then {offset, size}
    // regions; the last one is the PFS0 itself.
    uint32_t layers = ReadLE32(f + 8 + 0x24);
    if (s1 <= s0 || s1 > nca->size || layers < 2 || layers > 6) {
      continue;
    }
    m_exefs.base = nca->offset + s0;
    m_exefs.size = s1 - s0;
    m_exefs.inNca = s0;
    std::memcpy(m_exefs.ctrHigh, f + 0x140, 8);
    m_exefsPfs = ReadLE64(f + 8 + 0x28 + (layers - 1) * 16);
    m_exefsPfsSize = ReadLE64(f + 8 + 0x28 + (layers - 1) * 16 + 8);
    m_hasExefs = true;
    break;
  }

  m_open = true;
  return true;
}

const RomfsFile* Nsp::Find(const std::string& path) const {
  auto it = std::lower_bound(m_files.begin(), m_files.end(), path,
                             [](const RomfsFile& file, const std::string& key) { return file.path < key; });
  return it != m_files.end() && it->path == path ? &*it : nullptr;
}

bool Nsp::ReadSection(uint64_t offset, void* out, size_t size, std::string& error) const {
  Section section;
  section.base = m_sectionBase;
  section.size = m_sectionSize;
  section.inNca = m_sectionInNca;
  std::memcpy(section.ctrHigh, m_ctrHigh, 8);
  return ReadFrom(section, offset, out, size, error);
}

bool Nsp::ReadFrom(const Section& section, uint64_t offset, void* out, size_t size, std::string& error) const {
  uint8_t* dst = static_cast<uint8_t*>(out);
  if (offset > section.size || size > section.size - offset) {
    error = "read past the end of the section";
    return false;
  }
  // Per call, so that reads from several threads share nothing but the file.
  std::unique_ptr<EVP_CIPHER_CTX, void (*)(EVP_CIPHER_CTX*)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx) {
    error = "decryption failed";
    return false;
  }
  std::vector<uint8_t> scratch;
  while (size) {
    uint64_t position = offset;
    uint64_t aligned = position & ~uint64_t(15);
    size_t head = size_t(position - aligned);
    size_t take = std::min(size, kChunk - head);
    // CTR is a stream cipher, so only the start has to sit on a 16-byte boundary.
    scratch.resize(head + take);
    if (!m_file.ReadAt(section.base + aligned, scratch.data(), head + take)) {
      error = "short read from the image";
      return false;
    }
    if (!DecryptCtr(ctx.get(), m_contentKey, section.ctrHigh, section.inNca + aligned, scratch.data(), head + take)) {
      error = "decryption failed";
      return false;
    }
    std::memcpy(dst, scratch.data() + head, take);
    dst += take;
    offset += take;
    size -= take;
  }
  return true;
}

bool Nsp::Read(const RomfsFile& file, uint64_t offset, void* out, size_t size, std::string& error) const {
  if (!m_open) {
    error = "not open";
    return false;
  }
  if (offset > file.size || size > file.size - offset) {
    error = "read past the end of " + file.path;
    return false;
  }
  return ReadSection(m_romfsOffset + file.offset + offset, out, size, error);
}

bool Nsp::ReadExefsFile(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
  if (!m_open || !m_hasExefs) {
    error = "no ExeFS section";
    return false;
  }
  uint8_t head[16];
  if (!ReadFrom(m_exefs, m_exefsPfs, head, sizeof(head), error)) {
    return false;
  }
  const size_t headerSize = Pfs0HeaderSize(head);
  if (headerSize == 0 || headerSize > m_exefsPfsSize) {
    error = "ExeFS has no PFS0 header";
    return false;
  }
  std::vector<uint8_t> header(headerSize);
  if (!ReadFrom(m_exefs, m_exefsPfs, header.data(), header.size(), error)) {
    return false;
  }
  std::vector<Pfs0Entry> entries;
  if (!Pfs0Parse(header.data(), header.size(), entries, error)) {
    return false;
  }
  for (const Pfs0Entry& entry : entries) {
    if (entry.name != name) {
      continue;
    }
    if (entry.offset > m_exefsPfsSize || entry.size > m_exefsPfsSize - entry.offset || entry.size > (512u << 20)) {
      error = "ExeFS file " + name + " is out of bounds";
      return false;
    }
    out.resize(size_t(entry.size));
    return ReadFrom(m_exefs, m_exefsPfs + entry.offset, out.data(), out.size(), error);
  }
  error = "no " + name + " in the ExeFS";
  return false;
}

bool IsKnownBrdfLut(const uint8_t* data, size_t size) {
  static const uint8_t kWanted[32] = {
                                      0xdc, 0xe3, 0xde, 0x6e, 0x63, 0xda, 0x0a, 0x09, 0x8b, 0x95, 0x23,
                                      0xe1, 0xa4, 0xcd, 0xb8, 0xa5, 0x94, 0x32, 0x61, 0xba, 0x70, 0xde,
                                      0x00, 0x34, 0x18, 0x91, 0xc0, 0x6f, 0xa2, 0x22, 0x73, 0xde};
  uint8_t digest[32];
  unsigned int len = 0;
  return EVP_Digest(data, size, digest, &len, EVP_sha256(), nullptr) == 1 && len == 32 &&
         std::memcmp(digest, kWanted, 32) == 0;
}

bool ExtractBrdfLut(const Nsp& nsp, std::vector<uint8_t>& out, std::string& error) {
  std::vector<uint8_t> nso;
  if (!nsp.ReadExefsFile("main", nso, error)) {
    return false;
  }
  if (!NsoReadImage(nso, kBrdfLutMemOffset, kBrdfLutSize, out, error)) {
    return false;
  }
  if (!IsKnownBrdfLut(out.data(), out.size())) {
    out.clear();
    error = "the executable's BRDF table is not the known one (another version of the game?)";
    return false;
  }
  return true;
}

bool ExtractThermalNoise(const Nsp& nsp, std::vector<uint8_t>& out, std::string& error) {
  std::vector<uint8_t> nso;
  if (!nsp.ReadExefsFile("main", nso, error)) {
    return false;
  }
  return NsoReadImage(nso, kThermalNoiseMemOffset, kThermalNoiseSize, out, error);
}

#else // !MP_HAVE_OPENSSL

Nsp::~Nsp() = default;

void Nsp::Close() {
  m_files.clear();
  m_open = false;
}

bool Nsp::Open(const std::string&, const std::string&, std::string& error) {
  error = "built without OpenSSL";
  return false;
}

const RomfsFile* Nsp::Find(const std::string&) const { return nullptr; }

bool Nsp::ReadSection(uint64_t, void*, size_t, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

bool Nsp::Read(const RomfsFile&, uint64_t, void*, size_t, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

bool Nsp::ReadFrom(const Section&, uint64_t, void*, size_t, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

bool Nsp::ReadExefsFile(const std::string&, std::vector<uint8_t>&, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

bool IsKnownBrdfLut(const uint8_t*, size_t) { return false; }

bool ExtractBrdfLut(const Nsp&, std::vector<uint8_t>&, std::string& error) {
  error = "built without OpenSSL";
  return false;
}

bool ExtractThermalNoise(const Nsp&, std::vector<uint8_t>&, std::string& error) {
  error = "built without OpenSSL";
  return false;
}

#endif

} // namespace PortRemastered
