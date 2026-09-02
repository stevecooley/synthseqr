/*
 * Native unit tests for the Synthseqr v3 link codec.
 *
 *   cd tests/protocol && make
 *
 * Runs on the workstation with no board attached. The codec deliberately has
 * no Arduino dependency so that this is possible.
 */

#include "../../protocol/ss_frame.h"
#include "../../protocol/ss_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* Deterministic PRNG so a failing fuzz case is reproducible. */
static uint32_t rng_state = 0x5EED1234u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Push a buffer through the streaming decoder, return frames completed. */
static int feed(ss_rx_t *rx, const uint8_t *buf, size_t n, ss_frame_t *last)
{
    int got = 0;
    ss_frame_t f;
    for (size_t i = 0; i < n; i++)
        if (ss_rx_byte(rx, buf[i], &f)) {
            got++;
            if (last) *last = f;
        }
    return got;
}

/* ------------------------------------------------------------------ CRC */

static void test_crc(void)
{
    printf("crc16\n");

    /* CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
    CHECK(ss_crc16((const uint8_t *)"123456789", 9) == 0x29B1u,
          "check vector, got 0x%04X", ss_crc16((const uint8_t *)"123456789", 9));

    CHECK(ss_crc16(NULL, 0) == 0xFFFFu, "empty input is the init value");

    const uint8_t a[] = {0x01, 0x02, 0x03};
    const uint8_t b[] = {0x01, 0x02, 0x04};
    CHECK(ss_crc16(a, 3) != ss_crc16(b, 3), "differing input must differ");
}

/* ----------------------------------------------------------------- COBS */

static void cobs_roundtrip(const uint8_t *in, size_t len, const char *what)
{
    uint8_t enc[SS_MAX_ENCODED];
    uint8_t dec[SS_MAX_DECODED];

    const size_t n = ss_cobs_encode(in, len, enc, sizeof(enc));
    CHECK(n > 0 || len == 0, "%s: encode returned 0", what);
    CHECK(n <= SS_COBS_MAX(len), "%s: encode overshot bound (%zu > %zu)",
          what, n, (size_t)SS_COBS_MAX(len));

    /* The entire reason for COBS: no zero may survive encoding. */
    for (size_t i = 0; i < n; i++)
        CHECK(enc[i] != 0, "%s: zero byte at %zu of encoded output", what, i);

    const size_t m = ss_cobs_decode(enc, n, dec, sizeof(dec));
    CHECK(m == len, "%s: decoded length %zu, expected %zu", what, m, len);
    CHECK(len == 0 || memcmp(dec, in, len) == 0, "%s: payload mismatch", what);
}

static void test_cobs(void)
{
    printf("cobs\n");

    uint8_t buf[SS_MAX_DECODED];

    cobs_roundtrip((const uint8_t *)"", 0, "empty");

    buf[0] = 0x00;
    cobs_roundtrip(buf, 1, "single zero");

    buf[0] = 0x41;
    cobs_roundtrip(buf, 1, "single non-zero");

    memset(buf, 0, 64);
    cobs_roundtrip(buf, 64, "all zeros");

    memset(buf, 0xAA, 64);
    cobs_roundtrip(buf, 64, "no zeros");

    /* Run lengths either side of the 254-byte COBS block boundary. This is
     * where naive implementations emit a stray trailing code byte. */
    for (size_t len = 252; len <= 256 && len <= SS_MAX_DECODED; len++) {
        memset(buf, 0x5A, len);
        char label[64];
        snprintf(label, sizeof(label), "run of %zu non-zero", len);
        cobs_roundtrip(buf, len, label);
    }

    /* Same boundary, but terminated by a zero rather than end-of-input. */
    memset(buf, 0x5A, 253);
    buf[253] = 0x00;
    cobs_roundtrip(buf, 254, "254-run ending in zero");

    /* Alternating pattern exercises block open/close on every byte. */
    for (size_t i = 0; i < 128; i++) buf[i] = (i & 1) ? 0x00 : 0xFF;
    cobs_roundtrip(buf, 128, "alternating zero/0xFF");

    memset(buf, 0x11, SS_MAX_DECODED);
    cobs_roundtrip(buf, SS_MAX_DECODED, "max decoded length");

    /* Refuse to scribble past a short output buffer. */
    uint8_t tiny[2];
    memset(buf, 0x22, 64);
    CHECK(ss_cobs_encode(buf, 64, tiny, sizeof(tiny)) == 0,
          "encode must reject an undersized output buffer");

    /* A zero inside the encoded region is malformed, not a delimiter. */
    const uint8_t bad[] = {0x03, 0x11, 0x00, 0x22};
    CHECK(ss_cobs_decode(bad, sizeof(bad), buf, sizeof(buf)) == 0,
          "decode must reject an embedded zero");

    /* Code byte promising more data than the frame holds. */
    const uint8_t overshoot[] = {0x09, 0x11, 0x22};
    CHECK(ss_cobs_decode(overshoot, sizeof(overshoot), buf, sizeof(buf)) == 0,
          "decode must reject a run that overshoots the frame");
}

static void test_cobs_fuzz(void)
{
    printf("cobs fuzz\n");

    uint8_t in[SS_MAX_DECODED];

    for (int iter = 0; iter < 4000; iter++) {
        const size_t len = rng() % (SS_MAX_DECODED + 1);
        /* Bias hard toward zeros — they are what COBS actually has to handle. */
        const uint32_t zero_bias = (rng() % 4) + 1;
        for (size_t i = 0; i < len; i++)
            in[i] = (uint8_t)((rng() % zero_bias == 0) ? 0 : (rng() | 1));

        cobs_roundtrip(in, len, "fuzz");
        if (failures) { printf("  (seed 0x%08X)\n", rng_state); return; }
    }
}

/* ---------------------------------------------------------------- frames */

static void test_frame_roundtrip(void)
{
    printf("frame round-trip\n");

    uint8_t wire[SS_MAX_ENCODED];
    ss_rx_t rx;
    ss_frame_t got;

    const uint8_t body[] = {0xDE, 0xAD, 0x00, 0xBE, 0xEF};
    size_t n = ss_frame_encode(SS_MSG_LCD_RAW, 42, body, sizeof(body),
                               wire, sizeof(wire));
    CHECK(n > 0, "encode failed");
    CHECK(wire[n - 1] == 0x00, "frame must end with the delimiter");

    ss_rx_init(&rx);
    CHECK(feed(&rx, wire, n, &got) == 1, "expected exactly one frame");
    CHECK(got.type == SS_MSG_LCD_RAW, "type mismatch: 0x%02X", got.type);
    CHECK(got.seq == 42, "seq mismatch: %u", got.seq);
    CHECK(got.len == sizeof(body), "len mismatch: %u", got.len);
    CHECK(memcmp(got.payload, body, sizeof(body)) == 0, "payload mismatch");
    CHECK(rx.frames_ok == 1 && rx.err_crc == 0, "counters wrong");

    /* Zero-length payload is legal — PING and friends use it. */
    n = ss_frame_encode(SS_MSG_PING, 7, NULL, 0, wire, sizeof(wire));
    CHECK(n > 0, "empty-payload encode failed");
    ss_rx_init(&rx);
    CHECK(feed(&rx, wire, n, &got) == 1, "empty-payload frame lost");
    CHECK(got.len == 0 && got.type == SS_MSG_PING, "empty-payload frame wrong");

    /* Maximum payload must fit the declared buffers. */
    uint8_t big[SS_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    n = ss_frame_encode(SS_MSG_STORE_CHUNK, 0, big, sizeof(big),
                        wire, sizeof(wire));
    CHECK(n > 0, "max-payload encode failed");
    CHECK(n <= SS_MAX_ENCODED, "encoded frame exceeds SS_MAX_ENCODED");
    ss_rx_init(&rx);
    CHECK(feed(&rx, wire, n, &got) == 1, "max-payload frame lost");
    CHECK(got.len == SS_MAX_PAYLOAD && memcmp(got.payload, big, sizeof(big)) == 0,
          "max payload corrupted");

    /* Oversized payload is refused rather than truncated. */
    CHECK(ss_frame_encode(SS_MSG_STORE_CHUNK, 0, big, SS_MAX_PAYLOAD + 1,
                          wire, sizeof(wire)) == 0,
          "oversized payload must be refused");
}

static void test_corruption(void)
{
    printf("corruption detection\n");

    const uint8_t body[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    uint8_t wire[SS_MAX_ENCODED];
    const size_t n = ss_frame_encode(SS_MSG_STATE_FULL, 3, body, sizeof(body),
                                     wire, sizeof(wire));
    CHECK(n > 0, "encode failed");

    /* Every single-bit flip in the frame body must be caught. This is the
     * property that protects stored patterns from link noise. */
    int caught = 0, tried = 0;
    for (size_t i = 0; i < n - 1; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mangled[SS_MAX_ENCODED];
            memcpy(mangled, wire, n);
            mangled[i] ^= (uint8_t)(1u << bit);
            if (mangled[i] == 0x00) continue;  /* becomes a delimiter, not a flip */

            ss_rx_t rx;
            ss_rx_init(&rx);
            ss_frame_t f;
            tried++;
            if (feed(&rx, mangled, n, &f) == 0) caught++;
        }
    }
    CHECK(caught == tried, "single-bit flips: caught %d of %d", caught, tried);
}

static void test_resync(void)
{
    printf("resync and framing hygiene\n");

    const uint8_t body[] = {1, 2, 3, 4};
    uint8_t wire[SS_MAX_ENCODED];
    const size_t n = ss_frame_encode(SS_MSG_EVT_TOUCH, 9, body, sizeof(body),
                                     wire, sizeof(wire));

    ss_rx_t rx;
    ss_frame_t got;

    /* Starting mid-stream. This is the boot-order case: the CYD may come up
     * partway through a Feather transmission.
     *
     * Resync happens at a delimiter and nowhere else, so the frame that the
     * junk runs into is necessarily lost — the receiver cannot know where the
     * garbage ended. What matters is that the frame *after* that one is clean
     * and that the loss is counted rather than silently accepted. */
    ss_rx_init(&rx);
    const uint8_t junk[] = {0x77, 0x12, 0x9A};
    feed(&rx, junk, sizeof(junk), NULL);
    CHECK(feed(&rx, wire, n, NULL) == 0, "junk-contaminated frame must be dropped");
    CHECK(rx.err_cobs + rx.err_crc + rx.err_short == 1, "the loss must be counted");
    CHECK(feed(&rx, wire, n, &got) == 1, "failed to resync on the next frame");
    CHECK(got.type == SS_MSG_EVT_TOUCH && got.len == 4, "resynced frame wrong");

    /* An explicit delimiter resyncs immediately, losing nothing further. */
    ss_rx_init(&rx);
    feed(&rx, junk, sizeof(junk), NULL);
    feed(&rx, &(const uint8_t){0x00}, 1, NULL);
    CHECK(feed(&rx, wire, n, &got) == 1, "delimiter failed to resync");

    /* Idle delimiters between frames must not be mistaken for empty frames. */
    ss_rx_init(&rx);
    const uint8_t pad[] = {0x00, 0x00, 0x00};
    feed(&rx, pad, sizeof(pad), NULL);
    CHECK(rx.frames_ok == 0 && rx.err_cobs == 0 && rx.err_short == 0,
          "bare delimiters must be ignored silently");
    CHECK(feed(&rx, wire, n, &got) == 1, "frame after padding lost");

    /* Back-to-back frames with no gap. */
    ss_rx_init(&rx);
    CHECK(feed(&rx, wire, n, NULL) == 1, "first of pair lost");
    CHECK(feed(&rx, wire, n, NULL) == 1, "second of pair lost");
    CHECK(rx.frames_ok == 2, "frames_ok should be 2, is %u", rx.frames_ok);

    /* A truncated frame is dropped, and the following frame still arrives. */
    ss_rx_init(&rx);
    feed(&rx, wire, n - 3, NULL);          /* cut before the delimiter */
    const uint8_t delim = 0x00;
    feed(&rx, &delim, 1, NULL);            /* force the short frame to close */
    CHECK(rx.frames_ok == 0, "truncated frame must not be accepted");
    CHECK(feed(&rx, wire, n, &got) == 1, "frame after truncation lost");

    /* Overrun: a run of non-zero bytes longer than any legal frame is dropped
     * once, counted once, and does not wedge the decoder. */
    ss_rx_init(&rx);
    for (size_t i = 0; i < SS_MAX_ENCODED + 64; i++) {
        ss_frame_t f;
        const uint8_t b = 0x5A;
        ss_rx_byte(&rx, b, &f);
    }
    CHECK(rx.err_overrun == 1, "overrun counted %u times, expected 1",
          rx.err_overrun);
    feed(&rx, &delim, 1, NULL);
    CHECK(feed(&rx, wire, n, &got) == 1, "decoder wedged after overrun");
}

static void test_frame_fuzz(void)
{
    printf("frame fuzz\n");

    uint8_t payload[SS_MAX_PAYLOAD];
    uint8_t wire[SS_MAX_ENCODED];

    for (int iter = 0; iter < 3000; iter++) {
        const size_t len = rng() % (SS_MAX_PAYLOAD + 1);
        const uint32_t zero_bias = (rng() % 3) + 1;
        for (size_t i = 0; i < len; i++)
            payload[i] = (uint8_t)((rng() % zero_bias == 0) ? 0 : rng());

        const uint8_t type = (uint8_t)rng();
        const uint8_t seq  = (uint8_t)rng();

        const size_t n = ss_frame_encode(type, seq, payload, len,
                                         wire, sizeof(wire));
        if (n == 0) { CHECK(0, "fuzz encode failed at len %zu", len); return; }

        ss_rx_t rx;
        ss_rx_init(&rx);
        ss_frame_t got;
        if (feed(&rx, wire, n, &got) != 1) {
            CHECK(0, "fuzz frame lost at len %zu (seed 0x%08X)", len, rng_state);
            return;
        }
        if (got.type != type || got.seq != seq || got.len != len ||
            (len && memcmp(got.payload, payload, len) != 0)) {
            CHECK(0, "fuzz frame corrupted at len %zu (seed 0x%08X)",
                  len, rng_state);
            return;
        }
    }
}

int main(void)
{
    printf("== synthseqr link codec ==\n");

    test_crc();
    test_cobs();
    test_cobs_fuzz();
    test_frame_roundtrip();
    test_corruption();
    test_resync();
    test_frame_fuzz();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
