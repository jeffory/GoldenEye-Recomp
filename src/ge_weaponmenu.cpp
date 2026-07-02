// ge - second-screen weapon-selection menu. See ge_weaponmenu.h.
//
// Drawn as a single full-panel ImGui window (no title bar / not movable): this
// dialog owns the entire secondary display, so it fills io.DisplaySize. Layout
// is a responsive grid of large, touch-friendly buttons -- one per carried
// weapon -- with ammo counts; the equipped weapon is highlighted. Tapping a
// button posts an equip request to the game thread via the bridge.

#include "ge_weaponmenu.h"

#include "ge_gamestate.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ge {

namespace {

// Briefing-style palette, kept close to the pause menu (ge_menu.cpp) so the two
// screens read as one product.
constexpr ImU32 kBg = IM_COL32(18, 16, 12, 255);          // near-black backing
constexpr ImU32 kPanel = IM_COL32(214, 201, 162, 255);    // manila paper
constexpr ImU32 kBtn = IM_COL32(52, 46, 34, 255);         // unselected slate
constexpr ImU32 kBtnHover = IM_COL32(74, 66, 48, 255);
constexpr ImU32 kBtnActive = IM_COL32(96, 86, 62, 255);
constexpr ImU32 kEquipped = IM_COL32(196, 36, 28, 255);   // red = currently equipped
constexpr ImU32 kEquippedHover = IM_COL32(220, 60, 50, 255);

// Glyph magnification for the whole panel. The default ImGui font is unreadably
// small on the handheld's secondary display; ~3x makes it legible at arm's
// length. Applied via SetWindowFontScale in OnDraw.
constexpr float kFontScale = 3.0f;

}  // namespace

const char* WeaponMenuDialog::WeaponLabel(int id) {
  // Weapon ids confirmed on-device by reverse-engineering the equipped-weapon
  // field (ge_gamestate.cpp): switching to each weapon and reading its id. Only
  // ids we have actually verified are named here; anything else falls back to
  // "Weapon N" so the UI never shows a name it cannot stand behind. Extend this
  // as more ids are confirmed.
  switch (id) {
    case 1:  return "Unarmed";
    case 5:  return "PP7";
    case 7:  return "Klobb";
    case 8:  return "KF7 Soviet";
    case 17: return "Sniper Rifle";
    case 24: return "Grenade Launcher";
    case 29: return "Remote Mine";
    case 30: return "Detonator";
    default: break;
  }
  static char buf[16];
  std::snprintf(buf, sizeof(buf), "Weapon %d", id);
  return buf;
}

WeaponMenuDialog::WeaponMenuDialog(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}

void WeaponMenuDialog::OnDraw(ImGuiIO& io) {
  const ImVec2 screen = io.DisplaySize;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(screen);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  ImGui::PushStyleColor(ImGuiCol_WindowBg, kBg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
  if (!ImGui::Begin("##ge_weapon_menu", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return;
  }

  // The secondary panel is small and viewed at arm's length on the handheld, so
  // the default ImGui font renders tiny. Scale every glyph in this window up so
  // headers, the unavailable message, and weapon labels are legible.
  ImGui::SetWindowFontScale(kFontScale);

  const gamestate::WeaponSnapshot snap = gamestate::GetWeaponSnapshot();

  // Header.
  ImGui::PushStyleColor(ImGuiCol_Text, kPanel);
  ImGui::TextUnformatted("WEAPONS");
  ImGui::PopStyleColor();
  ImGui::Separator();
  ImGui::Spacing();

  if (!snap.valid || snap.held_count == 0) {
    // Single-screen fallback / pre-RE state: render an explicit "unavailable"
    // message instead of trusting stale or zeroed data.
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 190, 160, 255));
    const char* msg = snap.valid ? "No weapons carried." : "Weapon data unavailable.";
    const ImVec2 sz = ImGui::CalcTextSize(msg);
    ImGui::SetCursorPos(ImVec2((screen.x - sz.x) * 0.5f, screen.y * 0.5f));
    ImGui::TextUnformatted(msg);
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return;
  }

  // Responsive grid sizing. Target ~2 columns on the narrow AYN panel, more if
  // the surface is wider; each button is sized for a fingertip.
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float min_btn_w = 200.0f;
  int cols = std::max(1, static_cast<int>(avail_w / min_btn_w));
  cols = std::min(cols, 4);
  const float spacing = 10.0f;
  const float btn_w = (avail_w - spacing * (cols - 1)) / static_cast<float>(cols);
  // Two-line labels at the magnified font need headroom; size the button from the
  // scaled line height so the name + ammo never clip.
  const float min_btn_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 24.0f;
  const float btn_h = std::max(min_btn_h, screen.y * 0.16f);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));

  int drawn = 0;
  for (int i = 0; i < snap.held_count; ++i) {
    const int id = snap.held_ids[i];
    const bool equipped = (id == snap.equipped_id);
    const unsigned ammo = (id >= 0 && id < gamestate::kMaxWeaponSlots) ? snap.ammo[id] : 0;

    if (drawn % cols != 0) {
      ImGui::SameLine();
    }
    ++drawn;

    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, equipped ? kEquipped : kBtn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, equipped ? kEquippedHover : kBtnHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, equipped ? kEquippedHover : kBtnActive);

    // Ammo is currently known only for the equipped weapon (the per-weapon
    // reserve pool isn't mapped yet), so only show a count when we actually have
    // one -- otherwise just the name, plus an "(equipped)" marker.
    char label[64];
    if (ammo > 0) {
      std::snprintf(label, sizeof(label), "%s\nx%u%s", WeaponLabel(id), ammo,
                    equipped ? "  (equipped)" : "");
    } else {
      std::snprintf(label, sizeof(label), "%s%s", WeaponLabel(id),
                    equipped ? "\n(equipped)" : "");
    }
    if (ImGui::Button(label, ImVec2(btn_w, btn_h)) && !equipped) {
      // Post the switch; the game thread applies it on its next frame at a safe
      // point (see ge_gamestate.cpp). No-op on the equipped weapon.
      gamestate::RequestEquipWeapon(id);
    }

    ImGui::PopStyleColor(3);
    ImGui::PopID();
  }

  ImGui::PopStyleVar(2);

  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

}  // namespace ge
