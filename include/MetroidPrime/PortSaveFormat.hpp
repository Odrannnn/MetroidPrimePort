#ifndef _PORTSAVEFORMAT
#define _PORTSAVEFORMAT

#ifdef TARGET_PC
#include "port_disc.h"

// Port: sizes of the memory-card save, which differ between the USA disc (two
// one-block files "MetroidPrime A"/"B") and the PAL disc (one three-block file
// "MetroidPrime": a header sector and two copies of the save area).
namespace PortSaveFormat {

inline bool IsPal() { return PortDisc::Current() == PortDisc::Version::Pal; }

// CMemoryCardDriver's system options block.
inline int SystemBytes() { return IsPal() ? 114 : 174; }
// One save slot (SGameFileSlot, CGameState's backup buffer).
inline int SlotBytes() { return IsPal() ? 0xA7F : 940; }
// Save header (version, three present bytes) + system block + three slots. USA
// keeps the retail 3004 (3001 used); PAL's is 8182 of the 0x1ff8 a copy holds.
inline int BodyBytes() { return IsPal() ? 7 + SystemBytes() + 3 * SlotBytes() : 3004; }

constexpr int kMaxSystemBytes = 174;
constexpr int kMaxSlotBytes = 0xA7F;
// PAL: bytes of a copy sector (after its CRC) and the payload (after the seq).
constexpr int kPalSectorSize = 0x2000;
constexpr int kPalPayloadOffset = 8;
constexpr int kPalFileSize = 3 * kPalSectorSize;

} // namespace PortSaveFormat
#endif

#endif // _PORTSAVEFORMAT
