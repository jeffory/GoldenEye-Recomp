# Build image for shipped Linux releases.
#
# Base moved from debian:12 to ubuntu:24.04 (2026-08-04): the SDK specializes
# std::chrono::clock_time_conversion for its custom clocks, and Debian 12's libstdc++-12
# does not declare that template at all (zero occurrences in /usr/include/c++/12/chrono) —
# not a missing symbol, a missing template, so there was no ABI workaround. The documented
# fallback (libstdc++-13 from bookworm-backports) does not exist either:
# `apt-cache policy libstdc++-13-dev` returns an empty candidate on bookworm. Ubuntu 24.04
# ships g++-13 in main, which does declare the template.
#
# ubuntu:24.04 pins the ABI floor at glibc 2.39 / GLIBCXX_3.4.33 — still below SteamOS
# (Steam Deck: glibc 2.41 / GLIBCXX_3.4.34) and every currently-supported distro. Building
# releases here instead of on the Fedora 44 host is the fix for issue #12. Do not bump the
# base image without re-checking the floor.
#
# See docs/superpowers/specs/2026-08-04-linux-release-container-design.md
FROM ubuntu:24.04

ARG LLVM_VERSION=19
ARG CMAKE_VERSION=3.31.6
ENV DEBIAN_FRONTEND=noninteractive

# libgtk-3-dev, libx11-xcb-dev and libxi-dev are the external deps the build needs: the
# SDK's src/ui/CMakeLists.txt does pkg_check_modules(... REQUIRED gtk+-3.0 / x11-xcb / xi)
# for the GTK window, the XCB surface handle, and XInput2 raw mouse motion respectively.
# SDL3, Vulkan-Headers, volk, FFmpeg, glslang, SPIRV-Tools and imgui are all vendored
# submodules, and Vulkan itself is loaded through volk at runtime.
#
# g++-13 / libstdc++-13-dev is Ubuntu 24.04's default GCC in main — no PPA, no backports —
# and it is the whole reason the base moved: it declares std::chrono::clock_time_conversion,
# which libstdc++-12 does not.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg \
      ninja-build pkg-config python3 git file binutils \
      g++-13 libstdc++-13-dev libgtk-3-dev libx11-xcb-dev libxi-dev \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04's stock clang cannot do -std=c++23 the way the project needs; pull clang-19
# from apt.llvm.org's noble repo instead.
RUN wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.asc] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" \
 && rm -rf /var/lib/apt/lists/* \
 && ln -sf "/usr/bin/clang-${LLVM_VERSION}"   /usr/bin/clang \
 && ln -sf "/usr/bin/clang++-${LLVM_VERSION}" /usr/bin/clang++ \
 && ln -sf "/usr/bin/ld.lld-${LLVM_VERSION}"  /usr/bin/ld.lld

# Ubuntu 24.04 ships cmake 3.28.3, which only barely satisfies the project's
# cmake_minimum_required(VERSION 3.25). Use a current upstream build instead.
RUN wget -qO /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
 && tar -xzf /tmp/cmake.tar.gz -C /opt \
 && ln -sf "/opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/cmake" /usr/bin/cmake \
 && rm /tmp/cmake.tar.gz

WORKDIR /work
