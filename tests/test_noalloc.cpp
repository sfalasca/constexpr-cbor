#include "compile_time_ut.hpp"
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;
using namespace CompileTimeUnitTesting;

// Every check in this file goes through compile_time_ut's expect_true<>()/
// expect_eq<>() NTTP forms. Those require both operands to be converted
// constant expressions formed fresh, with no named variable carried in
// from an earlier statement -- which initially looked like a hard blocker
// here, since NoAlloc::decode_value takes its NodePool by non-const
// reference, so the pool itself can never be a constexpr input the way
// sibling files' plain byte vectors are. The actual rule is narrower than
// that, though: a variable of automatic storage duration is fine inside a
// constant expression as long as its lifetime began within that *same*
// evaluation. A plain constexpr helper function, with its own
// NoAllocStorage/NodePool declared and consumed entirely inside its own
// body, is exactly that -- the call `some_helper(vec) == expected` is one
// self-contained expression, so it's usable directly as an expect_true<>()
// argument.

constexpr bool noalloc_decode_ok(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  return decode_value(pool, bytes).has_value();
}

constexpr std::size_t noalloc_bytes_consumed(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  return result ? result->bytes_consumed : std::size_t{0};
}

constexpr std::size_t noalloc_array_count(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  return std::get<ArrayRef>(result->node->data).count;
}

constexpr std::uint64_t noalloc_array_child_u64(std::span<const std::uint8_t> bytes, std::size_t i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& arr = std::get<ArrayRef>(result->node->data);
  return std::get<std::uint64_t>(pool.child(arr, i).data);
}

// RFC 8949 Appendix A: [1, 2, 3] -> 0x83010203. Same vector test_array.cpp
// uses for the heap-based decoder, proving the noalloc one reaches the same
// result through pointer-based reference slots instead of std::vector.
consteval bool test_decode_flat_array_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  expect_true<noalloc_decode_ok(vec)>();
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{4}>();
  expect_eq<noalloc_array_count(vec), std::size_t{3}>();
  expect_eq<noalloc_array_child_u64(vec, 0), std::uint64_t{1}>();
  expect_eq<noalloc_array_child_u64(vec, 2), std::uint64_t{3}>();
  return true;
}
static_assert(test_decode_flat_array_matches_official_vector());

// RFC 8949 Appendix A: [_ 1, 2, 3] -> 0x9f010203ff. Exercises the
// count-then-reserve pre-pass (count_indefinite_array_items) this decoder
// needs that the heap-based one doesn't: a fixed-capacity pool can't grow
// a slot one element at a time the way std::vector::push_back does.
consteval bool test_decode_indefinite_array_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{5}>();
  expect_eq<noalloc_array_count(vec), std::size_t{3}>();
  expect_eq<noalloc_array_child_u64(vec, 2), std::uint64_t{3}>();
  return true;
}
static_assert(test_decode_indefinite_array_matches_official_vector());

constexpr std::uint64_t noalloc_nested_array_child_u64(std::span<const std::uint8_t> bytes, std::size_t outer_i,
                                                       std::size_t inner_i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& outer = std::get<ArrayRef>(result->node->data);
  const auto& inner = std::get<ArrayRef>(pool.child(outer, outer_i).data);
  return std::get<std::uint64_t>(pool.child(inner, inner_i).data);
}

