#include "port_savestate.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

using PortSaveState::Header;
} // namespace

int main() {
  Header in;
  in.worldId = 0x83F6FF6F;
  in.areaId = 17;
  in.position[0] = -120.5f;
  in.position[1] = 33.25f;
  in.position[2] = 9.f;
  in.forward[0] = 0.f;
  in.forward[1] = -1.f;
  in.forward[2] = 0.f;
  in.morphed = true;
  in.playTime = 3723.5;
  in.savedAt = 1790000000;
  in.world = "Chozo Ruins";
  in.room = "Main Plaza";
  std::vector< uint8_t > blob(4096, 0);
  for (size_t i = 0; i < 940; ++i)
    blob[i] = static_cast< uint8_t >(i * 7);

  const std::string data = PortSaveState::Encode(in, blob);

  // Round trip.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(PortSaveState::Decode(data, out, outBlob));
    REQUIRE(out.worldId == in.worldId);
    REQUIRE(out.areaId == in.areaId);
    for (int i = 0; i < 3; ++i) {
      REQUIRE(out.position[i] == in.position[i]);
      REQUIRE(out.forward[i] == in.forward[i]);
    }
    REQUIRE(out.morphed);
    REQUIRE(out.playTime == in.playTime);
    REQUIRE(out.savedAt == in.savedAt);
    REQUIRE(out.world == in.world);
    REQUIRE(out.room == in.room);
    REQUIRE(outBlob == blob);
  }

  // Every truncation fails and leaves the outputs alone.
  for (size_t len = 0; len < data.size(); ++len) {
    Header out;
    out.worldId = 1;
    std::vector< uint8_t > outBlob{9};
    REQUIRE(!PortSaveState::Decode(data.substr(0, len), out, outBlob));
    REQUIRE(out.worldId == 1);
    REQUIRE(outBlob.size() == 1);
  }

  // Trailing junk, a wrong magic, a wrong version and an empty blob fail.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(!PortSaveState::Decode(data + "x", out, outBlob));
    std::string bad = data;
    bad[0] = 'X';
    REQUIRE(!PortSaveState::Decode(bad, out, outBlob));
    bad = data;
    bad[4] = 2;
    REQUIRE(!PortSaveState::Decode(bad, out, outBlob));
    REQUIRE(!PortSaveState::Decode(PortSaveState::Encode(in, {}), out, outBlob));
  }

  // A NaN position is refused (it would teleport Samus into nowhere).
  {
    Header nan = in;
    nan.position[1] = std::numeric_limits< float >::quiet_NaN();
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(!PortSaveState::Decode(PortSaveState::Encode(nan, blob), out, outBlob));
  }

  // Long labels are cut, not rejected.
  {
    Header longLabel = in;
    longLabel.room.assign(1000, 'r');
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(PortSaveState::Decode(PortSaveState::Encode(longLabel, blob), out, outBlob));
    REQUIRE(out.room.size() == 256);
  }

  // Script message journal: what is kept, dedupe and cap, payload round trip.
  {
    using namespace PortSaveState;
    REQUIRE(JournalsScriptMsg(19) && JournalsScriptMsg(14) && JournalsScriptMsg(5));
    REQUIRE(!JournalsScriptMsg(1) && !JournalsScriptMsg(4) && !JournalsScriptMsg(17));  // active flag
    REQUIRE(!JournalsScriptMsg(0) && !JournalsScriptMsg(8));                           // Arrived, Next
    REQUIRE(!JournalsScriptMsg(22) && !JournalsScriptMsg(40));                         // notifications

    std::vector< ScriptMsgEntry > journal;
    REQUIRE(AppendScriptMsg(journal, {19, 0x00180074}));
    REQUIRE(!AppendScriptMsg(journal, {19, 0x00180074}));  // a repeat of the last one
    REQUIRE(AppendScriptMsg(journal, {14, 0xFFFFFFFFu}));
    REQUIRE(AppendScriptMsg(journal, {19, 0x00180074}));   // not the last one: kept
    REQUIRE(journal.size() == 3);
    std::vector< ScriptMsgEntry > full;
    for (size_t i = 0; i < kMaxScriptMsgs; ++i)
      REQUIRE(AppendScriptMsg(full, {static_cast< uint8_t >(i % 2 ? 5 : 7), static_cast< uint32_t >(i)}));
    REQUIRE(!AppendScriptMsg(full, {19, 1}) && full.size() == kMaxScriptMsgs);

    std::vector< ScriptMsgEntry > out;
    const std::string payload = EncodeScriptMsgs(journal);
    REQUIRE(payload.size() == 1 + 5 * 3);
    REQUIRE(DecodeScriptMsgs(payload, out) && out.size() == 3);
    REQUIRE(out[0].msg == 19 && out[0].sender == 0x00180074 && out[1].msg == 14 &&
            out[1].sender == 0xFFFFFFFFu && out[2].msg == 19);
    REQUIRE(DecodeScriptMsgs(EncodeScriptMsgs(full), out) && out.size() == kMaxScriptMsgs);
    REQUIRE(DecodeScriptMsgs(EncodeScriptMsgs({}), out) && out.empty());

    // Truncated, padded, oversized and unjournalled payloads leave `out` alone.
    out = journal;
    for (size_t len = 0; len < payload.size(); ++len)
      REQUIRE(!DecodeScriptMsgs(payload.substr(0, len), out) && out.size() == 3);
    REQUIRE(!DecodeScriptMsgs(payload + "x", out));
    std::string bad = payload;
    bad[0] = static_cast< char >(kMaxScriptMsgs + 1);
    REQUIRE(!DecodeScriptMsgs(bad, out));
    bad = payload;
    bad[1] = 1;  // Activate is never journalled
    REQUIRE(!DecodeScriptMsgs(bad, out) && out.size() == 3);

    // The generated and messages blocks survive the room section round trip.
    Header withMsgs = in;
    withMsgs.roomState.areas = {36};
    withMsgs.roomState.present = {0x0024001A};
    RoomObject wasp;
    wasp.editorId = 0x0024001A;
    wasp.active = true;
    wasp.blocks.push_back({10, payload});
    RoomObject copy = wasp;
    copy.blocks = {{9, std::string()}};
    withMsgs.roomState.objects = {wasp, copy};
    Header got;
    std::vector< uint8_t > gotBlob;
    REQUIRE(Decode(Encode(withMsgs, blob), got, gotBlob) && got.roomState.objects.size() == 2);
    const RoomBlock* m = FindBlock(got.roomState.objects[0], 10);
    REQUIRE(m != nullptr && m->data == payload && FindBlock(got.roomState.objects[0], 9) == nullptr);
    const RoomBlock* g = FindBlock(got.roomState.objects[1], 9);
    REQUIRE(g != nullptr && g->data.empty() && got.roomState.objects[1].editorId == 0x0024001A);
  }

  // Room state: round trip, skipping of unknown tags and versions, old files.
  {
    using namespace PortSaveState;
    Header withRoom = in;
    withRoom.roomState.areas = {2, 5};
    withRoom.roomState.present = {0x00020001, 0x00050007};
    RoomObject enemy;
    enemy.editorId = 0x00020001;
    enemy.active = true;
    BlockWriter hp;
    hp.F32(37.5f);
    hp.F32(0.25f);
    enemy.blocks.push_back({3, hp.bytes});
    BlockWriter ai;
    ai.U8(1);
    ai.Str("Patrol");
    ai.F32(4.5f);
    enemy.blocks.push_back({4, ai.bytes});
    enemy.blocks.push_back({0xBE, "future block"});
    RoomObject door;
    door.editorId = 0x00050007;
    // The door payload (tag 7) lives inside the fingerprinted section: a two-byte
    // mid-close payload (closed, normal close still owed, asked for by another
    // door) and the RMST version 2 fingerprints must survive the same round trip,
    // which is what the runtime writes when both features are on.
    door.blocks.push_back({7, EncodeDoorSaveState(DoorSaveState{false, true, false, true})});
    withRoom.roomState.objects = {enemy, door};
    // Fingerprints: one entry per captured area, in the same order, with the
    // area the entry belongs to; an unreadable area records itself as unknown.
    withRoom.roomState.fingerprintsRecorded = true;
    withRoom.roomState.fingerprints = {{2, true, 0x1122334455667788ull}, {5, false, 0}};

    const std::string file = Encode(withRoom, blob);
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(Decode(file, out, outBlob));
    REQUIRE(outBlob == blob);
    REQUIRE(out.roomState.areas == withRoom.roomState.areas);
    REQUIRE(out.roomState.present == withRoom.roomState.present);
    REQUIRE(out.roomState.fingerprintsRecorded);
    REQUIRE(out.roomState.fingerprints.size() == 2);
    REQUIRE(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
            out.roomState.fingerprints[0].hash == 0x1122334455667788ull);
    REQUIRE(out.roomState.fingerprints[1].area == 5 && !out.roomState.fingerprints[1].known &&
            out.roomState.fingerprints[1].hash == 0);
    REQUIRE(out.roomState.objects.size() == 2);
    const RoomObject& got = out.roomState.objects[0];
    REQUIRE(got.editorId == 0x00020001 && got.active);
    REQUIRE(out.roomState.objects[1].editorId == 0x00050007 && !out.roomState.objects[1].active);
    // The door block came back byte for byte and still decodes as mid-close.
    {
      const RoomBlock* doorBlock = FindBlock(out.roomState.objects[1], 7);
      REQUIRE(doorBlock != nullptr &&
              doorBlock->data == EncodeDoorSaveState(DoorSaveState{false, true, false, true}));
      DoorSaveState restored;
      REQUIRE(DecodeDoorSaveState(doorBlock->data, restored) && !restored.open &&
              restored.pendingClose && !restored.refusedClosePending && restored.doClose);
    }
    // The file carries the fingerprinted section (RMST version 2).
    const size_t roomTrailer = data.size();
    REQUIRE(file.compare(roomTrailer, 4, "RMST") == 0 &&
            static_cast< uint8_t >(file[roomTrailer + 4]) == 2);
    const RoomBlock* b = FindBlock(got, 3);
    REQUIRE(b != nullptr);
    BlockReader hpIn(b->data);
    REQUIRE(hpIn.F32() == 37.5f && hpIn.F32() == 0.25f && hpIn.ok);
    b = FindBlock(got, 4);
    REQUIRE(b != nullptr);
    BlockReader aiIn(b->data);
    REQUIRE(aiIn.U8() == 1 && aiIn.Str() == "Patrol" && aiIn.F32() == 4.5f && aiIn.ok);
    REQUIRE(FindBlock(got, 0xBE) != nullptr && FindBlock(got, 9) == nullptr);
    BlockReader shortIn(b->data);
    shortIn.U32();
    shortIn.U32();
    shortIn.U32();
    shortIn.U32();
    REQUIRE(!shortIn.ok);

    // A file without the section (an old state) loads with an empty room state.
    REQUIRE(Decode(data, out, outBlob) && out.roomState.empty());

    // A newer version of the section is skipped, the state still loads.
    std::string newer = file;
    const size_t trailer = data.size();
    REQUIRE(newer.compare(trailer, 4, "RMST") == 0);
    newer[trailer + 4] = 9;
    REQUIRE(Decode(newer, out, outBlob) && out.roomState.empty() && outBlob == blob);

    // A damaged section is dropped; the rest of the state is kept.
    for (size_t len = trailer + 1; len < file.size(); ++len) {
      Header cut;
      REQUIRE(!Decode(file.substr(0, len), cut, outBlob) || cut.roomState.empty());
    }
    std::string bogus = file;
    bogus[trailer + 12] = static_cast< char >(0xFF);  // area count absurd
    bogus[trailer + 15] = static_cast< char >(0x7F);
    REQUIRE(Decode(bogus, out, outBlob) && out.roomState.empty());
    REQUIRE(!Decode(data + "RMS", out, outBlob));

    // The resume wrapper carries it too.
    ResumeMeta meta;
    meta.fileIdx = 0;
    std::string backState;
    ResumeMeta backMeta;
    REQUIRE(DecodeResume(EncodeResume(meta, file), backMeta, backState));
    REQUIRE(Decode(backState, out, outBlob) && out.roomState.objects.size() == 2);
  }

  // Room script fingerprints (RMST version 2): the hash and the per-area
  // decision the runtime makes, a v2 round trip, an explicit v1 file, an
  // unknown version and malformed fingerprint associations.
  {
    using namespace PortSaveState;

    std::vector< uint8_t > script(64);
    for (size_t i = 0; i < script.size(); ++i)
      script[i] = static_cast< uint8_t >(i * 3 + 1);
    const uint64_t hash = FingerprintScriptData(script.data(), script.size());
    REQUIRE(hash != 0);
    REQUIRE(FingerprintScriptData(script.data(), script.size()) == hash);
    REQUIRE(FingerprintScriptData(nullptr, 0) == 0);
    REQUIRE(FingerprintScriptData(script.data(), 0) == 0);
    std::vector< uint8_t > changed = script;
    changed[script.size() - 1] ^= 0x80;
    REQUIRE(FingerprintScriptData(changed.data(), changed.size()) != hash);
    REQUIRE(FingerprintScriptData(script.data(), script.size() - 1) != hash);

    // Nothing to read means no fingerprint: unknown, never a hash of its own.
    const AreaFingerprint none = MakeAreaFingerprint(5, nullptr, 0);
    REQUIRE(none.area == 5 && !none.known && none.hash == 0);
    const AreaFingerprint known = MakeAreaFingerprint(2, script.data(), script.size());
    REQUIRE(known.area == 2 && known.known && known.hash == hash);

    // What ApplyRoomState decides per area, through the same helper: matching
    // accepted, changed and unreadable rejected (they are skipped whole), and a
    // state without fingerprints restored as it was before.
    RoomState saved;
    saved.fingerprintsRecorded = true;
    saved.areas = {2, 5};
    saved.fingerprints = {known, none};
    REQUIRE(DecideAreaRestore(saved, 2, script.data(), script.size()) == RoomAreaRestore::kRestore);
    REQUIRE(DecideAreaRestore(saved, 2, changed.data(), changed.size()) == RoomAreaRestore::kSkipChanged);
    REQUIRE(DecideAreaRestore(saved, 5, script.data(), script.size()) == RoomAreaRestore::kSkipUnreadable);
    REQUIRE(DecideAreaRestore(saved, 9, script.data(), script.size()) == RoomAreaRestore::kSkipUnreadable);
    REQUIRE(DecideAreaRestore(saved, 2, nullptr, 0) == RoomAreaRestore::kSkipUnreadable);
    RoomState legacy;
    legacy.areas = {2};
    REQUIRE(DecideAreaRestore(legacy, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);
    REQUIRE(DecideAreaRestore(legacy, 2, nullptr, 0) == RoomAreaRestore::kRestore);

    // v2 round trip through the encoder: both fingerprints come back attached
    // to their own area.
    Header withFp = in;
    withFp.roomState = saved;
    Header out;
    std::vector< uint8_t > outBlob;
    const std::string file = Encode(withFp, blob);
    REQUIRE(Decode(file, out, outBlob));
    REQUIRE(out.roomState.fingerprintsRecorded);
    REQUIRE(out.roomState.areas == saved.areas);
    REQUIRE(out.roomState.fingerprints.size() == 2);
    REQUIRE(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
            out.roomState.fingerprints[0].hash == hash);
    REQUIRE(out.roomState.fingerprints[1].area == 5 && !out.roomState.fingerprints[1].known &&
            out.roomState.fingerprints[1].hash == 0);
    REQUIRE(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) == RoomAreaRestore::kRestore);
    REQUIRE(DecideAreaRestore(out.roomState, 5, script.data(), script.size()) ==
            RoomAreaRestore::kSkipUnreadable);

    // Fingerprints that do not line up one per area fail closed: the writer
    // leaves out the whole optional room-state trailer, so the base game-state
    // blob still loads and nothing can be restored unconditionally onto areas
    // the fingerprints do not describe.
    Header unaligned = in;
    unaligned.roomState.areas = {2, 5};
    unaligned.roomState.fingerprintsRecorded = true;
    unaligned.roomState.fingerprints = {known};  // one for two areas
    const std::string shortFile = Encode(unaligned, blob);
    REQUIRE(shortFile.size() == data.size());  // no trailer at all
    REQUIRE(Decode(shortFile, out, outBlob));
    REQUIRE(outBlob == blob && out.worldId == in.worldId && out.room == in.room);
    REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    // ... and the same for entries that line up in count but not in area.
    Header swapped = in;
    swapped.roomState.areas = {2, 5};
    swapped.roomState.fingerprintsRecorded = true;
    swapped.roomState.fingerprints = {MakeAreaFingerprint(5, script.data(), script.size()),
                                      MakeAreaFingerprint(2, script.data(), script.size())};
    const std::string swappedFile = Encode(swapped, blob);
    REQUIRE(swappedFile.size() == data.size());
    REQUIRE(Decode(swappedFile, out, outBlob));
    REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    // A fingerprinted snapshot that lines up is RMST version 2 (the encoder's
    // own file), and a legacy one below stays version 1.
    REQUIRE(static_cast< uint8_t >(file[data.size() + 4]) == 2);

    // A state written before fingerprints existed (fingerprintsRecorded false)
    // still writes the legacy body and restores as it always did.
    Header legacyState = in;
    legacyState.roomState.areas = {2};
    legacyState.roomState.present = {0x00020001};
    RoomObject only;
    only.editorId = 0x00020001;
    legacyState.roomState.objects = {only};
    const std::string legacyFile = Encode(legacyState, blob);
    REQUIRE(static_cast< uint8_t >(legacyFile[data.size() + 4]) == 1);
    REQUIRE(Decode(legacyFile, out, outBlob));
    REQUIRE(!out.roomState.fingerprintsRecorded && out.roomState.fingerprints.empty());
    REQUIRE(out.roomState.areas.size() == 1 && out.roomState.objects.size() == 1);
    REQUIRE(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);

    // A hash of 0 is a value a fingerprint may legitimately carry (the known
    // bit is what says an entry was hashed): it survives the round trip and
    // still takes part in the decision, rather than being read as malformed.
    Header zeroHash = in;
    zeroHash.roomState.areas = {2};
    zeroHash.roomState.fingerprintsRecorded = true;
    zeroHash.roomState.fingerprints = {{2, true, 0}};
    REQUIRE(Decode(Encode(zeroHash, blob), out, outBlob));
    REQUIRE(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 1);
    REQUIRE(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
            out.roomState.fingerprints[0].hash == 0);
    REQUIRE(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) ==
            RoomAreaRestore::kSkipChanged);
    // An entry that was never hashed carries no hash, and stays unknown.
    Header unknown = in;
    unknown.roomState.areas = {2};
    unknown.roomState.fingerprintsRecorded = true;
    unknown.roomState.fingerprints = {MakeAreaFingerprint(2, nullptr, 0)};
    REQUIRE(Decode(Encode(unknown, blob), out, outBlob));
    REQUIRE(out.roomState.fingerprints[0].area == 2 && !out.roomState.fingerprints[0].known);
    REQUIRE(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) ==
            RoomAreaRestore::kSkipUnreadable);

    // Spawn layers (RMST version 3, issue #161): one entry per area in the same
    // order, the mask of layers the area spawned with, and an unobserved area stays
    // unknown with no mask. The version-2 file of the same state still parses and
    // reports no spawn layers.
    Header layered = in;
    layered.roomState = saved;
    layered.roomState.spawnLayersRecorded = true;
    layered.roomState.spawnLayers = {{2, true, 0x2Bull | (1ull << 40)}, {5, false, 0}};
    const std::string layeredFile = Encode(layered, blob);
    // Written as version 4: version 3 with 16-bit block lengths.
    REQUIRE(static_cast< uint8_t >(layeredFile[data.size() + 4]) == 4);
    REQUIRE(Decode(layeredFile, out, outBlob));
    REQUIRE(outBlob == blob && out.roomState.areas == saved.areas);
    REQUIRE(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 2);
    REQUIRE(out.roomState.spawnLayersRecorded && out.roomState.spawnLayers.size() == 2);
    REQUIRE(out.roomState.spawnLayers[0].area == 2 && out.roomState.spawnLayers[0].known &&
            out.roomState.spawnLayers[0].mask == (0x2Bull | (1ull << 40)));
    REQUIRE(out.roomState.spawnLayers[1].area == 5 && !out.roomState.spawnLayers[1].known &&
            out.roomState.spawnLayers[1].mask == 0);
    // A block past 255 bytes (an enemy's kTagFields) survives version 4; one of 64 KiB
    // or more is still dropped.
    Header wide = layered;
    RoomObject big;
    big.editorId = 0x00020001;
    big.blocks = {{13, std::string(1700, 'f')}, {3, std::string(8, 'h')}, {14, std::string(1u << 16, 'x')}};
    wide.roomState.present = {big.editorId};
    wide.roomState.objects = {big};
    REQUIRE(Decode(Encode(wide, blob), out, outBlob));
    REQUIRE(out.roomState.objects.size() == 1 && out.roomState.objects[0].blocks.size() == 2);
    REQUIRE(out.roomState.objects[0].blocks[0].tag == 13 &&
            out.roomState.objects[0].blocks[0].data == std::string(1700, 'f'));
    REQUIRE(out.roomState.objects[0].blocks[1].tag == 3 &&
            out.roomState.objects[0].blocks[1].data == std::string(8, 'h'));
    REQUIRE(Decode(file, out, outBlob));  // the version 2 file from above
    REQUIRE(out.roomState.fingerprintsRecorded && !out.roomState.spawnLayersRecorded &&
            out.roomState.spawnLayers.empty());
    // Entries that do not line up one per area fail closed, as fingerprints do.
    Header crooked = layered;
    crooked.roomState.spawnLayers = {{2, true, 3}};
    REQUIRE(Encode(crooked, blob).size() == data.size());
    crooked.roomState.spawnLayers = {{5, true, 3}, {2, true, 3}};
    REQUIRE(Encode(crooked, blob).size() == data.size());

    // Little-endian builders, so a hand-written trailer can carry bodies the
    // writer never produces.
    const auto le32 = [](uint32_t v) {
      std::string s;
      for (int i = 0; i < 4; ++i)
        s += static_cast< char >((v >> (i * 8)) & 0xFF);
      return s;
    };
    const auto le64 = [&](uint64_t v) {
      return le32(static_cast< uint32_t >(v)) + le32(static_cast< uint32_t >(v >> 32));
    };
    const auto fpEntry = [&](uint32_t area, uint32_t flags, uint64_t h) {
      return le32(area) + std::string(1, static_cast< char >(flags)) + le64(h);
    };
    const auto trailer = [&](uint32_t version, const std::string& body) {
      std::string t("RMST");
      return data + t + le32(version) + le32(static_cast< uint32_t >(body.size())) + body;
    };
    // A v1 body: one area, one object (editor id 2 = zigzag delta varint 0x04),
    // no fingerprint section.
    const std::string v1body = le32(1) + le32(2) + le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    const std::string v1file = trailer(1, v1body);
    REQUIRE(Decode(v1file, out, outBlob));
    REQUIRE(out.roomState.fingerprintsRecorded == false && out.roomState.fingerprints.empty());
    REQUIRE(out.roomState.areas.size() == 1 && out.roomState.areas[0] == 2);
    REQUIRE(out.roomState.objects.size() == 1 && out.roomState.objects[0].editorId == 2);
    REQUIRE(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);

    // A v2 body hand-written the way the writer writes it, next to the
    // encoder's own output: same fields, both areas with a fingerprint.
    const std::string v2body = le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) +
                               fpEntry(2, 1, 0x1111222233334444ull) + fpEntry(5, 1, 0x5555666677778888ull) +
                               le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    REQUIRE(Decode(trailer(2, v2body), out, outBlob));
    REQUIRE(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 2);
    REQUIRE((out.roomState.areas == std::vector< int32_t >{2, 5}));
    REQUIRE(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].hash == 0x1111222233334444ull);
    REQUIRE(out.roomState.fingerprints[1].area == 5 && out.roomState.fingerprints[1].hash == 0x5555666677778888ull);
    REQUIRE(DecideAreaRestore(out.roomState, 5, script.data(), script.size()) ==
            RoomAreaRestore::kSkipChanged);

    // A version this build does not know is skipped, as before v2 existed.
    REQUIRE(Decode(trailer(3, v2body), out, outBlob));
    REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    REQUIRE(Decode(trailer(0, v1body), out, outBlob));
    REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);

    // Malformed fingerprint associations: the whole room state is dropped (the
    // state itself still loads) rather than applied to areas it does not
    // describe. Truncation is covered with it: every prefix of such a body is
    // dropped or refused too.
    const std::string bodies2[] = {
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(1) + fpEntry(2, 1, 0x1111) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) + fpEntry(5, 1, 0x1111) +
            fpEntry(2, 1, 0x2222) + le32(0),
        le32(2) + le32(5) + le32(2) + std::string(1, 1) + le32(2) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 2) + le32(2) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) + fpEntry(2, 0, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(3) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
    };
    for (const std::string& body : bodies2) {
      const std::string bad = trailer(2, body);
      REQUIRE(Decode(bad, out, outBlob));
      REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
      // ...and every truncation of one is dropped or refused too.
      for (size_t len = data.size() + 12; len < bad.size(); ++len) {
        Header cut;
        REQUIRE(!Decode(bad.substr(0, len), cut, outBlob) || cut.roomState.empty());
      }
    }
    // A hand-written v2 body whose hashed entry carries a 0 hash is accepted:
    // the known bit, not the value, says the entry was hashed.
    const std::string zeroBody = le32(1) + le32(2) + std::string(1, 1) + le32(1) + fpEntry(2, 1, 0) +
                                 le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    REQUIRE(Decode(trailer(2, zeroBody), out, outBlob));
    REQUIRE(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 1);
    REQUIRE(out.roomState.fingerprints[0].known && out.roomState.fingerprints[0].hash == 0);
    // The v2 body cut short inside its last object, with the trailer length
    // still consistent: refused, not read past the end.
    REQUIRE(Decode(trailer(2, v2body.substr(0, v2body.size() - 4)), out, outBlob));
    REQUIRE(out.roomState.empty() && !out.roomState.fingerprintsRecorded);

    // A v2 body that says it carries no fingerprints restores as a legacy one
    // does (what an older writer produces, and the only remaining way to write
    // without fingerprints).
    const std::string v2NoFp =
        le32(1) + le32(2) + std::string(1, 0) + le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    REQUIRE(Decode(trailer(2, v2NoFp), out, outBlob));
    REQUIRE(!out.roomState.fingerprintsRecorded && out.roomState.areas.size() == 1);
    REQUIRE(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);
  }

  // Quick resume wrapper.
  {
    PortSaveState::ResumeMeta meta;
    meta.fileIdx = 1;
    meta.cardSerial = 0x1122334455667788ull;
    meta.identity = "ap:seed42";
    const std::string state = "state-bytes";
    const std::string file = PortSaveState::EncodeResume(meta, state);
    PortSaveState::ResumeMeta back;
    std::string backState;
    REQUIRE(PortSaveState::DecodeResume(file, back, backState));
    REQUIRE(back.fileIdx == 1 && back.cardSerial == meta.cardSerial && back.identity == meta.identity);
    REQUIRE(backState == state);
    REQUIRE(!PortSaveState::DecodeResume(file.substr(0, file.size() - 1), back, backState));
    REQUIRE(!PortSaveState::DecodeResume(file + "x", back, backState));
    REQUIRE(!PortSaveState::DecodeResume("MPSS" + file.substr(4), back, backState));
    REQUIRE(!PortSaveState::DecodeResume("", back, backState));
    PortSaveState::ResumeMeta badSlot = meta;
    badSlot.fileIdx = 7;
    REQUIRE(!PortSaveState::DecodeResume(PortSaveState::EncodeResume(badSlot, state), back, backState));

    REQUIRE(PortSaveState::ResumeMatches(meta, meta));
    PortSaveState::ResumeMeta other = meta;
    other.identity = "ap:seed43";
    REQUIRE(!PortSaveState::ResumeMatches(meta, other));
    other = meta;
    other.cardSerial = 1;
    REQUIRE(!PortSaveState::ResumeMatches(meta, other));
    other = meta;
    other.identity.clear();
    REQUIRE(!PortSaveState::ResumeMatches(meta, other));
  }

  // Quick resume skip rules.
  {
    PortSaveState::ResumeContext ctx;
    REQUIRE(PortSaveState::ResumeSkipReason(ctx) != nullptr);  // not in game
    ctx.inGame = true;
    ctx.initialized = true;
    REQUIRE(PortSaveState::ResumeSkipReason(ctx) == nullptr);
    bool PortSaveState::ResumeContext::*flags[] = {
        &PortSaveState::ResumeContext::quitting, &PortSaveState::ResumeContext::dead,
        &PortSaveState::ResumeContext::cinematic, &PortSaveState::ResumeContext::transition,
        &PortSaveState::ResumeContext::saveUi};
    for (auto flag : flags) {
      PortSaveState::ResumeContext c = ctx;
      c.*flag = true;
      REQUIRE(PortSaveState::ResumeSkipReason(c) != nullptr);
    }
    ctx.initialized = false;
    REQUIRE(PortSaveState::ResumeSkipReason(ctx) != nullptr);
  }

  // Snapshot selection at quit/background.
  {
    using PortSaveState::ResumeWriteChoice;
    REQUIRE(PortSaveState::ChooseResumeWrite(true, false) == ResumeWriteChoice::kRefreshThenWrite);
    REQUIRE(PortSaveState::ChooseResumeWrite(true, true) == ResumeWriteChoice::kRefreshThenWrite);
    REQUIRE(PortSaveState::ChooseResumeWrite(false, true) == ResumeWriteChoice::kWriteSnapshot);
    REQUIRE(PortSaveState::ChooseResumeWrite(false, false) == ResumeWriteChoice::kNothing);
  }
  // The kTagDoor payload (issue #131): byte 0 is the door's mIsOpen and is the
  // whole old format; byte 1 is a flag byte for the close paths a door can be
  // saved in the middle of - a normal close still animating (Think's animation-end
  // branch owes Closed, decrement included), a refused opening (the mClosing branch
  // owes Closed but skips the decrement) and a close another door asked for, which
  // also blocks this door from opening until it finishes. The policy is the pure
  // encode/decode pair the capture and the restore both call; that the engine
  // reaches them, and which branch sends what afterwards, is checked by
  // tests/port_door_save_wiring.py.
  {
    using PortSaveState::DoorSaveState;
    using PortSaveState::DecodeDoorSaveState;
    using PortSaveState::EncodeDoorSaveState;
    struct Case {
      DoorSaveState state;
      const char* bytes;
    };
    // Every state a door can be captured in, and its wire bytes.
    const Case cases[] = {
        {{false, false, false, false}, "\0\0"},  // closed, nothing owed
        {{true, false, false, false}, "\1\0"},   // open
        {{false, true, false, false}, "\0\1"},   // a normal close still animating
        {{false, true, false, true}, "\0\5"},    // ... asked for by another door
        {{false, false, true, false}, "\0\2"},   // a refused opening
        {{false, false, false, true}, "\0\4"},   // only the block on opening again
        // open, with its side of the door pair (issue #201)
        {{true, false, false, false, DoorSaveState::kPartnerNone}, "\1\x08"},
        {{true, false, false, false, DoorSaveState::kPartnerLeads}, "\1\x10"},
        {{true, false, false, false, DoorSaveState::kPartnerFollows}, "\1\x18"},
        {{true, false, false, true, DoorSaveState::kPartnerLeads}, "\1\x14"},
    };
    for (const Case& c : cases)
      REQUIRE(EncodeDoorSaveState(c.state) == std::string(c.bytes, 2));
    // Round trip of each, whole state compared.
    for (const Case& c : cases) {
      DoorSaveState got;
      REQUIRE(DecodeDoorSaveState(EncodeDoorSaveState(c.state), got));
      REQUIRE(got.open == c.state.open && got.pendingClose == c.state.pendingClose &&
              got.refusedClosePending == c.state.refusedClosePending &&
              got.doClose == c.state.doClose && got.partner == c.state.partner);
    }
    DoorSaveState out;
    // The mid-close payload an earlier build of this lane wrote (normal close, no
    // other flag) still decodes as exactly that.
    REQUIRE(DecodeDoorSaveState(std::string("\0\1", 2), out) && !out.open && out.pendingClose &&
            !out.refusedClosePending && !out.doClose);
    // Old snapshots: one byte, exactly the old behaviour (open flag alone).
    REQUIRE(DecodeDoorSaveState(std::string("\0", 1), out) && !out.open && !out.pendingClose &&
            !out.refusedClosePending && !out.doClose);
    REQUIRE(DecodeDoorSaveState(std::string("\1", 1), out) && out.open && !out.pendingClose &&
            !out.refusedClosePending && !out.doClose);

    // Invalid payloads are refused, not applied (the door is left as loaded).
    // Each payload is refused on its own, and each is checked against four
    // sentinels that differ field by field, so a decoder that published a field
    // before discovering the payload was malformed is caught whichever field it
    // wrote first.
    const std::string invalid[] = {
        std::string(),                 // empty
        std::string("\2", 1),          // open flag not 0/1
        std::string("\3", 1),          // ... and above the flag range
        std::string("\0\x08", 2),      // a pair side on a closed door
        std::string("\0\x18", 2),      // ... any side
        std::string("\0\x80", 2),      // unknown flag bit
        std::string("\1\x20", 2),      // unknown flag bit
        std::string("\1\1", 2),        // open with a normal close in flight
        std::string("\1\2", 2),        // open with a refused close in flight
        std::string("\0\3", 2),        // both close paths owed at once
        std::string("\0\0\0", 3),      // not a payload we write
        std::string("\1\0\0", 3),      // open, three bytes
    };
    const DoorSaveState sentinels[] = {DoorSaveState{true, false, false, false},
                                       DoorSaveState{false, true, false, false},
                                       DoorSaveState{false, false, true, false},
                                       DoorSaveState{false, false, false, true}};
    for (const std::string& payload : invalid) {
      for (const DoorSaveState& sentinel : sentinels) {
        out = sentinel;
        REQUIRE(!DecodeDoorSaveState(payload, out));
        REQUIRE(out.open == sentinel.open && out.pendingClose == sentinel.pendingClose &&
                out.refusedClosePending == sentinel.refusedClosePending &&
                out.doClose == sentinel.doClose);
      }
    }
  }
  // Snapshot refresh schedule.
  {
    using PortSaveState::ResumeRefreshDue;
    const double stable = PortSaveState::kResumeStableSeconds;
    const double period = PortSaveState::kResumeRefreshSeconds;
    // Never while unsafe.
    REQUIRE(!ResumeRefreshDue(false, true, true, 1000.0, 1000.0));
    REQUIRE(!ResumeRefreshDue(false, false, false, 1000.0, 0.0));
    // First snapshot, or a new room, waits for the room to be stable.
    REQUIRE(!ResumeRefreshDue(true, false, false, stable - 0.1, 0.0));
    REQUIRE(ResumeRefreshDue(true, false, false, stable, 0.0));
    REQUIRE(!ResumeRefreshDue(true, true, false, stable - 0.1, 5.0));
    REQUIRE(ResumeRefreshDue(true, true, false, stable, 5.0));
    // The same room refreshes on the period only.
    REQUIRE(!ResumeRefreshDue(true, true, true, 100.0, period - 0.1));
    REQUIRE(ResumeRefreshDue(true, true, true, 100.0, period));
  }

  // A label length over the cap is rejected, not read past.
  {
    std::string crafted = data;
    const size_t at = crafted.find("Main Plaza");
    REQUIRE(at != std::string::npos && at >= 4);
    crafted[at - 4] = 0x01;  // 257, little-endian
    crafted[at - 3] = 0x01;
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(!PortSaveState::Decode(crafted, out, outBlob));
  }

  // An object with more blocks than the decoder accepts keeps the first kMaxBlocks.
  {
    using namespace PortSaveState;
    Header many = in;
    RoomObject obj;
    obj.editorId = 0x00020001;
    obj.active = true;
    for (int i = 0; i < 130; ++i)
      obj.blocks.push_back({static_cast< uint32_t >(i), std::string(1, static_cast< char >(i))});
    many.roomState.areas = {2};
    many.roomState.present = {obj.editorId};
    many.roomState.objects = {obj};
    Header out;
    std::vector< uint8_t > outBlob;
    REQUIRE(Decode(Encode(many, blob), out, outBlob));
    REQUIRE(out.roomState.objects.size() == 1);
    const RoomObject& got = out.roomState.objects[0];
    REQUIRE(got.active && got.blocks.size() == 64);
    REQUIRE(got.blocks.back().tag == 63 && got.blocks.back().data == std::string(1, '\x3f'));
  }

  // Saved platforms are restored after every platform that carries them, whatever
  // order the snapshot lists them in, and a cycle is refused.
  {
    using PortSaveState::PlatformRestoreOrder;
    const auto before = [](const std::vector< size_t >& order, size_t a, size_t b) {
      return std::find(order.begin(), order.end(), a) < std::find(order.begin(), order.end(), b);
    };
    std::vector< size_t > order;
    REQUIRE(PlatformRestoreOrder({}, order) && order.empty());
    // 2 carries 0, 0 carries 1, 3 is alone.
    REQUIRE(PlatformRestoreOrder({{2}, {0}, {}, {}}, order));
    REQUIRE(order.size() == 4);
    REQUIRE(before(order, 2, 0) && before(order, 0, 1));
    // Two parents of one child.
    REQUIRE(PlatformRestoreOrder({{1, 2}, {}, {}}, order));
    REQUIRE(order.size() == 3 && before(order, 1, 0) && before(order, 2, 0));
    REQUIRE(!PlatformRestoreOrder({{1}, {2}, {0}}, order) && order.empty());
    REQUIRE(!PlatformRestoreOrder({{0}}, order) && order.empty());
    REQUIRE(!PlatformRestoreOrder({{5}}, order) && order.empty());
  }

  std::puts("savestate tests passed");
  return 0;
}
