#include "port_remastered_effect.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace PortRemastered {
namespace {

using port::ReadLE16;
using port::ReadLE32;

constexpr uint32_t kGpsm = EffectFourCC("GPSM");
constexpr uint32_t kEnd = EffectFourCC("_END");
constexpr uint32_t kCnst = EffectFourCC("CNST");
constexpr uint32_t kNone = EffectFourCC("NONE");
constexpr uint32_t kGrad = EffectFourCC("GRAD");
constexpr uint32_t kArry = EffectFourCC("ARRY");
constexpr uint32_t kKews = EffectFourCC("KEWS");
constexpr uint32_t kKssm = EffectFourCC("KSSM");
constexpr uint32_t kPvar = EffectFourCC("PVAR");
constexpr uint32_t kTmtr = EffectFourCC("TMTR");
constexpr uint32_t kPmtr = EffectFourCC("PMTR");
constexpr uint32_t kSmtr = EffectFourCC("SMTR");

// Child forms a root's _END can embed. GPSM has a 25-byte header (FourCC, u8,
// id, kind); the others have either nothing after the FourCC or a 17-byte
// u8 + id header, and the parse tries both.
constexpr uint32_t kForms[] = {kGpsm,
                               EffectFourCC("SWSH"),
                               EffectFourCC("ELC2"),
                               EffectFourCC("ELSM"),
                               EffectFourCC("WPSM"),
                               EffectFourCC("CRSM"),
                               EffectFourCC("DPSM"),
                               EffectFourCC("EPSM"),
                               EffectFourCC("SPSM")};

// Element signatures. Each letter is one argument: e an element, w four raw
// bytes, b a raw byte, g a 16-byte id, k a keyframe block, K one with the
// 30-byte header only (KEYF's, which is followed by its input element); '-'
// is no arguments. Alternatives are separated by spaces and tried in order. These are
// retail's element arities (merged across the int/real/vector/colour/... slots,
// since the file does not say which slot it is in) with Remastered's changes
// and additions, learned from the shipped files.
struct ElementSig {
  const char* fourcc;
  const char* sigs;
};
constexpr ElementSig kElementSigs[] = {
    {"ADD_", "ee"},    {"ANCR", "eeee ebeee"}, {"ANGC", "eeeee"},   {"ASPH", "eeeeeee"},
    {"ASPR", "eeeeee"}, {"ATEX", "geeeeee geeeeeb"}, {"ATX2", "geeee"}, {"BNCE", "eeeeb"}, {"CCLU", "eeee"},
    {"CEQL", "eeee"},  {"CEXT", "e"},          {"CFDE", "eeee"},    {"CHAN", "eee"},
    {"CIRC", "eeeee"}, {"CLMP", "eee"},        {"CLTN", "eeee"},    {"CNST", "w eee eeee b g"},
    {"CONE", "ee"},    {"CRCV", "eeeeeeb"},    {"CRLN", "bb eeeeeeebe eeeeeebe"},
    {"CRNG", "eeeee"}, {"CTVC", "e"},          {"DETH", "ee"},      {"DOTP", "ee"},
    {"DPVC", "ge"},    {"DPVF", "ge"},         {"DPVI", "ge"},      {"DPVV", "ge"},
    {"EMPL", "eeeeb"}, {"EXPL", "ee"},         {"EXTR", "-"},         {"EXTT", "-"},
    {"FADE", "eee"},   {"FIAT", "eeee eeeee"}, {"GAPC", "-"},       {"GEMT", "-"},
    {"GRAV", "e ebe"},  {"GTCA", "e"},          {"GTCB", "e"},       {"GTCG", "e"},
    {"GTCP", "-"},     {"GTCR", "e"},          {"ILPT", "e"},       {"IMPL", "e eeeee eeeeb"},
    {"IRND", "ee"},    {"ISWT", "ee"},         {"ITRL", "ee"},      {"KESP", "k"},
    {"KEYC", "k"},     {"KEYE", "k"},          {"KEYF", "Ke k"},       {"KEYI", "k"},
    {"KEYP", "k"},     {"KEYV", "k"},          {"KPIN", "e"},       {"LFTW", "ee"},
    {"LMPL", "eeeeb"}, {"MMSI", "eg"},         {"MODU", "ee"},      {"MULT", "ee"},
    {"NONE", "-"},     {"PAP1", "-"},          {"PAP2", "-"},       {"PAP3", "-"},
    {"PAP4", "-"},     {"PAP5", "-"},          {"PAP6", "-"},       {"PAP7", "-"},
    {"PAP8", "-"},     {"PCOL", "-"},          {"PEOD", "e"},       {"PLCO", "-"},
    {"PLOC", "-"},     {"PRLW", "-"},          {"PSLL", "-"},       {"PSOF", "-"},
    {"PSOR", "-"},     {"PSOU", "-"},          {"PSTR", "-"},       {"PULS", "eeee"},
    {"PVEL", "-"},     {"RADD", "ee"},         {"RAND", "ee"},      {"REUL", "eeeb"},
    {"RLPT", "e"},     {"RTOI", "ee"},         {"RTOV", "e"},       {"SCAL", "e"},
    {"SEMR", "ee"},    {"SETR", "e"},          {"SEVT", "eeee"},    {"SINE", "eee"},
    {"SMOV", "eeeee e"},     {"SPAC", "be"},         {"SPAF", "be"},      {"SPAH", "eee"},
    {"SPAI", "be"},    {"SPAV", "be"},         {"SPHE", "eee"},     {"SPOS", "e"},
    {"SUB_", "ee"},    {"SWRL", "eeee"},       {"TPVC", "ge"},      {"TPVF", "ge"},
    {"TPVI", "ge"},    {"TRSS", "eeeeee"},     {"TSCL", "e"},       {"VARC", "g"},
    {"VARF", "g"},     {"VARI", "g"},          {"VARV", "g"},       {"VECF", "g"},
    {"VMAG", "e"},     {"VXTR", "e"},          {"VYTR", "e"},       {"VZTR", "e"},
    {"WIND", "ee"},
    // Pinned down against the shipped files: MPCB wraps a vector, or MPAC
    // angles and a magnitude; MPRD is 2 or 4 elements; DFCP and DFCS 1 to 3.
    {"MPCB", "e ee"},  {"MPAC", "eeee"},       {"MPRD", "ee eeee"}, {"DFCP", "ee eee e"},
    {"DFCS", "ee eee e"},
    // EXTT, EXTR and EXTS are leaves; SMOV is (translation, offset, rotation, and
    // two more, NONE in most files); TRST is five reals and a raw word.
    {"TRST", "eeeeew"}, {"EXTS", "-"},
    // TEXR's texture elements besides ATEX: an id and three elements (TXP2),
    // or an id and one to four (TXFB, whose first is an LFTW).
    // ANTH is a property whose value is an ANTH element: an id, TRST, two keyframe
    // blocks and a real.
    {"ANTH", "eeeee"},  {"TXP2", "geee"},   {"TXFB", "ge gee geee geeee"},
};

// Elements by the type of value they are read as, with typed arguments: I an
// int, R a real, V a vector, M a mod vector, C a colour, E an emitter. A typed
// slot only takes the elements its type has, with their arguments, so nested
// values cannot be read across their neighbours (a vector's CNST takes three
// reals, a real's CNST one word). These are retail's (CParticleDataFactory)
// with Remastered's additions (a flag is CNST and a byte, read as `e`). An
// element a type does not list is read untyped; one it lists is only read
// typed, so a misread cannot slip in through the untyped table.
constexpr ElementSig kIntSigs[] = {
    {"CNST", "w"},   {"KEYE", "k"},   {"KEYP", "k"},   {"TSCL", "R"},    {"DETH", "II"},  {"CHAN", "III"},
    {"ADD_", "II"},  {"MULT", "II"},  {"MODU", "II"},  {"RAND", "II"},   {"IMPL", "I"},   {"ILPT", "I"},
    {"SPAH", "III"}, {"IRND", "II"},  {"CLMP", "III"}, {"PULS", "IIII"}, {"NONE", "-"},   {"RTOI", "RR"},
    {"SUB_", "II"},  {"GTCP", "-"},   {"GAPC", "-"},   {"GEMT", "-"},    {"MPRD", "II eeee"},
    {"TPVI", "gI"},  {"DPVI", "gI"},  {"SPAI", "bI"},
};
constexpr ElementSig kRealSigs[] = {
    {"CNST", "w"},    {"NONE", "-"},    {"KEYE", "k"},    {"KEYP", "k"},     {"SCAL", "R"},    {"SINE", "RRR"},
    {"ADD_", "RR"},   {"MULT", "RR"},   {"DOTP", "VV"},   {"RAND", "RR"},    {"IRND", "RR"},   {"CHAN", "RRI"},
    {"CLMP", "RRR"},  {"PULS", "IIRR"}, {"RLPT", "R"},    {"LFTW", "RR"},    {"PRLW", "-"},    {"PSLL", "-"},
    {"PAP1", "-"},    {"PAP2", "-"},    {"PAP3", "-"},    {"PAP4", "-"},     {"PAP5", "-"},    {"PAP6", "-"},
    {"PAP7", "-"},    {"PAP8", "-"},    {"VXTR", "V"},    {"VYTR", "V"},     {"VZTR", "V"},    {"VMAG", "V"},
    {"ISWT", "RR"},   {"CLTN", "RRRR"}, {"CEQL", "RRRR"}, {"CRNG", "RRRRR"}, {"CEXT", "I"},    {"ITRL", "IR"},
    {"SUB_", "RR"},   {"GTCR", "C"},    {"GTCG", "C"},    {"GTCB", "C"},     {"GTCA", "C"},    {"DFCP", "RR"},
    {"DFCS", "RR"}, {"MPRD", "RR eeee"}, {"TPVF", "gR"}, {"DPVF", "gR"}, {"SPAF", "bR"}, {"KPIN", "R"},
};
constexpr ElementSig kVectorSigs[] = {
    {"NONE", "-"},   {"CNST", "RRR"},   {"KEYE", "k"},    {"KEYP", "k"},   {"ANGC", "RRRRR"}, {"CONE", "VR"},
    {"CIRC", "VVRRR"}, {"CCLU", "VVIR"}, {"ADD_", "VV"},  {"MULT", "VV"},  {"CHAN", "VVI"},   {"PULS", "IIVV"},
    {"RTOV", "R"},   {"PLOC", "-"},     {"PLCO", "-"},    {"PVEL", "-"},   {"PSOF", "-"},     {"PSOU", "-"},
    {"PSOR", "-"},   {"PSTR", "-"},     {"SUB_", "VV"},   {"CTVC", "C"},   {"MPCB", "V VR"},  {"MPAC", "RRRR"},
    {"ANCR", "eRRR ebeee"}, {"ANCM", "eRRR"}, {"ANCV", "eRRRRRRRb"}, {"RNDV", "R"}, {"TPVV", "gV"}, {"DPVV", "gV"}, {"SPAV", "bV"},
};
constexpr ElementSig kModVectorSigs[] = {
    {"NONE", "-"},     {"CNST", "RRR"},   {"GRAV", "V"},     {"WIND", "VR"},   {"EXPL", "RR"},
    {"CHAN", "MMI"},   {"PULS", "IIMM"},  {"IMPL", "VRRRe VRRRb"}, {"LMPL", "VRRRe VRRRb"}, {"EMPL", "VRRRe VRRRb"},
    {"SWRL", "VVRR"},  {"BNCE", "VVRRe VVRRb"}, {"SPOS", "V"},
};
constexpr ElementSig kColorSigs[] = {
    {"CNST", "RRRR"},     {"KEYE", "k"},   {"KEYP", "k"},  {"FADE", "CCR"}, {"CFDE", "CCRR"}, {"CHAN", "CCI"},
    {"PULS", "IICC"},     {"PCOL", "-"},   {"NONE", "-"},  {"TPVC", "gC"},  {"DPVC", "gC"},   {"SPAC", "bC"},
    {"MDAO", "CR"},       {"SLCT", "Re"},       {"MULT", "CC"},       {"MUL2", "CC"},
};
constexpr ElementSig kEmitterSigs[] = {
    {"NONE", "-"}, {"SEMR", "VV"}, {"SPHE", "VRR"}, {"ASPH", "VRRRRRR"}, {"ASPR", "VeRRRR"},
    {"PLNE", "VVVRRR"}, {"ELPS", "VVVRe"}, {"PLNV", "VVRRRRRRRRb"}, {"SPEV", "VRRRRRRb"},
};

constexpr char kTypeLetters[] = "IRVMCE";
constexpr int kTypeCount = 6;

int TypeIndex(char letter) {
  for (int i = 0; i < kTypeCount; ++i) {
    if (kTypeLetters[i] == letter) {
      return i;
    }
  }
  return -1;
}

// The type retail reads each of its properties as (Remastered's LTM2 is LTME).
// Bool and asset properties are left to the untyped reading.
struct PropertyType {
  char type;
  const char* fourccs;
};
constexpr PropertyType kPropertyTypes[] = {
    {'I', "PSLT PSWT MBSP MAXP LTME LTM2 SEED NCSY CSSD NDSY PISY SISY SSSD SESD LTYP LFOT"},
    {'R', "PSTS GRTE SIZE ROTA LENG WIDT LINT LFOR LSLA ADV1 ADV2 ADV3 ADV4 ADV5 ADV6 ADV7 ADV8"},
    {'V', "PSIV PSOV ILOC IVEC POFS PMOP PMRT PMSC SSPO SEPO LOFF LDIR"},
    {'M', "PSVM VEL1 VEL2 VEL3 VEL4"},
    {'C', "COLR PMCL LCLR"},
    {'E', "EMTR"},
};

// Elements seen in the files whose arity is not pinned down; they parse with
// whatever arity fits, like any unknown FourCC, but they are never properties.
constexpr const char* kLooseElements =
    "ARRY CMPS CODE EMRV GPUA GRAD KEWS MDAO PAFM PLNE PSA0 PSA1 PSA2 PSA3 RNDV ROTV SLCT";

// Properties: retail's PART properties and the ones Remastered added. A FourCC
// that is in neither list may still be a property (the grammar lets unknown
// FourCCs be both), but one that is only an element never is.
constexpr const char* kProperties =
    "AAPH ANTH ADV1 ADV2 ADV3 ADV4 ADV5 ADV6 ADV7 ADV8 BLIT CIND COLR CSSD DBIS DFOG DVVN EMTR FRMD FXBM FXBR FXLL GRTE "
    "ICTS IDTS IEXP IITS ILOC INTS ISVF ITEN IVEC KPAL KSSM LCLR LDIR LENG LFOR LFOT LINE LINT LIT_ LIXP LOFF LSLA "
    "LTM2 LTME LTYP MAXP MBLR MBSP MTIN NCSY NDSY OPTS ORNT OSDM PBDM PISY PMAB PMCL PMDL PMOO PMOP PMRT PMSC PMTR "
    "PMUS POFS PSIV PSLT PSOV PSPS PSTS PSVM PSWT PVAR ROTA RSOP SCTR SEED SELC SEPO SESD SFTD SHTM SISY SIZE SMTR "
    "SMVR SNRA SNRD SORT SSPO SSSD SSWH SSZE SVC0 SVI0 SVR3 SVR4 SVR5 SVR6 SVV0 TEXR TIND TMTR VEL1 VEL2 VEL3 VEL4 "
    "VMD1 VMD2 VMD3 VMD4 VMPC WIDT XFMD ZBUF";

// Property values whose shape is not the default list below.
constexpr ElementSig kPropertySigs[] = {
    {"TEXR", "ee e"}, {"TIND", "ee e"}, {"SMVR", "e eeee"}, {"SORT", "w"}, {"FRMD", "w"},
    {"PSPS", "gw"},   {"MTIN", "bg"},   {"SNRD", "w"}, {"SNRA", "w"},
};
// Any other property: one element, a flag byte, a byte and an id, an id, or a
// byte and an element.
constexpr const char* kDefaultPropertySigs = "e b bg g be";

// Unknown elements are tried with up to this many element arguments.
constexpr int kMaxUnknownArity = 4;

uint32_t FourCCOf(const char* text) {
  return uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 | uint32_t(uint8_t(text[2])) << 8 |
         uint32_t(uint8_t(text[3]));
}

std::vector<std::string> SplitSigs(const char* text) {
  std::vector<std::string> out;
  std::string current;
  for (const char* c = text;; ++c) {
    if (*c == ' ' || *c == 0) {
      out.push_back(current == "-" ? std::string() : current);
      current.clear();
      if (*c == 0) {
        break;
      }
    } else {
      current += *c;
    }
  }
  return out;
}

void AddWords(std::unordered_set<uint32_t>& set, const char* text) {
  for (const char* c = text; *c != 0;) {
    set.insert(FourCCOf(c));
    c += 4;
    while (*c == ' ') {
      ++c;
    }
  }
}

struct Grammar {
  std::unordered_map<uint32_t, std::vector<std::string>> elements;
  std::unordered_map<uint32_t, std::vector<std::string>> properties;
  std::vector<std::string> defaultProperty;
  std::vector<std::string> unknownElement;
  std::unordered_set<uint32_t> elementNames;
  std::unordered_set<uint32_t> propertyNames;
  std::unordered_map<uint32_t, std::vector<std::string>> typed[kTypeCount];

