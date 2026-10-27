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

## No-malloc decoding/encoding (`ConstexprCbor::NoAlloc`)

`decode_value`/`encode_value` above rely on `std::vector<CborValue>` permitting
an incomplete element type, which is exactly what makes a recursive value type
possible without a heap pointer indirection of some kind — but it does mean a
heap. For environments without one at all, `ConstexprCbor::NoAlloc` (in its own
header, `include/constexpr_cbor_noalloc.hpp`) is the no-malloc counterpart: every
decoded value is a `Node` living in storage the *caller* owns and sizes —
`NoAllocStorage<MaxNodes, MaxRefs, MaxBytes>` bundles a `Node[]`, a `Node*[]`
reference-slot array (see below), and a flat byte array for string/bignum
content. `NodePool` is a small, non-owning view over those three spans — the
actual allocator, not the storage itself — so the one thing this whole design
rests on is that the caller's `NoAllocStorage` outlives every decoded reference
into it; see "Running out of capacity, and the buffer-lifetime precondition"
below for exactly what that means and how `NodePool` defensively checks for it.

The two headers are independently usable: a project that only wants the
heap-based API includes just `constexpr_cbor.hpp`, and a project that only
wants the no-malloc API includes just `constexpr_cbor_noalloc.hpp` (which pulls
in `constexpr_cbor.hpp` itself, for the primitives both share — `decode_length`,
the UTF-8 policies, `decode_byte_string`/`decode_text_string`, `decode_simple` —
none of which allocate). Since every symbol in both headers is a template,
including one and never touching the other costs nothing: unused templates
generate no code. A project that wants both just includes both (or includes
only `constexpr_cbor_noalloc.hpp`, since that already gets both) — doing so is
exactly how `RejectDuplicateKeys`/`KeepFirstDuplicateKey`/`KeepLastDuplicateKey`
end up usable with *both* `decode_value` and `NoAlloc::decode_value`: each is
the same type in both headers (see "Pluggable duplicate-map-key policies" above
for the NoAlloc-shaped overload each one also provides), not a parallel
NoAlloc-only copy of each.

