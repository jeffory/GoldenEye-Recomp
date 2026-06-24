// ge - Android secondary-display binding (dual-screen, sub-task A2, native side).
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
//
// The Java side (GoldenEyeActivity / WeaponMenuPresentation) enumerates displays
// with DisplayManager, creates a Presentation on the AYN Thor's secondary panel,
// hosts a SurfaceView there, and hands its Surface to us here. We turn it into an
// ANativeWindow and give it to the DualScreen controller, which builds the
// rexglue secondary ImGui surface from it on the UI thread.
//
// Compiled only on Android (NativeActivity / JNI). On every other platform this
// translation unit is empty -- the desktop binding lives elsewhere / uses the
// rexglue xcb test path.

#if defined(__ANDROID__)

#include "ge_dualscreen.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <rex/ui/external_window.h>

extern "C" {

// GoldenEyeActivity.nativeProvideSecondaryDisplaySurface(Surface, int, int)
JNIEXPORT void JNICALL
Java_com_sunjaycy_goldeneye_GoldenEyeActivity_nativeProvideSecondaryDisplaySurface(
    JNIEnv* env, jobject /*thiz*/, jobject surface, jint width, jint height) {
  if (surface == nullptr) {
    ge::DualScreen::Get().RequestSecondaryTeardown();
    return;
  }
  // Acquires a reference to the underlying ANativeWindow; released by the
  // on_release callback once the surface built from it is destroyed.
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) {
    return;
  }
  auto handle = rex::ui::ExternalWindowHandle::FromAndroidNativeWindow(window);
  ge::DualScreen::Get().ProvideSecondaryWindow(
      handle, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      [window] { ANativeWindow_release(window); });
}

// GoldenEyeActivity.nativeReleaseSecondaryDisplaySurface()
JNIEXPORT void JNICALL
Java_com_sunjaycy_goldeneye_GoldenEyeActivity_nativeReleaseSecondaryDisplaySurface(
    JNIEnv* /*env*/, jobject /*thiz*/) {
  ge::DualScreen::Get().RequestSecondaryTeardown();
}

// GoldenEyeActivity.nativeSecondaryTouch(int pointerId, int action, float x, float y)
JNIEXPORT void JNICALL
Java_com_sunjaycy_goldeneye_GoldenEyeActivity_nativeSecondaryTouch(
    JNIEnv* /*env*/, jobject /*thiz*/, jint pointer_id, jint action, jfloat x, jfloat y) {
  ge::DualScreen::Get().OnSecondaryTouch(static_cast<uint32_t>(pointer_id),
                                         static_cast<int>(action), x, y);
}

}  // extern "C"

#endif  // __ANDROID__
