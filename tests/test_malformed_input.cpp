#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// Malformed and truncated input (cf. QCBOR's not_well_formed_cbor.h and
// cbor-test-vectors' rfc8949/bad.cbor): each case must be refused, not crash.

consteval bool test_rejects_truncated_length_argument_at_every_width() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t missing_1byte_arg[] = {0x18};              // announces 1 following byte, has 0
  constexpr std::uint8_t missing_2byte_arg[] = {0x19, 0x00};        // announces 2, has 1
  constexpr std::uint8_t missing_4byte_arg[] = {0x1a, 0x00, 0x00};  // announces 4, has 2
  constexpr std::uint8_t missing_8byte_arg[] = {0x1b, 0x00, 0x00, 0x00, 0x00};  // announces 8, has 4
  expect_eq<decode_value(missing_1byte_arg).has_value(), false>();
  expect_eq<decode_value(missing_2byte_arg).has_value(), false>();
  expect_eq<decode_value(missing_4byte_arg).has_value(), false>();
  expect_eq<decode_value(missing_8byte_arg).has_value(), false>();
  return true;
}
static_assert(test_rejects_truncated_length_argument_at_every_width());

consteval bool test_rejects_reserved_additional_info_for_every_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t uint_reserved[] = {0x1c};
  constexpr std::uint8_t negint_reserved[] = {0x3c};
  constexpr std::uint8_t bstr_reserved[] = {0x5c};
  constexpr std::uint8_t tstr_reserved[] = {0x7c};
  constexpr std::uint8_t array_reserved[] = {0x9c};
  constexpr std::uint8_t map_reserved[] = {0xbc};
  constexpr std::uint8_t tag_reserved[] = {0xdc};
  constexpr std::uint8_t simple_reserved[] = {0xfc};
  expect_eq<decode_value(uint_reserved).has_value(), false>();
  expect_eq<decode_value(negint_reserved).has_value(), false>();
  expect_eq<decode_value(bstr_reserved).has_value(), false>();
  expect_eq<decode_value(tstr_reserved).has_value(), false>();
  expect_eq<decode_value(array_reserved).has_value(), false>();
  expect_eq<decode_value(map_reserved).has_value(), false>();
  expect_eq<decode_value(tag_reserved).has_value(), false>();
  expect_eq<decode_value(simple_reserved).has_value(), false>();
  return true;
}
static_assert(test_rejects_reserved_additional_info_for_every_major_type());

// decode accepts the long form (0xf8) for values 0-23, which QCBOR's
// not_well_formed_cbor.h forbids; encode canonicalizes it.
constexpr bool test_overlong_simple_value_form_is_accepted_not_canonical() {
  constexpr std::uint8_t vec[] = {0xf8, 0x00};  // simple(0) via the long form
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& s = std::get<CborValue::SimpleValue>(d->first.data);
  const auto reencoded = encode_value(d->first);
  // decode accepts it; encode always re-canonicalizes to the short form.
  constexpr std::uint8_t canonical[] = {0xe0};
  return s.number == 0 && std::equal(reencoded.begin(), reencoded.end(), canonical);
}
static_assert(test_overlong_simple_value_form_is_accepted_not_canonical());

consteval bool test_rejects_underfilled_definite_containers() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t array_len1_zero_items[] = {0x81};
  constexpr std::uint8_t array_len2_one_item[] = {0x82, 0x00};
  constexpr std::uint8_t map_len1_zero_pairs[] = {0xa1};
  constexpr std::uint8_t map_len2_one_pair[] = {0xa2, 0x01, 0x02};
  expect_eq<decode_value(array_len1_zero_items).has_value(), false>();
  expect_eq<decode_value(array_len2_one_item).has_value(), false>();
  expect_eq<decode_value(map_len1_zero_pairs).has_value(), false>();
  expect_eq<decode_value(map_len2_one_pair).has_value(), false>();
  return true;
}
static_assert(test_rejects_underfilled_definite_containers());

consteval bool test_rejects_break_byte_misuse() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t bare_break[] = {0xff};
  constexpr std::uint8_t break_after_closed_definite_array[] = {0x80, 0xff};
  constexpr std::uint8_t break_replacing_definite_array_member[] = {0x81, 0xff};
  constexpr std::uint8_t break_replacing_definite_map_member[] = {0xa1, 0xff};
  expect_eq<decode_value(bare_break).has_value(), false>();
  // The trailing 0xff is left unconsumed, not an error. A break inside a definite
  // container is the real error.
  expect_eq<decode_value(break_after_closed_definite_array)->second, std::size_t{1}>();
  expect_eq<decode_value(break_replacing_definite_array_member).has_value(), false>();
  expect_eq<decode_value(break_replacing_definite_map_member).has_value(), false>();
  return true;
}
static_assert(test_rejects_break_byte_misuse());

