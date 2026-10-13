#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "rstl/auto_ptr.hpp"
#include "rstl/single_ptr.hpp"
#include "rstl/rc_ptr.hpp"
#include "MetroidPrime/PathFinding/CPFBitSet.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
void Check(bool condition) {
  if (!condition) {
    std::fputs("port buffer regression failed\n", stderr);
    std::abort();
  }
}
struct Element {
  static int live;
  Element() { ++live; }
  ~Element() { --live; }
};
int Element::live = 0;
int gameFrees = 0;
}

// CMemoryStreamOut reports a truncated write; the game links OSReport from
// aurora, the test has to define it (macOS rejects an undefined weak symbol).
extern "C" void OSReport(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  std::vfprintf(stderr, msg, args);
  va_end(args);
}

rstl::CRefData rstl::CRefData::sNull(nullptr, 0x1000000 - 1);
class ForwardDeleted {
public:
  explicit ForwardDeleted(int& destroyed) : mDestroyed(destroyed) {}
  ~ForwardDeleted() { ++mDestroyed; }
private:
  int& mDestroyed;
};
void DropForwardOwner(rstl::rc_ptr<ForwardDeleted>& owner);

// The ownership test isolates the deleter from the emulated arena.
void CMemory::Free(const void* ptr) {
  if (ptr != nullptr) {
    ++gameFrees;
    std::free(const_cast<void*>(ptr));
  }
}

int main() {
  int destroyed = 0;
  rstl::rc_ptr<ForwardDeleted> forward(new ForwardDeleted(destroyed));
  DropForwardOwner(forward);
  Check(destroyed == 1 && forward.IsNull());
  {
    rstl::auto_ptr<Element[]> first(new Element[7]);
    rstl::auto_ptr<Element[]> second(first);
    Check(!first.owner() && second.owner() && Element::live == 7);
    first = new Element[3];
    second = first;
    Check(Element::live == 3 && !first.owner());
  }
  Check(Element::live == 0);
  {
    rstl::single_ptr<Element[]> first(new Element[4]);
    rstl::single_ptr<Element[]> second(first);
    Check(first.null() && Element::live == 4);
    second = second.get();
    Check(Element::live == 4);
    second = nullptr;
  }
  Check(Element::live == 0);
  {
    rstl::auto_ptr<rstl::game_memory<uchar>> first(static_cast<uchar*>(std::malloc(64)));
    rstl::auto_ptr<rstl::game_memory<uchar>> second(first);
  }
  Check(gameFrees == 1);
  CPFBitSet regions;
  regions.Add(511);
  regions.Add(512);
  regions.Add(4096);
  Check(regions.Test(511) && regions.Test(512) && regions.Test(4096));
  regions.Rmv(512);
  Check(!regions.Test(512) && regions.Test(511));
  regions.Clear();
  Check(!regions.Test(4096));

  // Retail save fields are MSB-first, including fields straddling words.
  const uchar golden[] = {0xb5, 0x12, 0x34, 0x56, 0x78, 0xa0};
  std::array<uchar, 6> encoded{};
  {
    CMemoryStreamOut out(encoded.data(), encoded.size());
    out.WriteBits(5, 3);
    out.WriteBits(0x15, 5);
    out.WriteBits(0, 0);
    out.WriteBits(0x12345678, 32);
    out.WriteBits(5, 3);
    out.Flush();
  }
  for (size_t i = 0; i < encoded.size(); ++i)
    Check(encoded[i] == golden[i]);
  CMemoryInStream in(golden, sizeof(golden));
  Check(in.ReadBits(0) == 0);
  Check(in.ReadBits(3) == 5);
  Check(in.ReadBits(5) == 0x15);
  Check(in.ReadBits(32) == 0x12345678);
  Check(in.ReadBits(3) == 5);

  // Both small and direct-read EOF paths used to spin forever.
  for (uint size : {1u, 300u}) {
    CMemoryInStream empty(nullptr, 0);
    std::array<uchar, 300> dest{};
    bool failed = false;
    try { empty.Get(dest.data(), size); }
    catch (const std::runtime_error&) { failed = true; }
    Check(failed);
  }
  std::puts("port buffer and serialization regressions passed");
}
