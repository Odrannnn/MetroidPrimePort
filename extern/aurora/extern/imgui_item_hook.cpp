// Port: ImGui's test-engine item hooks (IMGUI_ENABLE_TEST_ENGINE), forwarded to
// a callback. The F1 settings search uses it to find, open and scroll to a
// widget by its label. Items are only reported while g.TestEngineHookItems is
// set, so this costs nothing otherwise.
#include "imgui.h"
#include "imgui_internal.h"

void (*gImGuiItemAddHook)(ImGuiContext* ctx, ImGuiID id, const ImRect& rect) = nullptr;
void (*gImGuiItemInfoHook)(ImGuiContext* ctx, ImGuiID id, const char* label,
                           ImGuiItemStatusFlags flags) = nullptr;

// Every item, with no label (combos only report this one).
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
  if (gImGuiItemAddHook != nullptr) {
    gImGuiItemAddHook(ctx, id, rect);
  }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label,
                                  ImGuiItemStatusFlags flags) {
  if (gImGuiItemInfoHook != nullptr) {
    gImGuiItemInfoHook(ctx, id, label, flags);
  }
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
