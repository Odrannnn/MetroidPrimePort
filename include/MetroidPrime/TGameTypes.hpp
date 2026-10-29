#ifndef _TGAMETYPES
#define _TGAMETYPES

#include "types.h"

class CInputStream;
class COutputStream;

struct TAreaId;
struct TEditorId;
struct TUniqueId;

extern const TAreaId kInvalidAreaId;
extern const TEditorId kInvalidEditorId;
extern const TUniqueId kInvalidUniqueId;

struct TAreaId {
  int value;

  TAreaId() : value(-1) {}
  TAreaId(int value) : value(value) {}
  int Value() const { return value; }

  bool operator==(const TAreaId& other) const { return value == other.value; }
  bool operator!=(const TAreaId& other) const { return value != other.value; }
};
CHECK_SIZEOF(TAreaId, 0x4)

struct TEditorId {
  uint value;

  TEditorId(uint value) : value(value) {}
  TEditorId(CInputStream& in);
  // TODO
  uint Value() const { return value & 0x3FFFFFF; }
  uint Id() const { return value & 0xffff; }
  int AreaNum() const { return (value >> 16) & 0x3ff; }

  void PutTo(COutputStream&) const;

  bool operator==(const TEditorId& other) const { return Value() == other.Value(); }
  bool operator!=(const TEditorId& other) const { return Value() != other.Value(); }
  bool operator<(const TEditorId& other) const { return Value() < other.Value(); }
};
CHECK_SIZEOF(TEditorId, 0x4)

// Retail packs 10 index bits and 6 version bits; the port has 11 and 5, so 2048 object slots
// fit in the same 16 bits. Index 2047 + version 31 is 0xFFFF (kInvalidUniqueId), never allocated.
enum {
  kUniqueIdIndexBits = 11,
  kUniqueIdVersionBits = 16 - kUniqueIdIndexBits,
  kUniqueIdIndexMask = (1 << kUniqueIdIndexBits) - 1,
  kUniqueIdVersionMask = (1 << kUniqueIdVersionBits) - 1,
  kMaxObjects = 1 << kUniqueIdIndexBits,
};

struct TUniqueId {
  ushort value;
  TUniqueId() {}
  TUniqueId(const ushort version, const ushort id)
  : value(id | (version << kUniqueIdIndexBits)) {}

  ushort Value() const { return value & kUniqueIdIndexMask; }
  ushort Version() const { return (value >> kUniqueIdIndexBits) & kUniqueIdVersionMask; }

  bool operator==(const TUniqueId& other) const { return value == other.value; }
  bool operator!=(const TUniqueId& other) const { return value != other.value; }
  bool operator<(const TUniqueId& other) const { return value < other.value; }
  operator bool() const { return *this != kInvalidUniqueId; }

private:
};
CHECK_SIZEOF(TUniqueId, 0x2)

// struct TGameScriptId {
//   TEditorId editorId;
//   bool b;
// };
// CHECK_SIZEOF(TGameScriptId, 0x8)

typedef ushort TSfxId;
static TSfxId InvalidSfxId = 0xFFFFu;

#define ALIGN_UP(x, a) (((x) + (a - 1)) & ~(a - 1))

#endif // _TGAMETYPES
