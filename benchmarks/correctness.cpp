// Cross-library correctness corpus: feeds the exact same byte vectors to
// constexpr-cbor, tinycbor, QCBOR, and libcbor, and reports accept/reject
// per library against the RFC-correct expected answer. Most "reject"
// vectors are lifted directly from tests/test_malformed_input.cpp (which
// already cites tinycbor's not_well_formed_cbor.h and QCBOR's own test
// suite as sources for several of them) -- this file asks the
// complementary question: when constexpr-cbor refuses something per that
// file's own static_asserts, do the other libraries actually refuse it
// too, or is "refuse, don't crash" a claim constexpr-cbor backs up more
// strictly than its peers?
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "constexpr_cbor.hpp"

extern "C" {
#include <cbor.h>  // tinycbor
}
#include "qcbor/qcbor_decode.h"  // QCBOR

#include "libcbor_adapter.hpp"  // libcbor, via its own TU -- see that header's comment

namespace {

struct Vector {
  const char* name;
  std::vector<std::uint8_t> bytes;
  bool should_accept;  // the RFC-correct answer
  const char* source;
};

bool accepted_by_constexpr_cbor(const std::vector<std::uint8_t>& bytes) {
  return ConstexprCbor::decode_value(std::span<const std::uint8_t>(bytes)).has_value();
}

bool accepted_by_tinycbor(const std::vector<std::uint8_t>& bytes) {
  CborParser parser;
  CborValue it;
  if (cbor_parser_init(bytes.data(), bytes.size(), 0, &parser, &it) != CborNoError) return false;
  // A single top-level item, fully walked (containers recursively), with
  // no error anywhere -- matches what "accept" means for the other two.
  struct Walker {
    static bool walk(CborValue* v) {
      while (!cbor_value_at_end(v)) {
        const CborType type = cbor_value_get_type(v);
        if (type == CborArrayType || type == CborMapType) {
          CborValue recursed;
          if (cbor_value_enter_container(v, &recursed) != CborNoError) return false;
          if (!walk(&recursed)) return false;
          if (cbor_value_leave_container(v, &recursed) != CborNoError) return false;
        } else {
          if (cbor_value_advance(v) != CborNoError) return false;
        }
      }
      return true;
    }
  };
  return Walker::walk(&it);
}

bool accepted_by_qcbor(const std::vector<std::uint8_t>& bytes) {
  QCBORDecodeContext ctx;
  const UsefulBufC buf{bytes.data(), bytes.size()};
  QCBORDecode_Init(&ctx, buf, QCBOR_DECODE_MODE_NORMAL);
  QCBORItem item;
  for (;;) {
    const QCBORError err = QCBORDecode_GetNext(&ctx, &item);
    if (err == QCBOR_ERR_NO_MORE_ITEMS) break;
    if (err != QCBOR_SUCCESS) return false;
  }
  // QCBOR's GetNext loop alone cannot distinguish "legitimately ran out
  // of top-level input" from "ran out of bytes while still inside an
  // unclosed array/map" -- both surface as QCBOR_ERR_NO_MORE_ITEMS from
  // GetNext (see its header's own comment on QCBOR_ERR_HIT_END). Calling
  // Finish(), as QCBOR's own docs say a caller must, is what actually
  // catches the underfilled/unclosed-container vectors below -- this
  // corpus has no "valid item + trailing garbage" vectors, so Finish()
  // can be called unconditionally here.
  return QCBORDecode_Finish(&ctx) == QCBOR_SUCCESS;
}

// libcbor's own accept/reject check (::accepted_by_libcbor) lives in
// libcbor_adapter.cpp -- see libcbor_adapter.hpp's comment for why it
// can't be included directly here.

}  // namespace

