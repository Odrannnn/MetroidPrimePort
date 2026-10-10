#!/usr/bin/env python3
"""Guard the Android GCI save-dialog filename handoff."""

from pathlib import Path
import sys


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
java = (root / "android/app/src/main/java/org/libsdl/app/SDLActivity.java").read_text()
cpp = (root / "platform/debug_ui.cpp").read_text()

show_start = java.index("public static boolean showFileDialog(")
show_end = java.index("/* Internal class used to track active open file dialog */", show_start)
show = java[show_start:show_end]

assert "public static boolean showFileDialog(String[] filters, boolean allowMultiple, boolean forWrite, int requestCode)" in show
assert "private static final AtomicReference<String> sNextSaveDialogSuggestedFilename" in java
assert "sNextSaveDialogSuggestedFilename.set(filename);" in java
consume = "String suggestedFilename = forWrite ? sNextSaveDialogSuggestedFilename.getAndSet(null) : null;"
assert consume in show
assert show.count("getAndSet(null)") == 1
assert show.index(consume) < show.index("if (mSingleton == null)")
assert "if (forWrite) {\n            intent.putExtra(Intent.EXTRA_TITLE, safeSaveDialogTitle(suggestedFilename));\n        }" in show
title_start = java.index("private static String safeSaveDialogTitle(")
title_end = java.index("\n    /**", title_start)
title = java[title_start:title_end]
assert "lastIndexOf('/')" in title and "lastIndexOf('\\\\')" in title
assert "substring(separator + 1)" in title and "return \"save.gci\";" in title

# Import/open requests must not consume the one-shot title; it is only read by
# the forWrite conditional above. The native side must refresh it from the
# current queue member immediately before opening each Android save dialog.
card_start = cpp.index("void OpenCardDialog(CardPick pick)")
card_end = cpp.index("std::string CardSaveTo(", card_start)
card = cpp[card_start:card_end]
queue_name = "PortGci::PathString(sCardExportQueue.front().filename())"
assert queue_name in card
assert card.index(queue_name) < card.index("SetNextAndroidSaveDialogFilename(location.c_str());")
assert card.index("SetNextAndroidSaveDialogFilename(location.c_str());") < card.index("SDL_ShowSaveFileDialog(")

print("Android save-dialog title handoff checks passed")
