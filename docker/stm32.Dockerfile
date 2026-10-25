# syntax=docker/dockerfile:1
# Embedded target: STM32 (ARM Cortex-M4, e.g. STM32F4-class parts), via
# the apt-installable GNU Arm Embedded toolchain (arm-none-eabi-g++).
#
# Header-only library: this compiles (-c, no link) rather than building a
# flashable firmware image. A real firmware build needs a linker script,
# startup code, and a vendor HAL/BSP (e.g. STM32Cube), none of which is
# this library's concern; -c is the standard way to validate a header
# against a cross-compiler.
#
# Bare-metal Cortex-M firmware conventionally disables C++ exceptions
# and RTTI to save flash, so this is exactly the -fno-exceptions path
# exercised elsewhere in the matrix (see include/constexpr_cbor.hpp's
# error-reporting path).
#
# Deliberately NOT building with -ffreestanding: tried it first (bare-metal
# firmware often does), and it broke this toolchain's own bundled
# libstdc++ (arm-none-eabi-g++ 13.2.1's newlib build) -- std::vector's
# _Vector_impl_data::swap referenced _M_start/_M_finish/_M_end_of_storage
# members that don't exist under -ffreestanding on this specific
# toolchain/version, a libstdc++ bug exposed by that flag, not something
# this library's code did. Confirmed by removing just that one flag with
# everything else held constant: compiles clean either way. Since this
# build only compile-checks a header against the cross-compiler rather
# than linking real firmware, -ffreestanding was never load-bearing for
# that purpose here, just an untested assumption carried over from "this
# is what firmware does."
#
# Build: `docker build -f docker/stm32.Dockerfile -t cbor-stm32 .`
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libnewlib-dev \
        libstdc++-arm-none-eabi-dev libstdc++-arm-none-eabi-newlib \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY include/ include/
COPY tests/noexcept_tests.cpp tests/noexcept_tests.cpp

# cortex-m4/hard-float/fpv4-sp-d16: the common STM32F4 configuration.
RUN arm-none-eabi-g++ --version \
    && arm-none-eabi-g++ -std=c++23 \
         -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
         -fno-exceptions -fno-rtti \
         -Wall -Wextra -Wpedantic \
         -Iinclude -c tests/noexcept_tests.cpp -o /tmp/noexcept_tests.stm32.o