  Grammar() {
    for (const ElementSig& sig : kElementSigs) {
      elements[FourCCOf(sig.fourcc)] = SplitSigs(sig.sigs);
      elementNames.insert(FourCCOf(sig.fourcc));
    }
    AddWords(elementNames, kLooseElements);
    AddWords(propertyNames, kProperties);
    for (const ElementSig& sig : kPropertySigs) {
      properties[FourCCOf(sig.fourcc)] = SplitSigs(sig.sigs);
    }
    defaultProperty = SplitSigs(kDefaultPropertySigs);
    // Most arguments first: when two arities reach the same end, the one with
    // fewer arguments has an argument that swallowed its neighbour.
    for (int arity = kMaxUnknownArity; arity >= 0; --arity) {
      unknownElement.push_back(std::string(size_t(arity), 'e'));
    }
    auto addTyped = [this](char type, const ElementSig* sigs, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        typed[TypeIndex(type)][FourCCOf(sigs[i].fourcc)] = SplitSigs(sigs[i].sigs);
        elementNames.insert(FourCCOf(sigs[i].fourcc));
      }
    };
    addTyped('I', kIntSigs, std::size(kIntSigs));
    addTyped('R', kRealSigs, std::size(kRealSigs));
    addTyped('V', kVectorSigs, std::size(kVectorSigs));
    addTyped('M', kModVectorSigs, std::size(kModVectorSigs));
    addTyped('C', kColorSigs, std::size(kColorSigs));
    addTyped('E', kEmitterSigs, std::size(kEmitterSigs));
    // A typed property is read as its type first, then any way an untyped one
    // can be (Remastered stores some as a byte).
    for (const PropertyType& property : kPropertyTypes) {
      for (const char* c = property.fourccs; *c != 0;) {
        std::vector<std::string> sigs{std::string(1, property.type)};
        sigs.insert(sigs.end(), defaultProperty.begin(), defaultProperty.end());
        properties.emplace(FourCCOf(c), std::move(sigs));
        c += 4;
        while (*c == ' ') {
          ++c;
        }
      }
    }
  }