// RFC 8949 Appendix A: [1, [2, 3]] isn't a Table 6 row verbatim, but is
// built from two rows in it ([1,2,3] and nested-array handling above);
// proves the reference-slot indirection resolves correctly when a child is
// itself a composite that needed its own multi-node subtree.
consteval bool test_decode_array_handles_nesting() {
  constexpr std::uint8_t vec[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{5}>();
  expect_eq<noalloc_array_count(vec), std::size_t{2}>();
  expect_eq<noalloc_array_child_u64(vec, 0), std::uint64_t{1}>();
  expect_eq<noalloc_nested_array_child_u64(vec, 1, 0), std::uint64_t{2}>();
  expect_eq<noalloc_nested_array_child_u64(vec, 1, 1), std::uint64_t{3}>();
  return true;
}
static_assert(test_decode_array_handles_nesting());

template <typename KeyPolicy = RejectDuplicateKeys>
constexpr std::size_t noalloc_map_count(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, KeyPolicy>(pool, bytes);
  if (!result) return 0;
  return std::get<MapRef>(result->node->data).count;
}

template <typename KeyPolicy = RejectDuplicateKeys>
constexpr std::uint64_t noalloc_map_key_u64(std::span<const std::uint8_t> bytes, std::size_t i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, KeyPolicy>(pool, bytes);
  if (!result) return 0;
  const auto& m = std::get<MapRef>(result->node->data);
  return std::get<std::uint64_t>(pool.map_key(m, i).data);
}

template <typename KeyPolicy = RejectDuplicateKeys>
constexpr std::uint64_t noalloc_map_value_u64(std::span<const std::uint8_t> bytes, std::size_t i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, KeyPolicy>(pool, bytes);
  if (!result) return 0;
  const auto& m = std::get<MapRef>(result->node->data);
  return std::get<std::uint64_t>(pool.map_value(m, i).data);
}

// RFC 8949 Appendix A: {1: 2, 3: 4} -> 0xa201020304.
consteval bool test_decode_map_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{5}>();
  expect_eq<noalloc_map_count(vec), std::size_t{2}>();
  expect_eq<noalloc_map_key_u64(vec, 0), std::uint64_t{1}>();
  expect_eq<noalloc_map_value_u64(vec, 0), std::uint64_t{2}>();
  expect_eq<noalloc_map_key_u64(vec, 1), std::uint64_t{3}>();
  expect_eq<noalloc_map_value_u64(vec, 1), std::uint64_t{4}>();
  return true;
}
static_assert(test_decode_map_matches_official_vector());

// Indefinite-length map, same pre-count-then-reserve mechanism as the
// indefinite array above but walking key/value pairs
// (count_indefinite_map_pairs).
consteval bool test_decode_indefinite_map() {
  constexpr std::uint8_t vec[] = {0xbf, 0x01, 0x02, 0x03, 0x04, 0xff};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{6}>();
  expect_eq<noalloc_map_count(vec), std::size_t{2}>();
  expect_eq<noalloc_map_value_u64(vec, 1), std::uint64_t{4}>();
  return true;
}
static_assert(test_decode_indefinite_map());

constexpr bool noalloc_string_is_bytes_kind(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return false;
  return std::get<StringRef>(result->node->data).kind == CborValue::StringKind::Bytes;
}

constexpr std::size_t noalloc_string_length(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  return std::get<StringRef>(result->node->data).length;
}

constexpr std::uint8_t noalloc_string_byte_at(std::span<const std::uint8_t> bytes, std::size_t i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& s = std::get<StringRef>(result->node->data);
  return pool.bytes_of(s)[i];
}

// RFC 8949 Appendix A: (_ h'0102', h'030405') -> 0x5f42010243030405ff.
// Proves chunk content lands contiguously in the byte noalloc via repeated
// allocate_bytes calls, the same incremental-append discipline the
// heap-based decoder's content.insert() uses.
consteval bool test_decode_indefinite_byte_string() {
  constexpr std::uint8_t vec[] = {0x5f, 0x42, 0x01, 0x02, 0x43, 0x03, 0x04, 0x05, 0xff};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{9}>();
  expect_true<noalloc_string_is_bytes_kind(vec)>();
  expect_eq<noalloc_string_length(vec), std::size_t{5}>();
  expect_eq<noalloc_string_byte_at(vec, 0), std::uint8_t{0x01}>();
  expect_eq<noalloc_string_byte_at(vec, 4), std::uint8_t{0x05}>();
  return true;
}
static_assert(test_decode_indefinite_byte_string());

