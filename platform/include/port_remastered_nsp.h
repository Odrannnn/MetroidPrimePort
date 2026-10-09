#pragma once

// Reads the RomFS of Metroid Prime Remastered straight out of the user's .nsp
// or gamecard image (.xci).
//
// Nothing is extracted: Open() parses the PFS0 (or the gamecard's HFS0
// partitions), the ticket, the NCA header and the RomFS tables (a few MB), and
// Read() decrypts only the ranges asked for. The keys come from the user's own
// key file (hactool's "name = hex" format); the port embeds none and never
// prints them; only the one content key that Read() needs stays in memory, and
// it is wiped on Close().
//
// Supported: NCA3, title-key crypto with a common ticket or key-area crypto
// (gamecards, and .nsp files converted from them), an AES-CTR section holding
// a RomFS behind IVFC. Anything else (BKTR patch sections, NCA0/NCA2,
// compressed NCZ, personalized tickets) fails Open() with a clear error.
// Hashes are not verified.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace PortRemastered {

struct RomfsFile {
  // Forward slashes, no leading slash: "Worlds/MP1/.../x.pak".
  std::string path;
  // Where the file starts, counted from the start of the RomFS, and its length.
  uint64_t offset = 0;
  uint64_t size = 0;
};

// A file read at any offset. `path` is a path, or "fd:<n>" for a descriptor
// the caller already holds: Android's file picker gives a content:// address,
// which only the system can open, and an image of several GB is not worth
// copying to get a path. The descriptor is duplicated, so the caller keeps its own.
class SourceFile {
public:
  SourceFile() = default;
  SourceFile(const SourceFile&) = delete;
  SourceFile& operator=(const SourceFile&) = delete;
  ~SourceFile() { Close(); }

  bool Open(const std::string& path);
  void Close();
  // The bytes read, fewer than `size` at the end of the file. Safe from any
  // number of threads at once: a descriptor is read with pread, and the stream
  // (Windows) takes a lock around its seek and read.
  size_t ReadSome(uint64_t offset, void* out, size_t size);
  // False unless all of `size` bytes were read.
  bool ReadAt(uint64_t offset, void* out, size_t size);

private:
  std::mutex m_streamMutex;
  std::ifstream m_stream;
  int m_fd = -1;
};

class Nsp {
public:
  Nsp() = default;
  Nsp(const Nsp&) = delete;
  Nsp& operator=(const Nsp&) = delete;
  ~Nsp();

  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error);
  void Close();
  bool IsOpen() const { return m_open; }

  // Sorted by path.
  const std::vector<RomfsFile>& Files() const { return m_files; }
  const RomfsFile* Find(const std::string& path) const;

  // A file of the program NCA's ExeFS (a PFS0 section), "main" for the executable.
  // False, with `error` set, when the NCA has no such section or file. The
  // whole file is read, so ask only for what is needed.
  bool ReadExefsFile(const std::string& name, std::vector<uint8_t>& out, std::string& error) const;

  // Decrypts [offset, offset + size) of `file` into `out`. Safe from several
  // threads at once once Open() has returned: each call has its own buffer and
  // cipher context, and SourceFile's reads are positional.
  bool Read(const RomfsFile& file, uint64_t offset, void* out, size_t size, std::string& error) const;

private:
  // Where an AES-CTR section sits in the .nsp.
  struct Section {
    uint64_t base = 0;    // absolute, in the .nsp
    uint64_t size = 0;
    uint64_t inNca = 0;   // from the NCA start, for the counter
    uint8_t ctrHigh[8] = {};
  };
  // Reads and decrypts `size` bytes at `offset` in the RomFS section.
  bool ReadSection(uint64_t offset, void* out, size_t size, std::string& error) const;
  bool ReadFrom(const Section& section, uint64_t offset, void* out, size_t size, std::string& error) const;

  bool m_open = false;
  mutable SourceFile m_file;
  // Absolute position of the section in the .nsp, and of the RomFS inside it.
  uint64_t m_sectionBase = 0;
  uint64_t m_sectionSize = 0;
  uint64_t m_romfsOffset = 0;
  uint64_t m_dataOffset = 0;
  // Offset of the NCA from the section start's counter point of view.
  uint64_t m_sectionInNca = 0;
  uint8_t m_ctrHigh[8] = {};
  uint8_t m_contentKey[16] = {};
  std::vector<RomfsFile> m_files;
  // The ExeFS section, when the NCA has one, and where its PFS0 starts in it.
  bool m_hasExefs = false;
  Section m_exefs;
  uint64_t m_exefsPfs = 0;
  uint64_t m_exefsPfsSize = 0;
};

// Metroid Prime Remastered's environment BRDF table, 0x100 bytes of the executable
// ("main"'s NSO image at kBrdfLutMemOffset). Only its SHA-256 is known here: the
// table is read from the user's own image, never shipped.
constexpr uint32_t kBrdfLutMemOffset = 0x1d0dbfc;
constexpr size_t kBrdfLutSize = 0x100;
bool ExtractBrdfLut(const Nsp& nsp, std::vector<uint8_t>& out, std::string& error);
// False unless `data` is the table of the supported version (SHA-256 check).
bool IsKnownBrdfLut(const uint8_t* data, size_t size);

} // namespace PortRemastered
