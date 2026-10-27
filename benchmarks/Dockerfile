# Reproducible benchmark environment: clones pinned versions of tinycbor,
# QCBOR, and libcbor, builds all four libraries' decoders plus this
# project's own, and runs the CPU/memory benchmark, the cross-library
# correctness corpus, and the code-size comparison in one shot. See
# README.md for what each program actually measures and why.
FROM ubuntu:24.04

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    build-essential binutils ca-certificates git cmake \
    && rm -rf /var/lib/apt/lists/*

COPY include /work/include
COPY benchmarks /work/benchmarks
WORKDIR /work/benchmarks

# Pinned so results are reproducible across runs, not whatever HEAD
# happens to be the day someone rebuilds this image.
RUN git clone --quiet --depth 1 --branch v0.6.1 https://github.com/intel/tinycbor.git tinycbor
RUN git clone --quiet --depth 1 --branch v1.5.1 https://github.com/laurencelundblade/QCBOR.git qcbor
RUN git clone --quiet --depth 1 --branch v0.14.0 https://github.com/PJK/libcbor.git libcbor

# libcbor, unlike tinycbor/QCBOR, needs a real build step: its CMakeLists
# generates a configuration.h and an export-macro header that don't exist
# in the raw source tree. Static lib only, no examples/tests -- this one
# build is reused for both the CPU/memory/correctness programs below and
# the code-size comparison (unlike tinycbor/QCBOR, which get a *second*,
# -Os-specific compile of their .c files for the size comparison -- see
# benchmarks/README.md's "NoAlloc internals"-adjacent methodology notes for
# why libcbor's size number carries this one asymmetry honestly rather
# than silently matching the others' rebuild-at--Os treatment).
RUN cmake -S libcbor -B libcbor/build -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DWITH_EXAMPLES=OFF >/dev/null && \
    cmake --build libcbor/build --target cbor -j

RUN gcc -std=c11 -O2 -Itinycbor/src -c tinycbor/src/cborparser.c -o tinycbor_parser.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/qcbor_decode.c -o qcbor_decode.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/qcbor_encode.c -o qcbor_encode.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/UsefulBuf.c -o usefulbuf.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/ieee754.c -o ieee754.o

# libcbor_adapter.cpp is the *only* translation unit that includes
# libcbor's own header -- it declares several C functions under the same
# names tinycbor uses for different purposes (cbor_encode_uint and
# friends), a genuine extern "C" symbol collision if both ever appear in
# one TU. bench.cpp/correctness.cpp only ever see libcbor_adapter.hpp.
RUN g++ -std=c++20 -O2 -I../include -Ilibcbor/src -Ilibcbor/build/src -Ilibcbor/build \
    -c libcbor_adapter.cpp -o libcbor_adapter.o

RUN g++ -std=c++20 -O2 -I../include -Itinycbor/src -Iqcbor/inc \
    bench.cpp tinycbor_parser.o qcbor_decode.o qcbor_encode.o usefulbuf.o ieee754.o \
    libcbor_adapter.o libcbor/build/src/libcbor.a -o bench
RUN g++ -std=c++20 -O2 -I../include -Itinycbor/src -Iqcbor/inc \
    correctness.cpp tinycbor_parser.o qcbor_decode.o qcbor_encode.o usefulbuf.o ieee754.o \
    libcbor_adapter.o libcbor/build/src/libcbor.a -o correctness

# tinycbor's *encoder* (cborencoder.c, as opposed to the decoder above)
# cannot be linked into the same executable as libcbor at all: both
# export a couple of identically-named C functions for unrelated purposes
# (cbor_encode_uint, cbor_encode_tag) -- a genuine object-level symbol
# collision, confirmed by an actual link attempt, that only ever became
# live once tinycbor's encoder needed linking alongside libcbor (the
# decoder above shares no such names). See bench_tinycbor_encode.cpp's
# own comment for why this is a separate program rather than another
# translation-unit split.
RUN gcc -std=c11 -O2 -Itinycbor/src -c tinycbor/src/cborencoder.c -o tinycbor_encoder.o && \
    g++ -std=c++20 -O2 -I../include -Itinycbor/src \
    bench_tinycbor_encode.cpp tinycbor_encoder.o -o bench_tinycbor_encode

# Code-size comparison: dead-code-eliminated (-ffunction-sections +
# --gc-sections) single-entry-point executable per library, so each
# binary's .text reflects only what calling its decoder once actually
# pulls in, not a whole translation unit's worth of unused functions.
RUN g++ -std=c++20 -Os -ffunction-sections -fdata-sections -I../include -c size/size_cc.cpp -o size_cc.o && \
    g++ -std=c++20 -Os -ffunction-sections -fdata-sections -I../include -c size/size_noalloc.cpp -o size_noalloc.o && \
    g++ -std=c++20 -Os -ffunction-sections -fdata-sections -I../include -c size/size_sax.cpp -o size_sax.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Itinycbor/src -c size/size_tc.c -o size_tc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Itinycbor/src -c tinycbor/src/cborparser.c -o tp_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c size/size_qc.c -o size_qc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/qcbor_decode.c -o qd_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/UsefulBuf.c -o ub_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/ieee754.c -o ie_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Ilibcbor/src -Ilibcbor/build/src -Ilibcbor/build \
        -c size/size_libcbor.c -o size_libcbor.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -c size/size_main.c -o size_main.o && \
    g++ -Os -Wl,--gc-sections size_main.o size_cc.o -o exe_cc && \
    g++ -Os -Wl,--gc-sections size_main.o size_noalloc.o -o exe_noalloc && \
    g++ -Os -Wl,--gc-sections size_main.o size_sax.o -o exe_sax && \
    gcc -Os -Wl,--gc-sections size_main.o size_tc.o tp_gc.o -o exe_tc && \
    gcc -Os -Wl,--gc-sections size_main.o size_qc.o qd_gc.o ub_gc.o ie_gc.o -o exe_qc -lm && \
    gcc -Os -Wl,--gc-sections size_main.o size_libcbor.o libcbor/build/src/libcbor.a -o exe_libcbor -lm

CMD ["bash", "-c", "echo '=== CPU performance + memory ===' && ./bench && echo && echo '=== tinycbor encode (separate program -- see bench_tinycbor_encode.cpp) ===' && ./bench_tinycbor_encode && echo && echo '=== correctness corpus ===' && ./correctness && echo && echo '=== code size (single decode entry point, dead-code-eliminated) ===' && echo constexpr-cbor_heap: && size exe_cc && echo constexpr-cbor_noalloc: && size exe_noalloc && echo constexpr-cbor_sax: && size exe_sax && echo tinycbor: && size exe_tc && echo qcbor: && size exe_qc && echo libcbor: && size exe_libcbor"]