consteval bool test_rejects_indefinite_string_chunk_type_mismatches() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t bstr_indef_with_tstr_chunk[] = {0x5f, 0x61, 0x00, 0xff};
  constexpr std::uint8_t tstr_indef_with_bstr_chunk[] = {0x7f, 0x41, 0x00, 0xff};
  constexpr std::uint8_t bstr_indef_with_int_chunk[] = {0x5f, 0x00, 0xff};
  constexpr std::uint8_t bstr_indef_with_array_chunk[] = {0x5f, 0x80, 0xff};
  constexpr std::uint8_t nested_indefinite_string_chunk[] = {0x5f, 0x5f, 0x41, 0x00, 0xff, 0xff};
  expect_eq<decode_value(bstr_indef_with_tstr_chunk).has_value(), false>();
  expect_eq<decode_value(tstr_indef_with_bstr_chunk).has_value(), false>();
  expect_eq<decode_value(bstr_indef_with_int_chunk).has_value(), false>();
  expect_eq<decode_value(bstr_indef_with_array_chunk).has_value(), false>();
  expect_eq<decode_value(nested_indefinite_string_chunk).has_value(), false>();
  return true;
}
static_assert(test_rejects_indefinite_string_chunk_type_mismatches());

consteval bool test_rejects_unclosed_indefinite_forms() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t unclosed_indefinite_bstr[] = {0x5f, 0x41, 0x00};  // no trailing break
  constexpr std::uint8_t unclosed_indefinite_array[] = {0x9f, 0x00};       // no trailing break
  constexpr std::uint8_t unclosed_indefinite_map[] = {0xbf, 0x00, 0x00};   // no trailing break
  expect_eq<decode_value(unclosed_indefinite_bstr).has_value(), false>();
  expect_eq<decode_value(unclosed_indefinite_array).has_value(), false>();
  expect_eq<decode_value(unclosed_indefinite_map).has_value(), false>();
  return true;
}
static_assert(test_rejects_unclosed_indefinite_forms());

consteval bool test_rejects_indefinite_marker_on_types_that_cannot_be_indefinite() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t indefinite_uint[] = {0x1f};
  constexpr std::uint8_t indefinite_negint[] = {0x3f};
  constexpr std::uint8_t indefinite_tag[] = {0xdf, 0x00};
  expect_eq<decode_value(indefinite_uint).has_value(), false>();
  expect_eq<decode_value(indefinite_negint).has_value(), false>();
  expect_eq<decode_value(indefinite_tag).has_value(), false>();
  return true;
}
static_assert(test_rejects_indefinite_marker_on_types_that_cannot_be_indefinite());

consteval bool test_rejects_string_content_truncated_mid_payload() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t bstr_claims_4_has_3[] = {0x44, 0x01, 0x02, 0x03};
  constexpr std::uint8_t tstr_claims_4_has_3[] = {0x64, 0x49, 0x45, 0x54};
  // The claimed length must be bounds-checked before reading or allocating.
  constexpr std::uint8_t huge_claimed_length_tiny_buffer[] = {0x59, 0xff, 0xf0, 0x00};
  expect_eq<decode_value(bstr_claims_4_has_3).has_value(), false>();
  expect_eq<decode_value(tstr_claims_4_has_3).has_value(), false>();
  expect_eq<decode_value(huge_claimed_length_tiny_buffer).has_value(), false>();
  return true;
}
static_assert(test_rejects_string_content_truncated_mid_payload());

// Depth bound beyond the single boundary case. Limited to 10 levels: deeper
// nesting exceeds GCC's default -fconstexpr-depth of 512 (each CBOR level costs
// several constexpr frames).
template <std::size_t Depth>
constexpr std::array<std::uint8_t, Depth + 1> make_nested_array_of_depth() {
  std::array<std::uint8_t, Depth + 1> bytes{};
  for (std::size_t i = 0; i < Depth; ++i) {
    bytes[i] = 0x81;
  }
  bytes[Depth] = 0x00;
  return bytes;
}

constexpr bool test_nesting_depth_bound_holds_at_a_depth_past_the_simple_case() {
  constexpr auto deep = make_nested_array_of_depth<10>();
  return !decode_value<5>(deep).has_value() && decode_value<15>(deep).has_value();
}
static_assert(test_nesting_depth_bound_holds_at_a_depth_past_the_simple_case());

// skip_item and decode_value share one walk, so they must agree on whether an
// input is well-formed and how many bytes it spans.
consteval bool test_skip_item_agrees_with_decode_value() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t bstr_with_text_chunk[] = {0x5f, 0x61, 0x61, 0xff};
  constexpr std::uint8_t tstr_with_indefinite_chunk[] = {0x7f, 0x7f, 0x61, 0x61, 0xff, 0xff};
  constexpr std::uint8_t good_nested[] = {0xa2, 0x01, 0x9f, 0x01, 0x5f, 0x41, 0x00, 0xff, 0xff, 0x02, 0xc2, 0x41, 0x01};
  constexpr std::uint8_t duplicate_keys[] = {0xa2, 0x01, 0x02, 0x01, 0x03};
  expect_eq<skip_item(bstr_with_text_chunk).has_value(), false>();
  expect_eq<skip_item(tstr_with_indefinite_chunk).has_value(), false>();
  expect_eq<skip_item(good_nested).value_or(0), decode_value(good_nested)->second>();
  expect_eq<skip_item(duplicate_keys).value_or(0), duplicate_keys[0] == 0xa2 ? 5u : 0u>();
  return true;
}
static_assert(test_skip_item_agrees_with_decode_value());

int main() { return 0; }
