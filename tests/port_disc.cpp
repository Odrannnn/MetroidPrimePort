#include "port_disc.h"
#include <dolphin/dvd.h>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace {
std::array<uint8_t, 0x108> dol{};
s32 dolSize = dol.size();
void Big32(size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) dol[offset + i] = value >> (24 - i * 8);
}
void Check(bool condition) { if (!condition) std::abort(); }
bool Rejected(uint32_t address, uint32_t size) {
  try { PortReadDolResource(address, size); }
  catch (const std::runtime_error&) { return true; }
  return false;
}
}
extern "C" DVDDiskID* DVDGetCurrentDiskID(void) { return nullptr; }
extern "C" const u8* DVDGetDOLLocation(s32* size) {
  *size = dolSize;
  return dol.data();
}
int main() {
  Big32(7 * 4, 0x100);
  Big32(0x48 + 7 * 4, 0x81230000);
  Big32(0x90 + 7 * 4, 8);
  for (int i = 0; i < 8; ++i) dol[0x100 + i] = 0x30 + i;
  const auto data = PortReadDolResource(0x81230002, 4);
  Check(data.size() == 4 && data.front() == 0x32 && data.back() == 0x35);
  Check(Rejected(0x81230000, 9));
  Check(Rejected(0x8122ffff, 4));
  Big32(7 * 4, 0xfffffff0);
  Check(Rejected(0x81230002, 4));
  // Found by its first bytes when the address holds something else.
  const uint8_t sig[] = {0x33, 0x34};
  Big32(7 * 4, 0x100);
  const auto found = PortFindDolResource(0x81230000, 3, sig, sizeof(sig));
  Check(found.size() == 3 && found.front() == 0x33 && found.back() == 0x35);
  const uint8_t absent[] = {0x99};
  bool threw = false;
  try { PortFindDolResource(0x81230000, 1, absent, sizeof(absent)); }
  catch (const std::runtime_error&) { threw = true; }
  Check(threw);
  using PortDisc::Version;
  Check(PortDisc::Identify("GM8E01", 0, 0) == Version::Usa100);
  Check(PortDisc::Identify("GM8E01", 0, 2) == Version::Usa102);
  Check(PortDisc::Identify("GM8E01", 0, 3) == Version::Unknown);
  Check(PortDisc::Identify("GM8P01", 0, 0) == Version::Pal);
  Check(PortDisc::Identify("GM8J01", 0, 0) == Version::Unknown);
  Check(PortDisc::Identify("GM8E01", 1, 0) == Version::Unknown);
  Check(PortDisc::Current() == Version::Unknown);
  Check(PortDisc::IsAccepted(Version::Usa100) && !PortDisc::IsAccepted(Version::Unknown));
  dolSize = 4;
  Check(Rejected(0x81230000, 4));
}
