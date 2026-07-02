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
// JNI registration: NativeActivity dlopens libge.so without registering it with
// ART, so the Java `native` methods cannot resolve by name -- and Java-side
// System.loadLibrary("ge") is NOT an option because it would invoke the bundled
// SDL3's JNI_OnLoad, which FindClass()es the org.libsdl.app.* glue this app does
// not ship and JNI-aborts. So the methods are registered explicitly here
// (AndroidDsRegisterNatives, called from GeApp::OnConfigurePaths early in
// android_main); the Java side gates its first call on a live render loop.
//
// Compiled only on Android (NativeActivity / JNI). On every other platform this
// translation unit is empty -- the desktop binding lives elsewhere / uses the
// rexglue xcb test path.

#if defined(__ANDROID__)

#include "ge_dualscreen.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <rex/platform_android_jni.h>
#include <rex/ui/external_window.h>

namespace {

// GoldenEyeActivity.nativeProvideSecondaryDisplaySurface(Surface, int, int)
void NativeProvideSecondaryDisplaySurface(JNIEnv* env, jobject /*thiz*/,
                                          jobject surface, jint width,
                                          jint height) {
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
void NativeReleaseSecondaryDisplaySurface(JNIEnv* /*env*/, jobject /*thiz*/) {
  ge::DualScreen::Get().RequestSecondaryTeardown();
}

// GoldenEyeActivity.nativeSecondaryTouch(int pointerId, int action, float x, float y)
void NativeSecondaryTouch(JNIEnv* /*env*/, jobject /*thiz*/, jint pointer_id,
                          jint action, jfloat x, jfloat y) {
  ge::DualScreen::Get().OnSecondaryTouch(static_cast<uint32_t>(pointer_id),
                                         static_cast<int>(action), x, y);
}

}  // namespace

namespace ge {

void AndroidDsRegisterNatives() {
  JNIEnv* env = rex::GetAndroidJniEnv();
  jobject activity = rex::GetAndroidActivity();
  if (env == nullptr || activity == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, "GEDS",
                        "RegisterNatives skipped: no JNI env/activity");
    return;
  }
  // The runtime class of the activity instance == GoldenEyeActivity, exactly
  // where the Java `native` declarations live.
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, "GEDS",
                        "RegisterNatives skipped: GetObjectClass failed");
    return;
  }
  static const JNINativeMethod kMethods[] = {
      {"nativeProvideSecondaryDisplaySurface", "(Landroid/view/Surface;II)V",
       reinterpret_cast<void*>(&NativeProvideSecondaryDisplaySurface)},
      {"nativeReleaseSecondaryDisplaySurface", "()V",
       reinterpret_cast<void*>(&NativeReleaseSecondaryDisplaySurface)},
      {"nativeSecondaryTouch", "(IIFF)V",
       reinterpret_cast<void*>(&NativeSecondaryTouch)},
  };
  jint rc = env->RegisterNatives(cls, kMethods,
                                 sizeof(kMethods) / sizeof(kMethods[0]));
  if (rc != JNI_OK || env->ExceptionCheck()) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "GEDS",
                        "RegisterNatives failed (rc=%d); second-screen menu "
                        "disabled (Java side degrades gracefully)",
                        static_cast<int>(rc));
  } else {
    __android_log_print(ANDROID_LOG_INFO, "GEDS",
                        "dual-screen JNI natives registered");
  }
  env->DeleteLocalRef(cls);
}

}  // namespace ge

#endif  // __ANDROID__
