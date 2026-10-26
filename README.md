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
- **GCC 13+ or Clang 19+.** `CborValue` holds a recursive `std::variant`/`std::vector`, and that's only usable as a literal type (a `constexpr` function parameter/return type) once a standard library actually implements full C++20 constexpr container support. Checked directly, not assumed: GCC 13 (libstdc++) passes; Clang 15 through 18 (libc++) fail with "`CborValue` is not a literal type"; Clang 19 passes cleanly. The `docker/` matrix is the source of truth for this, not this line — run it yourself if a toolchain combination isn't listed here.
- Depends on [`compile_time_ut`](https://github.com/sfalasca/compile-time-unit-testing) for its compile-time test assertions.
- Proving a very deeply nested structure at compile time can hit the compiler's own `-fconstexpr-depth`/`-fconstexpr-steps` limit before this library's own `MaxDepth` bound does — a real, separate ceiling from a different axis (every level of CBOR nesting costs several `std::variant`/`std::vector` constructor frames, not one). Raise the compiler flag explicitly if you need to prove a deeper structure at compile time than the default allows; this isn't something the library can paper over.

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
All 81 distinct vectors in Appendix A's Table 6 are covered. A handful of rows are
deliberately non-preferred alternates (e.g. `Infinity`/`NaN` shown at all three
float widths, when the preferred encoding always picks the shortest): those assert
the decoded value is correct and that this library's own encoder picks the
canonical width, not that re-encoding reproduces that particular row's own bytes.

Beyond the RFC's own examples, `tests/test_malformed_input.cpp` and a handful of
cases folded into the per-type test files come from
[`cbor-wg/cbor-test-vectors`](https://github.com/cbor-wg/cbor-test-vectors) (an
actively maintained corpus beyond Appendix A that several real CBOR libraries
vendor), plus QCBOR's own `not_well_formed_cbor.h` matrix — the exact bytes are
hard-coded with a provenance comment at each use site, the same way RFC 8949's
own vectors are, rather than vendoring either source wholesale.

Two deliberate scope decisions worth stating rather than leaving implicit:
this library checks CBOR **well-formedness** (the byte structure), not **tag
content validity** (e.g. it does not check that a tag-0 item's nested value is
actually a valid date-time string) — a real divergence point between CBOR
implementations, and this one picks the structural-only side of it. And a
decoded `NaN`'s specific payload bits aren't guaranteed to survive a decode/
re-encode round-trip (this library always canonicalizes `NaN` to its shortest
form, discarding any signaling-vs-quiet distinction or payload bits the
original encoding carried) — some platforms' FPUs quiet a signaling NaN on
load regardless, so this isn't a portable guarantee to make anyway.

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
- `AsciiOnlyValidation` — a different thing than the three above, not a fourth
  strictness level of the same thing: ASCII is a subset of UTF-8 (every one of
  the other policies accepts it), but the reverse isn't true. A string like
  `"café"` is perfectly valid UTF-8 and still fails this one. For systems or
  protocol fields that can't handle multi-byte sequences at all, not for
  deciding how strictly to check UTF-8 itself.

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

## No-malloc decoding/encoding (`ConstexprCbor::Arena`)

`decode_value`/`encode_value` above rely on `std::vector<CborValue>` permitting
an incomplete element type, which is exactly what makes a recursive value type
possible without a heap pointer indirection of some kind — but it does mean a
heap. For environments without one at all, `ConstexprCbor::Arena` (in its own
header, `include/constexpr_cbor_arena.hpp`) is the no-malloc counterpart: every
decoded value lives in a caller-sized `NodePool<MaxNodes, MaxBytes>` (a flat
`std::array<Node, MaxNodes>` plus a flat byte arena for string/bignum content)
instead of on the heap.

The two headers are independently usable: a project that only wants the
heap-based API includes just `constexpr_cbor.hpp`, and a project that only
wants the no-malloc API includes just `constexpr_cbor_arena.hpp` (which pulls
in `constexpr_cbor.hpp` itself, for the primitives both share — `decode_length`,
the UTF-8 policies, `decode_byte_string`/`decode_text_string`, `decode_simple` —
none of which allocate). Since every symbol in both headers is a template,
including one and never touching the other costs nothing: unused templates
generate no code. A project that wants both just includes both (or includes
only `constexpr_cbor_arena.hpp`, since that already gets both) — doing so is
exactly how `RejectDuplicateKeys`/`KeepFirstDuplicateKey`/`KeepLastDuplicateKey`
end up usable with *both* `decode_value` and `Arena::decode_value`: each is
the same type in both headers (see "Pluggable duplicate-map-key policies" above
for the Arena-shaped overload each one also provides), not a parallel
Arena-only copy of each.

```cpp
#include "constexpr_cbor_arena.hpp"

using namespace ConstexprCbor;

constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

consteval bool round_trips() {
    Arena::NodePool<16, 16> pool;               // 16 nodes, 16 content bytes -- sized by the caller
    const auto decoded = Arena::decode_value(pool, bytes);
    if (!decoded) return false;
    const auto& items = std::get<Arena::ArrayRef>(pool.nodes[decoded->node_index].data);
    if (items.count != 3) return false;
    std::array<std::uint8_t, 16> out{};
    const auto written = Arena::encode_value(pool, decoded->node_index, out);
    return written && std::equal(out.begin(), out.begin() + *written, bytes);
}

static_assert(round_trips());
```

What's structurally different from the heap-based API, and why:

- **`std::array<T, N>` can't hold an incomplete `T`.** `std::vector<T>` explicitly
  permits an incomplete element type (internal pointer indirection); a fixed-size
  array cannot, so a self-referential value type needs *some* indirection no
  matter how it's declared. Here that's an index into the pool rather than a
  heap pointer: a `Node` never contains another `Node` by value, only by index
  (`ArrayRef`/`MapRef`/`TagRef` hold indices, not nested values).
- **Arrays and maps need their N children as a contiguous run of slots**, so a
  child can be reached by `first_child + i` instead of being, say, a
  singly-linked chain. But contiguity has to be reserved *before* any child is
  decoded, and a child's own node count isn't known until it's actually decoded
  (a nested array needs however many nodes its own subtree needs). The fix is
  one level of indirection per slot (`IndexCell`, resolved transparently by
  `NodePool::child`/`map_key`/`map_value`): the slot always holds a cheap,
  fixed-size index, and the child's real (possibly far-away, possibly
  many-node) subtree lives wherever it was actually decoded.