  // The typed signatures of `fourcc` as type `type`, or null when the type has
  // no such element.
  const std::vector<std::string>* TypedSigs(int type, uint32_t fourcc) const {
    auto it = typed[type].find(fourcc);
    return it != typed[type].end() ? &it->second : nullptr;
  }

  const std::vector<std::string>& ElementSigs(uint32_t fourcc) const {
    auto it = elements.find(fourcc);
    return it != elements.end() ? it->second : unknownElement;
  }
  const std::vector<std::string>& PropertySigs(uint32_t fourcc) const {
    auto it = properties.find(fourcc);
    return it != properties.end() ? it->second : defaultProperty;
  }
  // Only an element, never a property.
  bool ElementOnly(uint32_t fourcc) const { return elementNames.count(fourcc) != 0 && propertyNames.count(fourcc) == 0; }
};

const Grammar& TheGrammar() {
  static const Grammar grammar;
  return grammar;
}

// Possible end offsets of a parse, sorted and unique.
using Ends = std::vector<size_t>;

void AddEnd(Ends& ends, size_t end) {
  auto it = std::lower_bound(ends.begin(), ends.end(), end);
  if (it == ends.end() || *it != end) {
    ends.insert(it, end);
  }
}

void AddEnds(Ends& ends, const Ends& more) {
  for (size_t end : more) {
    AddEnd(ends, end);
  }
}

