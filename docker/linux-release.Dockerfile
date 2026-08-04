# Build image for shipped Linux releases.
#
# Debian 12 pins the ABI floor at glibc 2.36 / GLIBCXX_3.4.30 — below SteamOS (Steam Deck)
# and every currently-supported distro. Building releases here instead of on the Fedora 44
# host is the fix for issue #12. Do not bump the base image without re-checking the floor.
#
# See docs/superpowers/specs/2026-08-04-linux-release-container-design.md
FROM debian:12

ARG LLVM_VERSION=19
ARG CMAKE_VERSION=3.31.6
ENV DEBIAN_FRONTEND=noninteractive

# libgtk-3-dev is the ONLY external dependency the build needs: the SDK's
# rexglue_helpers.cmake does pkg_check_modules(GTK3 REQUIRED gtk+-3.0). SDL3,
# Vulkan-Headers, volk, FFmpeg, glslang, SPIRV-Tools and imgui are all vendored
# submodules, and Vulkan itself is loaded through volk at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg \
      ninja-build pkg-config python3 git file binutils \
      libstdc++-12-dev libgtk-3-dev \
 && rm -rf /var/lib/apt/lists/*

# Debian 12's stock clang-14 cannot do -std=c++23, which the project requires.
RUN wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.asc] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" \
 && rm -rf /var/lib/apt/lists/* \
 && ln -sf "/usr/bin/clang-${LLVM_VERSION}"   /usr/bin/clang \
 && ln -sf "/usr/bin/clang++-${LLVM_VERSION}" /usr/bin/clang++ \
 && ln -sf "/usr/bin/ld.lld-${LLVM_VERSION}"  /usr/bin/ld.lld

# Debian 12 ships cmake 3.25.1, which only barely satisfies the project's
# cmake_minimum_required(VERSION 3.25). Use a current upstream build instead.
RUN wget -qO /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
 && tar -xzf /tmp/cmake.tar.gz -C /opt \
 && ln -sf "/opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/cmake" /usr/bin/cmake \
 && rm /tmp/cmake.tar.gz

WORKDIR /work
