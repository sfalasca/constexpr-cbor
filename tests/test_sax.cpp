#include <ranges>

#include "compile_time_ut.hpp"
#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Sax;
using namespace CompileTimeUnitTesting;

// SaxRange's whole iteration state lives by value inside the iterator (no
// shared external cursor the way istream_iterator has), so it should be a
// genuine forward_range, not just input_range -- verified structurally,
// not just asserted in a comment.
static_assert(std::ranges::forward_range<SaxRange<32>>);
static_assert(std::ranges::view<SaxRange<32>>);

// Every helper below is a small, self-contained constexpr function (its
// own SaxRange declared and consumed entirely inside its own body),
// matching test_noalloc.cpp's established convention for the same reason:
// compile_time_ut's expect_eq<>()/expect_true<>() need one fresh,
// self-contained expression per check.

constexpr bool sax_decode_ok(std::span<const std::uint8_t> bytes) {
  for (auto ev : SaxRange<32>{bytes}) {
    if (ev.kind == EventKind::Error) return false;
  }
  return true;
}

constexpr std::size_t sax_event_count(std::span<const std::uint8_t> bytes) {
  std::size_t n = 0;
  for (auto ev : SaxRange<32>{bytes}) {
    (void)ev;
    ++n;
  }
  return n;
}

constexpr EventKind sax_nth_kind(std::span<const std::uint8_t> bytes, std::size_t n) {
  std::size_t i = 0;
  for (auto ev : SaxRange<32>{bytes}) {
    if (i == n) return ev.kind;
    ++i;
  }
  return EventKind::Error;
}

constexpr EventContext sax_nth_context(std::span<const std::uint8_t> bytes, std::size_t n) {
  std::size_t i = 0;
  for (auto ev : SaxRange<32>{bytes}) {
    if (i == n) return ev.context;
    ++i;
  }
  return EventContext::TopLevel;
}

constexpr std::uint64_t sax_nth_uint_value(std::span<const std::uint8_t> bytes, std::size_t n) {
  std::size_t i = 0;
  for (auto ev : SaxRange<32>{bytes}) {
    if (i == n) return ev.uint_value;
    ++i;
  }
  return 0;
}

constexpr bool sax_nth_has_count(std::span<const std::uint8_t> bytes, std::size_t n) {
  std::size_t i = 0;
  for (auto ev : SaxRange<32>{bytes}) {
    if (i == n) return ev.has_count;
    ++i;
  }
  return false;
}

// RFC 8949 Appendix A: [1, 2, 3] -> 0x83010203. Same vector test_array.cpp
// and test_noalloc.cpp use, proving the SAX decoder reaches the same
// values through five events instead of a tree.
consteval bool test_decode_flat_array_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  expect_true<sax_decode_ok(vec)>();
  expect_eq<sax_event_count(vec), std::size_t{5}>();  // BeginArray, 1, 2, 3, EndArray
  expect_eq<sax_nth_kind(vec, 0), EventKind::BeginArray>();
  expect_eq<sax_nth_context(vec, 0), EventContext::TopLevel>();
  expect_eq<sax_nth_uint_value(vec, 0), std::uint64_t{3}>();  // the array's own count
  expect_true<sax_nth_has_count(vec, 0)>();
  expect_eq<sax_nth_kind(vec, 1), EventKind::Uint>();
  expect_eq<sax_nth_context(vec, 1), EventContext::ArrayElement>();
  expect_eq<sax_nth_uint_value(vec, 1), std::uint64_t{1}>();
  expect_eq<sax_nth_uint_value(vec, 3), std::uint64_t{3}>();
  expect_eq<sax_nth_kind(vec, 4), EventKind::EndArray>();
  return true;
}
static_assert(test_decode_flat_array_matches_official_vector());

// RFC 8949 Appendix A: [_ 1, 2, 3] -> 0x9f010203ff.
consteval bool test_decode_indefinite_array() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  expect_true<sax_decode_ok(vec)>();
  expect_eq<sax_event_count(vec), std::size_t{5}>();
  expect_false<sax_nth_has_count(vec, 0)>();
  expect_eq<sax_nth_kind(vec, 4), EventKind::EndArray>();
  return true;
}
static_assert(test_decode_indefinite_array());

