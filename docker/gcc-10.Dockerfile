# syntax=docker/dockerfile:1
# Compiler matrix: GCC 10 (buster)
#
# Standalone image: `docker build -f docker/gcc-10.Dockerfile -t cbor-gcc10 .`
# from the repository root. Configures, builds, and runs the full ctest
# suite. CMAKE_CXX_STANDARD=23 is a *request*: CMake caps it to whatever
# this GCC actually supports.
FROM gcc:10

# The base image's apt-provided cmake is frequently older than this
# project's cmake_minimum_required(3.20); install a current one via pip
# instead. --break-system-packages is needed on newer (PEP 668) pip and
# unrecognized by older pip, hence the fallback.
# This base image is on Debian bullseye, whose deb.debian.org mirror
# has aged out of the regular archive (404s on every package as of
# late 2026). Point apt at the frozen snapshot.debian.org mirror
# instead, with the expiry check disabled since that snapshot is
# necessarily older than "now" by design.
RUN sed -i 's|^deb http://deb.debian.org|# deb http://deb.debian.org|; s|^# deb http://snapshot.debian.org|deb http://snapshot.debian.org|' /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends git python3-pip ninja-build \
    && rm -rf /var/lib/apt/lists/* \
    && (pip3 install --no-cache-dir --break-system-packages cmake \
        || pip3 install --no-cache-dir cmake)

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build -j \
    && ctest --test-dir build --output-on-failure
