#pragma once

// Remastered's text, as replacements for the disc's string tables.
//
// Remastered keeps its text in MSBT assets: an RFRM form with one chunk per
// language, each holding a standard "MsgStdBn" message file (labels in LBL1,
// UTF-16 text in TXT2). A string that came from the original game is labelled
// "[<STRG id>]_<index>", so the two games' tables line up without a list.
//
// Only strings whose wording changed are taken, and only when the text can be
// written in the original's markup: a colour tag becomes "&main-color", size
// and layout tags are dropped, a line break is kept only where the disc's
// string breaks a line too, and a string naming a button or an icon keeps
// the disc's version, Remastered's controls not being this game's.
//
// Remastered's translations are added to the same tables, as more languages
// named by their MSBT chunk ("EUFR"), which the port's language setting picks.
// A translated string keeps the disc's layout tags and button images (in the
// order Remastered names its buttons), so one naming more or fewer buttons
// than the disc's string stays English. Remastered's own names for the
// original's strings ("[0552A456]_InstructionsA") are placed by their English.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace PortRemastered {

struct TextEntry {
  std::string label;
  std::u16string text;  // as stored, tags included
};

// The messages of one language ("USEN") of an MSBT asset.
bool ParseMsbt(const uint8_t* data, size_t size, const char* language, std::vector<TextEntry>& out,
               std::string& error);

// "[0D1F9C75]_002" -> 0x0D1F9C75, 2. False for Remastered's own labels.
bool SplitTextLabel(const std::string& label, uint32_t& strg, uint32_t& index);
// "[0552A456]_InstructionsA" -> 0x0552A456, "InstructionsA". False for an
// index's label and Remastered's own.
bool SplitNamedLabel(const std::string& label, uint32_t& strg, std::string& name);

// The languages added to the tables, in their MSBT chunk's name, which is also
// their STRG language. English is "USEN" in Remastered and "ENGL" on the disc.
struct TextLanguage {
  const char* code;
  const char* name;
};
extern const TextLanguage kTextLanguages[];
extern const size_t kTextLanguageCount;
constexpr const char* kRemasteredEnglish = "USEN";

// Remastered's string in the original's markup. False when the disc's string
// should stay: the wording is the same, or the text needs something the
// original cannot draw.
bool ConvertText(const std::u16string& remastered, const std::u16string& retail, std::u16string& out);
// A translation in the original's markup, laid out as the disc's English
// string `retail`; `english` is Remastered's English for it. False when it
// should stay English: its buttons do not match the disc's, or it holds
// something the markup cannot.
bool TranslateText(const std::u16string& remastered, const std::u16string& retail, const std::u16string& english,
                   std::u16string& out);

// One table's strings in Remastered, each by language chunk ("USEN", "EUFR").
struct TableText {
  std::map<uint32_t, std::map<std::string, std::u16string>> byIndex;
  std::map<std::string, std::map<std::string, std::u16string>> byName;
};

// A copy of the disc's STRG with the English strings Remastered reworded
// (ConvertText) and a section for each of kTextLanguages it has translations
// for. False when the table cannot be read or nothing in it changed.
// `checkWording` (a disc other than 1.00, whose tables Remastered's indices
// don't fit) matches each indexed string by its English instead.
bool MergeStringTable(const uint8_t* retail, size_t size, const TableText& text, bool checkWording,
                      std::vector<uint8_t>& out, int& reworded, int& translated);

}  // namespace PortRemastered
