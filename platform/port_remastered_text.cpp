#include "port_remastered_text.h"
#include "port_bytes.h"

#include <algorithm>
#include <cstring>

namespace PortRemastered {
namespace {

constexpr uint32_t kStrgMagic = 0x87654321;
constexpr uint32_t kEnglish = 0x454E474C;  // 'ENGL'

using port::AppendBE32;
using port::ReadBE32;
using port::ReadLE16;
using port::ReadLE32;
using port::ReadLE64;

// One "MsgStdBn" file: a 32 byte header, then sections of a 16 byte header
// (name, size) and their data, each padded to 16 bytes.
bool ParseMessages(const uint8_t* data, size_t size, std::vector<TextEntry>& out, std::string& error) {
  if (size < 0x20 || std::memcmp(data, "MsgStdBn", 8) != 0 || data[8] != 0xFF || data[9] != 0xFE || data[12] != 1) {
    error = "not a little endian UTF-16 message file";
    return false;
  }
  const uint8_t* labels = nullptr;
  const uint8_t* texts = nullptr;
  size_t labelsSize = 0;
  size_t textsSize = 0;
  for (size_t at = 0x20; at + 16 <= size;) {
    const size_t length = ReadLE32(data + at + 4);
    if (length > size - at - 16) {
      error = "a section runs past the end";
      return false;
    }
    if (std::memcmp(data + at, "LBL1", 4) == 0) {
      labels = data + at + 16;
      labelsSize = length;
    } else if (std::memcmp(data + at, "TXT2", 4) == 0) {
      texts = data + at + 16;
      textsSize = length;
    }
    at = (at + 16 + length + 15) & ~size_t(15);
  }
  if (labels == nullptr || texts == nullptr || labelsSize < 4 || textsSize < 4) {
    error = "no labels or no text";
    return false;
  }
  const size_t count = ReadLE32(texts);
  if (count > (textsSize - 4) / 4) {
    error = "the text table is cut short";
    return false;
  }
  const size_t first = out.size();
  out.resize(first + count);
  for (size_t i = 0; i < count; ++i) {
    const size_t begin = ReadLE32(texts + 4 + 4 * i);
    const size_t end = i + 1 < count ? ReadLE32(texts + 8 + 4 * i) : textsSize;
    if (begin > end || end > textsSize) {
      error = "a text offset is out of range";
      return false;
    }
    std::u16string& text = out[first + i].text;
    for (size_t p = begin; p + 2 <= end; p += 2) {
      text.push_back(char16_t(ReadLE16(texts + p)));
    }
    // The terminator; a zero inside a tag's payload is not one, so only the last counts.
    if (!text.empty() && text.back() == 0) {
      text.pop_back();
    }
  }
  // Hash slots of (count, offset), each a run of: length, name, text index.
  const size_t slots = ReadLE32(labels);
  if (slots > (labelsSize - 4) / 8) {
    error = "the label table is cut short";
    return false;
  }
  for (size_t s = 0; s < slots; ++s) {
    const size_t labelCount = ReadLE32(labels + 4 + 8 * s);
    size_t at = ReadLE32(labels + 8 + 8 * s);
    for (size_t l = 0; l < labelCount; ++l) {
      if (at >= labelsSize || labels[at] + size_t(5) > labelsSize - at) {
        error = "a label is out of range";
        return false;
      }
      const size_t length = labels[at];
      const size_t index = ReadLE32(labels + at + 1 + length);
      if (index < count) {
        out[first + index].label.assign(reinterpret_cast<const char*>(labels + at + 1), length);
      }
      at += 5 + length;
    }
  }
  return true;
}

bool IsSpace(char16_t c) { return c == u' ' || c == u'\n' || c == u'\r' || c == u'\t'; }

// The words alone: tags out, runs of white space as one space.
std::u16string Words(const std::u16string& text, bool retailMarkup) {
  std::u16string out;
  bool space = false;
  for (size_t i = 0; i < text.size(); ++i) {
    if (retailMarkup && text[i] == u'&') {
      const size_t end = text.find(u';', i);
      if (end != std::u16string::npos) {
        i = end;
        continue;
      }
    }
    if (IsSpace(text[i])) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(u' ');
      space = false;
    }
    out.push_back(text[i]);
  }
  return out;
}

// The last word of a text, trailing white space ignored.
std::u16string LastWord(const std::u16string& text) {
  size_t end = text.size();
  while (end > 0 && IsSpace(text[end - 1])) {
    --end;
  }
  size_t begin = end;
  while (begin > 0 && !IsSpace(text[begin - 1])) {
    --begin;
  }
  return text.substr(begin, end - begin);
}

// The words a retail string ends its lines with. Remastered breaks its lines
// by hand to fit its own boxes; only a break after one of these words belongs
// to the text ("Morphology: <name>\n", a log's heading), the rest are left to
// the original's word wrap.
std::vector<std::u16string> LineEnds(const std::u16string& retail) {
  std::vector<std::u16string> ends;
  std::u16string text;
  for (size_t i = 0; i < retail.size(); ++i) {
    if (retail[i] == u'&') {
      const size_t end = retail.find(u';', i);
      if (end != std::u16string::npos) {
        i = end;
        continue;
      }
    }
    if (retail[i] == u'\n') {
      const std::u16string word = LastWord(text);
      if (!word.empty()) {
        ends.push_back(word);
      }
    }
    text.push_back(retail[i]);
  }
  return ends;
}

bool StartsWith(const std::u16string& text, size_t at, const char16_t* prefix) {
  return text.compare(at, std::char_traits<char16_t>::length(prefix), prefix) == 0;
}

// The tags a retail string opens with that say how its widget lays text out,
// which Remastered leaves to the widget itself.
std::u16string LayoutPrefix(const std::u16string& retail) {
  static const char16_t* const kLayout[] = {u"&just=", u"&vjust=", u"&font=", u"&line-spacing=",
                                            u"&line-extra-space="};
  size_t at = 0;
  while (at < retail.size() && retail[at] == u'&') {
    bool layout = false;
    for (const char16_t* tag : kLayout) {
      layout = layout || StartsWith(retail, at, tag);
    }
    const size_t end = retail.find(u';', at);
    if (!layout || end == std::u16string::npos) {
      break;
    }
    at = end + 1;
  }
  return retail.substr(0, at);
}

// A colour tag's payload (bytes R G B A) in the original's markup. Opaque black
// ends every highlighted run, on dark backgrounds too: it gives the text its
// widget's colour back.
void AppendColour(const char16_t* payload, std::u16string& body, bool& coloured) {
  static const char16_t kHex[] = u"0123456789ABCDEF";
  if (coloured) {
    body += u"&pop;";
    coloured = false;
  }
  if (payload[0] != 0 || payload[1] != 0xFF00) {
    body += u"&push;&main-color=#";
    for (int k = 0; k < 2; ++k) {
      const unsigned low = payload[k] & 0xFF;
      const unsigned high = payload[k] >> 8;
      body += {kHex[low >> 4], kHex[low & 15], kHex[high >> 4], kHex[high & 15]};
    }
    body += u';';
    coloured = true;
  }
}

// A tag's length in units after its 0x0E (group, type, payload size, payload),
// or 0 when it runs past the end.
size_t TagUnits(const std::u16string& text, size_t at) {
  if (at + 3 >= text.size()) {
    return 0;
  }
  const size_t units = (size_t(text[at + 3]) + 1) / 2;
  return units > text.size() - at - 4 ? 0 : 3 + units;
}

// A message's words, its tags left out.
std::u16string MessageWords(const std::u16string& text) {
  std::u16string plain;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == 0x0E) {
      const size_t units = TagUnits(text, i);
      if (units == 0) {
        break;
      }
      i += units;
    } else if (text[i] == 0x0F) {
      i += 2;
    } else {
      plain.push_back(text[i]);
    }
  }
  return Words(plain, false);
}

