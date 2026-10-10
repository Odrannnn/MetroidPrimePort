#include "port_input_bindings.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

using namespace PortInput;

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "input bindings regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

Input In(Device d, uint16_t code, int8_t dir = 0, uint8_t threshold = 50) { return {d, dir, threshold, code}; }

Binding Bind(Action a, std::initializer_list<Input> inputs) {
  Binding b;
  b.action = a;
  for (const Input& in : inputs)
    b.inputs[b.count++] = in;
  return b;
}

bool RoundTrips(const Input& in) {
  Input back;
  return InputFromText(InputToText(in), back) && back == in;
}

std::string FakeKeyName(int code) { return code == 44 ? "space" : code == 30 ? "1" : code == 31 ? "@" : ""; }
int FakeKeyParse(std::string_view name) {
  return name == "space" ? 44 : name == "1" ? 30 : name == "@" ? 31 : -1;
}

int ErrorLine(const char* text) {
  UserBindings out;
  std::string error;
  if (ParseUserBindings(text, out, &error))
    return 0;
  return std::atoi(error.c_str() + 5); // "line N: ..."
}

void TestInputText() {
  for (int d = int(Device::Key); d <= int(Device::Gyro); ++d)
    for (uint16_t code : {0, 1, 2})
      for (int8_t dir : {0, 1, -1}) {
        Device device = Device(d);
        if ((device == Device::MouseWheel || device == Device::MouseMotion) && code > 1)
          continue;
        bool directed = device == Device::MouseWheel || device == Device::MouseMotion ||
                        device == Device::PadAxis || device == Device::TouchAxis || device == Device::Gyro;
        CHECK(RoundTrips(In(device, code, directed ? dir : 0)));
        CHECK(RoundTrips(In(device, code, directed ? dir : 0, 75)));
      }
  for (uint16_t code = 0; code < kMouseButtonCount; ++code)
    CHECK(RoundTrips(In(Device::MouseButton, code)));
  for (uint16_t code = 0; code < kPadButtonCount; ++code)
    CHECK(RoundTrips(In(Device::PadButton, code)));
  for (uint16_t code = 0; code < kPadAxisCount; ++code)
    CHECK(RoundTrips(In(Device::PadAxis, code, -1)));
  CHECK(RoundTrips(In(Device::Key, kKeyCount - 1)));
  CHECK(RoundTrips(In(Device::Touch, kTouchCount - 1)));

  CHECK(InputToText(In(Device::PadButton, 9)) == "pad:leftshoulder");
  CHECK(InputToText(In(Device::PadButton, 16)) == "pad:paddle1");
  CHECK(InputToText(In(Device::PadButton, 30)) == "pad:30");
  CHECK(InputToText(In(Device::PadAxis, 5, 1)) == "axis:righttrigger+");
  CHECK(InputToText(In(Device::PadAxis, 2)) == "axis:rightx");
  CHECK(InputToText(In(Device::MouseButton, 8)) == "mouse:left");
  CHECK(InputToText(In(Device::MouseButton, 2)) == "mouse:aim:right");
  CHECK(InputToText(In(Device::MouseButton, 17)) == "mouse:menu:middle");
  CHECK(InputToText(In(Device::MouseButton, 28)) == "mouse:held:x2");
  CHECK(InputToText(In(Device::MouseWheel, 0, 1)) == "wheel:up");
  CHECK(InputToText(In(Device::MouseWheel, 1, -1)) == "wheel:left");
  CHECK(InputToText(In(Device::MouseMotion, 1)) == "motion:y");
  CHECK(InputToText(In(Device::Gyro, 1, -1)) == "gyro:yaw-");
  CHECK(InputToText(In(Device::TouchAxis, 3, 1, 20)) == "touchaxis:3+@20");
  CHECK(InputToText(In(Device::Key, 44)) == "key:44");

  Input in;
  CHECK(InputFromText("axis:lefttrigger+@30", in) && in == In(Device::PadAxis, 4, 1, 30));
  CHECK(InputFromText(" pad:a ", in) && in == In(Device::PadButton, 0));
  CHECK(!InputFromText("pad:a@101", in));
  CHECK(!InputFromText("pad:nope", in));
  CHECK(!InputFromText("pad:32", in));
  CHECK(!InputFromText("touch:64", in));
  CHECK(!InputFromText("key:space", in)); // no key names installed
  CHECK(!InputFromText("bogus:1", in));
  CHECK(!InputFromText("pad:", in));
  CHECK(!InputFromText("gyro:3", in));

  SetKeyNames(FakeKeyName, FakeKeyParse);
  CHECK(InputToText(In(Device::Key, 44)) == "key:space");
  CHECK(InputToText(In(Device::Key, 45)) == "key:45");
  CHECK(InputFromText("key:space", in) && in == In(Device::Key, 44));
  CHECK(InputFromText("key:45", in) && in == In(Device::Key, 45));
  CHECK(!InputFromText("key:enter", in));
  CHECK(InputFromText("key:1", in) && in == In(Device::Key, 30)); // the name wins
  CHECK(InputFromText("key:@", in) && in == In(Device::Key, 31));
  CHECK(InputFromText("key:@@20", in) && in == In(Device::Key, 31, 0, 20));
  CHECK(RoundTrips(In(Device::Key, 31)) && RoundTrips(In(Device::Key, 30, 0, 75)));
  SetKeyNames(nullptr, nullptr);

  CHECK(InputToText(In(Device::Touch, 3)) == "touch:a");
  CHECK(InputToText(In(Device::Touch, 28)) == "touch:tturbo");
  CHECK(InputToText(In(Device::Touch, 32)) == "touch:beam_shift");
  CHECK(InputToText(In(Device::Touch, 40)) == "touch:40");
  CHECK(InputFromText("touch:missile", in) && in == In(Device::Touch, 20));
  CHECK(InputFromText("touch:map_tap", in) && in == In(Device::Touch, 34));
  CHECK(InputFromText("touch:5", in) && in == In(Device::Touch, 5));
  CHECK(!InputFromText("touch:nope", in));
  for (uint16_t code = 0; code < kTouchCount; ++code)
    CHECK(RoundTrips(In(Device::Touch, code)));
}

