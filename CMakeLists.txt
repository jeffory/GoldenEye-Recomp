
# ge - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# C and ASM are required by the ReXGlue SDK (add_subdirectory'd via
# generated/rexglue.cmake): C for FFmpeg + the Android NativeActivity glue, ASM
# for the Android fiber context switch.
project(ge LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GE_SOURCES
    src/main.cpp
    src/ge_fps.cpp
    src/ge_replay.cpp      # input record/replay/bench harness + poll-cadence probe
    src/ge_hooks.cpp
    src/ge_gamestate.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
    src/ge_dualscreen.cpp   # dual-screen weapon-menu controller (all platforms)
    src/ge_weaponmenu.cpp   # second-screen weapon-menu UI (all platforms)
    src/ge_touchpad.cpp     # on-screen touch controls -> synthesized pad (all platforms)
    src/ge_input_map.cpp    # gamepad button remap (applied at the XInput boundary)
    src/ge_asset_check.cpp  # startup required-file manifest check
)

# Android-only JNI binding: turns the secondary-display Surface (created by the
# Java Presentation) into an ANativeWindow for the controller. It is also
# #ifdef __ANDROID__-guarded, but it includes android/native_window_jni.h which
# only exists in the NDK, so it is compiled on Android alone.
if(ANDROID)
    list(APPEND GE_SOURCES src/ge_android_ds.cpp)
    list(APPEND GE_SOURCES src/ge_android_touch.cpp)  # on-screen touch-controls JNI
endif()

if(WIN32)
    # Embed the application icon (GoldenEye.rc -> "MAINICON"). Use LLVM's resource
    # compiler since the toolchain is clang/lld (MSVC rc.exe may not be on PATH).
    if(NOT CMAKE_RC_COMPILER)
        set(CMAKE_RC_COMPILER "llvm-rc")
    endif()
    enable_language(RC)
    add_executable(ge WIN32 ${GE_SOURCES} GoldenEye.rc)
elseif(ANDROID)
    # NativeActivity loads this as a shared library (android.app.lib_name=ge).
    # The android_main entry + app-glue come from the SDK via rexglue_setup_target.
    add_library(ge SHARED ${GE_SOURCES})
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output a GoldenEye.exe (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
# NOT on Android: NativeActivity dlopens the lib named by android.app.lib_name
# ("ge" -> libge.so); renaming it to libGoldenEye.so makes the app unlaunchable.
if(NOT ANDROID)
    set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye")
endif()

# Some hosts (e.g. Fedora clang) need libatomic for 16-byte atomics used by
# PadState and friends.
find_library(ATOMIC_LIBRARY NAMES atomic HINTS /usr/lib/gcc/x86_64-redhat-linux/16)
if(ATOMIC_LIBRARY)
    target_link_libraries(ge PRIVATE ${ATOMIC_LIBRARY})
else()
    target_link_options(ge PRIVATE -latomic)
endif()

rexglue_setup_target(ge)
