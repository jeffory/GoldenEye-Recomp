// ge - second-screen weapon-selection menu (the dual-screen UI, sub-task C).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// A WeaponMenuDialog is an ImGui dialog meant to own a whole *secondary* display
// (the AYN Thor's 3.92" bottom touch panel). It renders the player's carried
// weapons + ammo read from the game-state bridge (ge_gamestate.h) and lets the
// player tap a weapon to switch to it. It draws into the secondary surface's own
// ImGuiContext, so it never touches the primary game surface.
//
// Construction self-registers with the supplied drawer (see rex::ui::ImGuiDialog):
//
//     new ge::WeaponMenuDialog(secondary->imgui_drawer());
//
// The dialog only READS the bridge snapshot and POSTS equip requests; both calls
// are thread-safe and lock-free, and the dialog itself only runs on the UI thread
// (driven by SecondaryUiSurface::Paint), so there is no extra synchronisation
// here. It is intentionally free of any platform / Android headers.

#pragma once

#include <rex/ui/imgui_dialog.h>

namespace ge {

class WeaponMenuDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit WeaponMenuDialog(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Best-effort, human-friendly label for a weapon slot id. Falls back to
  // "Slot N" when the id is outside the provisional name table -- the real
  // GoldenEye slot->name mapping is confirmed on-device (see ge_gamestate.cpp),
  // so this never asserts a name it is unsure of.
  static const char* WeaponLabel(int id);
};

}  // namespace ge
