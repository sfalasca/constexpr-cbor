// .get<T>() on a T with no built-in support, ADL hook, or CborSerializer<T> must
// fail to compile with a clear message.
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

struct NoHookAtAll {
  int value;
};

constexpr CborValue v(std::uint64_t{1});
constexpr auto bad_instantiation = v.get<NoHookAtAll>();

int main() { return 0; }
