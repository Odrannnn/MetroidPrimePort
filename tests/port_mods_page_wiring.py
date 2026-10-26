"""Source checks for the merged F1 Mods page (the Remastered and Dread sections on it).

F1 used to hold a Remastered page beside Mods. The port's own two mods now live
in one page: the global controls, then "Built-in mods" with one key file row
above two sibling sections, Prime Remastered and Metroid Dread (each its own
switch, imported state and import), then "Other mods" for the folder's other
mods, then texture packs. What the checks here pin down is what a compiler
cannot see:

  * the page list holds one Mods page and no Remastered page, and MP_DEBUG_TAB
    still reaches it by the old names, opening the section each one named;
  * both built-in mods are drawn by the shared switch, which keeps them in
    mods_disabled as the other mods are, says whether this launch loaded them
    apart from what the switch selects, and gives way to Original experience;
    neither is listed again under Other mods;
  * the key file is one row for both imports, drawn once above both sections and
    inside neither, with one buffer and one pick (the settings search counts rows
    by label, so a per-import copy would be a second row for one file);
  * each section's own header is the only wrapper of its content: the import and
    gallery are drawn straight into the indented body, so nothing nests under a
    section and the Dread one is a sibling of Prime Remastered, not a child;
  * the Dread image picker says "Load Metroid Dread image" and both imports say
    "Import": the settings index tells the two apart by hidden id, which the
    search matches exactly, so a jump lands on the right one with either section
    open or closed;
  * each built-in section keeps its import and its progress, and Remastered's
    warning, and the search rows reach all of it on the Mods page.

It is a source check: no engine code runs and no game is started, so it cannot
say a jump lands on the right widget - the search audit run does that, with the
Prime section collapsed and expanded.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, root_argument  # noqa: E402

root = root_argument()
SEARCH = root / "platform/debug_ui/search.cpp"
REMASTERED = root / "platform/debug_ui/remastered.cpp"
MODS = root / "platform/debug_ui/mods.cpp"
INTERNAL = root / "platform/debug_ui/debug_ui_internal.h"
TABS = root / "platform/debug_ui/tabs.cpp"
INDEX = root / "platform/port_settings_index.inc"


def index_rows(text):
    """The rows of the settings index, as {page, section, label, keywords}."""
    rows = []
    for line in text.splitlines():
        if not line.startswith("{"):
            continue
        page, _sub, section, label, keywords = re.findall(r'"([^"]*)"', line)[:5]
        rows.append(dict(page=page, section=section, label=label, keywords=keywords))
    return rows


def builtin_bodies():
    """The DrawBuiltinMods body, and the Prime Remastered and Metroid Dread bodies in it."""
    text = REMASTERED.read_text()
    body = text[text.index("void DrawBuiltinMods() {"):]
    body = body[:body.index("\n}\n") + 3]
    prime = body[body.index('if (ImGui::CollapsingHeader("Prime Remastered"))'):]
    prime = prime[prime.index("\n") + 1:prime.index('if (StartSection("Mods", "Metroid Dread"))')]
    dread = body[body.index('if (ImGui::CollapsingHeader("Metroid Dread"))'):]
    dread = dread[dread.index("\n") + 1:]
    return text, body, prime, dread


class Wiring(WiringCase):
    def test_pending_restart_is_not_a_reloadable_mismatch(self):
        text = MODS.read_text()
        body = text[text.index("void DrawMods() {"):]
        # Keep the global-setting mismatch independent: turning mods off still reloads.
        self.assertIn("bool changed = ModsEnabled() != status.active;", body)
        self.assertIn("const bool wanted = ModsEnabled() && on;", body)
        self.assertIn("restartRequired = restartRequired || (wanted && mod.pendingRestart);", body)
        self.assertIn("changed = changed || (wanted != mod.enabled && !(wanted && mod.pendingRestart));", body)
        self.assertIn('"Restart the application to apply pending room geometry mods."', body)
        # Ordinary mods, not just the built-in toggle, expose the deferred state.
        other = body[body.index('ImGui::SeparatorText("Other mods");'):]
        self.assertIn("ModsEnabled() && on && mod.pendingRestart", other)
        self.assertIn('"not loaded: Restart the application to apply."', other)

    def test_startup_installs_pending_import_before_geometry_capacity_detection(self):
        text = (root / "platform/main.cpp").read_text()
        install = text.index("PortRemastered::ApplyPendingImport()")
        geometry = text.index("PortMods::HasRoomGeometry()")
        ready = text.index("PortRoomGeo::SetBuffersReady(")
        legacy = text.index("PortRemastered::RemoveLegacyImport()")
        mods = text.index("PortMods::Initialize()")
        self.assertEqual(text.count("PortRemastered::ApplyPendingImport()"), 1)
        self.assertLess(text.index("PortLogFile::Start()"), install)
        self.assertLess(text.index("PortPaths::MigrationLog()"), install)
        self.assertLess(install, geometry)
        self.assertLess(geometry, ready)
        self.assertLess(ready, legacy)
        self.assertLess(text.index("disc mounted: %s"), legacy)
        self.assertLess(legacy, mods)
        self.assertIn("installed the imported Remastered models", text[install:geometry])

    def test_pending_restart_is_not_reported_as_reloadable(self):
        text = REMASTERED.read_text()
        pending = text.index("modsOn && mod->pendingRestart")
        reloadable = text.index('"Not loaded: reload the mods to apply."')
        self.assertLess(pending, reloadable)
        self.assertIn('"Not loaded: Restart the application to apply."', text[pending:reloadable])

    def test_one_mods_page_and_the_old_names_still_reach_it(self):
        search = SEARCH.read_text()
        self.assertEqual(search.count('{"Mods", DrawModsTab}'), 1)
        self.assertNotIn('{"Remastered", DrawRemasteredTab}', search)
        self.assertNotIn("DrawRemasteredTab", search + INTERNAL.read_text() + REMASTERED.read_text())
        # MP_DEBUG_TAB=<old name> opens the Mods page, on its section, for the captures and
        # scripts that use it.
        self.assertIn('{"Remastered", "Mods", "Prime Remastered"}', search)
        self.assertIn('{"Dread", "Mods", "Metroid Dread"}', search)

    def test_the_old_names_open_their_section(self):
        # A sub-tab name no tab bar has: StartSection spends it on the first frame the Mods page
        # is drawn, so a closed header (as ImGui last left it) still opens for the capture.
        search = SEARCH.read_text()
        self.assertIn("bool StartSection(const char* page, const char* name)", search + INTERNAL.read_text())
        self.assertIn("if (start) {\n    pending = false;\n  }", search)
        text = REMASTERED.read_text()
        for section in ("Prime Remastered", "Metroid Dread"):
            self.assertIn(f'if (StartSection("Mods", "{section}"))', text)
            self.assertIn("ImGui::SetNextItemOpen(true);", text)

    def test_the_mods_page_draws_the_built_in_sections(self):
        # The page: global controls, the built-in sections, then the other mods.
        text = MODS.read_text()
        self.assertIn("DrawBuiltinMods();", text)
        self.assertLess(text.index('ImGui::Checkbox("Load mods"'), text.index("DrawBuiltinMods();"))
        self.assertLess(text.index("DrawBuiltinMods();"), text.index('ImGui::SeparatorText("Other mods");'))
        # The built-in mods are never listed again under Other mods.
        other = text[text.index('ImGui::SeparatorText("Other mods");'):]
        self.assertIn("mod.name == PortRemastered::kImportModName", other)
        self.assertIn("mod.name == PortDreadSamus::kModName", other)
        self.assertIn('ImGui::TextDisabled("No other mods.");', other)

    def test_each_built_in_mod_has_a_section_with_its_switch_and_import(self):
        text, _body, prime, dread = builtin_bodies()
        # Prime Remastered: switch, the warning, the import and the gallery, all in its body.
        self.assertIn(
            'DrawBuiltinModToggle(PortRemastered::kImportModName, "Use the Remastered models"', prime)
        self.assertIn("DrawRemasteredWarning();", prime)
        self.assertIn("DrawRemasteredImport();", prime)
        # Metroid Dread: its own switch and import, a sibling of the Prime section.
        self.assertIn('DrawBuiltinModToggle(PortDreadSamus::kModName, "Use the Dread Suit models"', dread)
        self.assertIn("DrawDreadSamusImport();", dread)
        # Nothing to switch while the mod is not installed.
        self.assertIn('ImGui::TextDisabled("Not imported yet.");', text)
        # The one switch both sections use.
        self.assertEqual(text.count("void DrawBuiltinModToggle("), 1)
        # The Dread import no longer says to switch the mod off on another page.
        self.assertNotIn("switch it off on the Mods page", MODS.read_text())
        # The page's body is still the Mods function and the textures under it.
        self.assertIn("void DrawModsTab() {\n  DrawMods();\n  DrawTexturesSection();\n}", TABS.read_text())

    def test_one_keys_row_above_both_imports(self):
        text, body, prime, dread = builtin_bodies()
        mods = MODS.read_text()
        # One picker for both imports, drawn once: above either section, and inside neither.
        self.assertEqual(text.count("DrawImportKeys("), 1)
        self.assertEqual(body.count("DrawImportKeys();"), 1)
        self.assertLess(body.index("DrawImportKeys();"), body.index('if (StartSection("Mods", "Prime Remastered"))'))
        self.assertNotIn("DrawImportKeys", prime + dread)
        # One function with no argument, and no second buffer or label to keep apart.
        self.assertIn("void DrawImportKeys()", mods)
        self.assertNotIn("DrawImportKeys(int", mods + INTERNAL.read_text())
        self.assertIn('ImGui::TextUnformatted("Your Nintendo Switch\'s prod.keys")', mods)
        for gone in ("Dread keys", "import-keys-dread", "Keys (used by both imports)"):
            self.assertNotIn(gone, mods)

    def test_each_section_header_is_the_only_wrapper_of_its_content(self):
        text, _body, prime, dread = builtin_bodies()
        mods = MODS.read_text()
        # No header inside either section: the import controls and the gallery are drawn straight
        # into the indented body, which ends before the next top-level header.
        for body_text in (prime, dread):
            self.assertNotIn("CollapsingHeader", body_text)
            self.assertIn("ImGui::Indent();", body_text)
            self.assertIn("ImGui::Unindent();", body_text)
            self.assertLess(body_text.index("ImGui::Indent();"), body_text.index("ImGui::Unindent();"))
        self.assertNotIn('CollapsingHeader("Metroid Prime Remastered models")', mods + text)
        self.assertNotIn('CollapsingHeader("Metroid Dread Samus models")', mods + text)
        self.assertNotIn('CollapsingHeader("Prime Gallery")', text)
        # The gallery is Prime's own subsection, after its import, and still opens the viewer.
        self.assertIn('ImGui::SeparatorText("Gallery");', prime)
        self.assertLess(prime.index("DrawRemasteredImport();"), prime.index('ImGui::SeparatorText("Gallery");'))
        self.assertIn("PortMods::GalleryPaths()", prime)
        self.assertIn("sGalleryOpen = true;", prime)
        self.assertNotIn("Gallery", dread)
        # The two section bodies end (Unindent) before the section's own header closes.
        self.assertLess(prime.index("ImGui::Unindent();"), prime.index("\n  }"))

    def test_both_imports_are_labelled_import(self):
        mods = MODS.read_text()
        # Same visible text, one hidden id each: the search tells them apart without the user.
        self.assertIn('ImGui::Button("Import##remastered")', mods)
        self.assertIn('ImGui::Button("Import##dread")', mods)
        self.assertNotIn("Import the Dread Samus models", mods)
        # The image picker keeps the text the user asked for, on both platforms.
        self.assertEqual(mods.count('"Load Metroid Dread image##dread-image"'), 2)
        for gone in ("Pick the Dread image...", "Browse the Dread image..."):
            self.assertNotIn(gone, mods)
        # A typed Dread path still reaches the shared key file.
        self.assertIn("char* const sDreadKeys = sKeys;", mods)

    def test_the_search_matches_a_rows_hidden_id(self):
        # An index row may carry a widget's hidden id; the jump then matches that widget exactly,
        # whatever else on the page shows the same text, and the results list hides the id.
        search = SEARCH.read_text()
        self.assertIn('if (std::strstr(text, "##") != nullptr) {\n    return std::strcmp(label, text) == 0;\n  }', search)
        self.assertIn("ImGui::FindRenderedTextEnd(row.label)", search)
        rows = index_rows(INDEX.read_text())
        for label in ("Import##remastered", "Import##dread"):
            found = [row for row in rows if row["label"] == label]
            self.assertEqual(len(found), 1, label)
            self.assertEqual(found[0]["page"], "Mods")
            self.assertTrue(found[0]["section"].startswith("Built-in mods"), found[0])

    def test_the_index_has_one_shared_keys_row(self):
        rows = index_rows(INDEX.read_text())
        keys = [row for row in rows if "prod.keys" in row["keywords"]]
        # One row per platform, both above the sections: no section owns the key file any more.
        self.assertEqual(sorted(row["label"] for row in keys), ["Browse...", "Pick the keys..."])
        for row in keys:
            self.assertEqual(row["page"], "Mods")
            self.assertEqual(row["section"], "Built-in mods")
            for word in ("keys", "prod.keys", "remastered", "dread"):
                self.assertIn(word, row["keywords"])
        self.assertFalse([row for row in rows if "Dread keys" in row["label"] or "Dread keys" in row["section"]])
        # The Dread import's controls have rows of their own, on the same page.
        dread = [row for row in rows if row["section"].startswith("Built-in mods > Metroid Dread")]
        self.assertTrue(dread)
        images = [row for row in dread if row["label"] == "Load Metroid Dread image"]
        self.assertEqual(len(images), 2, images)  # one row per platform
        for row in images:
            self.assertEqual(row["section"], "Built-in mods > Metroid Dread > Import")
        # No row names a header the page no longer has.
        for row in rows:
            self.assertNotIn("Metroid Prime Remastered models", row["section"])
            self.assertNotIn("Metroid Dread Samus models", row["section"])
            self.assertNotIn("Prime Gallery", row["section"])

    def test_the_search_reaches_both_built_in_mods_on_the_mods_page(self):
        rows = index_rows(INDEX.read_text())
        for label, keywords in (("Use the Remastered models", "remastered"),
                                ("Use the Dread Suit models", "dread")):
            found = [row for row in rows if row["label"] == label]
            self.assertEqual(len(found), 1, label)
            self.assertEqual(found[0]["page"], "Mods")
            self.assertTrue(found[0]["section"].startswith("Built-in mods"), found[0])
            self.assertIn(keywords, found[0]["keywords"])
        # The index names no page that the overlay no longer has, and no empty conditional.
        self.assertFalse([row for row in rows if row["page"] == "Remastered"])
        self.assertNotIn("#if defined(__ANDROID__)\n#else\n#endif", INDEX.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1)
