
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
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
)

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

rexglue_setup_target(ge)
