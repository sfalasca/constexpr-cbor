/* See size_cc.cpp's comment -- same fairness fix: a full GetNext loop
 * over every item, not just one peek. */
#include <stdint.h>

#include "qcbor/qcbor_decode.h"

static volatile uint64_t sink = 0;

int decode_entry(const uint8_t* p, size_t n) {
  QCBORDecodeContext ctx;
  UsefulBufC buf = {p, n};
  QCBORDecode_Init(&ctx, buf, QCBOR_DECODE_MODE_NORMAL);
  QCBORItem item;
  for (;;) {
    const QCBORError err = QCBORDecode_GetNext(&ctx, &item);
    if (err == QCBOR_ERR_NO_MORE_ITEMS) break;
    if (err != QCBOR_SUCCESS) return 0;
    sink++;
    if (item.uDataType == QCBOR_TYPE_TEXT_STRING || item.uDataType == QCBOR_TYPE_BYTE_STRING) {
      sink += item.val.string.len;
    }
  }
  return QCBORDecode_Finish(&ctx) == QCBOR_SUCCESS;
}
