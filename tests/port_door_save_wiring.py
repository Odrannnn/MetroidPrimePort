"""Production wiring guards for the issue #131 door save payload.

Byte 0 of the kTagDoor payload is the door's mIsOpen and is the whole old format;
byte 1 is a flag byte for the close paths a door can be captured in the middle of:
a normal close still animating (Think's animation-end branch owes Closed with its
dock decrement), a refused opening (mClosing: its own branch owes Closed but skips
the decrement connection) and a close another door asked for (mDoClose), which also
blocks this door from opening until it finishes.

The pure encode/decode pair is checked in tests/port_savestate.cpp; this checks that
the capture writes the flags from the door's own state, that the restore reaches the
door with them and sends nothing itself, and that Think keeps one branch per close
path with the message each one owes. A capture still written as IsOpen() alone, a
restore that drops a flag, or a restore that sends Closed itself, silently breaks
one of them.

It is a source check: no engine code runs and no door object is created here.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, braced, count, root_argument  # noqa: E402

root = root_argument()
HEADER = root / "include/MetroidPrime/ScriptObjects/CScriptDoor.hpp"
DOOR = root / "src/MetroidPrime/ScriptObjects/CScriptDoor.cpp"
ROOM = root / "platform/port_savestate_room.cpp"
FORMAT = root / "platform/port_savestate_format.cpp"


class Wiring(WiringCase):
    def test_capture_writes_every_flag(self):
        room = ROOM.read_text()
        block = between(room, "if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {",
                        "if (CScriptPlatform", str(ROOM))
        # The payload comes from the encoder, not from hand-written bytes.
        self.assertIn("EncodeDoorSaveState(state)", block)
        self.assertIn("state.open = door->IsOpen()", block)
        self.assertIn("state.pendingClose = door->PortClosingPending()", block)
        self.assertIn("state.refusedClosePending = door->PortRefusedClosePending()", block)
        self.assertIn("state.doClose = door->PortDoClosePending()", block)
        self.assertNotIn("U8(", block)
        # The pending bits are only read while the door reports closed: a payload
        # with a close in flight and open set is one the decoder refuses.
        self.assertIn("if (!state.open) {", block)
        # An open door records which side of its pair it is (issue #201): the
        # restore relinks only from the saved leader.
        self.assertIn("door->PortPartnerSide()", block)

    def test_restore_applies_every_flag(self):
        room = ROOM.read_text()
        block = between(room, "if (const RoomBlock* b = FindBlock(obj, kTagDoor)) {",
                        "if (CScriptPlatform", str(ROOM))
        # Invalid payloads are not applied at all: the apply is inside the decode.
        self.assertIn("if (DecodeDoorSaveState(b->data, want)) {", block)
        for flag in ("open", "normalClosePending", "refusedClosePending", "doClose", "partner"):
            self.assertIn(f"flags.{flag} = want.", block)
        self.assertIn("door->PortRestoreDoorState(flags)", block)
        # A pending close or a blocked re-open is applied even when the door
        # already reads closed.
        self.assertIn("flags.normalClosePending || flags.refusedClosePending || flags.doClose",
                      block)
        # Closed is not delivered from the restore: Think sends it.
        self.assertNotIn("SendScriptMsgs", block)
        self.assertNotIn("DeliverScriptMsg", block)

    def test_accessors_are_the_real_flags(self):
        header = HEADER.read_text()
        guard = between(header, "#ifdef TARGET_PC", "#endif", str(HEADER))
        bodies = {
            "PortClosingPending": "!mIsOpen && mWasOpen",
            "PortRefusedClosePending": "mClosing",
            "PortDoClosePending": "mDoClose",
        }
        for name, expr in bodies.items():
            self.assertIn(f"bool {name}() const {{ return {expr}; }}", guard)
            self.assertEqual(len([l for l in header.splitlines() if name in l]), 1, name)
        # The normal-close accessor is not the refused one.
        self.assertNotIn("mClosing", between(guard, "bool PortClosingPending() const {", "}",
                                            str(HEADER)))

    def test_restore_sets_every_bit_and_plays_close(self):
        door = DOOR.read_text()
        body = between(door, "void CScriptDoor::PortRestoreDoorState(const PortDoorFlags& state) {",
                       "\n}", str(DOOR))
        self.assertIn("mIsOpen = state.open;", body)
        self.assertIn("mWasOpen = state.open || state.normalClosePending;", body)
        self.assertIn("mClosing = state.refusedClosePending;", body)
        self.assertIn("mDoClose = state.doClose;", body)
        self.assertIn("mConditionsMet = false;", body)
        self.assertIn("SetDoorAnimation(state.open ? kDAT_Open : kDAT_Close);", body)
        self.assertNotIn("SendScriptMsgs", body)
        # The single-flag entry point is exactly the old behaviour, so a caller
        # that only knows open/closed cannot inherit a close in flight.
        header = HEADER.read_text()
        wrapper = braced(header, "void PortRestoreOpen(bool open)", str(HEADER))
        self.assertIn("PortRestoreDoorState(PortDoorFlags{open, false, false, false})", wrapper)

    def test_think_owes_one_message_per_close_path(self):
        door = DOOR.read_text()
        # A normal close: the animation-end branch, Closed with every connection.
        normal = between(door, "if (mWasOpen && !GetModelData()->IsAnimating()) {", "\n    }",
                         str(DOOR))
        self.assertIn("SendScriptMsgs(kSS_Closed, mgr, kSM_None);", normal)
        self.assertIn("mWasOpen = false;", normal)
        # A refused opening: its own branch, Closed skipping the dock decrement
        # (the third argument is the message to leave out).
        refused = between(door, "if (mClosing) {", "\n  }", str(DOOR))
        self.assertIn("SendScriptMsgs(kSS_Closed, mgr, kSM_Decrement);", refused)
        self.assertIn("mWasOpen = false;", refused)
        self.assertIn("mClosing = false;", refused)
        self.assertNotIn("kSM_None", refused)
        # Two branches only: a restored door cannot produce a duplicate Closed.
        self.assertEqual(count(door, "SendScriptMsgs(kSS_Closed"), 5)

    def test_decoder_publishes_only_valid_payloads(self):
        fmt = FORMAT.read_text()
        body = between(fmt, "bool DecodeDoorSaveState(const std::string& data, DoorSaveState& out) {",
                       "\n}", str(FORMAT))
        # Parsed locally and assigned once, after every check: a refused payload
        # leaves the caller's state exactly as it was.
        self.assertIn("out = parsed;", body)
        self.assertNotIn("out.open =", body)
        self.assertNotIn("out.pendingClose =", body)
        self.assertNotIn("out.refusedClosePending =", body)
        self.assertNotIn("out.doClose =", body)
        self.assertIn("kDoorSaveFlagsKnown", body)
        self.assertIn("parsed.open && (parsed.pendingClose || parsed.refusedClosePending)",
                      body)
        self.assertIn("parsed.pendingClose && parsed.refusedClosePending", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
