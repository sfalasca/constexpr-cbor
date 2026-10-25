// A duplicate-key policy missing the right Arena-shaped member template
// must fail to compile with a clear, named message -- the Arena
// counterpart of wrong_shape_duplicate_key_policy.cpp, since
// Arena::decode_value checks its own Arena::is_duplicate_key_policy_v
// trait (the std::vector-shaped overload checked by the heap-only test
// doesn't satisfy it; Arena::decode_value needs the templated
// NodePool-shaped on_duplicate instead).
#include "constexpr_cbor_arena.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Arena;

struct WrongShapeKeyPolicy {
  static constexpr bool on_duplicate(int) { return true; }  // wrong signature entirely, not even a template
};

constexpr std::uint8_t vec[] = {0xa0};  // {}

constexpr bool trigger() {
  NodePool<16, 16> pool;
  const auto result = decode_value<32, NoUtf8Validation, WrongShapeKeyPolicy>(pool, vec);
  return result.has_value();
}
constexpr bool bad_instantiation = trigger();

int main() { return 0; }
