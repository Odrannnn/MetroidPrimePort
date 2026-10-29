// The self-contained half of the mods folder (port_mods.h): PAK tables, the
// virtual file layout and its reader. Kept apart from the startup code so the
// unit test needs neither Aurora nor a disc.

#include "port_mods.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace PortMods {
namespace {

constexpr uint32_t kPakVersion = 0x00030005;
// Far past any retail PAK; only here to reject garbage before allocating.
constexpr uint32_t kMaxNameLength = 1024;
constexpr uint32_t kMaxResources = 1000000;

using port::ReadBE32;

void WriteBE32(uint8_t* data, uint32_t value) {
  data[0] = uint8_t(value >> 24);
  data[1] = uint8_t(value >> 16);
  data[2] = uint8_t(value >> 8);
  data[3] = uint8_t(value);
}

uint64_t RoundUp32(uint64_t value) { return (value + 31) & ~uint64_t(31); }

using port::HexDigit;

std::filesystem::path HostPath(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace

bool ParsePakTable(const uint8_t* data, size_t size, PakTable& table, size_t& needed) {
  table = {};
  needed = 12;
  if (size < needed) {
    return false;
  }
  if (ReadBE32(data) != kPakVersion) {
    needed = 0;
    return false;
  }
  const uint32_t nameCount = ReadBE32(data + 8);
  if (nameCount > kMaxResources) {
    needed = 0;
    return false;
  }
  size_t pos = 12;
  for (uint32_t i = 0; i < nameCount; ++i) {
    needed = pos + 12;
    if (size < needed) {
      return false;
    }
    const uint32_t nameLength = ReadBE32(data + pos + 8);
    if (nameLength > kMaxNameLength) {
      needed = 0;
      return false;
    }
    pos += 12 + nameLength;
  }
  needed = pos + 4;
  if (size < needed) {
    return false;
  }
  const uint32_t count = ReadBE32(data + pos);
  if (count > kMaxResources) {
    needed = 0;
    return false;
  }
  pos += 4;
  needed = pos + size_t(count) * 20;
  if (size < needed) {
    return false;
  }
  table.resources.reserve(count);
  for (uint32_t i = 0; i < count; ++i, pos += 20) {
    PakResource& resource = table.resources.emplace_back();
    resource.compressed = ReadBE32(data + pos);
    resource.type = ReadBE32(data + pos + 4);
    resource.id = ReadBE32(data + pos + 8);
    resource.size = ReadBE32(data + pos + 12);
    resource.offset = ReadBE32(data + pos + 16);
    resource.entryOffset = pos;
  }
  table.headerEnd = pos;
  return true;
}

// "<8 hex digits>.<ext>", the extension in any case (`ext` is lower case).
static bool ParseHexIdName(const std::string& fileName, const char* ext, uint32_t& id) {
  const size_t extLength = std::strlen(ext);
  if (fileName.size() != 9 + extLength || fileName[8] != '.') {
    return false;
  }
  for (size_t i = 0; i < extLength; ++i) {
    if ((fileName[9 + i] | 0x20) != ext[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

bool ParseNativeTextureName(const std::string& fileName, uint32_t& id) {
  return ParseHexIdName(fileName, "dds", id);
}

bool ParseMaterialCubeName(const std::string& fileName, uint32_t& id) {
  return ParseHexIdName(fileName, "envcube", id);
}

bool ImportStampStale(const std::string& stampText, int current) {
  size_t i = 0;
  while (i < stampText.size() && (stampText[i] == ' ' || stampText[i] == '\t')) {
    ++i;
  }
  long value = 0;
  size_t digits = 0;
  for (; i < stampText.size() && stampText[i] >= '0' && stampText[i] <= '9'; ++i, ++digits) {
    value = value * 10 + (stampText[i] - '0');
    if (value > 1000000) {
      break;
    }
  }
  return digits == 0 || value < current;
}

bool ParseLooseName(const std::string& fileName, uint32_t& type, uint32_t& id) {
  if (fileName.size() != 13 || fileName[8] != '.') {
    return false;
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  type = 0;
  for (size_t i = 9; i < 13; ++i) {
    char c = fileName[i];
    if (c >= 'a' && c <= 'z') {
      c = char(c - 'a' + 'A');
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      return false;
    }
    type = (type << 8) | uint8_t(c);
  }
  return true;
}

std::string FourCCString(uint32_t type) {
  std::string text(4, ' ');
  for (int i = 0; i < 4; ++i) {
    const char c = char(type >> (24 - 8 * i));
    text[i] = c >= 32 && c < 127 ? c : '?';
  }
  return text;
}

VirtualFile PatchPak(const std::vector<uint8_t>& header, const PakTable& table, uint64_t originalSize,
                     const std::vector<const LooseResource*>& loose, const std::string& sourceHost,
                     const std::vector<const LooseResource*>& added) {
  VirtualFile file;
  if (header.size() < table.headerEnd || originalSize < table.headerEnd || table.headerEnd < table.resources.size() * 20 + 4 ||
      table.resources.size() + added.size() > kMaxResources) {
    return file;
  }
  // memory[0] is the header, patched below. New entries go at the table's end,
  // and everything after it moves down by a multiple of 32 to keep alignment.
  const uint64_t shift = RoundUp32(added.size() * 20);
  const size_t countOffset = table.headerEnd - table.resources.size() * 20 - 4;
  file.memory.emplace_back(header.begin(), header.begin() + table.headerEnd);
  file.memory[0].resize(table.headerEnd + shift, 0);
  file.segments.push_back({Segment::kMemory, 0, table.headerEnd + shift, 0});
  if (shift > 0) {
    WriteBE32(file.memory[0].data() + countOffset, uint32_t(table.resources.size() + added.size()));
    for (const PakResource& entry : table.resources) {
      WriteBE32(file.memory[0].data() + entry.entryOffset + 16, uint32_t(entry.offset + shift));
    }
  }

  Segment original;
  original.start = table.headerEnd + shift;
  original.length = originalSize - table.headerEnd;
  original.sourceOffset = table.headerEnd;
  if (!sourceHost.empty()) {
    original.kind = Segment::kHost;
    original.hostPath = sourceHost;
    original.hostSize = originalSize;
  } else {
    original.kind = Segment::kSource;
  }
  if (original.length > 0) {
    file.segments.push_back(original);
  }
  const uint64_t originalEnd = originalSize + shift;
  uint64_t end = RoundUp32(originalEnd);
  if (end > originalEnd) {
    file.memory.emplace_back(end - originalEnd, uint8_t(0));
    file.segments.push_back({Segment::kMemory, originalEnd, end - originalEnd, file.memory.size() - 1});
  }
  auto append = [&](const LooseResource* resource, size_t entryOffset, bool isNew) {
    const uint64_t length = RoundUp32(resource->hostSize);
    if (length == 0 && !isNew) {
      return;
    }
    bool used = false;
    auto point = [&](size_t at) {
      uint8_t* bytes = file.memory[0].data() + at;
      WriteBE32(bytes, 0);
      WriteBE32(bytes + 12, uint32_t(length));
      WriteBE32(bytes + 16, uint32_t(end));
      used = true;
    };
    if (isNew) {
      WriteBE32(file.memory[0].data() + entryOffset + 4, resource->type);
      WriteBE32(file.memory[0].data() + entryOffset + 8, resource->id);
      point(entryOffset);
    } else {
      for (const PakResource& entry : table.resources) {
        if (entry.type == resource->type && entry.id == resource->id) {
          point(entry.entryOffset);
        }
      }
    }
    if (!used || length == 0) {
      return;
    }
    Segment appended;
    appended.kind = Segment::kHost;
    appended.start = end;
    appended.length = length;
    appended.hostPath = resource->hostPath;
    appended.hostSize = resource->hostSize;
    file.segments.push_back(appended);
    end += length;
  };
  for (const LooseResource* resource : loose) {
    append(resource, 0, false);
  }
  for (size_t i = 0; i < added.size(); ++i) {
    append(added[i], table.headerEnd + i * 20, true);
  }
  file.size = end;
  return file;
}

VirtualFile NewPak(const std::vector<const LooseResource*>& added) {
  // Version, an unused word, no names, no resources.
  std::vector<uint8_t> header(16, 0);
  WriteBE32(header.data(), kPakVersion);
  PakTable table;
  table.headerEnd = header.size();
  return PatchPak(header, table, header.size(), {}, {}, added);
}

std::vector<std::vector<const LooseResource*>> SplitAdded(const std::vector<const LooseResource*>& added,
                                                           uint64_t homeSize) {
  std::vector<std::vector<const LooseResource*>> groups(1);
  // The size PatchPak gives the group with one more entry and `data` more bytes.
  uint64_t base = homeSize;
  uint64_t data = 0;
  auto sizeWith = [&](uint64_t more) {
    return RoundUp32(base + RoundUp32((groups.back().size() + 1) * 20)) + data + RoundUp32(more);
  };
  for (const LooseResource* resource : added) {
    if (RoundUp32(16 + 20) + RoundUp32(resource->hostSize) > kMaxFileSize) {
      continue;
    }
    if (sizeWith(resource->hostSize) > kMaxFileSize) {
      groups.emplace_back();
      base = 16;
      data = 0;
    }
    groups.back().push_back(resource);
    data += RoundUp32(resource->hostSize);
  }
  return groups;
}

// --- Reader ---------------------------------------------------------------------

struct Reader::Host {
  std::string path;
  std::ifstream in;
};

Reader::Reader(std::shared_ptr<const VirtualFile> file, const SourceIo* io) : mFile(std::move(file)), mIo(io) {}

Reader::~Reader() {
  if (mSource != nullptr && mIo != nullptr && mIo->close != nullptr) {
    mIo->close(mSource);
  }
}

int64_t Reader::Seek(int64_t offset, int32_t whence) {
  int64_t base = 0;
  switch (whence) {
  case 0:
    break;
  case 1:
    base = int64_t(mPos);
    break;
  case 2:
    base = int64_t(mFile->size);
    break;
  default:
    return -1;
  }
  const int64_t target = base + offset;
  if (target < 0) {
    return -1;
  }
  mPos = uint64_t(target);
  return target;
}

int64_t Reader::Read(uint8_t* buffer, size_t length) {
  const std::vector<Segment>& segments = mFile->segments;
  size_t done = 0;
  while (done < length && mPos < mFile->size) {
    // The last segment starting at or before mPos.
    auto next = std::upper_bound(segments.begin(), segments.end(), mPos,
                                 [](uint64_t pos, const Segment& segment) { return pos < segment.start; });
    if (next == segments.begin()) {
      return done > 0 ? int64_t(done) : -1;
    }
    const Segment& segment = *(next - 1);
    const uint64_t within = mPos - segment.start;
    if (within >= segment.length) {
      return done > 0 ? int64_t(done) : -1;
    }
    const size_t chunk = size_t(std::min<uint64_t>(length - done, segment.length - within));
    const int64_t got = ReadSegment(segment, within, buffer + done, chunk);
    if (got < 0) {
      return done > 0 ? int64_t(done) : -1;
    }
    done += size_t(got);
    mPos += uint64_t(got);
    if (size_t(got) < chunk) {
      break;
    }
  }
  return int64_t(done);
}

int64_t Reader::ReadSegment(const Segment& segment, uint64_t at, uint8_t* buffer, size_t length) {
  switch (segment.kind) {
  case Segment::kMemory:
    std::memcpy(buffer, mFile->memory[segment.memory].data() + at, length);
    return int64_t(length);
  case Segment::kSource: {
    if (!mSourceTried) {
      mSourceTried = true;
      if (mIo != nullptr && mIo->open != nullptr) {
        mSource = mIo->open(*mFile);
      }
    }
    if (mSource == nullptr) {
      return -1;
    }
    return mIo->readAt(mSource, segment.sourceOffset + at, buffer, length);
  }
  case Segment::kHost: {
    if (mHost == nullptr) {
      mHost = std::make_unique<Host>();
    }
    const uint64_t offset = segment.sourceOffset + at;
    // Past the file's end (the 32-byte padding, or a file that shrank since
    // startup) reads as zeros, so the table's sizes stay true.
    size_t fromFile = 0;
    if (offset < segment.hostSize) {
      fromFile = size_t(std::min<uint64_t>(length, segment.hostSize - offset));
      if (mHost->path != segment.hostPath || !mHost->in.is_open()) {
        mHost->in = std::ifstream(HostPath(segment.hostPath), std::ios::binary);
        mHost->path = segment.hostPath;
      }
      mHost->in.clear();
      mHost->in.seekg(std::streamoff(offset));
      mHost->in.read(reinterpret_cast<char*>(buffer), std::streamsize(fromFile));
      const size_t got = size_t(std::max<std::streamsize>(mHost->in.gcount(), 0));
      if (got < fromFile) {
        std::memset(buffer + got, 0, fromFile - got);
      }
    }
    std::memset(buffer + fromFile, 0, length - fromFile);
    return int64_t(length);
  }
  }
  return -1;
}

std::vector<std::string> SplitDisabled(const std::string& list) {
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= list.size()) {
    const size_t end = std::min(list.find('/', start), list.size());
    if (end > start) {
      names.push_back(list.substr(start, end - start));
    }
    start = end + 1;
  }
  return names;
}

std::string JoinDisabled(const std::vector<std::string>& names) {
  std::string list;
  for (const std::string& name : names) {
    if (name.empty() || name.find('/') != std::string::npos) {
      continue;
    }
    if (!list.empty()) {
      list += '/';
    }
    list += name;
  }
  return list;
}

bool ImportPartHidden(const std::string& list, const char* part) {
  const size_t length = std::strlen(part);
  for (size_t at = 0; at < list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    size_t begin = at;
    size_t end = comma;
    while (begin < end && std::isspace(static_cast<unsigned char>(list[begin]))) {
      ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(list[end - 1]))) {
      --end;
    }
    if (end - begin == length && list.compare(begin, length, part) == 0) {
      return true;
    }
    at = comma + 1;
  }
  return false;
}

} // namespace PortMods