- **Indefinite-length arrays/maps don't know their count upfront at all** (no
  length marker, just items until a `0xFF` break), yet still need that same
  contiguous reservation before decoding any child. `Arena::skip_item` is a
  non-allocating structural walker that pre-counts them in a first pass, so
  the real decode can reserve the right-sized block before its second pass.
  This is a genuine extra cost indefinite-length forms pay here that they
  don't in the heap-based decoder (which just appends to a `std::vector`):
  **O(n) extra structural work**, not a second allocation pass — `skip_item`
  never calls into `NodePool` at all.
- **Chunked (indefinite-length) byte/text strings** still work: each chunk is
  appended directly into the pool's flat byte arena via repeated
  `allocate_bytes` calls, the same incremental-append discipline the
  heap-based decoder's `content.insert()` uses, just writing into a
  fixed-capacity array instead of growing a `std::vector`.
- **Duplicate-map-key policies work here too, via a different mechanism.** The
  heap-based `KeyPolicy` re-scans a flat `std::vector<pair<CborValue, CborValue>>`
  of entries seen so far; an arena map has no such flat, directly-comparable
  container (keys/values are scattered across whatever subtrees their own
  recursive decode needed), so `Arena::decode_value` instead compares each new
  key structurally (`Arena::node_equal`, a recursive by-index equality covering
  every alternative, including composite Array/Map/Tag keys) against the keys
  already written, and calls the same `KeyPolicy::on_duplicate` — through its
  Arena-shaped overload — on a match. `KeepFirstDuplicateKey` simply doesn't
  advance the write cursor for the dropped pair; `KeepLastDuplicateKey`
  overwrites the earlier pair's value slot in place. Reserved-but-unused
  trailing slots from dropped pairs are harmless (`MapRef.count` only ever
  covers the pairs actually written).
- **Running out of `MaxNodes` or `MaxBytes` is a refusal** (`std::nullopt`),
  exactly like a malformed-input refusal elsewhere in this library — never a
  crash or a silent truncation. Sizing either pool is the caller's job, the
  same way sizing an output buffer for `Arena::encode_value` is.
- **`Arena::encode_value` writes into a caller-supplied `std::span<std::uint8_t>`**
  instead of returning a `std::vector`, returning how many bytes it used or
  `std::nullopt` if the buffer was too small — never a truncated or
  out-of-bounds write. Like the heap-based encoder, it always emits the
  canonical (shortest) form: an indefinite-length input decodes fine but
  re-encodes to its definite-length equivalent, not byte-for-byte back to the
  original bytes.