```cpp
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;

constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

consteval bool round_trips() {
    NoAlloc::NoAllocStorage<16, 16, 16> storage;     // 16 nodes, 16 ref slots, 16 content bytes -- sized by the caller
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

What's structurally different from the heap-based API, and why:

- **`std::array<T, N>` can't hold an incomplete `T`.** `std::vector<T>` explicitly
  permits an incomplete element type (internal pointer indirection); a fixed-size
  array cannot, so a self-referential value type needs *some* indirection no
  matter how it's declared. Here that's a pointer into caller-owned storage
  rather than a heap pointer: a `Node` never contains another `Node` by value,
  only by pointer (`ArrayRef`/`MapRef`/`TagRef` hold pointers, not nested
  values) — safe precisely because the caller's `NoAllocStorage` is guaranteed
  not to move or be destroyed while any decoded tree into it is alive (see
  below).
- **Arrays and maps need their N children as a contiguous run of slots**, so a
  child can be reached by `first_child[i]` instead of being, say, a
  singly-linked chain. But contiguity has to be reserved *before* any child is
  decoded, and a child's own node count isn't known until it's actually decoded
  (a nested array needs however many nodes its own subtree needs). The fix is
  a dedicated reference-slot array, separate from the `Node` storage itself
  (`NoAllocStorage`'s `Node*[]`, sized independently via `MaxRefs`): each slot is
  a bare 8-byte pointer, resolved directly by `NodePool::child`/`map_key`/
  `map_value` with no further indirection, and the child's real (possibly
  far-away, possibly many-node) subtree lives wherever it was actually decoded.
- **Indefinite-length arrays/maps don't know their count upfront at all** (no
  length marker, just items until a `0xFF` break), yet still need that same
  contiguous reservation before decoding any child. `NoAlloc::skip_item` is a
  non-allocating structural walker that pre-counts them in a first pass, so
  the real decode can reserve the right-sized block before its second pass.
  This is a genuine extra cost indefinite-length forms pay here that they
  don't in the heap-based decoder (which just appends to a `std::vector`):
  **O(n) extra structural work**, not a second allocation pass — `skip_item`
  never calls into `NodePool` at all.
- **Chunked (indefinite-length) byte/text strings** still work: each chunk is
  appended directly into the pool's flat byte storage via repeated
  `allocate_bytes` calls, the same incremental-append discipline the
  heap-based decoder's `content.insert()` uses, just writing into a
  fixed-capacity array instead of growing a `std::vector`.
- **Duplicate-map-key policies work here too, via a different mechanism.** The
  heap-based `KeyPolicy` re-scans a flat `std::vector<pair<CborValue, CborValue>>`
  of entries seen so far; a no-malloc map has no such flat, directly-comparable
  container (keys/values are scattered across whatever subtrees their own
  recursive decode needed), so `NoAlloc::decode_value` instead compares each new
  key structurally (`NoAlloc::node_equal`, a recursive by-pointer equality covering
  every alternative, including composite Array/Map/Tag keys) against the keys
  already written, and calls the same `KeyPolicy::on_duplicate` — through its
  NoAlloc-shaped overload — on a match. `KeepFirstDuplicateKey` simply doesn't
  advance the write cursor for the dropped pair; `KeepLastDuplicateKey`
  overwrites the earlier pair's value slot in place. Reserved-but-unused
  trailing slots from dropped pairs are harmless (`MapRef.count` only ever
  covers the pairs actually written).
- **Running out of `MaxNodes`, `MaxRefs`, or `MaxBytes` is a refusal**
  (`std::nullopt`), exactly like a malformed-input refusal elsewhere in this
  library — never a crash or a silent truncation. Sizing `NoAllocStorage` is the
  caller's job, the same way sizing an output buffer for `NoAlloc::encode_value`
  is. `MaxRefs` is independent of `MaxNodes`: every node that's ever an
  array/map child needs one reference slot, so `MaxRefs == MaxNodes` is a safe
  default, but a caller decoding shallow, mostly-scalar data can size it
  smaller.
- **`NoAlloc::encode_value` writes into a caller-supplied `std::span<std::uint8_t>`**
  instead of returning a `std::vector`, returning how many bytes it used or
  `std::nullopt` if the buffer was too small — never a truncated or
  out-of-bounds write. Like the heap-based encoder, it always emits the
  canonical (shortest) form: an indefinite-length input decodes fine but
  re-encodes to its definite-length equivalent, not byte-for-byte back to the
  original bytes.
- **A CBOR length field is checked before it's ever cast to `std::size_t`.**
  The wire format allows an array/map to claim up to 2^64-1 items, but the
  reference-slot array indexes with `std::size_t`, which can be narrower than
  64 bits (32 on common embedded targets). `NoAlloc::decode_value` reserves
  reference slots for a whole array/map's children *before* decoding any of
  them, so a silently-truncating cast there would be the exact shape of
  libcbor PR #387's bug: a huge count wraps to a small one, the reservation
  "succeeds" against that undersized number, and the loop — which still
  iterates the real, untruncated count — writes past it.
  `NoAlloc::detail::checked_node_count` refuses (`std::nullopt`) instead, the
  same contract every other refusal in this codec uses; it also guards the
  `*2` a map's key+value slot pairs need.
- **Running out of capacity, and the buffer-lifetime precondition.** Every
  `Node*` this file ever hands back is a raw, unchecked pointer into whatever
  `NoAllocStorage` the caller constructed `NodePool` over — safe as long as that
  storage outlives every decoded reference into it, which is a precondition
  the caller upholds, not something `NodePool` can enforce structurally (a
  `unique_ptr` doesn't help here: it implies heap ownership, which doesn't
  apply to a stack-resident `NoAllocStorage` at all, and the actual contract —
  "don't move or destroy this while a decoded tree points into it" — isn't
  something any smart pointer adds over a plain reference; a moving-buffer
  content check doesn't help either, since the content just moves with it).
  As a defensive, best-effort check against a narrower, genuinely catchable
  mistake, `NodePool::owns(const Node*)` checks — via pure address-range
  comparison, never a dereference — whether a given pointer actually falls
  inside *this* pool's own node storage, and `NOALLOC_ASSERT(owns(...))` (a
  thin `assert()` wrapper, stripped under `NDEBUG`) guards every place a
  `Node*` crosses back into the library (`child`/`map_key`/`map_value`/
  `tag_child`, `encode_value`, `node_equal`, `NoAllocBuilder::make_array`/
  `make_map`). This catches a `Node*` that never belonged to this pool at
  all — one left over from an unrelated or already-destroyed pool, bad
  pointer arithmetic, a stray null. It does **not** catch the pool's own
  storage having moved: `nodes_`/`node_capacity_` are themselves just
  members captured once, and if the whole buffer relocates without this
  `NodePool`'s members being updated, every pointer involved — `nodes_`
  included — is equally stale, so the range check can't notice anything
  is wrong. That category of mistake is the one genuinely uncatchable
  precondition violation here, same as dereferencing any other dangling
  pointer in C++.

See `tests/test_noalloc.cpp` for the full suite (round-trips against the same
official vectors `tests/test_array.cpp`/`test_map.cpp`/`test_tag.cpp` use for
the heap-based decoder, plus pool-exhaustion and buffer-too-small refusal
cases). It *is* fully `compile_time_ut`-based, same as every other test file:
`NodePool` can never itself be a `constexpr` input the way sibling test
files' plain byte vectors are (it's mutated by `decode_value` through a
non-const reference), but a small `constexpr` helper function that declares
and consumes its own `NodePool` entirely inside its own body is a
self-contained expression, usable directly as an `expect_eq<>()`/
`expect_true<>()` argument.

## Event-based SAX decoding (`ConstexprCbor::Sax`)

A third decode strategy, alongside the heap and no-malloc tree-builders
above: `ConstexprCbor::Sax::SaxRange` (its own header,
`include/constexpr_cbor_sax.hpp`) doesn't build any value at all — it
pulls one CBOR *event* at a time directly off the wire (`BeginArray`,
`Uint`, `TextString`, `EndMap`, ...), the same shape tinycbor and
QCBOR's own pull-style APIs already expose. It reuses every one of
this library's byte-level primitives unchanged (`decode_length`,
`decode_uint`/`decode_negint`, `decode_byte_string`/`decode_text_string`,
`decode_simple`, `skip_item`) — nothing in `SaxRange` reimplements wire
parsing, only the control flow deciding which primitive to call next and
when to pause does.

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

**Why new control flow was unavoidable, not just new API surface**: both
the heap and no-malloc decoders always decode one fully-contained
subtree per recursive call and return only once it's done — there's no
"decode just the next event and suspend" shape to borrow from either.
The obvious tool for that shape, a C++20 coroutine (`co_yield` per
event), was considered and rejected: coroutines cannot be evaluated in a
constant expression, and this library's whole point is `consteval`-
provable decoding, so a mode that can't run at compile time would
contradict its own reason for existing. `SaxRange::Iterator` instead
keeps an explicit frame stack bounded by `MaxDepth` — the same
nesting-depth contract `decode_value` already enforces, just checked
against an array index instead of a recursive call's own stack depth.

**Fully range-compatible, not just "has `begin()`/`end()`"**: every byte
of `SaxRange`'s iteration state (byte position, the frame stack, the
duplicate-key tracking buffer below) lives *by value* inside the
iterator itself — there's no hidden shared external cursor the way
`istream_iterator` has. Two copies of the same iterator genuinely
support independent, non-interfering advancement, which is what makes
`SaxRange` a real `std::ranges::forward_range` rather than only
`input_range`. It also implements `std::ranges::view_interface`, so it
composes directly with range adaptors (`| std::views::take(10)`, and so
on).

**Policy reuse, and where it structurally can't carry over unchanged**:
`Utf8Policy` (`NoUtf8Validation`/`RoughUtf8Validation`/
`CompleteUtf8Validation`/`AsciiOnlyValidation`) is reused exactly as-is —
it's invoked once per fully-assembled string payload with zero retained
state, the same as every other decode path. `KeyPolicy` needed more
thought: duplicate-key detection fundamentally needs to remember "keys
seen so far in the current open map," which a single-pass event stream
doesn't have for free the way the tree decoders' already-materialized
entries do.
- A new, fully shared policy, `NoDuplicateKeyCheck` (pass every pair
  through, no bookkeeping at all), is `SaxRange`'s default — the only
  sensible default for a mode whose whole point is not retaining state.
- `RejectDuplicateKeys`/`KeepFirstDuplicateKey` work with `SaxRange`
  too, opted into via a `MaxTrackedKeys` template parameter (a capacity,
  exactly like `NoAllocStorage`'s `MaxNodes`/`MaxRefs`/`MaxBytes` —
  exceeding it is a clean refusal, not UB). Detection compares keys by
  **raw encoded bytes**, not decoded structural value equality the way
  `node_equal`/`CborValue::operator==` do: a documented, deliberate
  simplification (two non-canonically-but-value-equal encodings of the
  same integer key, `0x00` vs. `0x18 0x00`, won't be caught here, unlike
  in the tree decoders) that avoids needing to decode a key into a
  comparable value just to check it, which would mean building exactly
  the kind of tree this mode exists to avoid.
- `KeepLastDuplicateKey` is **not supported** here, deliberately: "keep
  the last value for this key" means retroactively overwriting an
  already-emitted event, which a push-forward event stream cannot do
  without buffering the whole map first — the thing SAX exists not to
  do. Selecting it with `SaxRange` is a compile error (a named
  `static_assert`), not a silent behavior change.

See `tests/test_sax.cpp` for the full suite, including a
`std::ranges::forward_range`/`std::ranges::view` conformance check and
the duplicate-key-policy cases above.

## nlohmann::json-style ergonomics

Both `CborValue` and the NoAlloc's `NoAllocView` support an nlohmann::json-style
ergonomic interface on top of the raw `std::get`/`std::visit` access shown
above — entirely additive, and kept exception-free throughout: a miss or
type mismatch returns a shared sentinel or `std::optional`, never throws,
preserving `-fno-exceptions` compatibility and `consteval`-safety for every
path. Their iteration is a genuine `std::ranges::forward_range` too, the
same bar `SaxRange` is held to below (`.begin()`/`.end()`/`.items()` all
return copyable iterators with no shared external cursor) — proven, not
just asserted, via a `static_assert(std::ranges::forward_range<...>)` in
`tests/test_ergonomics_iteration.cpp`/`tests/test_noalloc_view.cpp`.

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

**NoAlloc (`ConstexprCbor::NoAlloc::NoAllocView`)**: the same read-only surface
(`is_*()`/`operator[]`/`.get<T>()`/iteration/`.items()`), via
`#include "constexpr_cbor_noalloc_ergonomics.hpp"` — a third, separately
includable header (depends on both existing ones), so a minimal embedded
consumer who only wants raw `NoAlloc::decode_value` never pays for it:

