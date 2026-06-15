
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
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
)

if(WIN32)
    add_executable(ge WIN32 ${GE_SOURCES})
elseif(ANDROID)
    # NativeActivity loads this as a shared library (android.app.lib_name=ge).
    # The android_main entry + app-glue come from the SDK via rexglue_setup_target.
    add_library(ge SHARED ${GE_SOURCES})
else()
    add_executable(ge ${GE_SOURCES})
endif()

rexglue_setup_target(ge)