int main() {
  const std::vector<Vector> corpus = {
      // --- should be refused ---------------------------------------
      {"missing_1byte_arg", {0x18}, false, "test_malformed_input.cpp"},
      {"missing_2byte_arg", {0x19, 0x00}, false, "test_malformed_input.cpp"},
      {"missing_4byte_arg", {0x1a, 0x00, 0x00}, false, "test_malformed_input.cpp"},
      {"missing_8byte_arg", {0x1b, 0x00, 0x00, 0x00, 0x00}, false, "test_malformed_input.cpp"},
      {"reserved_additional_info_uint", {0x1c}, false, "test_malformed_input.cpp"},
      {"reserved_additional_info_array", {0x9c}, false, "test_malformed_input.cpp"},
      {"reserved_additional_info_simple", {0xfc}, false, "test_malformed_input.cpp"},
      {"underfilled_array_len1_zero_items", {0x81}, false, "test_malformed_input.cpp"},
      {"underfilled_map_len2_one_pair", {0xa2, 0x01, 0x02}, false, "test_malformed_input.cpp"},
      {"bare_break", {0xff}, false, "test_malformed_input.cpp"},
      {"break_replacing_definite_array_member", {0x81, 0xff}, false, "test_malformed_input.cpp"},
      {"break_replacing_definite_map_member", {0xa1, 0xff}, false, "test_malformed_input.cpp"},
      {"bstr_indef_with_tstr_chunk", {0x5f, 0x61, 0x00, 0xff}, false, "test_malformed_input.cpp"},
      {"bstr_indef_with_int_chunk", {0x5f, 0x00, 0xff}, false, "test_malformed_input.cpp"},
      {"nested_indefinite_string_chunk", {0x5f, 0x5f, 0x41, 0x00, 0xff, 0xff}, false, "test_malformed_input.cpp"},
      {"unclosed_indefinite_bstr", {0x5f, 0x41, 0x00}, false, "test_malformed_input.cpp"},
      {"unclosed_indefinite_array", {0x9f, 0x00}, false, "test_malformed_input.cpp"},
      {"unclosed_indefinite_map", {0xbf, 0x00, 0x00}, false, "test_malformed_input.cpp"},
      {"indefinite_uint", {0x1f}, false, "test_malformed_input.cpp"},
      {"indefinite_negint", {0x3f}, false, "test_malformed_input.cpp"},
      {"indefinite_tag", {0xdf, 0x00}, false, "test_malformed_input.cpp"},
      {"bstr_claims_4_has_3", {0x44, 0x01, 0x02, 0x03}, false, "test_malformed_input.cpp"},
      {"tstr_claims_4_has_3", {0x64, 0x49, 0x45, 0x54}, false, "test_malformed_input.cpp"},
      {"huge_claimed_length_tiny_buffer", {0x59, 0xff, 0xf0, 0x00}, false, "test_malformed_input.cpp"},
      // RFC 8949 Section 5.3.1's own overlong-UTF-8 example: a single
      // continuation byte (0xae) encoded via the 2-byte lead-byte form
      // (0xc0) that should have been the 1-byte form -- not well-formed
      // UTF-8 even though the CBOR framing around it is fine. Neither
      // tinycbor nor QCBOR validate UTF-8 content by default (confirmed
      // against their docs), so both are expected to *accept* this at
      // the CBOR-structural level -- constexpr-cbor also accepts it
      // under NoUtf8Validation (its default), and only refuses it when
      // the caller opts into decode_value<32, CompleteUtf8Validation>.
      // Listed here with should_accept=true to reflect what the
      // structural (no-UTF-8-check) decode path actually does in all
      // three libraries, not to claim UTF-8 is being validated.
      {"overlong_utf8_structurally_well_formed", {0x62, 0xc0, 0xae}, true, "RFC 8949 Section 5.3.1"},
      // --- should be accepted (controls, not just reject cases) ----
      {"empty_array", {0x80}, true, "RFC 8949 Appendix A"},
      {"array_1_2_3", {0x83, 0x01, 0x02, 0x03}, true, "RFC 8949 Appendix A"},
      {"map_1_2_3_4", {0xa2, 0x01, 0x02, 0x03, 0x04}, true, "RFC 8949 Appendix A"},
      {"text_IETF", {0x64, 0x49, 0x45, 0x54, 0x46}, true, "RFC 8949 Appendix A"},
      {"nested_array", {0x82, 0x01, 0x82, 0x02, 0x03}, true, "RFC 8949 Appendix A"},
      {"indefinite_array_123", {0x9f, 0x01, 0x02, 0x03, 0xff}, true, "RFC 8949 Appendix A"},
      {"duplicate_keys_1_1", {0xa2, 0x01, 0x02, 0x01, 0x03}, true,
       "structurally well-formed; constexpr-cbor's *default* decode_value policy "
       "(RejectDuplicateKeys) refuses this one -- see note below"},
  };

  int mismatches = 0;
  std::printf("%-42s %-8s %-10s %-10s %-10s %-10s\n", "vector", "expect", "cbor", "tinycbor", "qcbor", "libcbor");
  for (const auto& v : corpus) {
    const bool cc = accepted_by_constexpr_cbor(v.bytes);
    const bool tc = accepted_by_tinycbor(v.bytes);
    const bool qc = accepted_by_qcbor(v.bytes);
    const bool lc = accepted_by_libcbor(std::span<const std::uint8_t>(v.bytes));
    // constexpr-cbor's *default* decode_value<32, NoUtf8Validation,
    // RejectDuplicateKeys> intentionally refuses the duplicate-key
    // vector even though it's structurally well-formed CBOR -- that's a
    // deliberate policy choice (see README's "Pluggable duplicate-map-
    // key policies"), not a structural-decode disagreement, so it's
    // special-cased out of the plain accept/reject diff below.
    const bool is_dup_key_policy_case = std::string(v.name) == "duplicate_keys_1_1";
    const bool cc_matches = is_dup_key_policy_case || (cc == v.should_accept);
    if (!cc_matches || tc != v.should_accept || qc != v.should_accept || lc != v.should_accept) ++mismatches;
    std::printf("%-42s %-8s %-10s %-10s %-10s %-10s%s\n", v.name, v.should_accept ? "accept" : "reject",
                cc ? "accept" : "reject", tc ? "accept" : "reject", qc ? "accept" : "reject",
                lc ? "accept" : "reject", is_dup_key_policy_case ? "  (policy choice, see note)" : "");
  }
  std::printf("\n%d / %zu vectors disagree with the RFC-correct expectation (excluding the policy-choice case)\n",
              mismatches, corpus.size());
  return 0;
}
