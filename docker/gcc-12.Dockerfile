# syntax=docker/dockerfile:1
# Compiler matrix: GCC 12 (bookworm)
#
# Standalone image: `docker build -f docker/gcc-12.Dockerfile -t cbor-gcc12 .`
# from the repository root. Configures, builds, and runs the full ctest
# suite. CMAKE_CXX_STANDARD=23 is a *request*: CMake caps it to whatever
# this GCC actually supports.
FROM gcc:12

# The base image's apt-provided cmake is frequently older than this
# project's cmake_minimum_required(3.20); install a current one via pip
# instead. --break-system-packages is needed on newer (PEP 668) pip and
# unrecognized by older pip, hence the fallback.
RUN apt-get update && apt-get install -y --no-install-recommends git python3-pip ninja-build \
    && rm -rf /var/lib/apt/lists/* \
    && (pip3 install --no-cache-dir --break-system-packages cmake \
        || pip3 install --no-cache-dir cmake)

WORKDIR /src
COPY . .

# Built and tested at both C++20 (this project's documented floor) and
# C++23 explicitly, into separate build directories -- not just a single
# "request 23, let CMake cap it down" pass. The two differ in one real
# way: ConstexprCbor::Expected<T, E> resolves to a small, variant-backed
# fallback at C++20 (std::expected is gated to C++23 in libstdc++) and to
# real std::expected at C++23 -- this is the only way to prove the
# fallback actually works against a real toolchain, not just locally.
RUN cmake -S . -B build-cxx20 -G Ninja -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build-cxx20 -j \
    && ctest --test-dir build-cxx20 --output-on-failure
RUN cmake -S . -B build-cxx23 -G Ninja -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build-cxx23 -j \
    && ctest --test-dir build-cxx23 --output-on-failure