constexpr std::size_t noalloc_string_length_with_policy(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, ConstexprCbor::CompleteUtf8Validation>(pool, bytes);
  if (!result) return 0;
  return std::get<StringRef>(result->node->data).length;
}

constexpr bool noalloc_string_is_text_kind_with_policy(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, ConstexprCbor::CompleteUtf8Validation>(pool, bytes);
  if (!result) return false;
  return std::get<StringRef>(result->node->data).kind == CborValue::StringKind::Text;
}

// RFC 8949 Appendix A: "IETF" -> 0x6449455446, with the same
// CompleteUtf8Validation policy test_textstring.cpp exercises on the
// heap-based decoder -- proves the policy template parameter threads
// through NoAlloc::decode_value identically.
consteval bool test_decode_text_string_with_utf8_policy() {
  constexpr std::uint8_t vec[] = {0x64, 0x49, 0x45, 0x54, 0x46};
  expect_true<noalloc_string_is_text_kind_with_policy(vec)>();
  expect_eq<noalloc_string_length_with_policy(vec), std::size_t{4}>();
  return true;
}
static_assert(test_decode_text_string_with_utf8_policy());

constexpr bool noalloc_bignum_negative(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return false;
  return std::get<BigNumRef>(result->node->data).negative;
}

constexpr std::size_t noalloc_bignum_length(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  return std::get<BigNumRef>(result->node->data).length;
}

constexpr std::uint8_t noalloc_bignum_byte_at(std::span<const std::uint8_t> bytes, std::size_t i) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& b = std::get<BigNumRef>(result->node->data);
  return pool.bytes_of(b)[i];
}

// RFC 8949 Appendix A: 2(h'010203') -> 0xc243010203 (positive bignum).
// Same automatic tag-2/3 recognition as the heap-based decoder, just
// producing a BigNumRef into the byte noalloc instead of an owned vector.
consteval bool test_decode_bignum_tag() {
  constexpr std::uint8_t vec[] = {0xc2, 0x43, 0x01, 0x02, 0x03};
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{5}>();
  expect_false<noalloc_bignum_negative(vec)>();
  expect_eq<noalloc_bignum_length(vec), std::size_t{3}>();
  expect_eq<noalloc_bignum_byte_at(vec, 0), std::uint8_t{0x01}>();
  expect_eq<noalloc_bignum_byte_at(vec, 2), std::uint8_t{0x03}>();
  return true;
}
static_assert(test_decode_bignum_tag());

constexpr std::uint64_t noalloc_tag_number(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  return std::get<TagRef>(result->node->data).number;
}

constexpr std::size_t noalloc_tag_child_length(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& t = std::get<TagRef>(result->node->data);
  return std::get<StringRef>(pool.tag_child(t).data).length;
}

constexpr std::uint8_t noalloc_tag_child_first_byte(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0;
  const auto& t = std::get<TagRef>(result->node->data);
  const auto& s = std::get<StringRef>(pool.tag_child(t).data);
  return pool.bytes_of(s)[0];
}

// RFC 8949 Appendix A: 32("http://www.example.com") shape (shortened here)
// -- an ordinary tag, not a bignum, proving TagRef's direct (non-indirected)
// child pointer works.
consteval bool test_decode_plain_tag() {
  constexpr std::uint8_t vec[] = {0xd8, 0x20, 0x61, 0x61};  // tag 32, "a"
  expect_eq<noalloc_bytes_consumed(vec), std::size_t{4}>();
  expect_eq<noalloc_tag_number(vec), std::uint64_t{32}>();
  expect_eq<noalloc_tag_child_length(vec), std::size_t{1}>();
  expect_eq<noalloc_tag_child_first_byte(vec), std::uint8_t{'a'}>();
  return true;
}
static_assert(test_decode_plain_tag());

