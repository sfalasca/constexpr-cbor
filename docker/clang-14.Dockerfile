# syntax=docker/dockerfile:1
# Compiler matrix: Clang 14 (silkeh/clang, Debian)
#
# Standalone image: `docker build -f docker/clang-14.Dockerfile -t cbor-clang14 .`
# from the repository root. Configures, builds, and runs the full ctest
# suite against libc++, bundled at a matching version by this base image
# rather than the distro's system libstdc++, which lags behind and is
# tied to the Debian release instead of the Clang version under test.
# CMAKE_CXX_STANDARD=23 is a *request*: CMake caps it to whatever this
# Clang actually supports (pre-17 lacks the "c++23" flag spelling
# entirely; CMake falls back to "c++2b" or lower as needed).
FROM silkeh/clang:14

# This base image is on Debian bullseye, whose deb.debian.org mirror
# has aged out of the regular archive (404s on every package as of
# late 2026). Point apt at the frozen snapshot.debian.org mirror
# instead, with the expiry check disabled since that snapshot is
# necessarily older than "now" by design.
RUN sed -i 's|^deb http://deb.debian.org|# deb http://deb.debian.org|; s|^# deb http://snapshot.debian.org|deb http://snapshot.debian.org|' /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends git python3-pip ninja-build libc++-14-dev libc++abi-14-dev \
    && rm -rf /var/lib/apt/lists/* \
    && (pip3 install --no-cache-dir --break-system-packages cmake \
        || pip3 install --no-cache-dir cmake)

WORKDIR /src
COPY . .

ENV CXXFLAGS="-stdlib=libc++"
ENV LDFLAGS="-stdlib=libc++ -lc++abi"

RUN cmake -S . -B build -G Ninja -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build -j \
    && ctest --test-dir build --output-on-failure
