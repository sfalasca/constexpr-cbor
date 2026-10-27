# constexpr-cbor

[![CI](https://github.com/sfalasca/constexpr-cbor/workflows/CI/badge.svg)](https://github.com/sfalasca/constexpr-cbor/actions/workflows/ci.yml)

[Project page](https://falasca.engineering/constexpr-cbor/)

A from-scratch, `constexpr`/`consteval`-evaluable C++20 CBOR (RFC 8949) decoder and encoder.
Every official RFC 8949 Appendix A test vector round-trips inside a `consteval` function, so a passing build is the conformance proof: out-of-bounds reads and narrowing-conversion UB on a given input become compile errors, not runtime bugs.

## Requirements

- C++20 minimum.
- GCC 13+ or Clang 19+. `CborValue` is a recursive `std::variant`/`std::vector`, which only works as a literal type on a standard library with full C++20 constexpr-container support. Clang 15–18 (libc++) fail to compile it; Clang 19+ and GCC 13+ (libstdc++) both work. The `docker/` matrix is the up-to-date source of truth for this.
- [`compile_time_ut`](https://github.com/sfalasca/compile-time-unit-testing) for the test suite's compile-time assertions.
- Very deep nesting can hit the compiler's own `-fconstexpr-depth`/`-fconstexpr-steps` ceiling before this library's own `MaxDepth` bound does. Raise the compiler flag if you need to prove a deeper structure than the default allows.

## Adding this to your project

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

It's an `INTERFACE` library: header-only, nothing to build or install. No CMake? Vendor `include/constexpr_cbor.hpp` directly.

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

`decode_value` returns `Expected<std::pair<CborValue, std::size_t>, Error>` (the decoded value, and how many bytes it consumed). A refusal is `unexpected(Error{...})`, never an exception or an out-of-bounds read. `Error` is `{ErrorReason reason; std::size_t offset;}`, so a caller can tell why and where a decode failed instead of getting a bare "no."

`Expected<T, E>` behaves like `std::optional<T>` (`operator bool`/`operator*`/`operator->`/`.value_or()`) plus `.error()`. `encode_value` always emits the canonical (shortest) encoding, which is what makes byte-for-byte comparison against RFC 8949's own vectors meaningful.

## Why `Expected`, not `std::optional` or `std::expected` directly

A bare `std::nullopt` discards *why* a decode failed. `std::expected<T, E>` is the obvious fix, but it's gated to C++23 in libstdc++ (`-std=c++20` rejects it outright), and this library's documented floor is C++20. `ConstexprCbor::Expected<T, E>` resolves that: it's real `std::expected<T, E>` when the compiler's standard library has it (feature-detected via `__cpp_lib_expected`), and a small `std::variant`-backed fallback covering the same operations otherwise. One alias, one call-site spelling either way; `docker/README.md`'s compiler matrix builds and tests both backends explicitly.

## RFC 8949 Appendix A vectors as compile-time proof

Every major type, including indefinite-length (chunked) forms, is tested against RFC 8949 Appendix A's own worked examples, decoded inside `consteval` and asserted against the documented value. Most are also re-encoded and compared byte-for-byte against the original. All 81 distinct vectors in Appendix A's Table 6 are covered.

Beyond the RFC's own examples, `tests/test_malformed_input.cpp` and a few per-type test files draw from [`cbor-wg/cbor-test-vectors`](https://github.com/cbor-wg/cbor-test-vectors) and QCBOR's `not_well_formed_cbor.h`, with a provenance comment at each use site. See `tests/` for the full suite, one file per major type.

Two scope decisions worth knowing: this library checks CBOR **well-formedness** (byte structure), not **tag content validity** (e.g. it won't verify a tag-0 date-time string is a valid date-time). And `NaN` is always canonicalized to its shortest form on re-encode, so a decode/re-encode round-trip doesn't preserve the original payload bits or signaling/quiet distinction.

## Pluggable UTF-8 validation policies

RFC 8949 §5.3.1 leaves UTF-8 validation of text strings up to the decoder. `decode_text_string` takes that as a template parameter:

| Policy | Behavior |
|---|---|
| `NoUtf8Validation` (default) | Skip the check entirely. |
| `RoughUtf8Validation` | Structural shape only — right continuation-byte count, no truncation check. Doesn't catch overlong encodings or surrogate halves. |
| `CompleteUtf8Validation` | Full RFC 3629 conformance: rejects overlong encodings, lone surrogate halves, and anything past U+10FFFF. |
| `AsciiOnlyValidation` | Not a stricter UTF-8 check — a different thing. ASCII is a subset of UTF-8, so this rejects valid multi-byte UTF-8 (`"café"` fails). For protocol fields that genuinely can't carry multi-byte sequences. |

A policy's shape is checked via `is_utf8_validation_policy_v`; the wrong shape is a named compile error, not a silent SFINAE dead end.

## Pluggable duplicate-map-key policies

RFC 8949 leaves duplicate map keys implementation-defined. `decode_value`'s `KeyPolicy` parameter works the same way:

- `RejectDuplicateKeys` (default) — any repeated key fails the whole map's decode.
- `KeepFirstDuplicateKey` — first occurrence wins.
- `KeepLastDuplicateKey` — last occurrence wins.

## No-malloc decoding/encoding (`ConstexprCbor::NoAlloc`)

The default API above uses `std::vector`, i.e. a heap. `ConstexprCbor::NoAlloc` (`include/constexpr_cbor_noalloc.hpp`) is the no-malloc counterpart for environments without one: every decoded value is a `Node` living in storage the *caller* owns and sizes.

```cpp
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;

constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

consteval bool round_trips() {
    NoAlloc::NoAllocStorage<16, 16, 16> storage;     // 16 nodes, 16 ref slots, 16 content bytes
    auto pool = storage.make_pool();
    const auto decoded = NoAlloc::decode_value(pool, bytes);
    if (!decoded) return false;
    const auto& items = std::get<NoAlloc::ArrayRef>(decoded->node->data);
    if (items.count != 3) return false;
    std::array<std::uint8_t, 16> out{};
    const auto written = NoAlloc::encode_value(pool, decoded->node, out);
    return written && std::equal(out.begin(), out.begin() + *written, bytes);
}

static_assert(round_trips());
```

The two headers are independent: include just `constexpr_cbor.hpp` for the heap API, just `constexpr_cbor_noalloc.hpp` for the no-malloc one (it pulls in the shared, non-allocating primitives itself). Every symbol is a template, so including one costs nothing if you only use the other.

What's different from the heap API, in short:

- A `Node` holds children by pointer into caller-owned storage, not by value — `std::array` can't hold an incomplete type the way `std::vector` can.
- Running out of `MaxNodes`/`MaxRefs`/`MaxBytes` is a refusal (`Error{NodeCapacityExhausted/RefCapacityExhausted/ByteCapacityExhausted, ...}`), never a crash or silent truncation. Sizing `NoAllocStorage` is the caller's job.
- `NoAlloc::encode_value` writes into a caller-supplied `std::span`, returning bytes written or `Error{OutputBufferTooSmall, ...}`.
- A CBOR length field (up to 2^64-1) is range-checked before any cast to `std::size_t`, which can be 32-bit on embedded targets — `checked_node_count` refuses rather than silently truncating a huge count into a small one (the shape of libcbor's historical PR #387 bug).
- `NodePool::owns(const Node*)` defensively checks, via address-range comparison, that a `Node*` crossing back into the library actually belongs to this pool. It catches a stray or foreign pointer; it can't catch the pool's own backing storage having moved (a precondition the caller upholds, like any other dangling-pointer risk in C++).

The full rationale for each of these lives as comments next to the code in `include/constexpr_cbor_noalloc.hpp`. See `tests/test_noalloc.cpp` for the suite.

## Event-based SAX decoding (`ConstexprCbor::Sax`)

A third decode strategy: `ConstexprCbor::Sax::SaxRange` (`include/constexpr_cbor_sax.hpp`) builds no value at all. It pulls one CBOR *event* at a time directly off the wire (`BeginArray`, `Uint`, `TextString`, `EndMap`, ...) — the same shape tinycbor's and QCBOR's own pull-style APIs expose — reusing this library's byte-level primitives unchanged.

```cpp
#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Sax;

constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

consteval bool sums_to_six() {
    std::uint64_t sum = 0;
    for (auto ev : SaxRange<32>{bytes}) {
        if (ev.kind == EventKind::Error) return false;
        if (ev.kind == EventKind::Uint) sum += ev.uint_value;
    }
    return sum == 6;
}

static_assert(sums_to_six());
```

An `Error`-kind event's `.error` field carries the same `Error` type the heap and `NoAlloc` decoders report. `SaxRange` is a real `std::ranges::forward_range` (every byte of iteration state lives by value in the iterator, no shared external cursor), so it composes with range adaptors like `| std::views::take(10)`.

A coroutine-based (`co_yield`) design was considered and rejected: coroutines can't run in a constant expression, which would contradict this library's whole point. `SaxRange::Iterator` instead keeps an explicit, `MaxDepth`-bounded frame stack.

`Utf8Policy` carries over unchanged. `KeyPolicy` needed a dedicated default, `NoDuplicateKeyCheck` (no bookkeeping), since duplicate detection needs to remember keys seen so far in the current map — something a single-pass stream doesn't get for free. `RejectDuplicateKeys`/`KeepFirstDuplicateKey` are opt-in via a `MaxTrackedKeys` capacity parameter, and compare keys by raw encoded bytes rather than decoded value (a documented simplification: `0x00` and `0x18 0x00` as keys won't be caught as duplicates here, unlike in the tree decoders). `KeepLastDuplicateKey` isn't supported — "keep the last value" means retroactively rewriting an already-emitted event, which a push-forward stream can't do — and selecting it is a compile error, not a silent behavior change.

See `tests/test_sax.cpp` for the full suite.

## nlohmann::json-style ergonomics

Both `CborValue` and `NoAlloc`'s `NoAllocView` support an nlohmann::json-style interface on top of the raw access shown above. It's exception-free throughout: a miss or type mismatch returns a sentinel or `Expected<T, Error>`, never throws.

```cpp
#include "constexpr_cbor.hpp"
using namespace ConstexprCbor;

CborValue config = {{"name", "sensor-1"}, {"thresholds", {10, 20, 30}}};
config["name"].get<std::string>();          // -> Expected<std::string, Error>{"sensor-1"}
config["thresholds"][1].get<std::uint64_t>();  // -> Expected<std::uint64_t, Error>{20}
config["missing"].is_undefined();           // -> true, no throw, no crash

for (const auto& v : config["thresholds"]) { /* ... */ }

CborValue built;
built["x"] = 42;           // auto-promotes Undefined -> Map
built["y"][2] = "z";       // auto-promotes/null-pads -> Array
```

Available: the usual `is_*()` predicates (`is_undefined`, `is_null`, `is_array`, `is_map`/`is_object`, etc.), `size()`/`empty()`/`contains()`, `operator[]` (index, key, or anything convertible to `CborValue`), `.get<T>()`, range-for iteration, and `.items()` for map pairs. There's no `.at()`: nlohmann's throws on a miss, and this library stays exception-free, so only `operator[]` is offered.

**Array vs. map literal construction** follows nlohmann's own heuristic, generalized since CBOR map keys aren't restricted to strings:

```cpp
CborValue arr = {1, 2, 3};                    // array
CborValue obj = {{"a", 1}, {"b", 2}};         // map (every element is a 2-elem array)
CborValue forced_arr = CborValue::array({});  // bypasses the heuristic (also covers {})
CborValue forced_obj = CborValue::object({{CborValue::array({1, 2}), "composite-key"}});
CborValue bytes = CborValue::binary(some_span);  // byte strings need an explicit call
```

**User types** plug in via three customization points, tried in this order:

1. **`cbor_fields<T>()` reflection** — the easiest path for a plain aggregate struct. Specialize this to list each field as a `(name, member pointer)` pair, and the default `to_cbor`/`from_cbor` is built for you:

   ```cpp
   namespace mylib { struct Point { int x, y; }; }

   namespace ConstexprCbor {
   template <>
   constexpr auto cbor_fields<mylib::Point>() {
     return std::tuple{std::pair{"x", &mylib::Point::x}, std::pair{"y", &mylib::Point::y}};
   }
   }

   CborValue v = mylib::Point{3, 4};
   auto p = v.get<mylib::Point>();  // Expected<mylib::Point, Error>
   ```

2. **ADL hooks** — `to_cbor`/`from_cbor` found unqualified in the type's own namespace. Use this when a type needs custom logic, or the CBOR shape shouldn't mirror the struct's fields 1:1:

   ```cpp
   namespace mylib {
   struct Point { int x, y; };
   constexpr bool to_cbor(CborValue& out, const Point& p) {
     out = CborValue::object({{"x", p.x}, {"y", p.y}});
     return true;
   }
   constexpr bool from_cbor(const CborValue& in, Point& p) {
     auto x = in["x"].get<std::int64_t>(), y = in["y"].get<std::int64_t>();
     if (!x || !y) return false;
     p = {static_cast<int>(*x), static_cast<int>(*y)};
     return true;
   }
   }
   ```

3. **`CborSerializer<T>` specialization** — the escape hatch for a type you can't attach a free function to (third-party/std types).

If both an ADL hook and a `cbor_fields<T>()` specialization exist, the ADL hook wins. A `.get<T>()` call for a `T` with none of the three is a compile error, not a runtime failure.

**NoAlloc's `NoAllocView`** offers the same read-only surface via `constexpr_cbor_noalloc_ergonomics.hpp` (a third, separate header, so a minimal consumer doesn't pay for it):

```cpp
#include "constexpr_cbor_noalloc_ergonomics.hpp"
using namespace ConstexprCbor::NoAlloc;

NoAllocStorage<32, 32, 32> storage;
auto pool = storage.make_pool();
const auto view = decode_view(pool, bytes);  // Expected<NoAllocView, Error>
view->operator[]("name").get<std::string>();
for (const auto& v : (*view)["thresholds"]) { /* ... */ }
```

It also has a minimal builder (`NoAllocBuilder`: `make_null`/`make_bool`/`make_uint`/`make_int`/`make_float`/`make_text`/`make_bytes`/`make_array`/`make_map`), where every write returns `Expected<NoAllocView, Error>` since pool exhaustion is an expected outcome here. There's no mutable `operator[]` or brace-initializer construction for NoAlloc — both would need the same fallible grow/promote machinery under a syntax that can't express "this might fail."

## Caller-tunable nesting depth

Unbounded recursive nesting is the most common hazard class across real-world CBOR decoders. `decode_value<MaxDepth, ...>`'s `MaxDepth` (default `32`) bounds it; exceeding it is a refusal, not a crash.

## Examples

`examples/` has one standalone, runnable `.cpp` file per decode mode (heap, `NoAlloc`, `Sax`). Build and run with `cmake --build build --target heap_example noalloc_example sax_example`. They're compile-checked across the matrix below, same as `tests/`.

## Browser viewer

`webapp/` decodes a `.cbor` file to pretty-printed JSON entirely client-side, via this library compiled to WebAssembly. No upload, no server:

```sh
docker build -t cbor-webapp -f webapp/Dockerfile .
docker run --rm -p 8080:8080 cbor-webapp
# open http://localhost:8080
```

See `webapp/README.md` for the local-build path and how CBOR concepts with no JSON equivalent are rendered.

## Benchmarks

`benchmarks/` compares this library's three decode paths against tinycbor, QCBOR, and libcbor on CPU performance (decode and encode), memory usage, correctness, and flexibility:

```sh
docker build -t cbor-bench -f benchmarks/Dockerfile .
docker run --rm cbor-bench
```

It's one container run, not a sampled distribution, and it isn't trying to crown one library "better" — the point is to make real tradeoffs (code size vs. throughput vs. allocation vs. what's provable at compile time) legible. Headline results: tinycbor and QCBOR have the smallest code-size footprint; this library's own `Sax` mode closes most of that gap while staying `consteval`-provable; raw throughput depends more on payload shape than on library choice; libcbor (a DOM-style C library, included as a control) pays a measured cost in every dimension for that shape, in plain C, confirming it's the design that costs, not C++. Full numbers and methodology in [`benchmarks/README.md`](benchmarks/README.md).

## Building and testing this library

```sh
cmake -S . -B build -DCMAKE_CXX_STANDARD=23   # optional; the library only requires C++20
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMAKE_CXX_STANDARD` is a request: CMake caps it to whatever the compiler supports (down to GCC 10, capped to `gnu++2a`).

Static analysis: `cmake -DCBOR_ENABLE_CLANG_TIDY=ON -DCBOR_ENABLE_CPPCHECK=ON ..` (opt-in, auto-skipped if not installed), plus `cmake --build build --target cbor_static_analysis` for `scan-build`.

Full compiler/platform matrix: [`docker/README.md`](docker/README.md). `docker/build-matrix.sh` builds every Dockerfile in `docker/` and reports pass/fail.

Planned: a runtime fuzzing harness as this library's complement — compile-time testing proves the finite set of cases a `consteval` function actually exercises; fuzzing covers the unbounded runtime input space `static_assert` can't reach.

## Author

Written by [Stefano Falasca](https://falasca.engineering), an embedded software consultant focused on safety-critical systems and agent-ready verification.

## License

MIT — see [`LICENSE`](LICENSE).