constexpr double noalloc_float_value(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  if (!result) return 0.0;
  return std::get<double>(result->node->data);
}

// RFC 8949 Appendix A: 1.5 -> 0xf93e00 (half-precision float).
consteval bool test_decode_float() {
  constexpr std::uint8_t vec[] = {0xf9, 0x3e, 0x00};
  expect_eq<noalloc_float_value(vec), 1.5>();
  return true;
}
static_assert(test_decode_float());

constexpr bool noalloc_decodes_as_true() {
  constexpr std::uint8_t vec[] = {0xf5};
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, vec);
  return result && std::get<bool>(result->node->data) == true;
}
constexpr bool noalloc_decodes_as_false() {
  constexpr std::uint8_t vec[] = {0xf4};
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, vec);
  return result && std::get<bool>(result->node->data) == false;
}
constexpr bool noalloc_decodes_as_null() {
  constexpr std::uint8_t vec[] = {0xf6};
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, vec);
  return result && std::holds_alternative<std::nullptr_t>(result->node->data);
}
constexpr bool noalloc_decodes_as_undefined() {
  constexpr std::uint8_t vec[] = {0xf7};
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, vec);
  return result && std::holds_alternative<std::monostate>(result->node->data);
}

// RFC 8949 Appendix A: true/false/null/undefined.
consteval bool test_decode_simples() {
  expect_true<noalloc_decodes_as_true()>();
  expect_true<noalloc_decodes_as_false()>();
  expect_true<noalloc_decodes_as_null()>();
  expect_true<noalloc_decodes_as_undefined()>();
  return true;
}
static_assert(test_decode_simples());

consteval bool test_decode_refuses_truncated_array() {
  constexpr std::uint8_t truncated[] = {0x83, 0x01, 0x02};  // declares 3 items, only 2 present
  expect_false<noalloc_decode_ok(truncated)>();
  return true;
}
static_assert(test_decode_refuses_truncated_array());

// Running out of node capacity is a refusal, not UB: a 3-element array
// needs 3 reference slots plus the array node itself (4 node allocations
// total, since reference slots are their own, separately-capacitied
// storage now -- see noalloc_decode_ok_tiny_ref_pool below for that axis
// isolated), which doesn't fit in a 2-node pool.
constexpr bool noalloc_decode_ok_tiny_node_pool(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<2, 8, 32> storage;
  auto pool = storage.make_pool();
  return decode_value(pool, bytes).has_value();
}

consteval bool test_decode_refuses_when_node_pool_exhausted() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  expect_false<noalloc_decode_ok_tiny_node_pool(vec)>();
  return true;
}
static_assert(test_decode_refuses_when_node_pool_exhausted());

// A genuinely new, previously-impossible-to-isolate exhaustion axis: node
// and byte budgets are generous, but only 2 reference slots are
// available, so a 3-element array (needing 3 contiguous Node* slots)
// still gets refused even though every other capacity has plenty of room.
// Before this redesign, reference slots were IndexCell *nodes*, sharing
// one budget with every other node -- this axis didn't exist on its own.
constexpr bool noalloc_decode_ok_tiny_ref_pool(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 2, 32> storage;
  auto pool = storage.make_pool();
  return decode_value(pool, bytes).has_value();
}

consteval bool test_decode_refuses_when_ref_pool_exhausted() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};  // needs 3 ref slots, only 2 available
  expect_false<noalloc_decode_ok_tiny_ref_pool(vec)>();
  return true;
}
static_assert(test_decode_refuses_when_ref_pool_exhausted());

// Same idea for the byte noalloc: a 3-byte string doesn't fit a 2-byte pool.
constexpr bool noalloc_decode_ok_tiny_byte_pool(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<16, 16, 2> storage;
  auto pool = storage.make_pool();
  return decode_value(pool, bytes).has_value();
}