// RFC 8949 Appendix A: {1: 2, 3: 4} -> 0xa201020304.
consteval bool test_decode_flat_map() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  expect_true<sax_decode_ok(vec)>();
  expect_eq<sax_event_count(vec), std::size_t{6}>();  // BeginMap, k,v,k,v, EndMap
  expect_eq<sax_nth_kind(vec, 0), EventKind::BeginMap>();
  expect_eq<sax_nth_context(vec, 1), EventContext::MapKey>();
  expect_eq<sax_nth_uint_value(vec, 1), std::uint64_t{1}>();
  expect_eq<sax_nth_context(vec, 2), EventContext::MapValue>();
  expect_eq<sax_nth_uint_value(vec, 2), std::uint64_t{2}>();
  expect_eq<sax_nth_kind(vec, 5), EventKind::EndMap>();
  return true;
}
static_assert(test_decode_flat_map());

// Nested array [1, [2, 3]] -> 0x8201820203, exercising frame-stack push/pop
// and the parent's remaining-count bookkeeping after a child frame closes.
consteval bool test_decode_nested_array() {
  constexpr std::uint8_t vec[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  expect_true<sax_decode_ok(vec)>();
  expect_eq<sax_event_count(vec), std::size_t{7}>();  // Begin,1,Begin,2,3,End,End
  expect_eq<sax_nth_kind(vec, 2), EventKind::BeginArray>();
  expect_eq<sax_nth_context(vec, 2), EventContext::ArrayElement>();
  expect_eq<sax_nth_kind(vec, 5), EventKind::EndArray>();
  expect_eq<sax_nth_kind(vec, 6), EventKind::EndArray>();
  return true;
}
static_assert(test_decode_nested_array());

// A single tagged value: tag 32 ("URI") wrapping the 1-byte text "a" ->
// 0xd8206161. Tag does not push a frame or get its own End event -- the
// tagged value's event(s) immediately follow, same context as the tag.
consteval bool test_decode_tag() {
  constexpr std::uint8_t vec[] = {0xd8, 0x20, 0x61, 0x61};
  expect_true<sax_decode_ok(vec)>();
  expect_eq<sax_event_count(vec), std::size_t{2}>();  // Tag, TextString
  expect_eq<sax_nth_kind(vec, 0), EventKind::Tag>();
  expect_eq<sax_nth_uint_value(vec, 0), std::uint64_t{32}>();
  expect_eq<sax_nth_kind(vec, 1), EventKind::TextString>();
  expect_eq<sax_nth_context(vec, 1), EventContext::TopLevel>();
  return true;
}
static_assert(test_decode_tag());

// A chain of nested tags deep enough to exceed MaxDepth must refuse, the
// same contract as the heap/NoAlloc decoders -- enforced here purely for
// consistency (a SAX tag chain costs no native call-stack depth, so this
// decoder doesn't structurally need the cap the way recursive decoders do).
constexpr std::array<std::uint8_t, 7> make_tag_chain() {
  // Six tags (0xc6 is tag 6, one byte each: major type 6, additional info
  // <24) wrapping a single true (0xf5) -- 7 bytes, 6 levels deep.
  return {0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xf5};
}

constexpr bool sax_decode_ok_with_depth(std::span<const std::uint8_t> bytes, std::size_t max_depth) {
  if (max_depth == 3) {
    for (auto ev : SaxRange<3>{bytes}) {
      if (ev.kind == EventKind::Error) return false;
    }
    return true;
  }
  for (auto ev : SaxRange<8>{bytes}) {
    if (ev.kind == EventKind::Error) return false;
  }
  return true;
}

consteval bool test_tag_chain_respects_max_depth() {
  constexpr auto vec = make_tag_chain();
  expect_false<sax_decode_ok_with_depth(vec, 3)>();
  expect_true<sax_decode_ok_with_depth(vec, 8)>();
  return true;
}
static_assert(test_tag_chain_respects_max_depth());

// Malformed input (truncated array header) is a clean refusal, not UB.
consteval bool test_decode_refuses_truncated_input() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01};  // indefinite array, never closed
  expect_false<sax_decode_ok(vec)>();
  return true;
}
static_assert(test_decode_refuses_truncated_input());

