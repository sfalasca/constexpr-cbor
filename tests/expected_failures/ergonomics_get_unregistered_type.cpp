// .get<T>() on a T with no built-in support, no ADL to_cbor/from_cbor hook,
// and no CborSerializer<T> specialization must fail to compile with a
// clear, named message -- the one deliberate compile-time-failure surface
// in the whole ergonomic interface (every runtime/consteval-execution-time
// mismatch instead returns std::nullopt, never throws, never hard-errors).
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

struct NoHookAtAll {
  int value;
};

constexpr CborValue v(std::uint64_t{1});
constexpr auto bad_instantiation = v.get<NoHookAtAll>();

int main() { return 0; }