- **A CBOR length field is checked before it's ever cast to `std::size_t`.**
  The wire format allows an array/map to claim up to 2^64-1 items, but
  `NodePool` indexes with `std::size_t`, which can be narrower than 64 bits
  (32 on common embedded targets). `Arena::decode_value` reserves node slots
  for a whole array/map's children *before* decoding any of them, so a
  silently-truncating cast there would be the exact shape of libcbor PR #387's
  bug: a huge count wraps to a small one, the reservation "succeeds" against
  that undersized number, and the loop — which still iterates the real,
  untruncated count — writes past it. `Arena::detail::checked_node_count`
  refuses (`std::nullopt`) instead, the same contract every other refusal in
  this codec uses; it also guards the `*2` a map's key+value slot pairs need.

See `tests/test_arena.cpp` for the full suite (round-trips against the same
official vectors `tests/test_array.cpp`/`test_map.cpp`/`test_tag.cpp` use for
the heap-based decoder, plus pool-exhaustion and buffer-too-small refusal
cases). It *is* fully `compile_time_ut`-based, same as every other test file:
`NodePool` can never itself be a `constexpr` input the way sibling test
files' plain byte vectors are (it's mutated by `decode_value` through a
non-const reference), but a small `constexpr` helper function that declares
and consumes its own `NodePool` entirely inside its own body is a
self-contained expression, usable directly as an `expect_eq<>()`/
`expect_true<>()` argument.

## nlohmann::json-style ergonomics

Both `CborValue` and the Arena's `ArenaView` support an nlohmann::json-style
ergonomic interface on top of the raw `std::get`/`std::visit` access shown
above — entirely additive, and kept exception-free throughout: a miss or
type mismatch returns a shared sentinel or `std::optional`, never throws,
preserving `-fno-exceptions` compatibility and `consteval`-safety for every
path.

```cpp
#include "constexpr_cbor.hpp"
using namespace ConstexprCbor;

CborValue config = {{"name", "sensor-1"}, {"thresholds", {10, 20, 30}}};
config["name"].get<std::string>();          // -> std::optional<std::string>{"sensor-1"}
config["thresholds"][1].get<std::uint64_t>();  // -> std::optional<std::uint64_t>{20}
config["missing"].is_undefined();           // -> true, no throw, no crash

for (const auto& v : config["thresholds"]) { /* ... */ }

CborValue built;
built["x"] = 42;           // auto-promotes Undefined -> Map
built["y"][2] = "z";       // auto-promotes/null-pads -> Array
```

