# constexpr-cbor

[![CI](https://github.com/sfalasca/constexpr-cbor/workflows/CI/badge.svg)](https://github.com/sfalasca/constexpr-cbor/actions/workflows/ci.yml)

[Project page](https://falasca.engineering/constexpr-cbor/)

A from-scratch, constexpr/consteval-evaluable C++20 CBOR (RFC 8949)
decoder and encoder. Every official RFC 8949 Appendix A test vector
round-trips inside a `consteval` function, so a passing build is the
conformance proof, and out-of-bounds/narrowing-UB inputs become hard
compile errors rather than runtime bugs.

## Requirements

- **C++20 minimum.** `target_compile_features(constexpr_cbor INTERFACE cxx_std_20)` in this repo's `CMakeLists.txt`.
- Depends on [`compile_time_ut`](https://github.com/sfalasca/compile-time-unit-testing) for its compile-time test assertions.

## Adding this to your project

The recommended way to consume this library is CMake's `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    constexpr_cbor
    GIT_REPOSITORY https://github.com/sfalasca/constexpr-cbor.git
    GIT_TAG        v1.0.1  # pin to a released tag, or a commit of your choice
)
FetchContent_MakeAvailable(constexpr_cbor)

target_link_libraries(your_target PRIVATE constexpr_cbor)
```

`constexpr_cbor` is an `INTERFACE` library, header-only with nothing to
build or install, so that's the entire integration: no
`install()`/`find_package()` step, no prebuilt artifact to manage, and
it pins to a specific commit like any other dependency. If you'd rather
not take a CMake dependency at all, vendoring `include/constexpr_cbor.hpp`
directly and adding it to your own include path works just as well.

## Quick start

```cpp
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: [1, 2, 3] -> 0x83010203
constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};

consteval bool round_trips() {
    const auto decoded = decode_value(bytes);
    if (!decoded) return false;
    const auto& items = std::get<std::vector<CborValue>>(decoded->first.data);
    if (items.size() != 3 || std::get<std::uint64_t>(items[0].data) != 1) return false;
    const auto reencoded = encode_value(decoded->first);
    return std::equal(reencoded.begin(), reencoded.end(), bytes);
}

static_assert(round_trips());   // fails the build if the round-trip breaks
```

`decode_value` returns `std::optional<std::pair<CborValue, std::size_t>>` (the decoded
value and how many bytes it consumed); a refused/malformed input is `std::nullopt`, not
a thrown exception or an out-of-bounds read. `encode_value` always emits the
shortest ("preferred"/canonical) encoding for a given value, which is what makes
byte-for-byte round-trip comparison against RFC 8949's own vectors meaningful.

## RFC 8949 Appendix A vectors as compile-time proof

Every CBOR major type (unsigned/negative integers, byte and text strings, arrays,
maps, tags, and the floats/booleans/null family), including the indefinite-length
(chunked, break-terminated) forms, is tested against RFC 8949 Appendix A's own
worked examples, fetched directly from the RFC text rather than reconstructed by
hand. Each one decodes inside a `consteval` function and is asserted against the
exact documented value; most are also re-encoded and compared byte-for-byte against
the original vector, since Appendix A's examples are already in canonical form.
See `tests/` for the full suite, one file per major type.

## Pluggable UTF-8 validation policies

RFC 8949 §5.3.1 explicitly leaves UTF-8 validation of text strings up to the
decoder — "a decoder might or might not want to verify" it. `decode_text_string`
takes that choice as a template parameter rather than hard-coding one answer:

- `NoUtf8Validation` (the default) — skip the check entirely.
- `RoughUtf8Validation` — structural shape only (right continuation-byte count,
  not truncated); doesn't catch overlong encodings or surrogate halves.
- `CompleteUtf8Validation` — full RFC 3629 conformance: also rejects overlong
  encodings (RFC 8949's own example of well-formed-but-invalid CBOR, `0x62c0ae`,
  fails this one), lone surrogate-half code points, and anything past U+10FFFF.

Each policy is checked against a real `<type_traits>`-based interface contract
(`is_utf8_validation_policy_v`), not just a naming convention — a policy with the
wrong method shape fails to compile with a named error, not a silent SFINAE dead end.

## Pluggable duplicate-map-key policies

RFC 8949 leaves duplicate map keys implementation-defined, and there's no settled
industry norm (checked against several real-world CBOR decoders before deciding
this). `decode_value`'s `KeyPolicy` template parameter is the same pattern as the
UTF-8 policy above:

- `RejectDuplicateKeys` (the default) — any repeated key fails the whole map's decode.
- `KeepFirstDuplicateKey` — first occurrence wins.
- `KeepLastDuplicateKey` — last occurrence wins (overwrite semantics).

## Caller-tunable nesting depth

Unbounded recursive nesting in arrays, maps, and tags is the single most common
hazard class across real-world CBOR decoders (stack overflow and related CVEs
turn up repeatedly). `decode_value<MaxDepth, ...>`'s `MaxDepth` (default `32`)
bounds it; exceeding it is a refusal, not a crash, the same as every other
contract violation in this codec.

## Building and testing this library

```sh
cmake -S . -B build -DCMAKE_CXX_STANDARD=23   # optional; the library only requires C++20
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMAKE_CXX_STANDARD` is a request, not a requirement: CMake caps it to
whatever the compiler actually supports (down to GCC 10, which gets
silently capped to `gnu++2a`).

Static analysis: `cmake -DCBOR_ENABLE_CLANG_TIDY=ON -DCBOR_ENABLE_CPPCHECK=ON ..` (both opt-in, auto-skip with a warning if the tool isn't installed), plus an always-registered `cmake --build build --target static_analysis` running the Clang Static Analyzer (`scan-build`) over the whole build.

Compiler/platform matrix: see [`docker/README.md`](docker/README.md). `docker/build-matrix.sh` builds every Dockerfile in `docker/` and reports pass/fail.

Planned future work: a runtime fuzzing harness as this library's
complement — compile-time testing only proves the finite set of cases
actually exercised in a `consteval` function; fuzzing covers the
unbounded runtime input space that `static_assert` can't reach.

## Author

Written by [Stefano Falasca](https://falasca.engineering), an embedded software consultant
focused on safety-critical systems and agent-ready verification.

## License

MIT — see [`LICENSE`](LICENSE).