void TestContexts() {
  CHECK(ContextsToText(kCtxAll) == "all");
  CHECK(ContextsToText(kCtxGameplay | kCtxMap) == "gameplay,map");
  CHECK(ContextsToText(kCtxLayer2) == "layer2");
  uint16_t c = 0;
  CHECK(ContextFromText("morphball", c) && c == kCtxMorphBall);
  CHECK(ContextFromText("all", c) && c == kCtxAll);
  CHECK(!ContextFromText("nowhere", c));
}

void TestRoundTrip() {
  UserBindings in;
  UserProfile base;
  base.name = "Default \"mine\" \\ here";
  base.unbind = {{Action::PadZ, Family::Pad}, {Action::Screenshot, Family::Keyboard}};
  Binding chord = Bind(Action::BeamWave, {In(Device::PadButton, 9), In(Device::PadButton, 14)});
  base.bindings.push_back(chord);
  Binding any = Bind(Action::SaveState, {In(Device::Key, 224), In(Device::Key, 22), In(Device::Key, 4)});
  any.anyOrder = true;
  any.contexts = kCtxGameplay | kCtxMorphBall;
  base.bindings.push_back(any);
  for (Trigger t : {Trigger::Tap, Trigger::Hold, Trigger::DoubleTap, Trigger::Toggle}) {
    Binding b = Bind(Action::PadA, {In(Device::MouseButton, 10)});
    b.trigger = t;
    b.tapMs = 150;
    b.holdMs = 400;
    b.doubleMs = 333;
    base.bindings.push_back(b);
  }
  Binding turbo = Bind(Action::PadA, {In(Device::Touch, 33)});
  turbo.turboHz = 15;
  turbo.contexts = kCtxLayer1;
  base.bindings.push_back(turbo);
  Binding look = Bind(Action::LookX, {In(Device::Gyro, 1)});
  look.scale = 2.37f;
  look.invert = true;
  base.bindings.push_back(look);
  Binding whole = Bind(Action::LookY, {In(Device::MouseMotion, 1)});
  whole.scale = 3.f;
  base.bindings.push_back(whole);
  in.profiles.push_back(base);

  UserProfile pad;
  pad.name = "DualSense";
  pad.match = "type:ps5";
  pad.replace = true;
  pad.chordWindowMs = 60;
  pad.bindings.push_back(Bind(Action::PadB, {In(Device::PadAxis, 4, 1, 25)}));
  in.profiles.push_back(pad);
  UserProfile empty;
  empty.name = "";
  empty.match = "guid:0300ab";
  in.profiles.push_back(empty);

  std::string text = SerializeUserBindings(in);
  UserBindings back;
  std::string error;
  CHECK(ParseUserBindings(text, back, &error));
  CHECK(back == in);
  CHECK(SerializeUserBindings(back) == text);
  // Defaults are left out.
  CHECK(text.find("tap_ms = 200") == std::string::npos);
  CHECK(text.find("contexts = [\"all\"]") == std::string::npos);
  CHECK(text.find("contexts = [\"gameplay\", \"morphball\"]") != std::string::npos);
  CHECK(text.find("scale = 3.0") != std::string::npos);

  // Hand-written forms: comments, trailing comma, integer scale, "all".
  const char* hand = "# mine\n"
                     "[[profile]]  # the default\n"
                     "name = \"x\"\n"
                     "unbind = []\n"
                     "\n"
                     "[[profile.bind]]\n"
                     "action = \"pad_a\"\n"
                     "inputs = [\"pad:a\", ]\n"
                     "scale = 2\n"
                     "contexts = [\"all\"]\n";
  CHECK(ParseUserBindings(hand, back, &error));
  CHECK(back.profiles.size() == 1 && back.profiles[0].bindings.size() == 1);
  CHECK(back.profiles[0].match.empty());
  CHECK(back.profiles[0].bindings[0].scale == 2.f);
  CHECK(back.profiles[0].bindings[0].contexts == kCtxAll);
  CHECK(ParseUserBindings("", back) && back.profiles.empty());
  CHECK(ParseUserBindings("\xEF\xBB\xBF[[profile]]\nname = \"bom\"\n", back, &error));
  CHECK(back.profiles.size() == 1 && back.profiles[0].name == "bom");
}

