// KeepLastDuplicateKey has no SAX-shaped on_duplicate() overload by design
// (see its own comment in constexpr_cbor.hpp): "keep the last value for
// this key" means retroactively overwriting an already-emitted event, which
// a push-forward event stream cannot do without buffering the whole map
// first. Selecting it with SaxRange must fail to compile, not silently
// misbehave or silently fall back to some other policy's semantics.
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
