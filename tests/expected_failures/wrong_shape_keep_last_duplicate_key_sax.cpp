// KeepLastDuplicateKey has no SAX overload (it would overwrite an already-emitted
// event); selecting it with SaxRange must fail to compile.
#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Sax;

constexpr std::uint8_t vec[] = {0xa0};  // {}

constexpr bool trigger() {
  SaxRange<32, 4, NoUtf8Validation, KeepLastDuplicateKey> range{vec};
  for (auto ev : range) {
    (void)ev;
  }
  return true;
}
constexpr bool bad_instantiation = trigger();

int main() { return 0; }
