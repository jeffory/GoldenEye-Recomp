// ge_ce_patches.cpp
//
// BeanTools "Community Edition" DATA bug-fixes, applied as one-shot guest-RAM
// writes at boot. The recomp loads the xex DATA segment into guest RAM at start,
// and the generated code reads level/setup/fog data from there at use-time, so
// writing these values at boot is exactly equivalent to BeanTools' finalizer.c
// patching the xex image (data_write / blob memcpy). This file ports ONLY the
// data-only finalizer functions that fix real game bugs on STOCK assets:
//   adjust_fog_bank, fix_setups, fix_item_watch_positions, tweak_natalya_scale,
//   silo_aztec_fix_shuttle_culling, fix_level_portals_bg_data, fix_mp_weapon_sizes
// Code/instruction patches and new hack-functions live in ge_hooks.cpp; CE
// asset-pack content (cuba->jungle, surface split, CE\ asset paths) is skipped.
//
// Values + addresses are 1:1 with finalizer.c (verified against our exact image).

#include <cstdint>
#include <cstring>

// BeanTools data blobs (patches/*.h). const arrays = internal linkage; only this
// TU includes them, so no ODR issue.
#include "ce_patches/runwaydoorpads.h"
#include "ce_patches/jungle_ai_list_040D.h"
#include "ce_patches/surface1cabindoors.h"
#include "ce_patches/surface2cabindoors.h"
#include "ce_patches/dam_intro_fix.h"
#include "ce_patches/egypt_puzzle_glass_door_fix.h"
#include "ce_patches/siloicbmview.h"
#include "ce_patches/aztecshuttlearea.h"
#include "ce_patches/frigatebg_fixportals.h"
#include "ce_patches/surfacebg_portaladditions.h"
#include "ce_patches/mpweaponsets.h"
#include "ce_patches/mpchars_altsandbonus.h"

