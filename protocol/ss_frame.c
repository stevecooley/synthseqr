#include "ss_frame.h"

#include <string.h>

/* ---------------------------------------------------------------- CRC */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor.
 * Bitwise rather than table-driven — at 260 bytes per frame the cost is
 * negligible next to the UART, and it keeps 512 bytes of flash free. */
uint16_t ss_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---------------------------------------------------------------- COBS */

size_t ss_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    if (in == NULL || out == NULL) return 0;
    if (out_cap < SS_COBS_MAX(len)) return 0;

    size_t  code_pos = 0;   /* where the pending code byte will be written */
    size_t  write    = 1;   /* first byte is reserved for that code */
    uint8_t code     = 1;

    for (size_t read = 0; read < len; read++) {
        if (in[read] != 0) {
            out[write++] = in[read];
            code++;
            if (code != 0xFFu) continue;
        }
        /* Either a zero (which the code byte encodes implicitly) or a full
         * 254-byte run. Close the block. */
        out[code_pos] = code;
        code = 1;
        code_pos = write;
        /* Reserve the next code byte, unless a maximal run ended exactly at
         * the last input byte — then there is no following block to describe
         * and reserving would emit a trailing 0x01. */
        if (in[read] == 0 || read + 1 < len) write++;
    }

    /* When a maximal run ends exactly at the last input byte, the loop already
     * closed the final block and reserved nothing, leaving code_pos == write.
     * Writing here would put a stray byte one past the returned length. */
    if (code_pos < write) out[code_pos] = code;
    return write;
}

size_t ss_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    if (in == NULL || out == NULL || len == 0) return 0;

    size_t read = 0, write = 0;

    while (read < len) {
        const uint8_t code = in[read];
        if (code == 0) return 0;               /* delimiter inside the frame */
        read++;

        const size_t run = (size_t)code - 1u;
        if (read + run > len) return 0;        /* run overshoots the frame */
        if (write + run > out_cap) return 0;

        memcpy(&out[write], &in[read], run);
        write += run;
        read  += run;

        /* A non-maximal block stands for a zero, except when the frame ends
         * there — that block was terminated by the delimiter, not by a zero. */
        if (code != 0xFFu && read < len) {
            if (write + 1 > out_cap) return 0;
            out[write++] = 0;
        }
    }

    return write;
}

/* ---------------------------------------------------------------- encode */

size_t ss_frame_encode(uint8_t type, uint8_t seq,
                       const uint8_t *payload, size_t len,
                       uint8_t *out, size_t out_cap)
{
    if (out == NULL) return 0;
    if (len > SS_MAX_PAYLOAD) return 0;
    if (len > 0 && payload == NULL) return 0;

    uint8_t raw[SS_MAX_DECODED];
    raw[0] = type;
    raw[1] = seq;
    if (len > 0) memcpy(&raw[SS_HEADER_BYTES], payload, len);

    const size_t body = SS_HEADER_BYTES + len;
    const uint16_t crc = ss_crc16(raw, body);
    raw[body]     = (uint8_t)(crc & 0xFFu);
    raw[body + 1] = (uint8_t)(crc >> 8);

    const size_t total = body + SS_CRC_BYTES;
    if (out_cap < SS_COBS_MAX(total) + 1u) return 0;

    const size_t n = ss_cobs_encode(raw, total, out, out_cap);
    if (n == 0) return 0;

    out[n] = 0x00;
    return n + 1;
}

/* ---------------------------------------------------------------- decode */

void ss_rx_init(ss_rx_t *rx)
{
    if (rx != NULL) memset(rx, 0, sizeof(*rx));
}

bool ss_rx_byte(ss_rx_t *rx, uint8_t b, ss_frame_t *out)
{
    if (rx == NULL || out == NULL) return false;

    if (b != 0x00) {
        if (rx->len < sizeof(rx->buf)) {
            rx->buf[rx->len++] = b;
        } else if (!rx->overrun) {
            /* Count once per frame, not once per excess byte, so the counter
             * reflects frames lost rather than bytes seen. */
            rx->overrun = true;
            rx->err_overrun++;
        }
        return false;
    }

    /* Delimiter: the frame, if any, ends here. */
    const uint16_t n = rx->len;
    const bool  bad  = rx->overrun;
    rx->len = 0;
    rx->overrun = false;

    if (bad) return false;
    if (n == 0) return false;   /* idle line or back-to-back delimiters */

    uint8_t raw[SS_MAX_DECODED];
    const size_t decoded = ss_cobs_decode(rx->buf, n, raw, sizeof(raw));
    if (decoded == 0) { rx->err_cobs++; return false; }
    if (decoded < SS_HEADER_BYTES + SS_CRC_BYTES) { rx->err_short++; return false; }

    const size_t body = decoded - SS_CRC_BYTES;
    const uint16_t want = (uint16_t)raw[body] | (uint16_t)((uint16_t)raw[body + 1] << 8);
    if (ss_crc16(raw, body) != want) { rx->err_crc++; return false; }

    out->type = raw[0];
    out->seq  = raw[1];
    out->len  = (uint16_t)(body - SS_HEADER_BYTES);
    if (out->len > 0) memcpy(out->payload, &raw[SS_HEADER_BYTES], out->len);

    rx->frames_ok++;
    return true;
}
