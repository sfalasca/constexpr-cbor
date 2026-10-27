// A duplicate-key policy missing the right NoAlloc-shaped member function
// must fail to compile with a clear, named message -- the NoAlloc
// counterpart of wrong_shape_duplicate_key_policy.cpp, since
// NoAlloc::decode_value checks its own NoAlloc::is_duplicate_key_policy_v
// trait (the std::vector-shaped overload checked by the heap-only test
// doesn't satisfy it; NoAlloc::decode_value needs the NodePool-shaped
// on_duplicate instead).
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;

struct WrongShapeKeyPolicy {
  static constexpr bool on_duplicate(int) { return true; }  // wrong signature entirely
};

constexpr std::uint8_t vec[] = {0xa0};  // {}

constexpr bool trigger() {
  NoAllocStorage<16, 16, 32> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, WrongShapeKeyPolicy>(pool, vec);
  return result.has_value();
}
constexpr bool bad_instantiation = trigger();

int main() { return 0; }
