// constexpr-cbor, SAX mode (ConstexprCbor::Sax)
// ---------------------------------------------------------------------
// No tree is built at all here: SaxRange pulls one CBOR *event* at a
// time directly off the wire (BeginArray, Uint, TextString, EndMap, ...),
// the same shape tinycbor/QCBOR's own pull-style APIs already expose.
// Reach for this mode when you want to fold a payload into something of
// your own shape (a running sum, a custom struct, a different tree
// entirely) without paying for an intermediate CborValue/Node tree you'd
// just throw away, or when you want to bail out after the first few
// events without decoding the rest of a large payload at all.
#include <iostream>
#include <ranges>
#include <string_view>

#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Sax;

namespace {

// Turns an EventKind into a short label -- useful for the kind of plain
// event trace a SAX consumer typically wants to print while developing
// against a new payload shape.
const char* kind_name(EventKind kind) {
  switch (kind) {
    case EventKind::BeginArray:
      return "BeginArray";
    case EventKind::EndArray:
      return "EndArray";
    case EventKind::BeginMap:
      return "BeginMap";
    case EventKind::EndMap:
      return "EndMap";
    case EventKind::Uint:
      return "Uint";
    case EventKind::NegInt:
      return "NegInt";
    case EventKind::Bool:
      return "Bool";
    case EventKind::Null:
      return "Null";
    case EventKind::Undefined:
      return "Undefined";
    case EventKind::Float:
      return "Float";
    case EventKind::TextString:
      return "TextString";
    case EventKind::ByteString:
      return "ByteString";
    case EventKind::Tag:
      return "Tag";
    case EventKind::Simple:
      return "Simple";
    case EventKind::Error:
      return "Error";
  }
  return "?";
}

void trace_every_event() {
  std::cout << "--- raw event trace ---\n";

  // {"a": 1, "b": [2, 3]} -- same payload the other two example files use.
  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  // SaxRange is a genuine std::ranges::forward_range (not just "has
  // begin()/end()"): every byte of iteration state lives by value inside
  // the iterator itself, with no shared external cursor, so it composes
  // with range-for exactly like any other range.
  for (const Event& ev : SaxRange<32>{bytes}) {
    std::cout << kind_name(ev.kind);
    // `context` tells you the event's role in its immediate parent --
    // the only thing that distinguishes "this Uint is a map key" from
    // "this Uint is a map value" from "this Uint is an array element",
    // since there's no separate event kind per role.
    if (ev.kind == EventKind::Uint) std::cout << "(" << ev.uint_value << ")";
    if (ev.kind == EventKind::TextString) std::cout << "(\"" << std::string_view(
        reinterpret_cast<const char*>(ev.bytes.data()), ev.bytes.size()) << "\")";
    std::cout << '\n';
  }
}

// A plain running fold, built directly out of the event stream -- no
// intermediate tree, so this scales to a payload far larger than you'd
// want to hold as a CborValue/Node tree in memory at once.
void fold_without_a_tree() {
  std::cout << "\n--- folding events directly, no tree built ---\n";

  constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

  std::uint64_t sum = 0;
  for (const auto& ev : SaxRange<32>{bytes}) {
    if (ev.kind == EventKind::Uint) sum += ev.uint_value;
  }
  std::cout << "sum = " << sum << " (expected 6)\n";
}

// SaxRange implements std::ranges::view_interface, so it composes with
// ordinary range adaptors -- here, stopping after the first 3 events
// without ever decoding the rest of the array.
void compose_with_range_adaptors() {
  std::cout << "\n--- composes with range adaptors (std::views::take) ---\n";

  constexpr std::uint8_t bytes[] = {0x85, 0x01, 0x02, 0x03, 0x04, 0x05};  // [1, 2, 3, 4, 5]

  for (const auto& ev : SaxRange<32>{bytes} | std::views::take(3)) {
    std::cout << kind_name(ev.kind) << '\n';
  }
}

// Duplicate-key detection needs its own opt-in here: a single-pass event
// stream has no materialized "entries so far" to scan the way the heap/
// NoAlloc tree decoders do, so tracking capacity is a template parameter
// (MaxTrackedKeys), and the default policy (NoDuplicateKeyCheck) tracks
// nothing at all -- the right default for a mode whose whole point is
// not retaining state. KeepLastDuplicateKey is not offered here: it
// would mean retroactively overwriting an already-emitted event, which
// a push-forward stream structurally cannot do.
void duplicate_key_policy() {
  std::cout << "\n--- duplicate-key policy, opted into via MaxTrackedKeys ---\n";

  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1: 2, 1: 3}

  // Default policy: no tracking, no rejection -- the duplicate pair
  // passes straight through as two ordinary key/value event pairs.
  bool saw_error = false;
  for (const auto& ev : SaxRange<32>{duplicate_key_map}) {
    if (ev.kind == EventKind::Error) saw_error = true;
  }
  std::cout << "default policy (NoDuplicateKeyCheck) sees an error: " << saw_error << " (expected false)\n";

  // Opted into RejectDuplicateKeys, with capacity for 4 tracked keys --
  // this map only needs 1 (the first "1"), but sizing generously costs
  // nothing beyond the storage itself.
  saw_error = false;
  for (const auto& ev : (SaxRange<32, 4, NoUtf8Validation, RejectDuplicateKeys>{duplicate_key_map})) {
    if (ev.kind == EventKind::Error) saw_error = true;
  }
  std::cout << "RejectDuplicateKeys sees an error: " << saw_error << " (expected true)\n";
}

}  // namespace

int main() {
  trace_every_event();
  fold_without_a_tree();
  compose_with_range_adaptors();
  duplicate_key_policy();
  return 0;
}