size_t CountBreaks(const std::u16string& text) { return size_t(std::count(text.begin(), text.end(), u'\n')); }

// A text's words as tokens: lower case, letters and digits only.
std::vector<std::u16string> Tokens(const std::u16string& words) {
  std::vector<std::u16string> tokens(1);
  for (const char16_t c : words) {
    if (c == u' ') {
      if (!tokens.back().empty()) {
        tokens.emplace_back();
      }
    } else if ((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || c >= 0x80) {
      tokens.back().push_back(c);
    } else if (c >= u'A' && c <= u'Z') {
      tokens.back().push_back(char16_t(c - u'A' + u'a'));
    }
  }
  if (tokens.back().empty()) {
    tokens.pop_back();
  }
  return tokens;
}

// Whether two strings say the same thing: the same tokens when short, else at
// least 3/4 of them in common, in order (Remastered rewords a little, a PAL
// disc rewrote some logs outright).
bool SameText(const std::vector<std::u16string>& a, const std::vector<std::u16string>& b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  if (a.size() < 4 || b.size() < 4) {
    return a == b;
  }
  std::vector<size_t> row(b.size() + 1, 0);
  for (size_t i = 0; i < a.size(); ++i) {
    size_t diagonal = 0;
    for (size_t j = 0; j < b.size(); ++j) {
      const size_t up = row[j + 1];
      row[j + 1] = a[i] == b[j] ? diagonal + 1 : std::max(row[j], up);
      diagonal = up;
    }
  }
  return 4 * 2 * row[b.size()] >= 3 * (a.size() + b.size());
}

constexpr uint32_t FourCc(const char* code) {
  return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
         uint32_t(uint8_t(code[3]));
}

}  // namespace