bool Contains(const Ends& ends, size_t end) { return std::binary_search(ends.begin(), ends.end(), end); }

class Parser {
public:
  Parser(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_grammar(TheGrammar()) {}

  size_t Furthest() const { return m_furthest; }

  // A FourCC at `at` (stored byte-reversed), or 0 when the bytes are not one.
  uint32_t FourCCAt(size_t at) const {
    if (at + 4 > m_size) {
      return 0;
    }
    for (size_t i = 0; i < 4; ++i) {
      const uint8_t c = m_data[at + i];
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
        return 0;
      }
    }
    return ReadLE32(m_data + at);
  }

  bool StartsWithElement(size_t at) const {
    const uint32_t fourcc = FourCCAt(at);
    return fourcc != 0 && m_grammar.elementNames.count(fourcc) != 0;
  }

  bool HoldsElement(size_t at) const {
    for (size_t i = 0; i < 16; i += 4) {
      if (StartsWithElement(at + i)) {
        return true;
      }
    }
    return false;
  }

  std::optional<uint32_t> U32(size_t at) const {
    if (at + 4 > m_size) {
      return std::nullopt;
    }
    return ReadLE32(m_data + at);
  }

  // Where the element starting at `at` can end.
  const Ends& Element(size_t at) {
    auto found = m_elements.find(at);
    if (found != m_elements.end()) {
      return found->second;
    }
    Ends ends;
    const uint32_t fourcc = FourCCAt(at);
    if (fourcc != 0 && !(m_grammar.propertyNames.count(fourcc) != 0 && m_grammar.elementNames.count(fourcc) == 0)) {
      if (fourcc == kGrad) {
        ends = Gradient(at + 4);
      } else if (fourcc == kArry) {
        ends = Array(at + 4);
      } else if (fourcc == kKews) {
        ends = KeyWeights(at + 4);
      } else {
        for (const std::string& sig : m_grammar.ElementSigs(fourcc)) {
          AddEnds(ends, Args(at + 4, sig));
        }
      }
    }
    return m_elements.emplace(at, std::move(ends)).first->second;
  }

  // Where the typed reading of an element of type `type` at `at` can end.
  const Ends& TypedOnly(size_t at, int type) {
    const uint64_t key = uint64_t(at) << 3 | uint64_t(type);
    auto found = m_typed.find(key);
    if (found != m_typed.end()) {
      return found->second;
    }
    Ends ends;
    if (const std::vector<std::string>* sigs = m_grammar.TypedSigs(type, FourCCAt(at))) {
      for (const std::string& sig : *sigs) {
        AddEnds(ends, Args(at + 4, sig));
      }
    }
    return m_typed.emplace(key, std::move(ends)).first->second;
  }

  // Where an element of type `type` at `at` can end: typed when the type
  // lists the element, else untyped.
  Ends Typed(size_t at, int type) {
    if (m_grammar.TypedSigs(type, FourCCAt(at)) == nullptr) {
      return Element(at);
    }
    return TypedOnly(at, type);
  }

  // Where `count` consecutive elements starting at `at` can end.
  const Ends& Sequence(size_t at, uint32_t count) {
    const uint64_t key = uint64_t(at) << 16 | count;
    auto found = m_sequences.find(key);
    if (found != m_sequences.end()) {
      return found->second;
    }
    Ends ends;
    if (count == 0) {
      ends.push_back(at);
    } else {
      const Ends first = Element(at);
      for (size_t next : first) {
        AddEnds(ends, Sequence(next, count - 1));
      }
    }
    return m_sequences.emplace(key, std::move(ends)).first->second;
  }

  // Where the arguments in `sig` starting at `at` can end.
  Ends Args(size_t at, const std::string& sig) {
    Ends current{at};
    for (char arg : sig) {
      Ends next;
      for (size_t pos : current) {
        switch (arg) {
        case 'w':
          if (pos + 4 <= m_size) {
            AddEnd(next, pos + 4);
          }
          break;
        case 'b':
          if (pos + 1 <= m_size) {
            AddEnd(next, pos + 1);
          }
          break;
        case 'g':
          // An id holding an element's FourCC is nested elements read as an
          // id (CNST(CNST(0), ...), or a word and then RLPT, NONE...): the
          // shorter reading would win. A real id holds one by chance only.
          if (pos + 16 <= m_size && !HoldsElement(pos)) {
            AddEnd(next, pos + 16);
          }
          break;
        case 'k':
          AddEnds(next, Keyframes(pos));
          break;
        case 'K':
          AddEnds(next, Keyframes(pos, true));
          break;
        case 'e':
          AddEnds(next, Element(pos));
          break;
        default:
          AddEnds(next, Typed(pos, TypeIndex(arg)));
          break;
        }
      }
      current = std::move(next);
      if (current.empty()) {
        break;
      }
    }
    return current;
  }

  // A keyframe block: a 22-byte header with the key count at +18, or (KEYF
  // type 02) a 30-byte one with it at +26, then 4, 8, 12 or 16 bytes per key.
  Ends Keyframes(size_t at, bool longOnly = false) const {
    Ends ends;
    if (at + 22 > m_size) {
      return ends;
    }
    const std::pair<size_t, size_t> layouts[] = {{at + 18, at + 22}, {at + 26, at + 30}};
    for (const auto& [countAt, start] : layouts) {
      if (longOnly && start == at + 22) {
        continue;
      }
      const std::optional<uint32_t> count = U32(countAt);
      if (!count || *count > 100000) {
        continue;
      }
      for (uint64_t keySize : {4, 8, 12, 16}) {
        const uint64_t end = uint64_t(start) + *count * keySize;
        if (end <= m_size) {
          AddEnd(ends, size_t(end));
        }
      }
    }
    return ends;
  }

  // GRAD: u8 key count, 12 bytes per key (half4 colour + f32 position), then an
  // optional interpolation element and an optional trailing byte.
  Ends Gradient(size_t at) {
    Ends ends;
    if (at >= m_size) {
      return ends;
    }
    const size_t keysEnd = at + 1 + 12 * size_t(m_data[at]);
    Ends base = Element(keysEnd);
    AddEnd(base, keysEnd);
    for (size_t end : base) {
      for (size_t candidate : {end, end + 1}) {
        if (candidate <= m_size) {
          AddEnd(ends, candidate);
        }
      }
    }
    return ends;
  }

  Ends Array(size_t at) {
    const std::optional<uint32_t> count = U32(at);
    if (!count || *count > 256) {
      return {};
    }
    return Sequence(at + 4, *count);
  }

  Ends KeyWeights(size_t at) const {
    const std::optional<uint32_t> count = U32(at);
    if (!count || uint64_t(at) + 4 + uint64_t(*count) * 4 > m_size) {
      return {};
    }
    return {at + 4 + size_t(*count) * 4};
  }

