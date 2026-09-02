/*
 * Synthseqr v3 — link framing: COBS + CRC-16.
 *
 * Wire format, per frame:
 *
 *     COBS( [type:1][seq:1][payload:0..256][crc16:2] ) 0x00
 *
 * COBS rather than SLIP so that 0x00 appears nowhere except as the delimiter,
 * which makes resync after a garbled byte unambiguous and bounds the overhead.
 * CRC-16/CCITT-FALSE over type+seq+payload, little-endian on the wire.
 *
 * Plain C99 so the Feather (C++/SAMD), the CYD (C++/ESP32), and the native
 * test build all compile the identical object code paths.
 *
 * No allocation, no I/O, no dependency on either Arduino core — which is what
 * makes the whole codec testable on a workstation.
 */

#ifndef SS_FRAME_H
#define SS_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SS_MAX_PAYLOAD   256u
#define SS_HEADER_BYTES  2u                       /* type, seq */
#define SS_CRC_BYTES     2u
#define SS_MAX_DECODED   (SS_HEADER_BYTES + SS_MAX_PAYLOAD + SS_CRC_BYTES)

/* COBS adds one byte per 254-byte run, plus a leading code byte. */
#define SS_COBS_MAX(n)   ((n) + ((n) / 254u) + 1u)
#define SS_MAX_ENCODED   (SS_COBS_MAX(SS_MAX_DECODED) + 1u)  /* +1 delimiter */

typedef struct {
    uint8_t  type;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[SS_MAX_PAYLOAD];
} ss_frame_t;

/* ---------------------------------------------------------------- CRC */

uint16_t ss_crc16(const uint8_t *data, size_t len);

/* ---------------------------------------------------------------- COBS */

/* Both return bytes written, or 0 on invalid input. Neither writes the
 * delimiter; ss_frame_encode does that. */
size_t ss_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);
size_t ss_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

/* ---------------------------------------------------------------- encode */

/* Serialises one frame, delimiter included, into out.
 * Returns bytes written, or 0 if len > SS_MAX_PAYLOAD or out_cap is too small.
 * Pass payload == NULL only with len == 0. */
size_t ss_frame_encode(uint8_t type, uint8_t seq,
                       const uint8_t *payload, size_t len,
                       uint8_t *out, size_t out_cap);

/* ---------------------------------------------------------------- decode */

/* Streaming decoder. Feed bytes as they arrive; it self-synchronises on the
 * 0x00 delimiter, so a receiver that starts mid-frame or drops bytes recovers
 * at the next frame boundary without special handling. */
typedef struct {
    uint8_t  buf[SS_MAX_ENCODED];
    uint16_t len;
    bool     overrun;      /* current frame already too long; discard to delimiter */

    /* Diagnostics. The Feather surfaces these over the link, because a link
     * that is quietly degrading is otherwise very hard to see from outside. */
    uint32_t frames_ok;
    uint32_t err_crc;
    uint32_t err_cobs;
    uint32_t err_overrun;
    uint32_t err_short;
} ss_rx_t;

void ss_rx_init(ss_rx_t *rx);

/* Consumes one byte. Returns true exactly when a valid frame completes, in
 * which case out is populated. Errors are counted, not reported, so the caller
 * can stay a tight loop. */
bool ss_rx_byte(ss_rx_t *rx, uint8_t b, ss_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SS_FRAME_H */
