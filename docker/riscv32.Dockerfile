# syntax=docker/dockerfile:1
# Embedded target: bare-metal RISC-V (rv32imac/ilp32 — e.g. CH32V,
# GD32VF103-class MCUs), via the xPack prebuilt riscv-none-elf-gcc.
#
# Ubuntu's apt-installable gcc-riscv64-unknown-elf package ships no C++
# standard library at all (not even <cstddef> resolves) — it's a bare C
# cross-compiler despite having a g++ binary. The xPack prebuilt tarball
# (built from real GCC releases with newlib + libstdc++) does provide one
# and is what this Dockerfile uses instead.
#
# Header-only library: this compiles (-c, no link) rather than producing
# a flashable image. Linking real firmware needs a linker script, startup
# code, and vendor HAL/BSP, none of which is this library's concern; -c
# is the standard way to validate a header against a cross-compiler.
#
# Compiles every test file in tests/ (not just noexcept_tests.cpp), the
# same scope as the desktop matrix's default `cmake --build` step --
# excluding tests/expected_failures/ (those are *supposed* to fail; a
# glob over tests/*.cpp doesn't descend into that subdirectory, so they're
# excluded structurally, not by an extra filter). Needs compile_time_ut,
# which the desktop matrix gets via CMake FetchContent; this image clones
# it directly at the same pinned tag CMakeLists.txt uses, since there's
# no CMake here.
#
# Build: `docker build -f docker/riscv32.Dockerfile -t cbor-riscv32 .`
FROM ubuntu:24.04

ARG XPACK_RISCV_VERSION=15.2.0-1

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl git xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch v1.0.1 \
    https://github.com/sfalasca/compile-time-unit-testing.git /tmp/ctut

RUN curl -fsSL \
      "https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${XPACK_RISCV_VERSION}/xpack-riscv-none-elf-gcc-${XPACK_RISCV_VERSION}-linux-x64.tar.gz" \
      -o /tmp/riscv-gcc.tar.gz \
    && mkdir -p /opt/riscv-none-elf-gcc \
    && tar xzf /tmp/riscv-gcc.tar.gz -C /opt/riscv-none-elf-gcc --strip-components=1 \
    && rm /tmp/riscv-gcc.tar.gz
ENV PATH="/opt/riscv-none-elf-gcc/bin:${PATH}"

WORKDIR /src
COPY include/ include/
COPY tests/ tests/

# rv32imac/ilp32: base integer + multiply/divide + atomics + compressed
# instructions, the common baseline for real rv32 MCUs.
RUN riscv-none-elf-g++ --version \
    && for f in tests/*.cpp; do \
         echo "=== compiling $f ==="; \
         riscv-none-elf-g++ -std=c++23 -march=rv32imac -mabi=ilp32 \
             -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic \
             -Iinclude -I/tmp/ctut/include \
             -c "$f" -o "/tmp/$(basename "$f" .cpp).riscv32.o" || exit 1; \
       done