  // Where a property's value starting at `at` can end, most likely first.
  std::vector<size_t> ValueEnds(uint32_t fourcc, size_t at) {
    if (fourcc == kKssm) {
      return SpawnTable(at);
    }
    if (fourcc == kPvar) {
      return ParameterTable(at);
    }
    if (fourcc == kTmtr) {
      if (at + 2 > m_size || m_data[at] != 0) {
        return {};
      }
      return Sequence(at + 2, m_data[at + 1]);
    }
    if (fourcc == kPmtr || fourcc == kSmtr) {
      return ModifierGroups(at, fourcc == kPmtr ? 3 : 6);
    }
    std::vector<size_t> out;
    for (const std::string& sig : m_grammar.PropertySigs(fourcc)) {
      const Ends ends = Args(at, sig);
      out.insert(out.end(), ends.begin(), ends.end());
    }
    return out;
  }

  // PVAR: u16 id count, 10 bytes, u16 data size, the ids, then the data.
  std::vector<size_t> ParameterTable(size_t at) const {
    if (at + 14 > m_size) {
      return {};
    }
    return {at + 14 + 16 * size_t(ReadLE16(m_data + at)) + size_t(ReadLE16(m_data + at + 12))};
  }

  // PMTR/SMTR: u8 0, u8 total, then five groups of (u8 count, count x (element
  // + trailer)). The trailer is 3 bytes in PMTR and 6 in SMTR.
  std::vector<size_t> ModifierGroups(size_t at, size_t trailer) {
    if (at >= m_size || m_data[at] != 0) {
      return {};
    }
    Ends current{at + 2};
    for (int group = 0; group < 5; ++group) {
      Ends next;
      for (size_t pos : current) {
        if (pos >= m_size) {
          continue;
        }
        Ends items{pos + 1};
        for (uint8_t i = 0; i < m_data[pos]; ++i) {
          Ends after;
          for (size_t item : items) {
            for (size_t end : Element(item)) {
              AddEnd(after, end + trailer);
            }
          }
          items = std::move(after);
        }
        AddEnds(next, items);
      }
      current = std::move(next);
    }
    return current;
  }

  // KSSM: NONE, or CNST with four u32 (retail's three header words and the
  // end frame, then the SEVT event count), the events, a u32 table count and
  // that many tables. A table is a u32, a selector element and its frames: a
  // u32 frame count, each frame a u32 frame and a u32 spawn count, each spawn
  // a 16-byte child id, its form's FourCC, a u32 and an element.
  std::vector<size_t> SpawnTable(size_t at) {
    const uint32_t fourcc = FourCCAt(at);
    if (fourcc == kNone) {
      return {at + 4};
    }
    if (fourcc != kCnst) {
      return {};
    }
    const std::optional<uint32_t> events = U32(at + 16);
    if (!events || *events > 1000) {
      return {};
    }
    Ends out;
    for (size_t pos : Sequence(at + 20, *events)) {
      const std::optional<uint32_t> tables = U32(pos);
      if (tables && *tables <= 16) {
        AddEnds(out, SpawnTables(pos + 4, *tables));
      }
    }
    return out;
  }

  Ends SpawnTables(size_t at, uint32_t count) {
    if (count == 0) {
      return {at};
    }
    Ends out;
    for (size_t selectorEnd : Element(at + 4)) {
      const std::optional<uint32_t> frames = U32(selectorEnd);
      if (!frames || *frames > 1000) {
        continue;
      }
      for (size_t tableEnd : SpawnFrames(selectorEnd + 4, *frames)) {
        AddEnds(out, SpawnTables(tableEnd, count - 1));
      }
    }
    return out;
  }

  Ends SpawnFrames(size_t at, uint32_t count) {
    if (count == 0) {
      return {at};
    }
    const std::optional<uint32_t> spawns = U32(at + 4);
    if (!spawns || *spawns > 100) {
      return {};
    }
    Ends out;
    for (size_t frameEnd : Spawns(at + 8, *spawns)) {
      AddEnds(out, SpawnFrames(frameEnd, count - 1));
    }
    return out;
  }

  Ends Spawns(size_t at, uint32_t count) {
    if (count == 0) {
      return {at};
    }
    Ends out;
    for (size_t spawnEnd : Element(at + 24)) {
      AddEnds(out, Spawns(spawnEnd, count - 1));
    }
    return out;
  }

  // Rebuilds the reading of the spawn table at [at, end) that SpawnTable()
  // found. Only the parts retail has are kept: the header and the spawns.
  bool BuildSpawnTable(size_t at, size_t end, EffectSpawnTable& out) {
    out = EffectSpawnTable();
    if (FourCCAt(at) == kNone) {
      return at + 4 == end;
    }
    if (FourCCAt(at) != kCnst || at + 20 > m_size) {
      return false;
    }
    for (int i = 0; i < 3; ++i) {
      out.header[i] = ReadLE32(m_data + at + 4 + 4 * i);
    }
    out.events = ReadLE32(m_data + at + 16);
    for (size_t pos : Sequence(at + 20, out.events)) {
      const std::optional<uint32_t> tables = U32(pos);
      if (tables && *tables <= 16 && BuildSpawnTables(pos + 4, *tables, end, out.tables)) {
        return true;
      }
    }
    return false;
  }

  bool BuildSpawnTables(size_t at, uint32_t count, size_t end, std::vector<EffectSpawnTable::Table>& out) {
    if (count == 0) {
      return at == end;
    }
    for (size_t selectorEnd : Element(at + 4)) {
      const std::optional<uint32_t> frames = U32(selectorEnd);
      if (!frames || *frames > 1000) {
        continue;
      }
      EffectSpawnTable::Table table;
      table.word = ReadLE32(m_data + at);
      if (!BuildElement(at + 4, selectorEnd, table.selector)) {
        continue;
      }
      out.push_back(std::move(table));
      if (BuildSpawnFrames(selectorEnd + 4, *frames, count, end, out)) {
        return true;
      }
      out.pop_back();
    }
    return false;
  }

  // The frames of out.back(), then the tables after it.
  bool BuildSpawnFrames(size_t at, uint32_t frames, uint32_t tables, size_t end,
                        std::vector<EffectSpawnTable::Table>& out) {
    if (frames == 0) {
      return BuildSpawnTables(at, tables - 1, end, out);
    }
    const std::optional<uint32_t> spawns = U32(at + 4);
    if (!spawns || *spawns > 100) {
      return false;
    }
    EffectSpawnTable::Frame frame;
    frame.frame = ReadLE32(m_data + at);
    out.back().frames.push_back(std::move(frame));
    if (BuildSpawns(at + 8, *spawns, frames, tables, end, out)) {
      return true;
    }
    out.back().frames.pop_back();
    return false;
  }