// Duplicate-key handling: {1: 2, 1: 3} -> 0xa20102010203, with
// MaxTrackedKeys sized for the map and RejectDuplicateKeys/
// KeepFirstDuplicateKey selected explicitly (SaxRange's own default,
// NoDuplicateKeyCheck, never tracks anything and so never rejects).

constexpr bool sax_decode_ok_with_policy_reject(std::span<const std::uint8_t> bytes) {
  for (auto ev : (SaxRange<32, 4, NoUtf8Validation, RejectDuplicateKeys>{bytes})) {
    if (ev.kind == EventKind::Error) return false;
  }
  return true;
}

constexpr std::size_t sax_event_count_keep_first(std::span<const std::uint8_t> bytes) {
  std::size_t n = 0;
  for (auto ev : (SaxRange<32, 4, NoUtf8Validation, KeepFirstDuplicateKey>{bytes})) {
    (void)ev;
    ++n;
  }
  return n;
}

constexpr std::uint64_t sax_nth_uint_value_keep_first(std::span<const std::uint8_t> bytes, std::size_t n) {
  std::size_t i = 0;
  for (auto ev : (SaxRange<32, 4, NoUtf8Validation, KeepFirstDuplicateKey>{bytes})) {
    if (ev.kind == EventKind::Uint) {
      if (i == n) return ev.uint_value;
      ++i;
    }
  }
  return 0;
}

consteval bool test_duplicate_key_policies() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1: 2, 1: 3}
  expect_false<sax_decode_ok_with_policy_reject(vec)>();
  // KeepFirstDuplicateKey: the second pair's events are dropped entirely --
  // BeginMap, key 1, value 2, EndMap (the duplicate pair never surfaces).
  expect_eq<sax_event_count_keep_first(vec), std::size_t{4}>();
  expect_eq<sax_nth_uint_value_keep_first(vec, 0), std::uint64_t{1}>();
  expect_eq<sax_nth_uint_value_keep_first(vec, 1), std::uint64_t{2}>();
  return true;
}
static_assert(test_duplicate_key_policies());

// No distinct keys beyond MaxTrackedKeys' capacity is a clean refusal, the
// same stance as every other capacity exhaustion in this codec.
constexpr bool sax_decode_ok_tiny_key_capacity(std::span<const std::uint8_t> bytes) {
  for (auto ev : (SaxRange<32, 1, NoUtf8Validation, RejectDuplicateKeys>{bytes})) {
    if (ev.kind == EventKind::Error) return false;
  }
  return true;
}

consteval bool test_duplicate_key_tracking_capacity_exhausted() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};  // {1: 2, 3: 4}, two distinct keys
  expect_false<sax_decode_ok_tiny_key_capacity(vec)>();
  return true;
}
static_assert(test_duplicate_key_tracking_capacity_exhausted());

// Text string content is zero-copy -- the span points directly into the
// original input, same discipline as decode_text_string itself.
constexpr bool sax_text_is_zero_copy(std::span<const std::uint8_t> bytes) {
  for (auto ev : SaxRange<32>{bytes}) {
    if (ev.kind == EventKind::TextString) {
      return ev.bytes.data() == bytes.data() + 1 && ev.bytes.size() == 2;
    }
  }
  return false;
}

consteval bool test_text_string_zero_copy() {
  constexpr std::uint8_t vec[] = {0x62, 'h', 'i'};  // "hi"
  expect_true<sax_text_is_zero_copy(vec)>();
  return true;
}
static_assert(test_text_string_zero_copy());

// AsciiOnlyValidation rejects a text string containing a non-ASCII byte,
// the same Utf8Policy type used by the other two decoders, unchanged.
constexpr bool sax_decode_ok_with_utf8_policy(std::span<const std::uint8_t> bytes) {
  for (auto ev : (SaxRange<32, 0, AsciiOnlyValidation>{bytes})) {
    if (ev.kind == EventKind::Error) return false;
  }
  return true;
}

consteval bool test_utf8_policy_applies() {
  constexpr std::uint8_t vec[] = {0x62, 0xc3, 0xa9};  // 2-byte UTF-8 (non-ASCII)
  expect_false<sax_decode_ok_with_utf8_policy(vec)>();
  return true;
}
static_assert(test_utf8_policy_applies());

int main() { return 0; }
