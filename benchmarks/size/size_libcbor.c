/* See size_cc.cpp's comment -- same fairness fix: a full decode (cbor_load)
 * plus a recursive walk of every item, not just the load call itself.
 * Plain C, matching size_tc.c/size_qc.c's own convention (libcbor, like
 * tinycbor and QCBOR, is a C library with a C API -- only constexpr-cbor's
 * own two entry points need C++). */
#include <stdint.h>

#include <cbor.h>

static volatile uint64_t sink = 0;

static void walk(cbor_item_t* item) {
  sink++;
  switch (cbor_typeof(item)) {
    case CBOR_TYPE_BYTESTRING:
      sink += cbor_bytestring_length(item);
      break;
    case CBOR_TYPE_STRING:
      sink += cbor_string_length(item);
      break;
    case CBOR_TYPE_ARRAY: {
      cbor_item_t** handle = cbor_array_handle(item);
      size_t n = cbor_array_size(item);
      for (size_t i = 0; i < n; ++i) walk(handle[i]);
      break;
    }
    case CBOR_TYPE_MAP: {
      struct cbor_pair* pairs = cbor_map_handle(item);
      size_t n = cbor_map_size(item);
      for (size_t i = 0; i < n; ++i) {
        walk(pairs[i].key);
        walk(pairs[i].value);
      }
      break;
    }
    case CBOR_TYPE_TAG:
      walk(cbor_tag_item(item));
      break;
    default:
      break;
  }
}

int decode_entry(const uint8_t* p, size_t n) {
  struct cbor_load_result result;
  cbor_item_t* root = cbor_load(p, n, &result);
  if (!root || result.error.code != CBOR_ERR_NONE) {
    if (root) cbor_decref(&root);
    return 0;
  }
  walk(root);
  cbor_decref(&root);
  return 1;
}
