/* See size_cc.cpp's comment -- same fairness fix: a full recursive walk,
 * not just cbor_parser_init(). */
#include <stdint.h>

#include <cbor.h>

static volatile uint64_t sink = 0;

static int walk(CborValue* v) {
  while (!cbor_value_at_end(v)) {
    sink++;
    const CborType type = cbor_value_get_type(v);
    if (type == CborArrayType || type == CborMapType) {
      CborValue recursed;
      if (cbor_value_enter_container(v, &recursed) != CborNoError) return 0;
      if (!walk(&recursed)) return 0;
      if (cbor_value_leave_container(v, &recursed) != CborNoError) return 0;
    } else {
      if (type == CborTextStringType || type == CborByteStringType) {
        size_t len = 0;
        cbor_value_get_string_length(v, &len);
        sink += len;
      }
      if (cbor_value_advance(v) != CborNoError) return 0;
    }
  }
  return 1;
}

int decode_entry(const uint8_t* p, size_t n) {
  CborParser parser;
  CborValue it;
  if (cbor_parser_init(p, n, 0, &parser, &it) != CborNoError) return 0;
  return walk(&it);
}