const TextLanguage kTextLanguages[] = {
    {"EUFR", "French (France)"}, {"USFR", "French (Canada)"},  {"EUSP", "Spanish (Spain)"},
    {"USSP", "Spanish (Latin America)"}, {"EUGE", "German"}, {"EUIT", "Italian"}, {"EUDU", "Dutch"},
};
const size_t kTextLanguageCount = sizeof(kTextLanguages) / sizeof(kTextLanguages[0]);

bool ParseMsbt(const uint8_t* data, size_t size, const char* language, std::vector<TextEntry>& out,
               std::string& error) {
  if (size < 0x20 || std::memcmp(data, "RFRM", 4) != 0 || std::memcmp(data + 0x14, "MSBT", 4) != 0) {
    error = "not an MSBT";
    return false;
  }
  // The form's length leaves out its header; what follows it is the extractor's footer.
  const uint64_t form = ReadLE64(data + 4);
  const size_t end = form < size - 0x20 ? size_t(form) + 0x20 : size;
  // Chunks of a 24 byte header: the language, the length, a version and a skip.
  for (size_t at = 0x20; at + 24 <= end;) {
    const uint64_t length = ReadLE64(data + at + 4);
    if (length > end - at - 24) {
      error = "a language runs past the end";
      return false;
    }
    if (std::memcmp(data + at, language, 4) == 0) {
      return ParseMessages(data + at + 24, size_t(length), out, error);
    }
    at += 24 + size_t(length);
  }
  error = std::string("no ") + language + " text";
  return false;
}

bool SplitTextLabel(const std::string& label, uint32_t& strg, uint32_t& index) {
  if (label.size() < 12 || label.size() > 16 || label[0] != '[' || label[9] != ']' || label[10] != '_') {
    return false;
  }
  strg = 0;
  for (size_t i = 1; i < 9; ++i) {
    const char c = label[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) {
      return false;
    }
    strg = strg << 4 | uint32_t(digit);
  }
  index = 0;
  for (size_t i = 11; i < label.size(); ++i) {
    if (label[i] < '0' || label[i] > '9') {
      return false;
    }
    index = index * 10 + uint32_t(label[i] - '0');
  }
  return true;
}

bool SplitNamedLabel(const std::string& label, uint32_t& strg, std::string& name) {
  uint32_t index = 0;
  if (label.size() < 12 || SplitTextLabel(label, strg, index) ||
      !SplitTextLabel(label.substr(0, 11) + "0", strg, index)) {
    return false;
  }
  name = label.substr(11);
  return true;
}

