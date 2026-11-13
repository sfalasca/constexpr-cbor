// constexpr-cbor no-malloc mode: decodes into a NodePool, a non-owning view over
// caller-provided storage. The storage must outlive every decoded reference
// into it (see README).
#include <array>
#include <iostream>

#include "constexpr_cbor_noalloc_ergonomics.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;

namespace {

void decode_and_inspect() {
  std::cout << "--- decode into caller-owned storage + ergonomic access ---\n";

  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  // NoAllocStorage<MaxNodes, MaxRefs, MaxBytes> is the caller-owned storage;
  // NodePool is a view over it.
  NoAllocStorage<32, 32, 32> storage;
  NodePool pool = storage.make_pool();

  // decode_view decodes straight into a NoAllocView.
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

  // The raw pool/Node* layer that NoAllocView wraps.
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

  // Children must be built before the array/map that references them.
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

  // Needs 4 nodes and 3 reference slots; the pool has 2 nodes, so decode fails
  // instead of overrunning.
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
