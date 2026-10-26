#include "qcbor/qcbor_decode.h"
int decode_entry(const uint8_t* p, size_t n) {
  QCBORDecodeContext ctx;
  UsefulBufC buf = {p, n};
  QCBORDecode_Init(&ctx, buf, QCBOR_DECODE_MODE_NORMAL);
  QCBORItem item;
  return QCBORDecode_GetNext(&ctx, &item) == QCBOR_SUCCESS;
}