bool ConvertText(const std::u16string& remastered, const std::u16string& retail, std::u16string& out) {
  std::u16string body;
  std::u16string plain;
  bool coloured = false;
  const std::vector<std::u16string> lineEnds = LineEnds(retail);
  for (size_t i = 0; i < remastered.size(); ++i) {
    const char16_t c = remastered[i];
    if (c == 0x0E) {
      // group, type, payload bytes, payload
      const size_t units = TagUnits(remastered, i);
      if (units == 0) {
        return false;
      }
      const unsigned group = remastered[i + 1];
      const unsigned type = remastered[i + 2];
      const char16_t* payload = remastered.data() + i + 4;
      i += units;
      if (group == 0 && type == 3 && units == 5) {
        AppendColour(payload, body, coloured);
      } else if ((group == 0 && type == 2) || (group == 1 && type == 3)) {
        // A size in percent, and a layout mode: the original's widgets have their own.
      } else {
        return false;  // a button of Remastered's controls, or one of its icons
      }
    } else if (c == 0x0F) {
      i += 2;  // a closing tag: group, type
    } else if (c == u'\n') {
      if (std::find(lineEnds.begin(), lineEnds.end(), LastWord(plain)) != lineEnds.end()) {
        body.push_back(c);
        plain.push_back(c);
      } else if (!plain.empty() && !IsSpace(plain.back()) && plain.back() != u'-') {
        // A word broken at its hyphen ("mass-\nproduction") stays joined.
        body.push_back(u' ');
        plain.push_back(u' ');
      }
    } else if (c >= 0x20 && c < 0x7F && c != u'&') {
      body.push_back(c);
      plain.push_back(c);
    } else {
      return false;  // a glyph the original's fonts may lack, or the start of a tag
    }
  }
  if (coloured) {
    body += u"&pop;";
  }
  if (Words(plain, false) == Words(retail, true)) {
    return false;
  }
  out = LayoutPrefix(retail) + body;
  return true;
}

bool TranslateText(const std::u16string& remastered, const std::u16string& retail, const std::u16string& english,
                   std::u16string& out) {
  // The disc's button images, in the order its string shows them.
  std::vector<std::u16string> images;
  for (size_t at = retail.find(u"&image="); at != std::u16string::npos; at = retail.find(u"&image=", at + 1)) {
    const size_t end = retail.find(u';', at);
    if (end == std::u16string::npos) {
      return false;
    }
    images.push_back(retail.substr(at, end + 1 - at));
  }
  // Breaks are where Remastered's English has the disc's (the same count), else
  // only between paragraphs: the rest were made for Remastered's boxes.
  const bool keepBreaks = CountBreaks(english) == CountBreaks(retail);
  std::u16string body;
  bool coloured = false;
  size_t image = 0;
  for (size_t i = 0; i < remastered.size(); ++i) {
    const char16_t c = remastered[i];
    if (c == 0x0E) {
      const size_t units = TagUnits(remastered, i);
      if (units == 0) {
        return false;
      }
      const unsigned group = remastered[i + 1];
      const unsigned type = remastered[i + 2];
      const char16_t* payload = remastered.data() + i + 4;
      i += units;
      if (group == 0 && type == 3 && units == 5) {
        AppendColour(payload, body, coloured);
      } else if ((group == 0 && type == 2) || (group == 1 && type == 3)) {
        // A size and a layout mode, as in ConvertText.
      } else if (group == 1 && (type == 0 || type == 1) && image < images.size()) {
        body += images[image++];  // a button or an icon, where the disc shows its own
      } else {
        return false;
      }
    } else if (c == 0x0F) {
      i += 2;
    } else if (c == u'\n') {
      if (keepBreaks) {
        body.push_back(c);
      } else if (i + 1 < remastered.size() && remastered[i + 1] == u'\n') {
        while (!body.empty() && body.back() == u' ') {
          body.pop_back();
        }
        body += u"\n\n";
        while (i + 1 < remastered.size() && remastered[i + 1] == u'\n') {
          ++i;
        }
      } else if (!body.empty() && !IsSpace(body.back()) && body.back() != u'-') {
        body.push_back(u' ');
      }
    } else if (c == u'\r') {
      // Windows line ends: the \n does the work.
    } else if (c >= 0x20 && c != u'&' && c != 0x7F) {
      body.push_back(c);
    } else {
      return false;  // the original's markup has no escape for '&'
    }
  }
  if (image != images.size()) {
    return false;
  }
  if (coloured) {
    body += u"&pop;";
  }
  if (Words(body, true).empty()) {
    return false;
  }
  out = LayoutPrefix(retail) + body;
  return true;
}

