// constexpr-cbor, no-malloc mode (ConstexprCbor::NoAlloc)
// ---------------------------------------------------------------------
// For environments without a heap at all (or that simply don't want one
// in the decode path): decodes into a NodePool, a small, non-owning view
// over storage the *caller* provides and sizes. Every decoded value is a
// Node living in that storage; there's no malloc anywhere in decode_value
// or encode_value below. See README.md's "No-malloc decoding/encoding"
// section for the full design rationale (why pointers into caller-owned
// storage are safe here, and the one precondition -- the storage must
// outlive every decoded reference into it -- that safety rests on).
#include <array>
#include <iostream>

#include "constexpr_cbor_noalloc_ergonomics.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;

namespace {

void decode_and_inspect() {
  std::cout << "--- decode into caller-owned storage + ergonomic access ---\n";

  // Same payload as heap_example.cpp: {"a": 1, "b": [2, 3]}.
  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  // NoAllocStorage<MaxNodes, MaxRefs, MaxBytes> is what the caller actually
  // owns (three plain arrays); NodePool is a trivially-copyable VIEW over
  // it, never itself the thing holding storage. Sized generously here --
  // see "Running out of capacity" below for what happens when it isn't.
  NoAllocStorage<32, 32, 32> storage;
  NodePool pool = storage.make_pool();

  // decode_view is the one-call ergonomic entry point: decode_value()
  // directly into a NoAllocView, nlohmann::json-style access on top of
  // the raw pool/Node* pair.
  const auto view = decode_view(pool, bytes);
  if (!view) {
    std::cout << "decode failed\n";
    return;
  }

  std::cout << "a = " << *(*view)["a"].get<std::uint64_t>() << '\n';
  std::cout << "b[1] = " << *(*view)["b"][1].get<std::uint64_t>() << '\n';

  for (const auto& v : (*view)["b"]) {
    std::cout << "b element: " << *v.get<std::uint64_t>() << '\n';
  }
}

void raw_pool_access() {
  std::cout << "\n--- the same decode, via the raw NodePool/Node* API ---\n";

  // NoAllocView above is sugar over exactly this -- useful to see once,
  // since a genuinely minimal embedded consumer may skip the ergonomics
  // header entirely and only pay for this layer.
  constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

  NoAllocStorage<16, 16, 16> storage;
  NodePool pool = storage.make_pool();

  const auto decoded = decode_value(pool, bytes);
  if (!decoded) {
    std::cout << "decode failed\n";
    return;
  }

  const auto& array = std::get<ArrayRef>(decoded->node->data);
  for (std::size_t i = 0; i < array.count; ++i) {
    const Node& element = pool.child(array, i);
    std::cout << "element " << i << " = " << std::get<std::uint64_t>(element.data) << '\n';
  }
}

void build_and_encode() {
  std::cout << "\n--- NoAllocBuilder + encode into a fixed buffer ---\n";

  NoAllocStorage<16, 16, 16> storage;
  NodePool pool = storage.make_pool();
  NoAllocBuilder builder{&pool};

  // Children must already be built (post-order) before the array/map that
  // references them -- no deferred/lazy construction, since every write
  // just advances the pool's own cursors.
  const auto a = builder.make_uint(10);
  const auto b = builder.make_uint(20);
  const std::array<NoAllocView, 2> children = {*a, *b};
  const auto array = builder.make_array(children);
  if (!array) {
    std::cout << "pool exhausted while building\n";
    return;
  }

  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, array->node, out);
  std::cout << "encoded " << (written ? *written : 0) << " bytes\n";
}

void capacity_exhaustion_is_a_refusal() {
  std::cout << "\n--- running out of capacity is a clean refusal, not UB ---\n";

  // [1, 2, 3] needs 4 Node slots (the array itself + 3 elements) and 3
  // reference slots -- this pool only has room for 2 nodes, so decode
  // must refuse rather than overrun anything.
  constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};
  NoAllocStorage<2, 2, 16> tiny_storage;
  NodePool tiny_pool = tiny_storage.make_pool();

  const auto decoded = decode_value(tiny_pool, bytes);
  std::cout << "decode into an undersized pool succeeded: " << decoded.has_value() << " (expected false)\n";
}

}  // namespace

int main() {
  decode_and_inspect();
  raw_pool_access();
  build_and_encode();
  capacity_exhaustion_is_a_refusal();
  return 0;
}
