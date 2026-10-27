# syntax=docker/dockerfile:1
# WebAssembly target, via the official Emscripten SDK image. Unlike the
# bare-metal embedded targets (esp32/stm32/riscv32), WASM is a fully
# capable target that can link AND run: `emmake ctest` executes each
# compiled .wasm test binary under Node (CMAKE_CROSSCOMPILING_EMULATOR),
# the same full-suite proof the desktop gcc-*/clang-* images give, not
# just a compile-check.
#
# Standalone image: `docker build -f docker/wasm.Dockerfile -t cbor-wasm .`
# from the repository root.
#
# Emscripten's compiler is Clang-based, and wasm32's std::size_t is
# 32 bits wide while std::uint64_t stays 64 -- this is what first caught
# the uint64_t->size_t narrowing bug `ConstexprCbor::detail::narrow<>()`
# exists to guard (see its definition in constexpr_cbor.hpp): a target
# this project's desktop/embedded matrix hadn't exercised before, since
# every embedded cross-compiler here is GCC-based and GCC's own
# -Wconversion doesn't flag that particular narrowing the way Clang's
# -Wshorten-64-to-32 does.
FROM emscripten/emsdk:latest

WORKDIR /src
COPY . .

# Built and tested at both C++20 (this project's documented floor) and
# C++23 explicitly -- see the gcc-*/clang-* Dockerfiles' own comment on
# this same two-pass pattern for why (ConstexprCbor::Expected<T, E>
# resolves to a fallback at C++20, real std::expected at C++23).
RUN emcmake cmake -S . -B build-cxx20 -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CROSSCOMPILING_EMULATOR=node \
    && emmake cmake --build build-cxx20 -j \
    && cd build-cxx20 && ctest --output-on-failure && cd ..
RUN emcmake cmake -S . -B build-cxx23 -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CROSSCOMPILING_EMULATOR=node \
    && emmake cmake --build build-cxx23 -j \
    && cd build-cxx23 && ctest --output-on-failure
