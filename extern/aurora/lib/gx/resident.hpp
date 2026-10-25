#pragma once

#include "../gfx/resources.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Data the game keeps on the GPU across frames (GXPortRetainResident): a model's vertex arrays
// and display lists, which would otherwise be copied into every frame's buffers. The command
// processor owns all of it; the game only names it by the pointer it was retained with.
namespace aurora::gx::resident {

// First-fit allocation from one resident region; every block is 256-aligned, which is what
// storage offsets need and more than vertices and indices do.
class Allocator {
public:
  void reset(gfx::Range region) noexcept;
  // An offset into the buffer, or false when nothing that size is free.
  bool alloc(uint32_t size, uint32_t& offset);
  void free(uint32_t offset, uint32_t size) noexcept;
  uint64_t used() const noexcept { return m_used; }

private:
  struct Block {
    uint32_t offset;
    uint32_t size;
  };
  std::vector<Block> m_free; // sorted by offset, never touching
  uint64_t m_used = 0;
};

// A display list made into triangle lists: one draw per vertex format, of at most 65535
// vertices (the indices are u16).
struct Chunk {
  GXVtxFmt fmt;
  uint16_t vtxCount;
  uint32_t vertOffset; // into Entry::vert
  uint32_t vertSize;
  uint32_t idxOffset; // into Entry::idx
  uint32_t idxCount;
  // Where the chunk's vertices start in the display list's bytes, and whether every draw it merged is
  // contiguous there (a NOP between them leaves no contiguous run to read).
  uint32_t srcOffset = 0;
  bool srcContiguous = true;
};

struct Entry {
  std::unique_ptr<std::vector<uint8_t>> bytes;

  // As a vertex array: its copy in the storage region, made on first use.
  bool arrayTried = false;
  gfx::Range storage;

  // As a display list: what it was made into, for the vertex sizes it was parsed with
  // (0 for a format it does not draw). `dlTried` with no chunks: it holds something other
  // than triangles, or there was no room, and it is processed as sent.
  bool dlTried = false;
  std::array<uint32_t, GX_MAX_VTXFMT> vtxSizes{};
  std::vector<Chunk> chunks;
  gfx::Range vert;
  gfx::Range idx;
};

void retain(const void* key, std::unique_ptr<std::vector<uint8_t>> bytes);
// Frees what the entry holds on the GPU. Draws already recorded keep what they read: the
// queue orders a later upload into the same place after them.
void release(const void* key);
Entry* find(const void* key) noexcept;

// The array's resident range (allocated and uploaded on first use), or false to push it per
// frame as before.
bool array_range(const void* data, uint32_t size, gfx::Range& out);

// Allocates and uploads a display list's vertices and indices, built by the caller.
bool store_dl(Entry& entry, const std::vector<uint8_t>& verts, const std::vector<uint8_t>& indices);
void free_dl(Entry& entry) noexcept;

void reset() noexcept;
uint64_t used_bytes() noexcept;

} // namespace aurora::gx::resident
