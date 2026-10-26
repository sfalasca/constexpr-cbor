# Reproducible benchmark environment: clones pinned versions of tinycbor
# and QCBOR, builds all three libraries' decoders plus this project's
# own, and runs the CPU/memory benchmark, the cross-library correctness
# corpus, and the code-size comparison in one shot. See README.md for
# what each program actually measures and why.
FROM ubuntu:24.04

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    build-essential binutils ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

COPY include /work/include
COPY benchmarks /work/benchmarks
WORKDIR /work/benchmarks

# Pinned so results are reproducible across runs, not whatever HEAD
# happens to be the day someone rebuilds this image.
RUN git clone --quiet --depth 1 --branch v0.6.1 https://github.com/intel/tinycbor.git tinycbor
RUN git clone --quiet --depth 1 --branch v1.5.1 https://github.com/laurencelundblade/QCBOR.git qcbor

RUN gcc -std=c11 -O2 -Itinycbor/src -c tinycbor/src/cborparser.c -o tinycbor_parser.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/qcbor_decode.c -o qcbor_decode.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/UsefulBuf.c -o usefulbuf.o && \
    gcc -std=c11 -O2 -Iqcbor/inc -c qcbor/src/ieee754.c -o ieee754.o

RUN g++ -std=c++20 -O2 -I../include -Itinycbor/src -Iqcbor/inc \
    bench.cpp tinycbor_parser.o qcbor_decode.o usefulbuf.o ieee754.o -o bench
RUN g++ -std=c++20 -O2 -I../include -Itinycbor/src -Iqcbor/inc \
    correctness.cpp tinycbor_parser.o qcbor_decode.o usefulbuf.o ieee754.o -o correctness

# Code-size comparison: dead-code-eliminated (-ffunction-sections +
# --gc-sections) single-entry-point executable per library, so each
# binary's .text reflects only what calling its decoder once actually
# pulls in, not a whole translation unit's worth of unused functions.
RUN g++ -std=c++20 -Os -ffunction-sections -fdata-sections -I../include -c size/size_cc.cpp -o size_cc.o && \
    g++ -std=c++20 -Os -ffunction-sections -fdata-sections -I../include -c size/size_arena.cpp -o size_arena.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Itinycbor/src -c size/size_tc.c -o size_tc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Itinycbor/src -c tinycbor/src/cborparser.c -o tp_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c size/size_qc.c -o size_qc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/qcbor_decode.c -o qd_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/UsefulBuf.c -o ub_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -Iqcbor/inc -c qcbor/src/ieee754.c -o ie_gc.o && \
    gcc -std=c11 -Os -ffunction-sections -fdata-sections -c size/size_main.c -o size_main.o && \
    g++ -Os -Wl,--gc-sections size_main.o size_cc.o -o exe_cc && \
    g++ -Os -Wl,--gc-sections size_main.o size_arena.o -o exe_arena && \
    gcc -Os -Wl,--gc-sections size_main.o size_tc.o tp_gc.o -o exe_tc && \
    gcc -Os -Wl,--gc-sections size_main.o size_qc.o qd_gc.o ub_gc.o ie_gc.o -o exe_qc -lm

CMD ["bash", "-c", "echo '=== CPU performance + memory ===' && ./bench && echo && echo '=== correctness corpus ===' && ./correctness && echo && echo '=== code size (single decode entry point, dead-code-eliminated) ===' && echo constexpr-cbor_heap: && size exe_cc && echo constexpr-cbor_arena: && size exe_arena && echo tinycbor: && size exe_tc && echo qcbor: && size exe_qc"]
