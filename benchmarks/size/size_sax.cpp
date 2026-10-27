// See size_cc.cpp's comment -- same fairness fix, SAX mode: decodes AND
// fully walks every event, not just the first one.
#include "constexpr_cbor_sax.hpp"

namespace {
volatile std::uint64_t sink = 0;
}  // namespace

extern "C" bool decode_entry(const unsigned char* p, unsigned long n) {
  using EK = ConstexprCbor::Sax::EventKind;
  bool ok = true;
  for (auto ev : ConstexprCbor::Sax::SaxRange<32>{std::span<const std::uint8_t>(p, n)}) {
    if (ev.kind == EK::Error) ok = false;
    sink = sink + 1;
    if (ev.kind == EK::TextString || ev.kind == EK::ByteString) sink += ev.bytes.size();
  }
  return ok;
}