  bool BuildSpawns(size_t at, uint32_t spawns, uint32_t frames, uint32_t tables, size_t end,
                   std::vector<EffectSpawnTable::Table>& out) {
    if (spawns == 0) {
      return BuildSpawnFrames(at, frames - 1, tables, end, out);
    }
    if (at + 24 > m_size) {
      return false;
    }
    EffectSpawnTable::Spawn spawn;
    std::memcpy(spawn.id.data(), m_data + at, 16);
    spawn.form = FourCCAt(at + 16);
    spawn.conditional = FourCCAt(at + 24) != kNone;
    out.back().frames.back().spawns.push_back(spawn);
    for (size_t spawnEnd : Element(at + 24)) {
      if (BuildSpawns(spawnEnd, spawns - 1, frames, tables, end, out)) {
        return true;
      }
    }
    // Deeper calls pushed and popped their own entries: back() is ours again.
    out.back().frames.back().spawns.pop_back();
    return false;
  }

  // Where a node's _END at `at` and the children after it end: a u32 count,
  // then per child a 16-byte id and the child.
  std::optional<size_t> Children(size_t at) {
    const std::optional<uint32_t> count = U32(at + 5);
    if (!count || *count > 64) {
      return std::nullopt;
    }
    std::optional<size_t> pos = at + 9;
    for (uint32_t i = 0; i < *count && pos; ++i) {
      pos = Child(*pos + 16);
    }
    return pos;
  }

  // The end of the property list starting at `at` (children included), or
  // nullopt when no reading of it parses. `root` is the GPSM's root flag: the
  // effect's own root has children after its _END; an embedded generator
  // with the flag may have them, or end at its _END like any other.
  std::optional<size_t> Properties(size_t at, bool root, bool top = false) {
    const uint64_t key = uint64_t(at) << 2 | (root ? 1 : 0) | (top ? 2 : 0);
    auto found = m_properties.find(key);
    if (found != m_properties.end()) {
      return found->second;
    }
    m_properties[key] = std::nullopt; // a cycle cannot parse
    const uint32_t fourcc = FourCCAt(at);
    if (fourcc == 0 || at + 5 > m_size || m_data[at + 4] > 5) {
      return std::nullopt;
    }
    if (m_grammar.propertyNames.count(fourcc) != 0) {
      m_furthest = std::max(m_furthest, at);
    }
    std::optional<size_t> result;
    if (fourcc == kEnd && !root) {
      result = at + 5;
    } else if (fourcc == kEnd) {
      result = Children(at);
      if (!result && !top) {
        result = at + 5;
      }
    } else if (!m_grammar.ElementOnly(fourcc)) {
      for (size_t valueEnd : ValueEnds(fourcc, at + 5)) {
        if (std::optional<size_t> end = Properties(valueEnd, root, top)) {
          result = end;
          break;
        }
      }
    }
    m_properties[key] = result;
    return result;
  }

  std::optional<size_t> Generator(size_t at, bool top = false) {
    if (FourCCAt(at) != kGpsm || at + 25 > m_size) {
      return std::nullopt;
    }
    return Properties(at + 25, ReadLE32(m_data + at + 21) != 0, top);
  }

  std::optional<size_t> Child(size_t at) {
    const uint32_t form = FourCCAt(at);
    if (form == kGpsm) {
      return Generator(at);
    }
    if (std::find(std::begin(kForms), std::end(kForms), form) == std::end(kForms)) {
      return std::nullopt;
    }
    if (std::optional<size_t> end = Properties(at + 4, false)) {
      return end;
    }
    return Properties(at + 21, false);
  }

  // Rebuilds the parse that Generator()/Child() found, as a tree.
  size_t BuildNode(size_t at, const EffectGuid& id, EffectNode& node, bool top = false) {
    node.form = FourCCAt(at);
    node.id = id;
    if (node.form == kGpsm) {
      node.root = ReadLE32(m_data + at + 21) != 0;
      at += 25;
    } else if (Properties(at + 4, false)) {
      at += 4;
    } else {
      at += 21;
    }
    for (;;) {
      const uint32_t fourcc = FourCCAt(at);
      if (fourcc == kEnd && !node.root) {
        return at + 5;
      }
      if (fourcc == kEnd && !Children(at)) {
        return at + 5;
      }
      if (fourcc == kEnd) {
        const uint32_t count = ReadLE32(m_data + at + 5);
        size_t pos = at + 9;
        for (uint32_t i = 0; i < count; ++i) {
          EffectGuid childId;
          std::memcpy(childId.data(), m_data + pos, 16);
          node.children.emplace_back();
          pos = BuildNode(pos + 16, childId, node.children.back());
        }
        return pos;
      }
      EffectProperty property;
      property.fourcc = fourcc;
      property.tag = m_data[at + 4];
      property.offset = at;
      size_t valueEnd = 0;
      for (size_t end : ValueEnds(fourcc, at + 5)) {
        if (Properties(end, node.root, top)) {
          valueEnd = end;
          break;
        }
      }
      property.size = valueEnd - at;
      BuildValue(fourcc, at + 5, valueEnd, property.value);
      node.properties.push_back(std::move(property));
      at = valueEnd;
    }
  }

private:
  // TMTR: the elements, in file order (six per UV set). PMTR/SMTR: one Word
  // per entry, `fourcc` = its group (0-4); PMTR `word` = b0 | b1 << 8 | b2 << 16
  // from the 3-byte trailer, SMTR `word` = the parameter id as the file's four
  // bytes read little-endian ('CCH0' = 0x30484343), with args[1] a Word of
  // b0 | b1 << 8. args[0] is the entry's element.
  bool BuildModifiers(uint32_t fourcc, size_t at, size_t end, std::vector<EffectValue>& out) {
    if (at + 2 > m_size || m_data[at] != 0) {
      return false;
    }
    if (fourcc == kTmtr) {
      return BuildElements(at + 2, m_data[at + 1], end, out);
    }
    return BuildGroups(at + 2, 0, fourcc == kPmtr ? 3 : 6, end, out);
  }

  bool BuildElements(size_t at, uint32_t count, size_t end, std::vector<EffectValue>& out) {
    if (count == 0) {
      return at == end;
    }
    for (size_t next : Element(at)) {
      EffectValue value;
      if (next > end || !BuildElement(at, next, value)) {
        continue;
      }
      const size_t mark = out.size();
      out.push_back(std::move(value));
      if (BuildElements(next, count - 1, end, out)) {
        return true;
      }
      out.resize(mark);
    }
    return false;
  }

  bool BuildGroups(size_t at, int group, size_t trailer, size_t end, std::vector<EffectValue>& out) {
    if (group == 5) {
      return at == end;
    }
    if (at >= m_size) {
      return false;
    }
    return BuildGroupItems(at + 1, m_data[at], group, trailer, end, out);
  }

