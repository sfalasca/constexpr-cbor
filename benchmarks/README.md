# constexpr-cbor vs. tinycbor vs. QCBOR vs. libcbor

A reproducible benchmark comparing constexpr-cbor against three
widely-used C CBOR libraries — [tinycbor](https://github.com/intel/tinycbor)
(Intel, pinned `v0.6.1`), [QCBOR](https://github.com/laurencelundblade/QCBOR)
(pinned `v1.5.1`), and [libcbor](https://github.com/PJK/libcbor) (pinned
`v0.14.0`) — across four dimensions: **CPU performance**, **memory
usage**, **correctness**, and **flexibility**. tinycbor and QCBOR were
chosen first as the most actively maintained, widely deployed
general-purpose *pull-parsing* C CBOR libraries; libcbor was added
because it's the one general-purpose C library in this space that's
**DOM-style** — `cbor_load()` builds an owned, ref-counted `cbor_item_t`
tree, the same shape of thing constexpr-cbor's heap `CborValue` path
builds, rather than pull-parsing — making it the more apples-to-apples
comparison for that path specifically. (The earlier competitive research
this project's top-level README cites also covered zcbor and NanoCBOR;
both are narrower-scope, embedded-targeted pull-parsers like tinycbor/QCBOR,
not DOM-style, so they weren't added here.) Other-language CBOR libraries
(Python's `cbor2`, Rust's `serde_cbor`/`ciborium`, Go's `fxamacker/cbor`,
JavaScript's `cbor-x`, ...) are deliberately out of scope for this specific
benchmark: a cross-language throughput number would mostly measure
interpreter/runtime overhead rather than library design, which would break
the "same work, same compiler, isolate the library" premise everything
below depends on.

**What this is, and isn't, for**: nothing below is a statistically
rigorous benchmark (one container run, not a sampled distribution across
many hosts/kernels), and none of it is meant as a claim that any one
library here is "better" in general. The numbers exist to make real
tradeoffs legible — code size vs. throughput vs. allocation behavior vs.
API shape vs. what's provable at compile time — so that someone picking
a CBOR library for their own constraints has something more concrete to
go on than a marketing page either way. Where a number favors one path
over another on one payload, read it as "this is what that payload's
shape costs with this design," not as a ranking.

## Running it

```sh
docker build -t cbor-bench -f benchmarks/Dockerfile .
docker run --rm cbor-bench
```

This clones all three libraries at their pinned tags, builds everything
with the same compiler (gcc/g++ inside `ubuntu:24.04`, `-O2` for the
performance/correctness programs, `-Os` for the size comparison), and
runs all three programs in one shot. The numbers below are one run of
that container; re-run it yourself if you want fresh ones.

**On timing trustworthiness**: an earlier version of this benchmark timed
each library with a single pass and reported one number, which is
exactly as fragile as it sounds — anything that preempts the process
mid-loop (another process stealing the core, a page fault, a scheduler
migration) can only ever make that one pass slower, never faster, so a
single bad pass silently inflated the reported average with nothing in
the output to show it had happened. Two runs of that version, same code
and same host, once showed the no-malloc path *trailing* tinycbor/QCBOR
on some payloads and *beating* both on others — a genuine
measurement-robustness failure, not a real result to read anything into.
Fixed two ways: `bench.cpp` now pins itself to a single CPU core via
`sched_setaffinity` before any timing starts (so the scheduler can't
migrate it mid-round — this doesn't protect against *other* processes
contending for that same core, only against this process itself bouncing
between cores), and every library is timed across `kRounds = 9`
independent rounds, interleaved (heap, then NoAlloc, then SAX, then
tinycbor, then QCBOR, then libcbor, repeated 9 times — not all 9 rounds
for one library before moving to the next), reporting both the
**minimum** (the trustworthy estimate — a disturbance can only push a
round's time up, never down, so the minimum across many independent
rounds approximates the undisturbed cost, the same technique
`hyperfine`/`criterion`/Google Benchmark use) and the **maximum** across
those same rounds, so a suspiciously wide spread is visible in the output
instead of hidden behind one falsely precise-looking number. With this
fix, repeated runs — with or without an external `--cpuset-cpus` pin —
now land within a few percent of each other; the numbers below reflect
that.

**One build asymmetry worth stating up front**: unlike tinycbor and
QCBOR (whose individual `.c` files this Dockerfile compiles directly,
with no build-system step at all), libcbor needs a real build step —
its CMakeLists generates a `configuration.h` and an export-macro header
that don't exist in the raw source tree, so it's built once via CMake
(`-DCMAKE_BUILD_TYPE=Release`, static lib only) and that one build is
reused for *all three* programs, including the size comparison. tinycbor
and QCBOR, by contrast, get a *second*, separate `-Os
-ffunction-sections` compile of their `.c` files specifically for the
size comparison. libcbor's size number therefore reflects whatever
optimization level its own CMakeLists chooses for a Release build (which
turned out to include LTO, per its own build output), not a `-Os`-matched
rebuild like the other two get — an asymmetry in libcbor's favor or
against it that this doc isn't trying to resolve, just disclose.

## Methodology

Every library decodes the **same encoded bytes** (`benchmarks/data/*.cbor`,
embedded as `payloads.h`, generated by `gen_payloads.py`):

| payload | bytes | shape |
|---|---|---|
| `small` | 45 | one flat 4-field map (a typical "sensor reading" record) |
| `medium` | 10,778 | an array of 200 such records (a realistic telemetry batch) |
| `deep` | 91 | an array nested 30 levels deep (recursion/stack stress) |
| `strings` | 8,792 | an array of 100 ~85-byte text strings (string-heavy) |

For CPU performance, every library's decode function does the **same
amount of work**: fully walk the decoded structure and fold every scalar
(integers, floats, string byte-lengths) into a checksum. This matters
because tinycbor and QCBOR are streaming pull-parsers (they don't build
a generic tree unless you ask them to), while constexpr-cbor's heap
`CborValue` path and libcbor *do* build one — "decode" means different
things by default, so the comparison is deliberately "parse this buffer
and visit every value," which is well-defined for all of them and is
also what the cross-library **checksum equality** doubles as a
correctness check: if two libraries disagree on a payload's checksum,
that's a bug worth knowing about, not just a timing footnote. This isn't
hypothetical — building this exact comparison caught a real bug in this
benchmark's own libcbor adapter (see "A bug this methodology actually
caught" below).

Six decode paths are measured:
- **constexpr-cbor, heap** (`ConstexprCbor::decode_value`) — builds an
  owned `CborValue` tree, like `nlohmann::json`.
- **constexpr-cbor, NoAlloc** (`ConstexprCbor::NoAlloc::decode_value`) —
  the library's no-malloc mode, decoding into a reused, fixed-capacity
  `NodePool`: the same tree shape as heap, but zero heap allocation.
- **constexpr-cbor, SAX** (`ConstexprCbor::Sax::SaxRange`) — no tree at
  all, one event (`BeginArray`, `Uint`, `TextString`, ...) at a time,
  the same pull-style shape tinycbor/QCBOR already are. This is the
  structurally fairest comparison point of the three constexpr-cbor
  paths against tinycbor/QCBOR specifically, since all three are
  pull-parsers now — heap and NoAlloc always carried "builds a tree or
  not" as an extra variable alongside whatever the numbers were
  actually measuring.
- **tinycbor** — `cbor_parser_init` + recursive
  `cbor_value_enter_container`/`advance` walk.
- **QCBOR** — `QCBORDecode_Init` + a `QCBORDecode_GetNext` loop.
- **libcbor** — `cbor_load()` (builds the full tree in one call) +
  recursive `cbor_typeof`/`cbor_array_handle`/`cbor_map_handle` walk,
  `cbor_decref`'d after measurement. Isolated in its own translation unit
  (`benchmarks/libcbor_adapter.cpp`) rather than included directly in
  `bench.cpp`/`correctness.cpp`: libcbor's header declares several C
  functions under the same names tinycbor uses for unrelated purposes
  (`cbor_encode_uint`, `cbor_encode_tag`, ...) — a genuine `extern "C"`
  symbol collision, not just an include-path ambiguity, that only a
  separate translation unit avoids.

### A bug this methodology actually caught

The first version of the libcbor adapter used `cbor_get_uint64(item)` for
every `CBOR_TYPE_UINT` item. libcbor's own source shows why that's wrong:
`cbor_get_uint64` is `*(uint64_t*)item->data` behind a `CBOR_ASSERT` that's
a no-op in a release build — it requires the item to actually be stored
at `CBOR_INT_64` width internally, and silently reads past a narrower
buffer (picking up adjacent heap memory as the high bytes) for anything
stored more compactly, which is most real-world small integers. This
didn't show up on the `small`/`deep`/`strings` payloads — apparently by
luck of width-matching for those particular values — but surfaced
immediately as a wildly wrong `isum` on `medium` (200 records' worth of
mixed-width integers), caught by exactly the cross-library checksum
comparison this benchmark exists to run. Fixed by switching to
`cbor_get_int(item)`, which switches on `cbor_int_get_width()` and calls
the correctly-sized accessor itself.

## Results (one container run; six decode paths)

### CPU performance (ns/decode, lower is better; MB/s, higher is better)

Figures below are the **minimum** across 9 interleaved rounds per
library (see "On timing trustworthiness" above); repeated container runs
now land within a few percent of these.

| payload | heap | NoAlloc | SAX | tinycbor | QCBOR | libcbor |
|---|---|---|---|---|---|---|
| small (45 B) | 778 ns / 57.8 MB/s | 364 ns / 123.6 MB/s | 435 ns / 103.5 MB/s | 447 ns / 100.6 MB/s | 445 ns / 101.1 MB/s | 635 ns / 70.9 MB/s |
| medium (10.8 KB) | 315,162 ns / 34.2 MB/s | 63,225 ns / 170.5 MB/s | 72,572 ns / 148.5 MB/s | 84,427 ns / 127.7 MB/s | 74,987 ns / 143.7 MB/s | 259,673 ns / 41.5 MB/s |
| deep (91 B, depth 30) | 74,392 ns / 1.2 MB/s | 49,440 ns / 1.8 MB/s | 49,953 ns / 1.8 MB/s | 49,809 ns / 1.8 MB/s | 24,516 ns / 3.7 MB/s *(when accepted — see Correctness)* | 52,285 ns / 1.7 MB/s |
| strings (8.8 KB) | 11,150 ns / 788.5 MB/s | 2,588 ns / 3,396.7 MB/s | 3,215 ns / 2,735.1 MB/s | 4,650 ns / 1,890.6 MB/s | 3,884 ns / 2,263.5 MB/s | 23,985 ns / 366.6 MB/s |

**How to read this table — tradeoffs by payload shape, not a ranking:**
- On `small`, `medium`, and `strings`, both constexpr-cbor paths that
  avoid building a tree (NoAlloc) or avoid building a tree *and* aren't a
  separately-compiled library (SAX, and NoAlloc again) measure faster
  than tinycbor/QCBOR's numbers here. On `deep`, QCBOR is clearly the
  fastest of all six — its fixed, small-array-backed nesting tracker
  (see Flexibility) is a structurally cheap data structure for exactly
  that one access pattern (many shallow, deeply-chained containers), and
  that advantage dominates everything else on this specific payload
  shape. Neither of these is "the fast one" in general — they're each
  the fast one *for that payload's shape*, which is the actual point of
  measuring four different shapes instead of one.
- **SAX lands in the same range as tinycbor and QCBOR on every
  payload** (it's the sixth path added specifically to make this
  comparison apples-to-apples, since tinycbor/QCBOR are themselves
  pull-parsers — see Methodology). Where it differs from them at all,
  the likely structural reason is that SAX's byte-parsing primitives and
  control flow live in the same translation unit as whatever calls them
  (this is a header-only library), so the compiler can inline across
  calls that cross tinycbor/QCBOR's own function-call API boundary —
  not a claim about which design is "better," since that inlining
  opportunity is itself a tradeoff against being a prebuilt, separately
  distributable library.
- **One build-configuration fact worth stating plainly**: none of these
  binaries are built with `-DNDEBUG`, so the `NOALLOC_ASSERT(owns(...))`
  checks described in "NoAlloc internals" below are live, real branches
  in both the timed and the size-measured NoAlloc binary — a release
  (`-DNDEBUG`) build would strip them and could plausibly measure both
  faster and smaller than what's shown here.
- **QCBOR and tinycbor are the only two with decades of hand-tuned C
  optimization and no dynamic dispatch** (`std::variant`/`std::visit`,
  recursive-descent-through-a-tagged-union); that's a real structural
  property of their design, independent of which way any one payload's
  wall-clock numbers happen to lean on a given run.
- **libcbor's numbers land behind tinycbor/QCBOR specifically on every
  payload here**, and behind constexpr-cbor's own heap path on `strings`
  while landing ahead of it on `small`/`medium`/`deep` — not a strict
  ranking in either direction, just the DOM-style cost (real allocation
  plus `cbor_incref`/`cbor_decref` bookkeeping on every item, see Memory
  usage below) showing up differently depending on how much of a given
  payload is strings versus structure, since per-item refcounting and
  heap's own allocation pattern scale differently with that ratio.

### NoAlloc internals: from index-based to pointer-based

NoAlloc's `NodePool` used to own its storage directly (`std::array<Node,MaxNodes>`,
`std::array<uint8_t,MaxBytes>`, both NTTP-sized) and reference array/map
children through `IndexCell` — a dedicated `Node`-variant alternative that
existed solely to carry one `size_t` index, so that a contiguous block of
"child slots" could be reserved before each child was actually decoded and
landed wherever the pool's cursor happened to be. The indices were chosen
to make the decoded tree *relocatable* — safe even if the whole `NodePool`
object were moved or copied. That turned out to be solving a problem this
library doesn't actually have: `NodePool`'s real contract is that the
*caller* owns the backing storage and it never moves while any decoded tree
into it is alive — under that contract, raw pointers are exactly as safe as
indices, and strictly cheaper, since a dedicated `Node*` reference slot is
8 bytes, versus `IndexCell`'s "a whole `Node`-variant-sized slot (sized to
the variant's largest alternative plus its discriminant) just to carry one
`size_t`."

The resulting design: `NodePool` is now a small, non-owning,
trivially-copyable *view* (in the spirit of `std::span`) over three
separate, caller-owned, typed spans (`Node[]`, a dedicated `Node*[]` for
reference slots, and `uint8_t[]` for string/bignum content) — `NoAllocStorage<MaxNodes,MaxRefs,MaxBytes>`
is the owning counterpart callers actually declare. `IndexCell` is gone
entirely; `ArrayRef`/`MapRef` now point directly at a block of `Node*`
slots, and `decode_value`'s array/map cases allocate one fewer `Node` per
child than before (no slot-sized placeholder to immediately overwrite).
The reference-slot array also became an independently-tunable capacity
(`MaxRefs`), decoupled from the node budget it used to silently share.

One defensive addition from this change: `NodePool::owns(const Node*)`
checks, via pure address-range comparison (never a dereference), whether a
given pointer actually falls inside this pool's own node storage, and
`NOALLOC_ASSERT(owns(...))` guards every place a `Node*` crosses back into
the library. An earlier version of this check wrote magic "canary" byte
values into the storage and verified their *content* on every allocation
call — abandoned once it became clear content checks can't catch what
they were meant to: if the buffer itself relocates, the canary bytes move
with it, and the check still reads through its own stale, captured
pointers, which is no safer than any other dangling-pointer dereference.
The address-range check is honest about a narrower, but genuinely
catchable, mistake instead: a `Node*` that never belonged to this pool at
all (left over from an unrelated or destroyed pool, bad pointer
arithmetic, a stray null) — not the pool's own storage having moved,
which no runtime check can see, since every pointer involved, including
the pool's own bookkeeping, would be equally stale.

### Memory usage (bytes allocated per decode, measured via `mallinfo2()`)

| payload | heap | NoAlloc | SAX | tinycbor | QCBOR | libcbor |
|---|---|---|---|---|---|---|
| small | 800 | 0 | 0 | 0 | 0 | 656 |
| medium | 157,760 | 0 | 0 | 0 | 0 | 234,032 |
| deep | 3,712 | 0 | 0 | 0 | *refused — see Correctness* | 5,376 |
| strings | 14,192 | 0 | 0 | 0 | 0 | 15,776 |

**Takeaways:**
- **NoAlloc, SAX, tinycbor, and QCBOR all allocate exactly zero heap
  bytes during decode.** This is the real, apples-to-apples finding:
  tinycbor and QCBOR are zero-copy pull-parsers over the caller's buffer
  by design, and both of constexpr-cbor's non-heap paths are explicitly
  built to match that property — `NodePool` is a fixed-capacity,
  caller-provisioned `std::array`-backed object (decoding into it is
  "running out of space," never "allocating more"), and `SaxRange`
  never materializes anything beyond the small, fixed-size iterator
  state described in the top-level README.
- **libcbor allocates on every payload, and noticeably *more* than
  constexpr-cbor's own heap path does** for the same data (234,032 vs.
  157,760 bytes on `medium`; 15,776 vs. 14,192 on `strings`) — confirming
  directly what "libcbor is DOM-style" predicts: `cbor_load()` builds a
  real, independently-owned `cbor_item_t` tree, with per-item reference
  counting on top. This is the expected, validating result for adding
  libcbor to this benchmark in the first place: a true DOM-style C
  library pays a real, measurable allocation cost for the same
  convenience constexpr-cbor's heap path offers, it isn't something only
  a `std::variant`-based or C++ design pays for.
- **Only constexpr-cbor's heap `CborValue` path and libcbor allocate at
  all**, and both scale with payload size. This is the cost of the
  ergonomic, generic, owned-tree API (think `nlohmann::json`'s `json`
  object) — tinycbor and QCBOR don't offer an equivalent "build me a
  generic value tree" mode at all, so there's nothing to compare the
  heap path's allocation *against* in those two; the more useful framing
  is "constexpr-cbor is the only library here offering a zero-alloc tree
  mode, a zero-alloc pull-parsing mode, and an ergonomic owned-tree mode
  as equally first-class options, and the owned-tree mode's allocation
  cost is the price of that specific mode's convenience — a price
  libcbor, the other owned-tree option, pays too, and currently pays
  more of."

### Code size (single decode-entry-point executable, `-Os`, dead-code-eliminated)

`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`, so each
binary's `.text` reflects only what *actually calling the decoder once*
pulls in, not a whole translation unit of unused library functions.

**A methodology correction, worth stating plainly**: the first version
of this comparison had tinycbor's entry point call only
`cbor_parser_init()` (never reading a single value) and QCBOR's call
only one `QCBORDecode_GetNext()` (never recursing into the array it
returned) — while constexpr-cbor's `decode_value` always fully decodes
*and* walks the whole structure in one call, by design. That made
tinycbor look 6x smaller than it really is for equivalent work. Fixed:
every entry point below now fully decodes **and** recursively walks the
same 45-byte `small` payload (a 4-field map), summing every scalar and
string length into a `volatile` sink so the walk itself can't be
optimized away.

| | .text | .data | .bss | total |
|---|---|---|---|---|
| constexpr-cbor (heap) | 18,133 | 1,408 | 16 | 19,557 |
| constexpr-cbor (NoAlloc) | 20,693 | 2,056 | 3,112 | 25,861 |
| constexpr-cbor (SAX) | 4,803 | 592 | 16 | 5,411 |
| tinycbor | 6,517 | 600 | 16 | 7,133 |
| QCBOR | 6,013 | 616 | 16 | 6,645 |
| libcbor | 15,317 | 640 | 16 | 15,973 |

**Takeaways:**
- **SAX is the smallest of all six paths measured here**, including
  tinycbor and QCBOR — worth knowing if code size specifically, not
  throughput or allocation behavior, is the actual constraint. The
  structural reason is straightforward: SAX carries no tree-shaped data
  type, no allocator/bookkeeping object, and no owned-value
  representation at all — just the shared byte-parsing primitives this
  whole library already has, plus a small, fixed-size frame stack. It
  reuses constexpr-cbor's existing `decode_length`/`decode_uint`/
  `decode_negint`/`decode_byte_string`/`decode_text_string`/
  `decode_simple` unchanged, so almost none of its `.text` is new code
  this benchmark hasn't already measured elsewhere (heap and NoAlloc
  both pull in those same primitives too).
- **libcbor lands between the two extremes, as expected for a DOM-style C
  library**: 15,317 B — smaller than constexpr-cbor's heap path (18,133 B,
  no `std::variant`/template instantiation overhead, it's plain C), but
  more than double tinycbor/QCBOR's size, since it still needs to build
  and tear down (`cbor_decref`) a real tree with reference counting,
  unlike their pull-parsing. This is consistent with the Memory usage and
  CPU performance results above: being DOM-style costs something in every
  dimension, in both C and C++, not just in C++'s case.
- **The gap between heap/NoAlloc and tinycbor/QCBOR is real and
  structural**, not a measurement artifact: `std::variant`-based
  recursive descent through a tagged union, plus (for NoAlloc) a
  caller-tunable allocator object, generates meaningfully more machine
  code than a hand-written C state machine over a `switch` on raw
  major-type bytes. On a flash-constrained target where every kilobyte
  of `.text` is a hard constraint, that's a genuine reason to reach for
  tinycbor, QCBOR, or constexpr-cbor's own SAX path instead of heap/
  NoAlloc — this benchmark isn't trying to argue that gap away, only to
  measure it honestly, and to show that a design *within the same
  library* (SAX) closes it almost entirely without changing language or
  compiler.
- **Removing `IndexCell` and replacing it with pointer-based reference
  slots** (see "NoAlloc internals" above) shrank NoAlloc's `.text`/`.data`
  noticeably versus the original index-based design (22,252/2,504 B), but
  this build also adds the `NodePool::owns(const Node*)` checks' own
  branches on top of that saving, since these binaries aren't built with
  `-DNDEBUG` — the two effects partly offset, landing at 20,693/2,056 B
  rather than showing the full size of either change in isolation. `.bss`
  grew versus the original design (2,616 B to 3,112 B) for an unrelated
  reason: the three separately-typed, independently-sized `NoAllocStorage`
  arrays (`Node[]`, the new `Node*[]` reference-slot array, `uint8_t[]`)
  don't pack as tightly as the old design's two NTTP-sized arrays did for
  this entry point's particular chosen capacities — a caller sizing
  `MaxRefs` tightly for their own data shape would see a smaller number
  than this generic single-entry-point benchmark shows.
- **NoAlloc is still larger than heap** in this measurement (20,693 vs.
  18,133 B) — the larger `Node` variant, the separate reference-slot
  array, and the `owns()` checks still cost more code than the removed
  allocator calls save, for a single entry point. The zero-heap-allocation
  property doesn't come free in `.text` for a tree-building mode; SAX is
  the path that gets zero allocation *and* a smaller `.text` footprint
  than heap, at the cost of not building a tree at all.

## Correctness

`correctness.cpp` feeds 32 byte vectors to all five libraries (six
decode paths, counting constexpr-cbor's heap and SAX modes separately):
24 malformed/adversarial vectors (truncated length fields, reserved
additional-info values, underfilled definite containers, break-byte
misuse, mismatched indefinite-length string chunks, unclosed indefinite
forms, indefinite-length markers on types that can't have one, and
truncated string content against both a small and a deliberately huge
claimed length) plus 7 valid-structure controls and one deliberate
policy-difference case. Most of the malformed vectors are lifted
directly from `tests/test_malformed_input.cpp`, which already cites
tinycbor's `not_well_formed_cbor.h` and QCBOR's own test suite as sources
for several of them — this corpus asks the complementary question: when
constexpr-cbor refuses something, do tinycbor, QCBOR, and libcbor
actually refuse it too?

**Result: 0 / 32 disagreements with the RFC-correct expectation**
(excluding the one deliberate policy case below) — every library
correctly rejects every malformed vector and correctly accepts every
valid one. A few things surfaced while building this corpus that are
worth documenting on their own:

- **QCBOR's `QCBORDecode_GetNext()` loop alone cannot detect an
  underfilled/unclosed container** (e.g. `{0x81}` — an array announcing
  1 element with 0 actually present). It only surfaces via
  `QCBORDecode_Finish()`, which QCBOR's own documentation says a caller
  must call — this isn't a bug in QCBOR, just a sharper edge in its API
  contract than constexpr-cbor's (where both `decode_value` and
  `SaxRange` catch this themselves, with no second call required),
  tinycbor's, or libcbor's (same for both: `cbor_load()` alone already
  catches it).
- **The one deliberate disagreement**: `{0xa2, 0x01, 0x02, 0x01, 0x03}`
  (a structurally well-formed map with a duplicate key, `{1:2, 1:3}`).
  constexpr-cbor's heap/NoAlloc *default* policy (`RejectDuplicateKeys`)
  refuses it; tinycbor, QCBOR, libcbor, and constexpr-cbor's SAX path (its
  own default policy is `NoDuplicateKeyCheck`, a deliberately different
  default for a mode with no tree to retroactively fix up — see the
  top-level README) all accept it. This is intentional on
  constexpr-cbor's side — see the README's "Pluggable duplicate-map-key
  policies" section — not a structural-decode disagreement.