void TestErrors() {
  CHECK(ErrorLine("[[profile]]\nname = \"a\"\n[[profile.bind]]\naction = \"jump\"\ninputs = [\"pad:a\"]\n") == 4);
  CHECK(ErrorLine("[[profile]]\ncolour = \"red\"\n") == 2);
  CHECK(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = [\"pad:zz\"]\n") == 4);
  CHECK(ErrorLine("\n[[profile.bind]]\naction = \"pad_a\"\n") == 2);
  CHECK(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\n"
                  "inputs = [\"pad:a\", \"pad:b\", \"pad:x\", \"pad:y\", \"pad:back\"]\n") == 4);
  CHECK(ErrorLine("[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = []\n") == 4);
  CHECK(ErrorLine("[[profile]]\n\n[[profile.bind]]\naction = \"pad_a\"\n") == 3);  // no inputs
  CHECK(ErrorLine("[[profile]]\nchord_window_ms = 70000\n") == 2);
  CHECK(ErrorLine("[[profile]]\nreplace = 1\n") == 2);
  CHECK(ErrorLine("[[profile]]\nname = \"open\n") == 2);
  CHECK(ErrorLine("[[profile]]\nunbind = [\"pad_a@mouse\"]\n") == 2);
  CHECK(ErrorLine("name = \"x\"\n") == 1);
  CHECK(ErrorLine("[controls]\n") == 1);
  const char* bind = "[[profile]]\n[[profile.bind]]\naction = \"pad_a\"\ninputs = [\"pad:a\"]\n";
  CHECK(ErrorLine((std::string(bind) + "scale = inf\n").c_str()) == 5);
  CHECK(ErrorLine((std::string(bind) + "scale = nan\n").c_str()) == 5);
  CHECK(ErrorLine((std::string(bind) + "scale = 1e300\n").c_str()) == 5);
  CHECK(ErrorLine((std::string(bind) + "scale = 101.0\n").c_str()) == 5);

  UserBindings out;
  out.profiles.emplace_back();
  CHECK(!ParseUserBindings("[[profile]]\nbad\n", out));
  CHECK(out.profiles.size() == 1); // left unchanged
}

void TestSelect() {
  UserBindings b;
  auto add = [&](const char* name, const char* match) {
    UserProfile p;
    p.name = name;
    p.match = match;
    b.profiles.push_back(p);
  };
  add("type", "type:PS5");
  add("base", "");
  add("guid", "guid:0300ABCD");
  add("base2", "");
  add("type2", "type:ps5");

  Selection s = Select(b, "0300abcd", "ps5");
  CHECK(s.base && s.base->name == "base");
  CHECK(s.pad && s.pad->name == "guid");
  s = Select(b, "ffff", "Ps5");
  CHECK(s.pad && s.pad->name == "type");
  s = Select(b, "", "");
  CHECK(s.base && s.base->name == "base" && !s.pad);
  s = Select(b, "", "xbox360");
  CHECK(!s.pad);
  CHECK(!Select({}, "a", "b").base);
}

