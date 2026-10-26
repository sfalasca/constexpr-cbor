// Fair code-size comparison: decodes AND fully walks the same payload
// every other size_*.c/cpp in this directory decodes and walks (see
// README.md's "a fairer size comparison" section) -- not just "parse
// the first item," which undersells libraries that lazily walk on
// demand versus this one's eager, single-call decode_value.
#include "../payloads.h"
#include "constexpr_cbor.hpp"

namespace {
volatile std::uint64_t sink = 0;

void walk(const ConstexprCbor::CborValue& v) {
  using CV = ConstexprCbor::CborValue;
  sink = sink + 1;
  std::visit(
      [](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::vector<CV>>) {
          for (const auto& e : x) walk(e);
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CV, CV>>>) {
          for (const auto& [k, val] : x) {
            walk(k);
            walk(val);
          }
        } else if constexpr (std::is_same_v<T, CV::Tag>) {
          walk(x.value.front());
        } else if constexpr (std::is_same_v<T, CV::OwnedString>) {
          sink += x.content.size();
        }
      },
      v.data);
}
}  // namespace

extern "C" bool decode_entry(const unsigned char* p, unsigned long n) {
  const auto decoded = ConstexprCbor::decode_value(std::span<const std::uint8_t>(p, n));
  if (!decoded) return false;
  walk(decoded->first);
  return true;
}