consteval bool test_decode_refuses_when_byte_noalloc_exhausted() {
  constexpr std::uint8_t vec[] = {0x43, 0x01, 0x02, 0x03};  // h'010203'
  expect_false<noalloc_decode_ok_tiny_byte_pool(vec)>();
  return true;
}
static_assert(test_decode_refuses_when_byte_noalloc_exhausted());

template <std::size_t MaxDepth>
constexpr bool noalloc_decode_ok_with_depth(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  return decode_value<MaxDepth>(pool, bytes).has_value();
}

// MaxDepth is caller-tunable here exactly as it is for the heap-based
// decoder: [[[1]]] is 3 levels deep, refused at MaxDepth=2.
consteval bool test_decode_respects_max_depth() {
  constexpr std::uint8_t vec[] = {0x81, 0x81, 0x81, 0x01};  // [[[1]]]
  expect_false<noalloc_decode_ok_with_depth<2>(vec)>();
  expect_true<noalloc_decode_ok_with_depth<3>(vec)>();
  return true;
}
static_assert(test_decode_respects_max_depth());

// --- Encoding: scalar facts via expect_eq<>()'s NTTP form (self-contained
// helper calls, same technique as decoding above); buffer *content* via
// expect_eq(array, array)'s ordinary function-argument form instead --
// that overload was never subject to the NTTP restriction in the first
// place (it takes its operands as plain arguments, not template
// arguments), so a helper returning a std::array by value is usable
// directly, no extra trick needed.

constexpr std::size_t noalloc_encode_written(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto decoded = decode_value(pool, bytes);
  if (!decoded) return 0;
  std::array<std::uint8_t, 32> out{};
  const auto written = encode_value(pool, decoded->node, out);
  return written ? *written : std::size_t{0};
}

template <std::size_t N>
constexpr std::array<std::uint8_t, N> noalloc_encode_bytes(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto decoded = decode_value(pool, bytes);
  std::array<std::uint8_t, N> out{};
  if (!decoded) return out;
  encode_value(pool, decoded->node, out);
  return out;
}

consteval bool test_encode_matches_official_vector() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  expect_eq<noalloc_encode_written(vec), std::size_t{4}>();
  return true;
}
static_assert(test_encode_matches_official_vector());

consteval bool test_encode_roundtrips_nested_array() {
  constexpr std::array<std::uint8_t, 5> vec{0x82, 0x01, 0x82, 0x02, 0x03};
  expect_eq(noalloc_encode_bytes<5>(vec), vec);
  return true;
}
static_assert(test_encode_roundtrips_nested_array());

consteval bool test_encode_roundtrips_map() {
  constexpr std::array<std::uint8_t, 5> vec{0xa2, 0x01, 0x02, 0x03, 0x04};
  expect_eq(noalloc_encode_bytes<5>(vec), vec);
  return true;
}
static_assert(test_encode_roundtrips_map());

consteval bool test_encode_roundtrips_bignum_and_tag() {
  constexpr std::array<std::uint8_t, 5> bignum_vec{0xc2, 0x43, 0x01, 0x02, 0x03};
  constexpr std::array<std::uint8_t, 4> tag_vec{0xd8, 0x20, 0x61, 0x61};
  expect_eq(noalloc_encode_bytes<5>(bignum_vec), bignum_vec);
  expect_eq(noalloc_encode_bytes<4>(tag_vec), tag_vec);
  return true;
}
static_assert(test_encode_roundtrips_bignum_and_tag());

// An indefinite-length input decodes to the same logical value as its
// definite-length equivalent, but NoAlloc::encode_value -- like the
// heap-based encoder -- always emits the canonical definite-length form,
// so this does NOT round-trip byte-for-byte; it must re-encode to the
// shorter, canonical bytes instead.
consteval bool test_indefinite_array_encodes_as_canonical_definite() {
  constexpr std::uint8_t indefinite_vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  constexpr std::array<std::uint8_t, 4> canonical_vec{0x83, 0x01, 0x02, 0x03};
  expect_eq<noalloc_encode_written(indefinite_vec), std::size_t{4}>();
  expect_eq(noalloc_encode_bytes<4>(indefinite_vec), canonical_vec);
  return true;
}
static_assert(test_indefinite_array_encodes_as_canonical_definite());