  bool BuildGroupItems(size_t at, uint32_t left, int group, size_t trailer, size_t end,
                       std::vector<EffectValue>& out) {
    if (left == 0) {
      return BuildGroups(at, group + 1, trailer, end, out);
    }
    for (size_t next : Element(at)) {
      if (next + trailer > end) {
        continue;
      }
      EffectValue item;
      item.kind = EffectValue::Kind::Word;
      item.fourcc = uint32_t(group);
      item.offset = at;
      item.size = next + trailer - at;
      EffectValue element;
      if (!BuildElement(at, next, element)) {
        continue;
      }
      item.args.push_back(std::move(element));
      const uint8_t* t = m_data + next;
      if (trailer == 3) {
        item.word = uint32_t(t[0]) | uint32_t(t[1]) << 8 | uint32_t(t[2]) << 16;
      } else {
        item.word = ReadLE32(t);
        EffectValue slot;
        slot.kind = EffectValue::Kind::Word;
        slot.word = uint32_t(t[4]) | uint32_t(t[5]) << 8;
        item.args.push_back(std::move(slot));
      }
      const size_t mark = out.size();
      out.push_back(std::move(item));
      if (BuildGroupItems(next + trailer, left - 1, group, trailer, end, out)) {
        return true;
      }
      out.resize(mark);
    }
    return false;
  }

  void BuildValue(uint32_t fourcc, size_t at, size_t end, std::vector<EffectValue>& out) {
    if (fourcc == kTmtr || fourcc == kPmtr || fourcc == kSmtr) {
      if (BuildModifiers(fourcc, at, end, out)) {
        return;
      }
      out.clear();
    }
    if (fourcc != kKssm && fourcc != kPvar && fourcc != kTmtr && fourcc != kPmtr && fourcc != kSmtr) {
      for (const std::string& sig : m_grammar.PropertySigs(fourcc)) {
        if (Contains(Args(at, sig), end) && BuildArgs(at, sig, 0, end, out)) {
          return;
        }
      }
    }
    EffectValue raw;
    raw.kind = EffectValue::Kind::Raw;
    raw.offset = at;
    raw.size = end - at;
    out.push_back(std::move(raw));
  }

  // Splits [at, end) into the arguments sig[index...].
  bool BuildArgs(size_t at, const std::string& sig, size_t index, size_t end, std::vector<EffectValue>& out) {
    if (index == sig.size()) {
      return at == end;
    }
    const std::string rest = sig.substr(index + 1);
    Ends options;
    switch (sig[index]) {
    case 'w':
      options = {at + 4};
      break;
    case 'b':
      options = {at + 1};
      break;
    case 'g':
      options = {at + 16};
      break;
    case 'k':
      options = Keyframes(at);
      break;
    case 'K':
      options = Keyframes(at, true);
      break;
    case 'e':
      options = Element(at);
      break;
    default:
      options = Typed(at, TypeIndex(sig[index]));
      break;
    }
    for (size_t next : options) {
      if (next > end || !Contains(Args(next, rest), end)) {
        continue;
      }
      EffectValue value;
      value.offset = at;
      value.size = next - at;
      switch (sig[index]) {
      case 'w':
        value.kind = EffectValue::Kind::Word;
        value.word = ReadLE32(m_data + at);
        break;
      case 'b':
        value.kind = EffectValue::Kind::Byte;
        value.word = m_data[at];
        break;
      case 'g':
        value.kind = EffectValue::Kind::Guid;
        std::memcpy(value.guid.data(), m_data + at, 16);
        break;
      case 'k':
      case 'K':
        value.kind = EffectValue::Kind::Keys;
        break;
      case 'e':
        if (!BuildElement(at, next, value)) {
          continue;
        }
        break;
      default:
        if (!BuildTyped(at, next, TypeIndex(sig[index]), value)) {
          continue;
        }
        break;
      }
      const size_t mark = out.size();
      out.push_back(std::move(value));
      if (BuildArgs(next, sig, index + 1, end, out)) {
        return true;
      }
      out.resize(mark);
    }
    return false;
  }

  bool BuildTyped(size_t at, size_t end, int type, EffectValue& value) {
    if (m_grammar.TypedSigs(type, FourCCAt(at)) == nullptr) {
      return BuildElement(at, end, value);
    }
    value.kind = EffectValue::Kind::Element;
    value.fourcc = FourCCAt(at);
    value.offset = at;
    value.size = end - at;
    for (const std::string& sig : *m_grammar.TypedSigs(type, value.fourcc)) {
      value.args.clear();
      if (BuildArgs(at + 4, sig, 0, end, value.args)) {
        return true;
      }
    }
    return false;
  }

public:
  // Walks a PMTR/SMTR payload (see ModifierGroups), keeping the one split of
  // its items that ends exactly at `end`.
  bool BuildMaterialTrack(size_t at, size_t end, size_t trailer, EffectMaterialTrack& out) {
    if (at + 2 > end || m_data[at] != 0) {
      return false;
    }
    out.items.clear();
    return BuildMaterialGroup(at + 2, end, trailer, 0, out);
  }

private:

  bool BuildMaterialGroup(size_t at, size_t end, size_t trailer, int group, EffectMaterialTrack& out) {
    if (group == 5) {
      return at == end;
    }
    if (at >= end) {
      return false;
    }
    return BuildMaterialItems(at + 1, end, trailer, group, m_data[at], out);
  }

  bool BuildMaterialItems(size_t at, size_t end, size_t trailer, int group, int left, EffectMaterialTrack& out) {
    if (left == 0) {
      return BuildMaterialGroup(at, end, trailer, group + 1, out);
    }
    for (size_t elementEnd : Element(at)) {
      if (elementEnd + trailer > end) {
        continue;
      }
      EffectMaterialTrack::Item item;
      item.group = group;
      std::copy_n(m_data + elementEnd, trailer, item.trailer.begin());
      if (!BuildElement(at, elementEnd, item.value)) {
        continue;
      }
      out.items.push_back(std::move(item));
      if (BuildMaterialItems(elementEnd + trailer, end, trailer, group, left - 1, out)) {
        return true;
      }
      out.items.pop_back();
    }
    return false;
  }

  bool BuildElement(size_t at, size_t end, EffectValue& value) {
    value.kind = EffectValue::Kind::Element;
    value.fourcc = FourCCAt(at);
    value.offset = at;
    value.size = end - at;
    if (value.fourcc == kGrad || value.fourcc == kArry || value.fourcc == kKews) {
      EffectValue raw;
      raw.kind = EffectValue::Kind::Raw;
      raw.offset = at + 4;
      raw.size = end - at - 4;
      value.args.push_back(std::move(raw));
      return true;
    }
    for (const std::string& sig : m_grammar.ElementSigs(value.fourcc)) {
      value.args.clear();
      if (BuildArgs(at + 4, sig, 0, end, value.args)) {
        return true;
      }
    }
    return false;
  }

  const uint8_t* m_data;
  size_t m_size;
  const Grammar& m_grammar;
  std::unordered_map<size_t, Ends> m_elements;
  std::unordered_map<uint64_t, Ends> m_typed;
  std::unordered_map<uint64_t, Ends> m_sequences;
  std::unordered_map<uint64_t, std::optional<size_t>> m_properties;
  size_t m_furthest = 0;
};

// Where the root GPSM starts, after the RFRM header.
constexpr size_t kRootAt = 0x3c;

