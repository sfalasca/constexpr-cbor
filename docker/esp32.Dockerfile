# syntax=docker/dockerfile:1
# Embedded target: ESP32 (Xtensa), via the official Espressif ESP-IDF
# toolchain image (which bundles xtensa-esp32-elf-g++).
#
# Header-only library: this compiles (-c, no link) rather than building a
# full flashable ESP-IDF app image. A real firmware build needs an
# idf.py project (sdkconfig, component registration, partition table),
# none of which is this library's concern; -c is the standard way to
# validate a header against a cross-compiler.
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
# ESP-IDF firmware conventionally disables C++ exceptions
# (CONFIG_COMPILER_CXX_EXCEPTIONS is off by default) to save flash, so
# this is exactly the -fno-exceptions path exercised elsewhere in the
# matrix (see include/constexpr_cbor.hpp's error-reporting path).
#
# Build: `docker build -f docker/esp32.Dockerfile -t cbor-esp32 .`
FROM espressif/idf:release-v5.4

SHELL ["/bin/bash", "-c"]
WORKDIR /src
COPY include/ include/
COPY tests/ tests/

RUN git clone --depth 1 --branch v1.0.1 \
    https://github.com/sfalasca/compile-time-unit-testing.git /tmp/ctut

RUN . /opt/esp/idf/export.sh > /dev/null \
    && xtensa-esp32-elf-g++ --version \
    && for f in tests/*.cpp; do \
         echo "=== compiling $f ==="; \
         xtensa-esp32-elf-g++ -std=c++23 -fno-exceptions -fno-rtti \
             -Wall -Wextra -Wpedantic \
             -Iinclude -I/tmp/ctut/include \
             -c "$f" -o "/tmp/$(basename "$f" .cpp).esp32.o" || exit 1; \
       done
