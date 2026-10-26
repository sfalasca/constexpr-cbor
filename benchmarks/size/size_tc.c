#include <cbor.h>
int decode_entry(const uint8_t* p, size_t n) {
  CborParser parser; CborValue it;
  return cbor_parser_init(p, n, 0, &parser, &it) == CborNoError;
}