void TestOverlay() {
  Profile base;
  base.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 44)}));
  base.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 0)}));
  base.bindings.push_back(Bind(Action::PadZ, {In(Device::PadButton, 10)}));
  base.bindings.push_back(Bind(Action::PadZ, {In(Device::Touch, 5)}));
  base.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 1)}));
  // A chord counts by its trigger (last input): this one is a keyboard binding.
  base.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 9), In(Device::Key, 5)}));

  UserProfile user;
  user.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 9), In(Device::PadButton, 2)}));
  user.unbind.push_back({Action::PadZ, Family::Pad});
  user.bindings.push_back(Bind(Action::PadB, {In(Device::Key, 6)}));

  Profile p = base;
  Overlay(p, user);
  CHECK(p.chordWindowMs == 40);
  CHECK(p.bindings.size() == 5);
  CHECK(p.bindings[0] == base.bindings[0]); // keyboard Fire kept
  CHECK(p.bindings[1] == base.bindings[3]); // touch Z kept
  CHECK(p.bindings[2] == base.bindings[4]); // pad B kept
  CHECK(p.bindings[3] == user.bindings[0]);
  CHECK(p.bindings[4] == user.bindings[1]);

  user.replace = true;
  user.chordWindowMs = 90;
  p = base;
  Overlay(p, user);
  CHECK(p.chordWindowMs == 90);
  CHECK(p.bindings == user.bindings);
}

void TestFamilyEditing() {
  Profile inherited;
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 44)}));
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::Key, 45)}));
  inherited.bindings.push_back(Bind(Action::PadA, {In(Device::PadButton, 0)}));
  inherited.bindings.push_back(Bind(Action::PadB, {In(Device::PadButton, 1)}));

  UserProfile user;
  CHECK(BindingFamily(inherited.bindings[2]) == Family::Pad);
  CHECK(!Overridden(user, Action::PadA, Family::Keyboard));

  // Editing one of two inherited keys keeps the other, and the pad binding alone.
  Materialize(user, Action::PadA, Family::Keyboard, inherited);
  CHECK(user.bindings.size() == 2);
  CHECK(Overridden(user, Action::PadA, Family::Keyboard));
  CHECK(!Overridden(user, Action::PadA, Family::Pad));
  Materialize(user, Action::PadA, Family::Keyboard, inherited); // already overridden: no copy
  CHECK(user.bindings.size() == 2);
  Profile p = inherited;
  Overlay(p, user);
  CHECK(p.bindings.size() == 4);
  CHECK(InFamily(p.bindings, Action::PadA, Family::Pad).size() == 1);

  // Removing the last one unbinds the family.
  RemoveUserBinding(user, 0);
  CHECK(user.unbind.empty());
  RemoveUserBinding(user, 0);
  CHECK(user.bindings.empty());
  CHECK(user.unbind.size() == 1 && user.unbind[0] == (Unbind{Action::PadA, Family::Keyboard}));
  p = inherited;
  Overlay(p, user);
  CHECK(InFamily(p.bindings, Action::PadA, Family::Keyboard).empty());
  CHECK(InFamily(p.bindings, Action::PadA, Family::Pad).size() == 1);

  // A new binding supersedes the unbind.
  AddUserBinding(user, Bind(Action::PadA, {In(Device::Key, 9)}));
  CHECK(user.unbind.empty() && user.bindings.size() == 1);

  // Revert and ClearFamily.
  AddUserBinding(user, Bind(Action::PadB, {In(Device::PadButton, 2)}));
  Revert(user, Action::PadA, Family::Keyboard);
  CHECK(!Overridden(user, Action::PadA, Family::Keyboard));
  CHECK(Overridden(user, Action::PadB, Family::Pad));
  user.unbind.push_back({Action::PadZ, Family::Keyboard});
  ClearFamily(user, Family::Pad);
  CHECK(user.bindings.empty());
  CHECK(user.unbind.size() == 1);
  ClearFamily(user, Family::Keyboard);
  CHECK(user.unbind.empty());
}
} // namespace

int main() {
  TestFamilyEditing();
  TestInputText();
  TestContexts();
  TestRoundTrip();
  TestErrors();
  TestSelect();
  TestOverlay();
  std::puts("input bindings ok");
  return 0;
}