bool MergeStringTable(const uint8_t* retail, size_t size, const TableText& text, bool checkWording,
                      std::vector<uint8_t>& out, int& reworded, int& translated) {
  reworded = 0;
  translated = 0;
  if (size < 16 || ReadBE32(retail) != kStrgMagic || ReadBE32(retail + 4) != 0) {
    return false;
  }
  const size_t languages = ReadBE32(retail + 8);
  const size_t count = ReadBE32(retail + 12);
  if (languages > (size - 16) / 8 || count > size / 4) {
    return false;
  }
  const size_t base = 16 + 8 * languages;
  // Every language, as its strings.
  std::vector<uint32_t> codes;
  std::vector<std::vector<std::u16string>> tables(languages);
  size_t english = languages;
  for (size_t l = 0; l < languages; ++l) {
    codes.push_back(ReadBE32(retail + 16 + 8 * l));
    const size_t offset = ReadBE32(retail + 20 + 8 * l);
    if (offset > size - base || 4 + 4 * count > size - base - offset) {
      return false;
    }
    const uint8_t* table = retail + base + offset + 4;
    const size_t room = size - base - offset - 4;
    for (size_t s = 0; s < count; ++s) {
      std::u16string string;
      for (size_t p = ReadBE32(table + 4 * s);; p += 2) {
        if (p + 2 > room) {
          return false;
        }
        const char16_t c = char16_t(table[p] << 8 | table[p + 1]);
        if (c == 0) {
          break;
        }
        string.push_back(c);
      }
      tables[l].push_back(std::move(string));
    }
    if (codes[l] == kEnglish && english == languages) {
      english = l;
    }
  }
  if (english == languages) {
    return false;
  }
  const std::vector<std::u16string> disc = tables[english];

  // Each disc string's Remastered versions: by its index, else by a name whose
  // English is the disc's word for word, or is the disc's screen title ("[ Inventory ]")
  // without its brackets.
  std::vector<const std::map<std::string, std::u16string>*> versions(count, nullptr);
  std::vector<bool> bracketed(count, false);
  std::vector<std::u16string> discWords;
  if (!text.byName.empty() || checkWording) {
    for (size_t s = 0; s < count; ++s) {
      discWords.push_back(Words(disc[s], true));
    }
  }
  // Remastered's indices are 1.00's. Another version keeps them unless it moved
  // a table's strings (PAL inserted some into the pause screen's): there each
  // string goes where its wording is, and one Remastered reworded moves as the
  // nearest string before it does.
  std::vector<std::pair<uint32_t, const std::map<std::string, std::u16string>*>> entries;
  for (const auto& [index, byLanguage] : text.byIndex) {
    entries.emplace_back(index, &byLanguage);
  }
  std::vector<long> placed(entries.size(), -1);
  bool moved = false;
  if (checkWording) {
    std::vector<std::vector<std::u16string>> discTokens;
    for (const std::u16string& words : discWords) {
      discTokens.push_back(Tokens(words));
    }
    std::vector<std::vector<std::u16string>> tokens;
    for (const auto& [index, byLanguage] : entries) {
      const auto found = byLanguage->find(kRemasteredEnglish);
      tokens.push_back(found != byLanguage->end() ? Tokens(MessageWords(found->second))
                                                  : std::vector<std::u16string>());
    }
    std::vector<bool> taken(count, false);
    for (size_t e = 0; e < entries.size(); ++e) {
      const uint32_t index = entries[e].first;
      if (index < count && SameText(tokens[e], discTokens[index])) {
        placed[e] = long(index);
        taken[index] = true;
      }
    }
    for (size_t e = 0; e < entries.size(); ++e) {
      for (size_t s = 0; s < count && placed[e] < 0; ++s) {
        if (!taken[s] && SameText(tokens[e], discTokens[s])) {
          placed[e] = long(s);
          taken[s] = true;
          moved = moved || tokens[e].size() >= 4;  // a sentence, not a word two strings share
        }
      }
    }
    for (size_t e = 0; moved && e < entries.size(); ++e) {
      if (placed[e] >= 0) {
        continue;
      }
      long shift = 0;
      bool anchored = false;
      for (size_t k = e; k-- > 0 && !anchored;) {
        if (placed[k] >= 0) {
          shift = placed[k] - long(entries[k].first);
          anchored = true;
        }
      }
      for (size_t k = e + 1; k < entries.size() && !anchored; ++k) {
        if (placed[k] >= 0) {
          shift = placed[k] - long(entries[k].first);
          anchored = true;
        }
      }
      const long at = long(entries[e].first) + shift;
      if (at >= 0 && at < long(count) && !taken[at]) {
        placed[e] = at;
        taken[at] = true;
      }
    }
  }
  for (size_t e = 0; e < entries.size(); ++e) {
    if (moved && placed[e] >= 0) {
      versions[placed[e]] = entries[e].second;
    } else if (!moved && entries[e].first < count) {
      versions[entries[e].first] = entries[e].second;
    }
  }
  for (const auto& [name, byLanguage] : text.byName) {
    const auto found = byLanguage.find(kRemasteredEnglish);
    if (found == byLanguage.end()) {
      continue;
    }
    const std::u16string words = MessageWords(found->second);
    for (size_t s = 0; s < count && !words.empty(); ++s) {
      if (versions[s] != nullptr) {
        continue;
      }
      if (discWords[s] == words || discWords[s] == u"[ " + words + u" ]") {
        versions[s] = &byLanguage;
        bracketed[s] = discWords[s] != words;
      }
    }
  }

  for (size_t s = 0; s < count; ++s) {
    if (versions[s] == nullptr) {
      continue;
    }
    const auto found = versions[s]->find(kRemasteredEnglish);
    std::u16string converted;
    if (found != versions[s]->end() && !bracketed[s] && ConvertText(found->second, disc[s], converted)) {
      tables[english][s] = std::move(converted);
      ++reworded;
    }
  }
  // A language starts as the disc's own section for it (PAL's FREN for EUFR), else
  // the English, so a string without a translation stays readable.
  static const struct {
    const char* remastered;
    uint32_t disc;
  } kDiscSections[] = {
      {"EUFR", FourCc("FREN")}, {"USFR", FourCc("FREN")}, {"EUGE", FourCc("GERM")},
      {"EUSP", FourCc("SPAN")}, {"USSP", FourCc("SPAN")}, {"EUIT", FourCc("ITAL")},
  };
  for (size_t k = 0; k < kTextLanguageCount; ++k) {
    const uint32_t code = FourCc(kTextLanguages[k].code);
    if (std::find(codes.begin(), codes.end(), code) != codes.end()) {
      continue;
    }
    size_t base = english;
    for (const auto& section : kDiscSections) {
      if (std::strcmp(section.remastered, kTextLanguages[k].code) == 0) {
        const auto at = std::find(codes.begin(), codes.end(), section.disc);
        if (at != codes.end()) {
          base = size_t(at - codes.begin());
        }
      }
    }
    std::vector<std::u16string> strings = tables[base];
    int done = 0;
    for (size_t s = 0; s < count; ++s) {
      if (versions[s] == nullptr) {
        continue;
      }
      const auto found = versions[s]->find(kTextLanguages[k].code);
      const auto remasteredEnglish = versions[s]->find(kRemasteredEnglish);
      std::u16string converted;
      if (found != versions[s]->end() &&
          TranslateText(found->second, disc[s],
                        remasteredEnglish != versions[s]->end() ? remasteredEnglish->second : std::u16string(),
                        converted)) {
        if (bracketed[s]) {
          const size_t prefix = LayoutPrefix(disc[s]).size();
          converted = converted.substr(0, prefix) + u"[ " + converted.substr(prefix) + u" ]";
        }
        strings[s] = std::move(converted);
        ++done;
      }
    }
    if (done > 0) {
      codes.push_back(code);
      tables.push_back(std::move(strings));
      translated += done;
    }
  }
  if (reworded == 0 && translated == 0) {
    return false;
  }
  out.clear();
  AppendBE32(out, kStrgMagic);
  AppendBE32(out, 0);
  AppendBE32(out, uint32_t(tables.size()));
  AppendBE32(out, uint32_t(count));
  size_t offset = 0;
  for (size_t l = 0; l < tables.size(); ++l) {
    AppendBE32(out, codes[l]);
    AppendBE32(out, uint32_t(offset));
    offset += 4 + 4 * count;
    for (const std::u16string& text : tables[l]) {
      offset += 2 * (text.size() + 1);
    }
  }
  for (size_t l = 0; l < tables.size(); ++l) {
    size_t length = 4 * count;
    for (const std::u16string& text : tables[l]) {
      length += 2 * (text.size() + 1);
    }
    AppendBE32(out, uint32_t(length));
    size_t at = 4 * count;
    for (const std::u16string& text : tables[l]) {
      AppendBE32(out, uint32_t(at));
      at += 2 * (text.size() + 1);
    }
    for (const std::u16string& text : tables[l]) {
      for (const char16_t c : text) {
        out.push_back(uint8_t(c >> 8));
        out.push_back(uint8_t(c));
      }
      out.push_back(0);
      out.push_back(0);
    }
  }
  // A resource in a pak is a whole number of 32 byte blocks.
  out.resize((out.size() + 31) & ~size_t(31), 0xFF);
  return true;
}

}  // namespace PortRemastered