// A buffer too small to hold the encoded output is a refusal, not a
// truncated or out-of-bounds write.
constexpr bool noalloc_encode_fits_in_3_bytes(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto decoded = decode_value(pool, bytes);
  if (!decoded) return false;
  std::array<std::uint8_t, 3> too_small{};
  return encode_value(pool, decoded->node, too_small).has_value();
}

consteval bool test_encode_refuses_when_output_buffer_too_small() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x02, 0x03};
  expect_false<noalloc_encode_fits_in_3_bytes(vec)>();
  return true;
}
static_assert(test_encode_refuses_when_output_buffer_too_small());

// --- Duplicate-key policy: RejectDuplicateKeys/KeepFirstDuplicateKey/
// KeepLastDuplicateKey are the exact same types usable with
// ConstexprCbor::decode_value (see constexpr_cbor.hpp), not a parallel
// NoAlloc-only copy of each -- each one's NoAlloc-shaped on_duplicate overload
// works over pointers into the pool instead of a std::vector<pair<...>>,
// since an noalloc map has no flat, directly-comparable entries container to
// search or mutate.

// {1: 2, 1: 3} -> 0xa2 0102 0103 (two entries, same key 1, values 2 then 3).
constexpr std::uint8_t duplicate_key_vec[] = {0xa2, 0x01, 0x02, 0x01, 0x03};

template <typename KeyPolicy>
constexpr bool noalloc_decode_ok_with_policy(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  return decode_value<32, NoUtf8Validation, KeyPolicy>(pool, bytes).has_value();
}

consteval bool test_reject_duplicate_keys_refuses() {
  expect_false<noalloc_decode_ok_with_policy<RejectDuplicateKeys>(duplicate_key_vec)>();
  return true;
}
static_assert(test_reject_duplicate_keys_refuses());

consteval bool test_keep_first_duplicate_key_keeps_first_value() {
  expect_eq<noalloc_map_count<KeepFirstDuplicateKey>(duplicate_key_vec), std::size_t{1}>();
  expect_eq<noalloc_map_key_u64<KeepFirstDuplicateKey>(duplicate_key_vec, 0), std::uint64_t{1}>();
  expect_eq<noalloc_map_value_u64<KeepFirstDuplicateKey>(duplicate_key_vec, 0), std::uint64_t{2}>();
  return true;
}
static_assert(test_keep_first_duplicate_key_keeps_first_value());

consteval bool test_keep_last_duplicate_key_keeps_last_value() {
  expect_eq<noalloc_map_count<KeepLastDuplicateKey>(duplicate_key_vec), std::size_t{1}>();
  expect_eq<noalloc_map_key_u64<KeepLastDuplicateKey>(duplicate_key_vec, 0), std::uint64_t{1}>();
  expect_eq<noalloc_map_value_u64<KeepLastDuplicateKey>(duplicate_key_vec, 0), std::uint64_t{3}>();
  return true;
}
static_assert(test_keep_last_duplicate_key_keeps_last_value());

