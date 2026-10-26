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

RUN emcmake cmake -S . -B build -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CROSSCOMPILING_EMULATOR=node \
    && emmake cmake --build build -j \
    && cd build && ctest --output-on-failure