std::string FormatWord(uint32_t word) {
  char text[32];
  float value;
  std::memcpy(&value, &word, 4);
  // Small values are ints (counts, flags, frame numbers); anything else reads
  // as a float.
  if (word < 0x10000 || word >= 0xFFFF0000u) {
    std::snprintf(text, sizeof(text), "%d", int32_t(word));
  } else if (std::isfinite(value)) {
    std::snprintf(text, sizeof(text), "%gf", double(value));
  } else {
    std::snprintf(text, sizeof(text), "0x%08x", word);
  }
  return text;
}

void DumpValue(const EffectValue& value, const uint8_t* data, std::string& out) {
  char text[48];
  switch (value.kind) {
  case EffectValue::Kind::Element:
    out += EffectFourCCString(value.fourcc);
    if (!value.args.empty()) {
      out += '(';
      for (size_t i = 0; i < value.args.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        DumpValue(value.args[i], data, out);
      }
      out += ')';
    }
    break;
  case EffectValue::Kind::Byte:
    std::snprintf(text, sizeof(text), "#%02x", value.word);
    out += text;
    break;
  case EffectValue::Kind::Word:
    if (!value.args.empty()) { // a PMTR/SMTR entry
      std::snprintf(text, sizeof(text), "g%u:%06x(", value.fourcc, value.word);
      out += text;
      for (size_t i = 0; i < value.args.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        DumpValue(value.args[i], data, out);
      }
      out += ')';
      break;
    }
    out += FormatWord(value.word);
    break;
  case EffectValue::Kind::Guid:
    out += EffectGuidString(value.guid);
    break;
  case EffectValue::Kind::Keys:
    std::snprintf(text, sizeof(text), "<keys %zu bytes>", value.size);
    out += text;
    break;
  case EffectValue::Kind::Raw:
    if (value.size <= 32) {
      out += '<';
      for (size_t i = 0; i < value.size; ++i) {
        std::snprintf(text, sizeof(text), "%02x", data[value.offset + i]);
        out += text;
      }
      out += '>';
    } else {
      std::snprintf(text, sizeof(text), "<raw %zu bytes>", value.size);
      out += text;
    }
    break;
  }
}

void DumpNode(const EffectNode& node, const uint8_t* data, int depth, std::string& out) {
  const std::string indent(size_t(depth) * 2, ' ');
  out += indent + EffectFourCCString(node.form);
  if (depth > 0) {
    out += ' ' + EffectGuidString(node.id);
  }
  out += node.root ? " root\n" : "\n";
  for (const EffectProperty& property : node.properties) {
    char tag[8];
    std::snprintf(tag, sizeof(tag), " %02x ", property.tag);
    out += indent + "  " + EffectFourCCString(property.fourcc) + tag;
    for (size_t i = 0; i < property.value.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      DumpValue(property.value[i], data, out);
    }
    out += '\n';
  }
  for (const EffectNode& child : node.children) {
    DumpNode(child, data, depth + 1, out);
  }
}

void CollectGuids(const EffectValue& value, std::vector<EffectGuid>& out) {
  if (value.kind == EffectValue::Kind::Guid && std::find(out.begin(), out.end(), value.guid) == out.end()) {
    out.push_back(value.guid);
  }
  for (const EffectValue& arg : value.args) {
    CollectGuids(arg, out);
  }
}

void CollectGuids(const EffectNode& node, std::vector<EffectGuid>& out) {
  for (const EffectProperty& property : node.properties) {
    for (const EffectValue& value : property.value) {
      CollectGuids(value, out);
    }
  }
  for (const EffectNode& child : node.children) {
    CollectGuids(child, out);
  }
}

} // namespace

bool ParseEffect(const uint8_t* data, size_t size, EffectNode& out, std::string& error, size_t* failOffset) {
  out = EffectNode();
  if (size < kRootAt + 25 || std::memcmp(data, "RFRM", 4) != 0 ||
      (std::memcmp(data + 0x14, "GENP", 4) != 0 && std::memcmp(data + 0x14, "SWSH", 4) != 0 &&
       std::memcmp(data + 0x14, "ELSM", 4) != 0 && std::memcmp(data + 0x14, "ELC2", 4) != 0)) {
    error = "not a GENP form";
    return false;
  }
  Parser parser(data, size);
  // A standalone swoosh (SWSH form) or electric effect (ELSM, ELC2) has the same header, with
  // that form as its root.
  const bool child = std::memcmp(data + 0x14, "GENP", 4) != 0;
  if (!(child ? parser.Child(kRootAt) : parser.Generator(kRootAt, true))) {
    char text[64];
    std::snprintf(text, sizeof(text), "no parse past 0x%zx", parser.Furthest());
    error = text;
    if (failOffset) {
      *failOffset = parser.Furthest();
    }
    return false;
  }
  parser.BuildNode(kRootAt, EffectGuid{}, out, true);
  return true;
}

bool ParseSpawnTable(const uint8_t* data, size_t size, const EffectProperty& kssm, EffectSpawnTable& out) {
  if (kssm.size < 5 || kssm.offset + kssm.size > size) {
    return false;
  }
  Parser parser(data, size);
  return parser.BuildSpawnTable(kssm.offset + 5, kssm.offset + kssm.size, out);
}

bool ParseMaterialTrack(const uint8_t* data, size_t size, const EffectProperty& property, EffectMaterialTrack& out) {
  if (property.size < 5 || property.offset + property.size > size) {
    return false;
  }
  Parser parser(data, size);
  const size_t trailer = property.fourcc == kPmtr ? 3 : 6;
  return parser.BuildMaterialTrack(property.offset + 5, property.offset + property.size, trailer, out);
}

uint32_t EffectMaterialShader(const uint8_t* mati, size_t size) {
  constexpr size_t kShaderAt = 0x48;  // the MTRL guid, checked in all 2061 MATIs
  if (size < kShaderAt + 4) {
    return 0;
  }
  const uint8_t* p = mati + kShaderAt;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string DumpEffect(const EffectNode& effect, const uint8_t* data) {
  std::string out;
  DumpNode(effect, data, 0, out);
  return out;
}

std::vector<EffectGuid> EffectReferences(const EffectNode& effect) {
  std::vector<EffectGuid> out;
  CollectGuids(effect, out);
  return out;
}

std::string EffectGuidString(const EffectGuid& guid) {
  // Stored as a little-endian UUID: the first three groups are byte-swapped.
  static const int kOrder[] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11, 12, 13, 14, 15};
  std::string text;
  char hex[4];
  for (int index : kOrder) {
    if (index < 0) {
      text += '-';
    } else {
      std::snprintf(hex, sizeof(hex), "%02x", guid[size_t(index)]);
      text += hex;
    }
  }
  return text;
}

std::string EffectFourCCString(uint32_t fourcc) {
  std::string text(4, ' ');
  for (int i = 0; i < 4; ++i) {
    text[size_t(i)] = char(fourcc >> (24 - 8 * i));
  }
  return text;
}

} // namespace PortRemastered
