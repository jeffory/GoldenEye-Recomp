// ge - Android on-screen touch-controls binding (native side).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// The Java side (GoldenEyeActivity + TouchControlsView) draws a translucent
// virtual gamepad over the game whenever no physical controller is present,
// tracks multitouch, and pushes a fully-formed pad frame here via
// nativeSetTouchState. It reads its policy/tuning cvars back through the small
// getters below, and the optional in-overlay weapon grid reads the live weapon
// snapshot + posts equip requests through the gamestate bridge.
//
// JNI registration: like the dual-screen methods, these must be registered
// explicitly with RegisterNatives (NativeActivity dlopens libge.so without
// registering it with ART, and System.loadLibrary("ge") would trip the bundled
// SDL3 JNI_OnLoad -> abort). See ge_android_ds.cpp for the full rationale.
// AndroidTouchRegisterNatives() is called from GeApp::OnConfigurePaths next to
// AndroidDsRegisterNatives().
//
// Compiled only on Android; empty everywhere else.

#if defined(__ANDROID__)

#include "ge_touchpad.h"

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/platform_android_jni.h>

#include "ge_gamestate.h"

namespace {

int16_t ClampAxis(jint v) {
  return static_cast<int16_t>(std::clamp<jint>(v, -32767, 32767));
}
uint8_t ClampTrigger(jint v) {
  return static_cast<uint8_t>(std::clamp<jint>(v, 0, 255));
}

// GoldenEyeActivity.nativeSetTouchState(int buttons,int lt,int rt,int lx,int ly,int rx,int ry)
void NativeSetTouchState(JNIEnv* /*env*/, jobject /*thiz*/, jint buttons, jint lt,
                         jint rt, jint lx, jint ly, jint rx, jint ry) {
  ge::PadState s;
  s.buttons = static_cast<uint16_t>(buttons & 0xFFFF);
  s.left_trigger = ClampTrigger(lt);
  s.right_trigger = ClampTrigger(rt);
  s.thumb_lx = ClampAxis(lx);
  s.thumb_ly = ClampAxis(ly);
  s.thumb_rx = ClampAxis(rx);
  s.thumb_ry = ClampAxis(ry);
  ge::TouchPad::Get().SetState(s);
}

jstring CvarString(JNIEnv* env, const char* name) {
  std::string v = rex::cvar::GetFlagByName(name);
  return env->NewStringUTF(v.c_str());
}

jstring NativeTouchControlsMode(JNIEnv* env, jobject /*thiz*/) {
  return CvarString(env, "ge_touch_controls");
}
jstring NativeTouchLookMode(JNIEnv* env, jobject /*thiz*/) {
  return CvarString(env, "ge_touch_look_mode");
}
jfloat NativeTouchLookSens(JNIEnv* /*env*/, jobject /*thiz*/) {
  const std::string v = rex::cvar::GetFlagByName("ge_touch_look_sens");
  return v.empty() ? 1.0f : static_cast<jfloat>(std::atof(v.c_str()));
}
jfloat NativeTouchOpacity(JNIEnv* /*env*/, jobject /*thiz*/) {
  const std::string v = rex::cvar::GetFlagByName("ge_touch_opacity");
  return v.empty() ? 0.5f : static_cast<jfloat>(std::atof(v.c_str()));
}

// --- Weapon grid (optional in-overlay menu) -------------------------------
void NativeRequestEquipWeapon(JNIEnv* /*env*/, jobject /*thiz*/, jint id) {
  ge::gamestate::RequestEquipWeapon(static_cast<int32_t>(id));
}

jint NativeEquippedWeaponId(JNIEnv* /*env*/, jobject /*thiz*/) {
  const auto snap = ge::gamestate::GetWeaponSnapshot();
  if (!snap.valid) return ge::gamestate::kNoWeapon;
  return static_cast<jint>(snap.equipped_id);
}

// Fills ids[]/ammo[] with the carried weapons in slot order; returns the count
// (>=0), or -1 when there is no live player snapshot. Arrays shorter than the
// held count are filled up to their length.
jint NativeCarriedWeapons(JNIEnv* env, jobject /*thiz*/, jintArray ids,
                          jintArray ammo) {
  const auto snap = ge::gamestate::GetWeaponSnapshot();
  if (!snap.valid) return -1;
  const jsize cap_ids = ids ? env->GetArrayLength(ids) : 0;
  const jsize cap_ammo = ammo ? env->GetArrayLength(ammo) : 0;
  const int n = std::min<int>(snap.held_count, ge::gamestate::kMaxWeaponSlots);
  for (int i = 0; i < n; ++i) {
    const int wid = snap.held_ids[i];
    if (ids && i < cap_ids) {
      jint v = wid;
      env->SetIntArrayRegion(ids, i, 1, &v);
    }
    if (ammo && i < cap_ammo) {
      jint a = (wid >= 0 && wid < ge::gamestate::kMaxWeaponSlots)
                   ? snap.ammo[wid]
                   : 0;
      env->SetIntArrayRegion(ammo, i, 1, &a);
    }
  }
  return static_cast<jint>(n);
}

}  // namespace

namespace ge {

void AndroidTouchRegisterNatives() {
  JNIEnv* env = rex::GetAndroidJniEnv();
  jobject activity = rex::GetAndroidActivity();
  if (env == nullptr || activity == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, "GETOUCH",
                        "RegisterNatives skipped: no JNI env/activity");
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, "GETOUCH",
                        "RegisterNatives skipped: GetObjectClass failed");
    return;
  }
  static const JNINativeMethod kMethods[] = {
      {"nativeSetTouchState", "(IIIIIII)V",
       reinterpret_cast<void*>(&NativeSetTouchState)},
      {"nativeTouchControlsMode", "()Ljava/lang/String;",
       reinterpret_cast<void*>(&NativeTouchControlsMode)},
      {"nativeTouchLookMode", "()Ljava/lang/String;",
       reinterpret_cast<void*>(&NativeTouchLookMode)},
      {"nativeTouchLookSens", "()F",
       reinterpret_cast<void*>(&NativeTouchLookSens)},
      {"nativeTouchOpacity", "()F",
       reinterpret_cast<void*>(&NativeTouchOpacity)},
      {"nativeRequestEquipWeapon", "(I)V",
       reinterpret_cast<void*>(&NativeRequestEquipWeapon)},
      {"nativeEquippedWeaponId", "()I",
       reinterpret_cast<void*>(&NativeEquippedWeaponId)},
      {"nativeCarriedWeapons", "([I[I)I",
       reinterpret_cast<void*>(&NativeCarriedWeapons)},
  };
  jint rc = env->RegisterNatives(cls, kMethods,
                                 sizeof(kMethods) / sizeof(kMethods[0]));
  if (rc != JNI_OK || env->ExceptionCheck()) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "GETOUCH",
                        "RegisterNatives failed (rc=%d); touch controls "
                        "disabled (Java side degrades gracefully)",
                        static_cast<int>(rc));
  } else {
    __android_log_print(ANDROID_LOG_INFO, "GETOUCH",
                        "touch-controls JNI natives registered");
  }
  env->DeleteLocalRef(cls);
}

}  // namespace ge

#endif  // __ANDROID__