- **Getting libcbor's own checksum to agree took a real fix** — see
  "A bug this methodology actually caught" above. The accept/reject
  corpus here never actually exercised the buggy code path (none of
  these 32 vectors' integers happened to expose the width mismatch), so
  this particular correctness table would have shown 0 disagreements
  either way; it was specifically the CPU/memory benchmark's checksum
  comparison, not this corpus, that caught it. Worth noting as its own
  small methodology point: "accept/reject agreement" and "decoded-value
  agreement" are different, complementary checks, and this project's own
  bug only tripped the second one.

## Flexibility

A qualitative comparison, since "more flexible" isn't a single number.
Earlier competitive research for this project (cited in the top-level
README) also covered zcbor and NanoCBOR; those are included below from
documentation only (neither is built/benchmarked here — both are
narrower-scope, embedded-targeted pull-parsers, closer in spirit to
tinycbor/QCBOR than to the DOM-style comparison libcbor's inclusion is
specifically for).

| | constexpr-cbor | tinycbor | QCBOR | libcbor | zcbor / NanoCBOR (docs only) |
|---|---|---|---|---|---|
| Compile-time (`consteval`) evaluation | **Yes**, across all three decode paths — the entire test suite runs inside `consteval` functions | No (plain C) | No (plain C) | No (plain C) | No |
| Zero-heap decode mode | Yes (NoAlloc, SAX) | Yes (native) | Yes (native) | No (always heap + refcounting) | Yes |
| Generic owned-tree decode mode | Yes (heap `CborValue`, nlohmann-style ergonomics; NoAlloc offers the same tree shape without heap allocation) | No | No (spiffy-decode helpers exist but no generic tree) | Yes (`cbor_load`, always heap) | No |
| Pull/event-style decode mode | Yes (SAX, `std::ranges::forward_range`) | Yes (native) | Yes (native) | No | varies |
| Max nesting depth | Caller-tunable **per call** (`MaxDepth` NTTP, default 32, shared across all three decode paths) | Unbounded (recursion-stack-bound only — no built-in limit) | **Fixed at compile time** (`QCBOR_MAX_ARRAY_NESTING = 15`, a `#define`, not a per-call parameter) | Unbounded (recursion-stack-bound only, same as tinycbor) | varies |
| Pluggable UTF-8 validation | Yes (`NoUtf8Validation`/`Rough`/`Complete`, policy template param, shared across all three decode paths) | No validation | No validation | No validation | No (industry-wide gap, per earlier research) |
| Pluggable duplicate-key policy | Yes — heap/NoAlloc: `Reject`/`KeepFirst`/`KeepLast`; SAX: `Reject`/`KeepFirst`/`NoDuplicateKeyCheck` (no `KeepLast` — see top-level README for why a push-forward event stream can't support it) | No (silently accepts) | No (silently accepts) | No (silently accepts) | No |
| Exception-free | Yes (`std::optional`, proven to compile under `-fno-exceptions`) | Yes (C) | Yes (C) | Yes (C) | varies |
| Bignum (tag 2/3) as a native value, not an opaque tag | Yes (heap/NoAlloc; SAX surfaces it as `Tag` + `ByteString`, the same shape tinycbor/libcbor already use) | No (generic tag) | Yes (`QCBOR_TYPE_POSBIGNUM`/`NEGBIGNUM`) | No (generic tag) | varies |
| Browser/WebAssembly target proven in CI | Yes (`docker/wasm.Dockerfile`, `webapp/`) | Not out of the box | Not out of the box | Not out of the box | No |
| Code size (this benchmark, full decode+walk) | 5.4-21 KB `.text` for one call, depending on decode path | 6.5 KB | 6.0 KB | 15.3 KB | — |
| Memory allocated/decode (this benchmark) | 0 (NoAlloc/SAX) to ~158 KB (heap) | 0 | 0 | ~0.7-234 KB — allocates on every payload | — |
| Raw decode throughput (this benchmark) | 1-3,397 MB/s depending on mode/payload — see CPU performance above for the breakdown by payload shape | 2-1,891 MB/s | 2-2,264 MB/s (refused on `deep` in some runs) | 2-367 MB/s | — |

**How to read this table**: none of the above is meant to crown one
library "better" — it's meant to make the tradeoffs legible enough that
you can match a library to your own constraints. tinycbor and QCBOR are
mature, hand-optimized C libraries doing one job (pull-parsing) very
well, and their code-size numbers here are small for a reason: no
generic value representation, no allocator abstraction to parameterize,
decades of tuning for exactly this access pattern. libcbor's inclusion
is a control, not a competitor: it shows that the DOM-style cost (real
allocation, reference counting, more code than a pull-parser) is a cost
of that *design*, not of C++ or `std::variant` specifically — a plain-C
DOM-style library pays a comparable price in every dimension measured
here. constexpr-cbor's own three paths trade differently against each
other and against the other libraries depending on what's actually
needed: heap gives a generic, ergonomic owned tree; NoAlloc gives the
same tree shape with zero heap allocation, at a real `.text` cost; SAX
gives the pull-parsing style tinycbor/QCBOR already offer, with
comparable code size and comparable throughput, but reusing the exact
same `consteval`-provable, policy-templated primitives the other two
paths use rather than being a separate implementation. What's
structural and not timing-dependent, regardless of which way any one
payload's numbers lean on a given run, is that constexpr-cbor is the
only library here that (a) proves its own correctness at *compile time*
via `consteval`, (b) offers a generic owned-tree mode, a zero-alloc tree
mode, and a zero-alloc pull-parsing mode as equally first-class options
rather than being limited to whichever one its design happened to pick
(libcbor has an owned-tree mode but no zero-alloc alternative; tinycbor/
QCBOR have a zero-alloc pull-parsing mode but no owned-tree alternative),
and (c) exposes nesting depth, UTF-8 validation, and duplicate-key
handling as caller-chosen policies, shared across all three decode
paths, rather than fixed library behavior. Whether any of that is worth
its measured cost in a given dimension — code size, allocation, raw
throughput on your own data's shape — is a question only your own
constraints can answer; this benchmark exists to give real,
repeatedly-verified numbers to answer it with, not to supply the answer
itself.

## Files

- `gen_payloads.py` — regenerates `data/*.cbor` and `payloads.h` (the
  payload bytes embedded into the C++ benchmark, so the Docker build
  doesn't need Python).
- `bench.cpp` — the CPU/memory benchmark described above.
- `correctness.cpp` — the cross-library correctness corpus.
- `libcbor_adapter.hpp`/`.cpp` — the only translation unit that includes
  libcbor's own header, isolating its `extern "C"` symbol collision with
  tinycbor from `bench.cpp`/`correctness.cpp` (see "Methodology" above).
- `size/` — the six minimal single-entry-point sources used for the
  code-size comparison (`size_cc.cpp`/`size_noalloc.cpp`/`size_sax.cpp`
  for constexpr-cbor's three modes, `size_tc.c`/`size_qc.c`/
  `size_libcbor.c` for tinycbor/QCBOR/libcbor, `size_main.c` the shared
  trivial `main()`).
- `Dockerfile` — clones all three third-party libraries at pinned tags
  and builds/runs everything above in one `docker run`.
