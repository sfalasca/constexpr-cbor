# Examples

One standalone, runnable `.cpp` file per decode mode, each commented to
stand on its own (not just a cross-reference into the main README):

- `heap_example.cpp` — `ConstexprCbor::decode_value`/`CborValue`: the
  default, most ergonomic mode. Decode + ergonomic access, brace-init
  construction + canonical re-encode, a user-type ADL hook
  (`to_cbor`/`from_cbor`), and the pluggable UTF-8/duplicate-key policies.
- `noalloc_example.cpp` — `ConstexprCbor::NoAlloc`: zero heap allocation,
  decoding into caller-owned storage. The same ergonomic access via
  `NoAllocView`, the raw `NodePool`/`Node*` API underneath it,
  `NoAllocBuilder`, and what happens when storage capacity runs out
  (a clean refusal, not UB).
- `sax_example.cpp` — `ConstexprCbor::Sax`: no tree at all, one event at
  a time. A raw event trace, folding a payload directly into a running
  value with no intermediate tree, composing `SaxRange` with
  `std::views::take`, and the duplicate-key policy tradeoffs specific to
  a single-pass decoder.

Build and run any of them:

```sh
cmake -S . -B build
cmake --build build --target heap_example noalloc_example sax_example
./build/examples/heap_example
./build/examples/noalloc_example
./build/examples/sax_example
```

These are plain runtime programs, not `consteval` tests — see `tests/`
for the compile-time-proven suite each mode's API is actually validated
against. The point here is readable, runnable output, not another layer
of proof.