`is_undefined()`/`is_null()`/`is_boolean()`/`is_unsigned()`/`is_negative_int()`/
`is_bignum()`/`is_integer()`/`is_float()`/`is_number()`/`is_string()`/
`is_binary()`/`is_simple()`/`is_array()`/`is_map()` (alias `is_object()`)/
`is_tag()`, plus `size()`/`empty()`/`contains()`, `operator[]` (index, key,
or any type implicitly convertible to `CborValue`, including string
literals), `.get<T>()` (built-ins return `std::optional<T>`; see below for
user types), range-for iteration (array elements, or map *values* — matching
nlohmann's own default), and `.items()` (map key/value pairs).

**No `.at()`**: nlohmann's `.at()` throws on a miss; this library stays
exception-free, so rather than offer a method whose name implies that
contract and silently diverges from it, only `operator[]` is provided.

**Array vs. map literal construction**, adapted from nlohmann's own
heuristic (generalized here since CBOR map keys aren't restricted to
strings — a nested 2-element array in the braced list is read as a
key/value pair regardless of what the key is):

```cpp
CborValue arr = {1, 2, 3};                    // array
CborValue obj = {{"a", 1}, {"b", 2}};         // map (every element is a 2-elem array)
CborValue forced_arr = CborValue::array({});  // bypasses the heuristic (also covers {})
CborValue forced_obj = CborValue::object({{CborValue::array({1, 2}), "composite-key"}});
CborValue bytes = CborValue::binary(some_span);  // byte strings need an explicit call,
                                                   // same ambiguity nlohmann's json::binary solves
```

**User types**, via ADL hooks mirroring nlohmann's `adl_serializer`
(`to_cbor`/`from_cbor` found unqualified in the type's own namespace, or a
`CborSerializer<T>` specialization for types you can't attach a free
function to):

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

CborValue v = mylib::Point{3, 4};
auto p = v.get<mylib::Point>();  // std::optional<mylib::Point>
```

A `.get<T>()` call for a `T` with no built-in support, no ADL hook, and no
`CborSerializer<T>` specialization is a **compile error** — the one place
this interface is allowed to hard-fail at compile time (a template
instantiation failure, not a runtime/consteval-execution-time one).

**Arena (`ConstexprCbor::Arena::ArenaView`)**: the same read-only surface
(`is_*()`/`operator[]`/`.get<T>()`/iteration/`.items()`), via
`#include "constexpr_cbor_arena_ergonomics.hpp"` — a third, separately
includable header (depends on both existing ones), so a minimal embedded
consumer who only wants raw `Arena::decode_value` never pays for it:

```cpp
#include "constexpr_cbor_arena_ergonomics.hpp"
using namespace ConstexprCbor::Arena;

NodePool<32, 32> pool;
const auto view = decode_view(pool, bytes);  // std::optional<ArenaView<32, 32>>
view->operator[]("name").get<std::string>();
for (const auto& v : (*view)["thresholds"]) { /* ... */ }
```

`ArenaView<MaxNodes, MaxBytes>` bundles a `(pool, node_index)` pair so
`view["a"][2]["b"]` chains without re-threading `pool` at every step, and is
templated on its pool's exact capacity (consistent with how `NodePool`/
`decode_value` already work — a view never crosses pools of different
capacity). It also provides a minimal, explicitly pool-exhaustion-aware
**builder** (`ArenaBuilder<MaxNodes, MaxBytes>`, `make_null`/`make_bool`/
`make_uint`/`make_int`/`make_float`/`make_text`/`make_bytes`/`make_array`/
`make_map`) that writes new nodes into an *existing* pool's remaining
capacity — every write returns `std::optional`, since pool exhaustion is an
ordinary, expected failure here (unlike the heap builder, where `std::vector`
growth can't fail). `make_array`/`make_map` take already-built (post-order)
child `ArenaView`s into the *same* pool; `make_map` does no duplicate-key
handling of its own (the builder isn't policy-templated — a caller needing
dedup resolves it in their own pair list first). A **mutable `operator[]`**
and **brace-initializer construction** are deliberately out of scope for
Arena: both would need the exact same optional-returning grow/promote
machinery the builder already has, just duplicated under a value-type
syntax that can't naturally express "this might fail" — not a small
addition once pool exhaustion is taken seriously, and read-only view
ergonomics plus the explicit builder calls above already cover the common
cases.

## Caller-tunable nesting depth

Unbounded recursive nesting in arrays, maps, and tags is the single most common
hazard class across real-world CBOR decoders (stack overflow and related CVEs
turn up repeatedly). `decode_value<MaxDepth, ...>`'s `MaxDepth` (default `32`)
bounds it; exceeding it is a refusal, not a crash, the same as every other
contract violation in this codec.

## Browser viewer

`webapp/` is a standalone static page that decodes a `.cbor` file to
pretty-printed JSON entirely in the browser, via this library compiled to
WebAssembly (`docker/wasm.Dockerfile`'s same toolchain). No upload, no
server-side component: `docker build -t cbor-webapp -f webapp/Dockerfile .
&& docker run --rm -p 8080:8080 cbor-webapp`, then open
`http://localhost:8080`. See `webapp/README.md` for the local-build path
and exactly how CBOR concepts with no JSON equivalent (byte strings,
bignums, tags, non-string map keys) are rendered.

## Benchmarks

`benchmarks/` compares this library against tinycbor and QCBOR on CPU
performance, memory usage, correctness, and flexibility, with real
numbers from a reproducible Docker build (`docker build -t cbor-bench -f
benchmarks/Dockerfile . && docker run --rm cbor-bench`). Short version:
tinycbor/QCBOR are faster and smaller (hand-tuned C state machines);
constexpr-cbor's case is compile-time-proven correctness, a real
zero-malloc mode *and* a generic owned-tree mode as equally first-class
options, and caller-tunable nesting depth/UTF-8 validation/duplicate-key
policy where the others hard-code one behavior. See `benchmarks/README.md`
for the full numbers and methodology.

## Building and testing this library

```sh
cmake -S . -B build -DCMAKE_CXX_STANDARD=23   # optional; the library only requires C++20
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMAKE_CXX_STANDARD` is a request, not a requirement: CMake caps it to
whatever the compiler actually supports (down to GCC 10, which gets
silently capped to `gnu++2a`).

Static analysis: `cmake -DCBOR_ENABLE_CLANG_TIDY=ON -DCBOR_ENABLE_CPPCHECK=ON ..` (both opt-in, auto-skip with a warning if the tool isn't installed), plus an always-registered `cmake --build build --target cbor_static_analysis` running the Clang Static Analyzer (`scan-build`) over the whole build.

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