```cpp
#include "constexpr_cbor_noalloc_ergonomics.hpp"
using namespace ConstexprCbor::NoAlloc;

NoAllocStorage<32, 32, 32> storage;
auto pool = storage.make_pool();
const auto view = decode_view(pool, bytes);  // std::optional<NoAllocView>
view->operator[]("name").get<std::string>();
for (const auto& v : (*view)["thresholds"]) { /* ... */ }
```

`NoAllocView` bundles a `(pool, node)` pair so `view["a"][2]["b"]` chains
without re-threading `pool` at every step; a non-template, concrete type
(the pool it points into is no longer sized via template parameters either —
see "No-malloc decoding/encoding" above). It also provides a minimal,
explicitly pool-exhaustion-aware **builder** (`NoAllocBuilder`, `make_null`/
`make_bool`/`make_uint`/`make_int`/`make_float`/`make_text`/`make_bytes`/
`make_array`/`make_map`) that writes new nodes into an *existing* pool's remaining
capacity — every write returns `std::optional`, since pool exhaustion is an
ordinary, expected failure here (unlike the heap builder, where `std::vector`
growth can't fail). `make_array`/`make_map` take already-built (post-order)
child `NoAllocView`s into the *same* pool; `make_map` does no duplicate-key
handling of its own (the builder isn't policy-templated — a caller needing
dedup resolves it in their own pair list first). A **mutable `operator[]`**
and **brace-initializer construction** are deliberately out of scope for
NoAlloc: both would need the exact same optional-returning grow/promote
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

## Examples

`examples/` has one standalone, runnable `.cpp` file per decode mode
(heap, `NoAlloc`, `Sax`), each commented to stand on its own. Build and
run any of them with `cmake --build build --target heap_example
noalloc_example sax_example`. They're also compile-checked across the
entire compiler/platform matrix below, the same as `tests/`.

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

`benchmarks/` compares this library's three decode paths (heap,
no-malloc `NoAlloc`, and event-based `Sax`) against tinycbor, QCBOR, and
libcbor on CPU performance, memory usage, correctness, and flexibility,
with real numbers from a reproducible Docker build (`docker build -t
cbor-bench -f benchmarks/Dockerfile . && docker run --rm cbor-bench`).
This isn't a statistically rigorous benchmark (one container run, not a
sampled distribution across hosts) and it isn't trying to crown one
library "better" — the point is to make real tradeoffs (code size vs.
throughput vs. allocation behavior vs. what's provable at compile time)
legible enough that picking a library for your own constraints doesn't
have to be a guess.

tinycbor/QCBOR are pull-parsers; this library's own `Sax` mode was added
specifically to give them a structurally fair comparison point (heap and
`NoAlloc` both build a tree, which was always an extra variable
alongside whatever the raw numbers were measuring). libcbor was added as
a different kind of control: it's a DOM-style C library — the same
owned-tree shape as this library's heap `CborValue` path — and it pays a
real, measured cost in every dimension (allocation, code size, speed)
for that shape, in plain C, confirming that cost belongs to the *design*,
not to C++ or `std::variant` specifically.

What the numbers actually show: tinycbor and QCBOR have the smallest
code-size footprint of the libraries compared, consistently — mature,
hand-tuned C state machines with no dynamic dispatch and no generic
value representation to carry. This library's own `Sax` mode closes
most of that gap from within the same codebase (smaller `.text` than
either tinycbor or QCBOR in this benchmark, in fact, while reusing the
exact same byte-parsing primitives `NoAlloc`/heap use), without
switching language or giving up `consteval`-provability. Raw throughput
varies by payload shape more than by library: `NoAlloc` and `Sax`
measure faster than tinycbor/QCBOR on three of the four payload shapes
tested, while QCBOR is clearly fastest on the deeply-nested one, where
its fixed-size nesting tracker is a structurally cheap fit for that one
access pattern. libcbor's numbers land behind tinycbor/QCBOR on every
payload and unevenly against this library's own heap path, consistent
with its DOM-style allocation cost showing up differently depending on
how string-heavy a given payload is. constexpr-cbor's own differentiators
are structural rather than timing-dependent: compile-time-proven
correctness via `consteval`, a generic owned-tree mode and two
zero-allocation modes (tree-shaped and event-shaped) as equally
first-class options rather than being limited to whichever one a design
happened to pick, and caller-tunable nesting depth/UTF-8 validation/
duplicate-key policy — shared across all three decode paths — where the
others hard-code one behavior. See `benchmarks/README.md` for the full
numbers, methodology, and the complete tradeoff discussion, including
how `NoAlloc`'s internals moved from index-based to pointer-based
indirection and what that did to code size.

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
