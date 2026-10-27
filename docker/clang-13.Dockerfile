# syntax=docker/dockerfile:1
# Compiler matrix: Clang 13 (silkeh/clang, Debian)
#
# Standalone image: `docker build -f docker/clang-13.Dockerfile -t cbor-clang13 .`
# from the repository root. Configures, builds, and runs the full ctest
# suite against libc++, bundled at a matching version by this base image
# rather than the distro's system libstdc++, which lags behind and is
# tied to the Debian release instead of the Clang version under test.
# CMAKE_CXX_STANDARD=23 is a *request*: CMake caps it to whatever this
# Clang actually supports (pre-17 lacks the "c++23" flag spelling
# entirely; CMake falls back to "c++2b" or lower as needed).
FROM silkeh/clang:13

# This base image is on Debian bullseye, whose deb.debian.org mirror
# has aged out of the regular archive (404s on every package as of
# late 2026). Point apt at the frozen snapshot.debian.org mirror
# instead, with the expiry check disabled since that snapshot is
# necessarily older than "now" by design.
RUN sed -i 's|^deb http://deb.debian.org|# deb http://deb.debian.org|; s|^# deb http://snapshot.debian.org|deb http://snapshot.debian.org|' /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends git python3-pip ninja-build libc++-13-dev libc++abi-13-dev \
    && rm -rf /var/lib/apt/lists/* \
    && (pip3 install --no-cache-dir --break-system-packages cmake \
        || pip3 install --no-cache-dir cmake)

WORKDIR /src
COPY . .

ENV CXXFLAGS="-stdlib=libc++"
ENV LDFLAGS="-stdlib=libc++ -lc++abi"

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
