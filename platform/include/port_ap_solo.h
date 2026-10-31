#ifndef METROID_PRIME_PORT_PORT_AP_SOLO_H
#define METROID_PRIME_PORT_PORT_AP_SOLO_H

#include "port_json.h"
#include "port_rando_gen.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// An Archipelago server for one built-in randomizer seed, in this process. It
// stands in for PortWs::Client in the AP client's worker loop, so a solo game
// goes through the same Protocol::Session, item grants and pickup patches as a
// multiworld, and nothing else needs to know the difference.
//
// It speaks only what the client sends: one player ("Samus"), whose
// locations hold the seed's placements. Every method may be called from
// different threads; the client's socket thread is the one that uses it.
namespace port {
class JsonWriter;
}

namespace PortApSolo {

// The slot name a solo game is played as, and the server string that selects
// this server ("solo:<seed name>").
constexpr const char* kSlotName = "Samus";
constexpr const char* kServerPrefix = "solo:";

// "solo:<name>" -> name; false for any other server string.
bool ParseServer(const std::string& server, std::string& seedName);

// <user dir>/randomizer_seeds/<name>.state.json: the checked locations, in
// order, so a later launch hands out the same item indices.
std::string StatePath(const std::string& seedName);

class Server {
public:
  // The seed from its file in randomizer_seeds, with its state beside it.
  bool Open(const std::string& seedName, std::string& error);
  // A seed in memory; `statePath` empty keeps the state in memory only.
  bool OpenSeed(const PortRandoGen::Seed& seed, const std::string& statePath, std::string& error);

  // The same shape as PortWs::Client, which the worker loop is written for.
  bool IsOpen() const;
  bool SendText(const std::string& message);
  // Waits up to timeoutMs for the next message; false with Error() "receive
  // timed out" when none came.
  bool ReceiveText(std::string& message, int timeoutMs);
  const char* Error() const { return mError.c_str(); }
  void Close();

  // The locations checked so far, in order.
  std::vector< int64_t > CheckedLocations() const;

private:
  struct Received {
    int64_t item;
    int64_t location;
  };

  // Everything below runs with mMutex held.
  void Handle(const PortJson::Value& packet);
  void Queue(const std::string& packet);
  int64_t Flags(int64_t item) const;
  void WriteItem(port::JsonWriter& json, const Received& received) const;
  std::string ReceivedItemsJson(size_t from) const;
  bool Check(int64_t location);
  void Give(int64_t item);
  void SaveState();
  void Command(const std::string& text);

  mutable std::mutex mMutex;
  std::condition_variable mWake;
  bool mOpen = false;
  std::string mError;
  PortRandoGen::Seed mSeed;
  std::string mStatePath;
  std::vector< int64_t > mChecked;
  std::vector< Received > mReceived; // start items, then one per check or command grant
  // Items granted by a command, each with the number of checks made before it.
  std::vector< std::pair< size_t, int64_t > > mGiven;
  std::deque< std::string > mOutgoing;
};

} // namespace PortApSolo

#endif // METROID_PRIME_PORT_PORT_AP_SOLO_H
