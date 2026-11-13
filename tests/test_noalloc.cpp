#include "compile_time_ut.hpp"
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;
using namespace CompileTimeUnitTesting;

// The pool is passed by non-const reference, so each helper is a constexpr
// function that declares and consumes its own NoAllocStorage/NodePool.

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

// Exercises the count-then-reserve pre-pass for indefinite arrays.
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

// Ordinary tag, not a bignum.
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

// A 3-element array needs 4 nodes (array + 3 elements); a 2-node pool fails.
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

// Only 2 reference slots, but a 3-element array needs 3.
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

// [[[1]]] is 3 levels deep, refused at MaxDepth=2.
consteval bool test_decode_respects_max_depth() {
  constexpr std::uint8_t vec[] = {0x81, 0x81, 0x81, 0x01};  // [[[1]]]
  expect_false<noalloc_decode_ok_with_depth<2>(vec)>();
  expect_true<noalloc_decode_ok_with_depth<3>(vec)>();
  return true;
}
static_assert(test_decode_respects_max_depth());

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

// An indefinite-length input re-encodes to the shorter definite form, so it does
// not round-trip byte for byte.
consteval bool test_indefinite_array_encodes_as_canonical_definite() {
  constexpr std::uint8_t indefinite_vec[] = {0x9f, 0x01, 0x02, 0x03, 0xff};
  constexpr std::array<std::uint8_t, 4> canonical_vec{0x83, 0x01, 0x02, 0x03};
  expect_eq<noalloc_encode_written(indefinite_vec), std::size_t{4}>();
  expect_eq(noalloc_encode_bytes<4>(indefinite_vec), canonical_vec);
  return true;
}
static_assert(test_indefinite_array_encodes_as_canonical_definite());

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

// {1: 2, 1: 3}
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

// Composite duplicate key {[1,2]: "a", [1,2]: "b"}: node_equal recurses.
consteval bool test_structural_key_duplicate_detection() {
  // [1,2] -> 0x820102 (3 bytes); the map wraps two (key, value) pairs.
  constexpr std::uint8_t vec[] = {0xa2, 0x82, 0x01, 0x02, 0x61, 0x61, 0x82, 0x01, 0x02, 0x61, 0x62};
  expect_eq<noalloc_structural_dup_count(vec), std::size_t{1}>();
  expect_eq<noalloc_structural_dup_value_first_byte(vec), std::uint8_t{'b'}>();
  return true;
}
static_assert(test_structural_key_duplicate_detection());

// A count of 2^64-1 must be refused, not truncated into an undersized
// reservation (libcbor PR #387).

// Array claiming 2^64-1 elements (major type 4, additional info 27).
constexpr std::uint8_t huge_array_count_vec[] = {0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

consteval bool test_huge_array_count_is_refused_not_truncated() {
  expect_false<noalloc_decode_ok(huge_array_count_vec)>();
  return true;
}
static_assert(test_huge_array_count_is_refused_not_truncated());

// Map claiming 2^64-1 pairs; exercises the *2 overflow check.
constexpr std::uint8_t huge_map_count_vec[] = {0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

consteval bool test_huge_map_count_is_refused_not_truncated() {
  expect_false<noalloc_decode_ok(huge_map_count_vec)>();
  return true;
}
static_assert(test_huge_map_count_is_refused_not_truncated());

// Error.offset is absolute: [1, <truncated uint>] fails at byte 2.
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