namespace {

// Guest RAM is big-endian. data_write(addr,val) wrote val MSB-first; replicate.
inline void W(uint8_t* b, uint32_t ga, uint32_t v) {
  uint32_t s = __builtin_bswap32(v);
  std::memcpy(b + ga, &s, 4);
}
inline uint32_t RD(uint8_t* b, uint32_t ga) {
  uint32_t v;
  std::memcpy(&v, b + ga, 4);
  return __builtin_bswap32(v);
}
// Blob memcpy: finalizer wrote blob[i] to consecutive guest addrs (raw bytes,
// already big-endian) -- a straight byte copy.
#define CPY(b, ga, blob) std::memcpy((b) + (ga), (blob), sizeof(blob))

// LEVELID + fog-range offsets (finalizer.c).
enum {
  L_STATUE = 0x16, L_ARCHIVES = 0x18, L_TRAIN = 0x19, L_BUNKER2 = 0x1B,
  L_STREETS = 0x1D, L_EGYPT = 0x20, L_DAM = 0x21, L_FACILITY = 0x22,
  L_RUNWAY = 0x23, L_JUNGLE = 0x25, L_TEMPLE = 0x26, L_CITADEL = 0x28,
  L_CRADLE = 0x29, L_SURFACE2 = 0x2B, L_FRIGATE = 0x1A,
  O_SP_CINEMA = 0x0384, O_P2 = 0xC8, O_P3 = 0x12C, O_P4 = 0x190,
};

// ---- adjust_fog_bank(): retune fog / draw distance (pure data) --------------
void ce_adjust_fog_bank(uint8_t* b) {
  for (uint32_t i = 0x82858860u; i < 0x82859510u; i += 0x38u) {
    if (RD(b, i) == 0) break;
    switch ((RD(b, i) & 0xFFFF0000u) >> 16) {
      case L_RUNWAY:
        W(b, i + 0x24, 0x0000D6D8); W(b, i + 0x28, 0x85ADCA00);
        W(b, i + 0x2C, 0x000124F8);
        [[fallthrough]];
      case L_DAM: case L_FACILITY: case L_BUNKER2: case L_EGYPT:
        W(b, i + 0x30, 0x40C00000); break;
      case L_DAM + O_SP_CINEMA:
        W(b, i + 0x28, 0x85ADCA00); break;
      case L_STREETS:
        W(b, i + 0x08, 0x271003E8); W(b, i + 0x2C, 0x00003A98);
        W(b, i + 0x30, 0x40C00000); break;
      case L_TRAIN:
        W(b, i + 0x04, 0x11940000); W(b, i + 0x24, 0x00001194);
        W(b, i + 0x30, 0x40C00000); break;
      case L_STATUE:
        W(b, i + 0x30, 0x40C00000); W(b, i + 0x24, 0x00001388);
        W(b, i + 0x28, 0x09070800); W(b, i + 0x2C, 0x00002EE0); break;
      case L_SURFACE2: case L_SURFACE2 + O_P2:
      case L_SURFACE2 + O_P3: case L_SURFACE2 + O_P4:
        W(b, i + 0x24, 0x00001964); break;
      case L_CRADLE: case L_CRADLE + O_P2:
      case L_CRADLE + O_P3: case L_CRADLE + O_P4:
        W(b, i + 0x28, 0x6E819600); W(b, i + 0x30, 0x40C00000); break;
      case L_JUNGLE:
        W(b, i + 0x04, 0x157C05DC); W(b, i + 0x08, 0x196403E8);
        W(b, i + 0x10, 0x797C7900); W(b, i + 0x24, 0x00001194);
        W(b, i + 0x28, 0x797C7900); W(b, i + 0x2C, 0x00001388);
        W(b, i + 0x30, 0x40C00000); break;
      case L_ARCHIVES: case L_ARCHIVES + O_P2:
      case L_ARCHIVES + O_P3: case L_ARCHIVES + O_P4:
        W(b, i + 0x04, 0x11940DAC); W(b, i + 0x24, 0x00001194);
        W(b, i + 0x2C, 0x000057E4); break;
      case L_TEMPLE: case L_TEMPLE + O_P2:
      case L_TEMPLE + O_P3: case L_TEMPLE + O_P4:
        W(b, i + 0x04, 0x57E40BB8); W(b, i + 0x08, 0x0FA00258);
        W(b, i + 0x10, 0x10306001); W(b, i + 0x14, 0x138800FF);
        W(b, i + 0x18, 0xFFFF0000); W(b, i + 0x1C, 0xEC780000);
        W(b, i + 0x24, 0x000057E4); W(b, i + 0x28, 0x10306000);
        W(b, i + 0x2C, 0x000057E4); break;
      case L_CITADEL + O_P2:  // unused sky bank -> bunker i
        W(b, i + 0x00, 0x0009000A); W(b, i + 0x04, 0x3A9803E8);
        W(b, i + 0x08, 0x4E2002EE); W(b, i + 0x0C, 0x03E403E8);
        W(b, i + 0x10, 0x60608001); W(b, i + 0x14, 0x271000F0);
        W(b, i + 0x18, 0x781E0000); W(b, i + 0x1C, 0xFC180100);
        W(b, i + 0x20, 0xFFFF0700); W(b, i + 0x24, 0x0000AFC8);
        W(b, i + 0x28, 0xA4968200); W(b, i + 0x2C, 0x0000AFC8);
        W(b, i + 0x30, 0x40000000); W(b, i + 0x34, 0xB8D1B717); break;
      case L_CITADEL + O_P3:  // unused sky bank -> silo
        W(b, i + 0x00, 0x0014000A); W(b, i + 0x04, 0x271003E8);
        W(b, i + 0x08, 0x3A9802EE); W(b, i + 0x0C, 0x03E403E8);
        W(b, i + 0x10, 0x18181801); W(b, i + 0x14, 0x27100006);
        W(b, i + 0x18, 0x1B550000); W(b, i + 0x1C, 0xFC180100);
        W(b, i + 0x20, 0xFFFF0000); W(b, i + 0x24, 0x00002710);
        W(b, i + 0x28, 0x18181800); W(b, i + 0x2C, 0x00002710);
        W(b, i + 0x30, 0x40000000); W(b, i + 0x34, 0xB8D1B717); break;
      default: break;
    }
  }
  for (uint32_t i = 0x82859510u; i < 0x828595F0u; i += 0x38u) {
    switch (RD(b, i)) {
      case L_FRIGATE: case L_FRIGATE + O_P2:
      case L_FRIGATE + O_P3: case L_FRIGATE + O_P4:
        W(b, i + 0x20, 0xC37D0000); break;  // water height -253.f
      default: break;
    }
  }
}

// ---- fix_setups(): the big level-bug-fix batch (pure data) ------------------
void ce_fix_setups(uint8_t* b) {
  W(b, 0x82D5BB6Cu, 0x00002902);                 // runway guards spawn with weapons
  CPY(b, 0x82D56DE8u, runwaydoorpads);           // runway door pads off ground

  W(b, 0x82D85420u + 0x00, 0x00C10780);          // surface i dual-flagged klobbs
  W(b, 0x82D85420u + 0x10, 0xC1079000);

  W(b, 0x82D43728u + 0x00, 0x9D00000A);          // jungle Xenia trigger vs new fog
  W(b, 0x82D43728u + 0x04, 0x00020803);
  W(b, 0x82D43728u + 0x08, 0x55005C2C);
  W(b, 0x82D43728u + 0x0C, 0xF8FD2C3C);
  W(b, 0x82D43728u + 0x10, 0x2C010802);
  CPY(b, 0x82D43BC0u, jungle_ai_list_040D);      // jungle guards detect at start

  W(b, 0x82DAE62Cu, 0xFFFF0710);                 // statue end-cinema guard weapon
  W(b, 0x82DAD7F0u + 0x00, 0x4A003B23);          // statue Trev force-spawn vs fog
  W(b, 0x82DAD7F0u + 0x14, 0x1023AD54);
  W(b, 0x82DAD7F0u + 0x3C, 0x00001123);
  W(b, 0x82DAD7F0u + 0x4C, 0x00001123);
  W(b, 0x82DAD7F0u + 0x5C, 0x00001123);
  W(b, 0x82DAD7F0u + 0x6C, 0x00001123);

  W(b, 0x82CF5AECu, 0x8200B7C8);                 // egypt MP ammo box plink
  W(b, 0x82D0D124u, 0x01000009);                 // dam padlock drops on destroy

  W(b, 0x82D7C554u, 0x000201E1);                 // surface i bookshelves invincible
  W(b, 0x82D7C5D4u, 0x000201E1);                 // (game glitches if destroyed)

  W(b, 0x82D77DE0u + 0x00, 0xC59B4000);          // surface i/ii radar railing pad links
  W(b, 0x82D8882Cu + 0x00, 0xC59B4000);
  W(b, 0x82D77DE0u + 0x1C, 0x82012798);
  W(b, 0x82D8882Cu + 0x1C, 0x82012798);

  W(b, 0x82CAD4C0u, 0x00004000);                 // Trev DK5 right-handed

  W(b, 0x82C91238u, 0x43988000);                 // archives MP ammo pad shift
  W(b, 0x82DC7A04u, 0x000527FC);                 // archives MP ammo -> pad 27FC

  W(b, 0x82DDFC20u + 0x00, 0x43730000);          // temple MP ammo pad
  W(b, 0x82DDFC20u + 0x08, 0x43490000);
  W(b, 0x82DDFC20u + 0x10, 0x3F800000);
  W(b, 0x82DDFC20u + 0x18, 0xBF800000);
  W(b, 0x82DDFC20u + 0x20, 0x00000000);
  W(b, 0x82DE05E0u + 0x0000, 0x0005001C);        // temple MP ammo objects
  W(b, 0x82DE05E0u + 0x00B4, 0x0005000E);
  W(b, 0x82DE05E0u + 0x01F4, 0x0005001F);
  W(b, 0x82DE05E0u + 0x02A8, 0x00050020);
  W(b, 0x82DE05E0u + 0x03E8, 0x00050013);
  W(b, 0x82DE05E0u + 0x049C, 0x00050014);
  W(b, 0x82DE05E0u + 0x0A78, 0x00050010);

  W(b, 0x82D98BF8u + 0x00, 0x007400E3);          // silo armor model + 007-diff load
  W(b, 0x82D98BF8u + 0x08, 0x00000000);
  W(b, 0x82DAAA74u, 0x00000000);                 // statue armor 007-diff
  W(b, 0x82D408E8u, 0x00000000);                 // jungle armor 007-diff
  W(b, 0x82D27DDCu, 0x00732745);                 // frigate hidden armor moved
  W(b, 0x82D8D55Cu, 0x000000C0);                 // surface ii half armor -> SA diff

  CPY(b, 0x82D788ACu, surface1cabindoors);       // surface i cabin door opens
  CPY(b, 0x82D89450u, surface2cabindoors);       // surface ii cabin door opens

  W(b, 0x82CACD48u, 0x00040004);                 // control 1F blast door squish

  W(b, 0x82CC7D30u, 0x233300EE);                 // facility Doak uses Dave head
  W(b, 0x82CC7D48u, 0x233300EF);
  W(b, 0x82CC7D60u, 0x233300F0);
  W(b, 0x82CC7D78u, 0x233300F1);
  W(b, 0x82CC7D90u, 0x233300F2);
  W(b, 0x82CC7DA8u, 0x233300F3);
  W(b, 0x82CC7DBCu, 0xBD233300);

  W(b, 0x82CAD418u, 0x00004000);                 // jungle Natalya drops magnum
  W(b, 0x82D3D2F0u, 0x00004000);                 // control Natalya drops magnum

  CPY(b, 0x82D0E7D8u, dam_intro_fix);            // dam intro camera
  CPY(b, 0x82CF9634u, egypt_puzzle_glass_door_fix);  // egypt puzzle glass doors

  W(b, 0x82DE24A4u + 0x00, 0xC5198000);          // basement flag pad
  W(b, 0x82DE24A4u + 0x10, 0xBF800000);
  W(b, 0x82DE24A4u + 0x18, 0x00000000);
  W(b, 0x82DE24A4u + 0x1C, 0x820154A0);
  W(b, 0x82DC2D94u, 0x014D0029);                 // library flag -> pad 0029

  // glass bullet-hole pads (control SP)
  const uint32_t ctrl_pads[] = {0x82CAB11Cu, 0x82CAB1B0u, 0x82CAB244u, 0x82CAB2D8u,
                                0x82CAB36Cu, 0x82CAB400u, 0x82CAB494u};
  for (uint32_t p : ctrl_pads) { W(b, p + 0, 0x10000B62); W(b, p + 4, 0x00304000); }

  // glass bullet-hole pads (facility SP)
  const uint32_t fsp_pads[] = {0x82CC0890u, 0x82CC0924u, 0x82CC09B8u, 0x82CC0A4Cu,
                               0x82CC0AE0u, 0x82CC0B74u, 0x82CC0C08u, 0x82CC0C9Cu,
                               0x82CC0D30u, 0x82CC0DC4u, 0x82CC0E58u, 0x82CC0EECu,
                               0x82CC0F80u, 0x82CC1014u};
  for (uint32_t p : fsp_pads) { W(b, p + 0, 0x10000262); W(b, p + 4, 0x00304000); }

  // glass bullet-hole pads (facility MP)
  const uint32_t fmp_pads[] = {0x82DCB458u, 0x82DCB4ECu, 0x82DCB580u, 0x82DCB614u,
                               0x82DCB6A8u, 0x82DCB73Cu, 0x82DCB7D0u, 0x82DCB864u};
  for (uint32_t p : fmp_pads) { W(b, p + 0, 0x10000262); W(b, p + 4, 0x00304000); }

  W(b, 0x82804B9Cu, 0x07550CCD);                 // bunker ii stairs nook fix
  W(b, 0x82804BECu, 0x075B0CC4);
}

// ---- fix_item_watch_positions() --------------------------------------------
void ce_fix_item_watch_positions(uint8_t* b) {
  W(b, 0x82422744u + 0x00, 0x41800000);          // clipboard
  W(b, 0x82422744u + 0x08, 0x43ED0000);
  W(b, 0x82422744u + 0x1C, 0xC0C00000);
  W(b, 0x82422744u + 0x20, 0x43AF0000);
  W(b, 0x82ECFF18u + 0x00, 0x41F29AEA);          // circuitboard
  W(b, 0x82ECFF18u + 0x04, 0x3F3A5311);
  W(b, 0x82ECFF18u + 0x08, 0x42C43EDF);
  W(b, 0x82422BA4u + 0x00, 0xC2D20000);          // bolt key
  W(b, 0x82422BA4u + 0x04, 0xC3E10000);
  W(b, 0x82422BA4u + 0x08, 0x43C80000);
  W(b, 0x82422BA4u + 0x18, 0xC3E10000);
  W(b, 0x82422BA4u + 0x1C, 0xC2FA0000);
  W(b, 0x82422BA4u + 0x20, 0x43C80000);
  W(b, 0x82422B6Cu + 0x00, 0x43F30000);          // yale key
  W(b, 0x82422B6Cu + 0x04, 0xC3F60000);
  W(b, 0x82422B6Cu + 0x08, 0x43020000);
  W(b, 0x82422B6Cu + 0x18, 0xC3F00000);
  W(b, 0x82422B6Cu + 0x1C, 0x43EB0000);
  W(b, 0x82422B6Cu + 0x20, 0x43960000);
}

void ge_apply_ce_data_patches_impl(uint8_t* b) {
  ce_adjust_fog_bank(b);
  ce_fix_setups(b);
  ce_fix_item_watch_positions(b);
  // tweak_natalya_scale
  W(b, 0x82729270u, 0x3F733333);                 // scale 0.95
  W(b, 0x8272BA94u, 0x3F6EB1C4);                 // MP POV height
  // silo_aztec_fix_shuttle_culling (portal/culling)
  CPY(b, 0x82A7A540u, siloicbmview);
  CPY(b, 0x828E8340u, aztecshuttlearea);
  // fix_level_portals_bg_data (portal/BG)
  CPY(b, 0x829B5CC0u, frigatebg_fixportals);
  CPY(b, 0x82A5DF70u, surfacebg_portaladditions);
  // fix_mp_weapon_sizes
  CPY(b, 0x82424728u, mpweaponsets);
  // increase_mp_characters: new/alt character struct data (count bumped to 0x32
  // by the li-hooks in ge_hooks.cpp). Re-skins of existing models, no new assets.
  CPY(b, 0x8272BA80u, mpchars_altsandbonus);
  // hardcode_near_clip_to_2: initial global value (the per-fog store hook in
  // ge_hooks.cpp keeps it pinned to 2.0f thereafter).
  W(b, 0x82858804u, 0x40000000u);  // near-clip = 2.0f
  // MP death-tune chain: 0x824203AC was a 5.0f constant in vanilla; CE repurposes
  // it as the "this death isn't yours" bypass flag and relocates the 5.0f read to
  // 0x824203A8 (see ge_ce_killtune_float). Seed both.
  W(b, 0x824203A8u, 0x40A00000u);  // 5.0f (relocated kill-tune constant)
  W(b, 0x824203ACu, 0x00000000u);  // bypass-death-tune flag = 0
}

}  // namespace

// Public entry, called once at boot from ge_hooks.cpp.
void ge_apply_ce_data_patches(uint8_t* base) { ge_apply_ce_data_patches_impl(base); }
