// constexpr-cbor SAX mode: SaxRange yields one event at a time (BeginArray, Uint,
// TextString, ...) without building a tree. Use it to fold a payload into your
// own shape, or to stop early.
#include <iostream>
#include <ranges>
#include <string_view>

#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Sax;

namespace {

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

  // {"a": 1, "b": [2, 3]}
  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  for (const Event& ev : SaxRange<32>{bytes}) {
    std::cout << kind_name(ev.kind);
    // `context` is the event's role in its parent (map key, map value, array element).
    if (ev.kind == EventKind::Uint) std::cout << "(" << ev.uint_value << ")";
    if (ev.kind == EventKind::TextString) std::cout << "(\"" << std::string_view(
        reinterpret_cast<const char*>(ev.bytes.data()), ev.bytes.size()) << "\")";
    std::cout << '\n';
  }
}

// Running fold over the event stream; no intermediate tree.
void fold_without_a_tree() {
  std::cout << "\n--- folding events directly, no tree built ---\n";

  constexpr std::uint8_t bytes[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

  std::uint64_t sum = 0;
  for (const auto& ev : SaxRange<32>{bytes}) {
    if (ev.kind == EventKind::Uint) sum += ev.uint_value;
  }
  std::cout << "sum = " << sum << " (expected 6)\n";
}

// SaxRange is a std::ranges view: stop after the first 3 events without decoding the rest.
void compose_with_range_adaptors() {
  std::cout << "\n--- composes with range adaptors (std::views::take) ---\n";

  constexpr std::uint8_t bytes[] = {0x85, 0x01, 0x02, 0x03, 0x04, 0x05};  // [1, 2, 3, 4, 5]

  for (const auto& ev : SaxRange<32>{bytes} | std::views::take(3)) {
    std::cout << kind_name(ev.kind) << '\n';
  }
}

// Duplicate-key detection is opt-in: tracking capacity is the MaxTrackedKeys
// template parameter, and the default NoDuplicateKeyCheck tracks nothing.
// KeepLastDuplicateKey is not available in SAX mode.
void duplicate_key_policy() {
  std::cout << "\n--- duplicate-key policy, opted into via MaxTrackedKeys ---\n";

  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1: 2, 1: 3}

  // Default policy: the duplicate pair passes through as two ordinary pairs.
  bool saw_error = false;
  for (const auto& ev : SaxRange<32>{duplicate_key_map}) {
    if (ev.kind == EventKind::Error) saw_error = true;
  }
  std::cout << "default policy (NoDuplicateKeyCheck) sees an error: " << saw_error << " (expected false)\n";

  // RejectDuplicateKeys with room for 4 tracked keys.
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