// A map with no actual duplicates decodes identically regardless of which
// policy is selected -- the policy only changes behavior when it actually
// matters.
consteval bool test_no_duplicates_same_result_under_every_policy() {
  constexpr std::uint8_t vec[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  expect_eq<noalloc_map_count<RejectDuplicateKeys>(vec), std::size_t{2}>();
  expect_eq<noalloc_map_count<KeepFirstDuplicateKey>(vec), std::size_t{2}>();
  expect_eq<noalloc_map_count<KeepLastDuplicateKey>(vec), std::size_t{2}>();
  return true;
}
static_assert(test_no_duplicates_same_result_under_every_policy());

constexpr std::uint8_t noalloc_structural_dup_value_first_byte(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(pool, bytes);
  if (!result) return 0;
  const auto& m = std::get<MapRef>(result->node->data);
  const auto& val = std::get<StringRef>(pool.map_value(m, 0).data);
  return pool.bytes_of(val)[0];
}

constexpr std::size_t noalloc_structural_dup_count(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(pool, bytes);
  if (!result) return 0;
  return std::get<MapRef>(result->node->data).count;
}

// Structural (non-scalar) duplicate key: {[1,2]: "a", [1,2]: "b"} under
// KeepLastDuplicateKey -- proves node_equal's recursive comparison runs
// for composite keys too, not just scalar ==.
consteval bool test_structural_key_duplicate_detection() {
  // [1,2] -> 0x820102 (3 bytes); the map wraps two (key, value) pairs.
  constexpr std::uint8_t vec[] = {0xa2, 0x82, 0x01, 0x02, 0x61, 0x61, 0x82, 0x01, 0x02, 0x61, 0x62};
  expect_eq<noalloc_structural_dup_count(vec), std::size_t{1}>();
  expect_eq<noalloc_structural_dup_value_first_byte(vec), std::uint8_t{'b'}>();
  return true;
}
static_assert(test_structural_key_duplicate_detection());

// --- Overflow-then-undersized-buffer guard: a CBOR length field can claim
// up to 2^64-1 items, but the reference-slot array indexes with
// std::size_t (which can be narrower, e.g. 32 bits on common embedded
// targets). NoAlloc::decode_value reserves reference slots for a whole
// array/map's children *before* decoding any of them, so a silently-
// truncating uint64_t->size_t cast there would be the libcbor PR #387
// shape of bug: a huge count wraps to a small one, the reservation
// "succeeds" against that undersized number, and the loop -- which still
// iterates the real, untruncated count -- writes past it.

// Array claiming 2^64-1 elements (major type 4, additional info 27).
constexpr std::uint8_t huge_array_count_vec[] = {0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

consteval bool test_huge_array_count_is_refused_not_truncated() {
  expect_false<noalloc_decode_ok(huge_array_count_vec)>();
  return true;
}
static_assert(test_huge_array_count_is_refused_not_truncated());

// Map claiming 2^64-1 pairs (major type 5, additional info 27) -- also
// exercises the *2 (key+value slots per pair) overflow check specifically.
constexpr std::uint8_t huge_map_count_vec[] = {0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

consteval bool test_huge_map_count_is_refused_not_truncated() {
  expect_false<noalloc_decode_ok(huge_map_count_vec)>();
  return true;
}
static_assert(test_huge_map_count_is_refused_not_truncated());

// Proves Error.offset telescopes to an absolute position here too, the
// same as the heap decoder's own equivalent test (test_array.cpp):
// [1, <truncated uint>] with the truncated element starting at byte 2.
constexpr ErrorReason noalloc_decode_error_reason(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  return result ? ErrorReason::TruncatedInput /* unreachable in this test's own usage */ : result.error().reason;
}

constexpr std::size_t noalloc_decode_error_offset(std::span<const std::uint8_t> bytes) {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value(pool, bytes);
  return result ? std::size_t{0} : result.error().offset;
}

consteval bool test_decode_error_offset_points_at_the_failing_element() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x18};  // [1, <truncated>] (array announces 3, only 2 present)
  expect_false<noalloc_decode_ok(vec)>();
  expect_eq<noalloc_decode_error_reason(vec), ErrorReason::TruncatedInput>();
  expect_eq<noalloc_decode_error_offset(vec), std::size_t{2}>();
  return true;
}
static_assert(test_decode_error_offset_points_at_the_failing_element());

int main() { return 0; }
