/*
 * Native tests for the Synthseqr v3 link transport.
 *
 * Wires two ss_link_t instances to each other through a controllable "wire"
 * that can drop, corrupt, or stall, and drives them with a fake clock. This is
 * the Feather and the CYD talking, minus the silicon.
 */

#include "../../protocol/ss_link.h"

#include <stdio.h>
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

/* ------------------------------------------------------------- harness */

typedef struct {
    ss_link_t  link;
    int        rx_frames;
    ss_frame_t last;
} node_t;

static void on_frame(const ss_frame_t *f, void *user)
{
    node_t *n = (node_t *)user;
    n->rx_frames++;
    n->last = *f;
}

static void node_init(node_t *n)
{
    memset(n, 0, sizeof(*n));
    ss_link_init(&n->link, on_frame, n);
}

/* Wire behaviour, applied to bytes moving in either direction. */
typedef struct {
    bool     cut;          /* deliver nothing */
    uint32_t corrupt_at;   /* flip a bit in the Nth byte delivered; 0 = never */
    uint32_t delivered;
    size_t   rate;         /* bytes moved per step; 0 selects the default */
} wire_t;

#define WIRE_DEFAULT_RATE 256u   /* ~1 ms of a 2.5 Mbaud link */

/* A real UART moves a bounded number of bytes per unit time, and the receiving
 * ring drains between bursts. Modelling that matters: an unlimited pump would
 * overrun the RX ring in ways the hardware never will, and would hide whether
 * the ring is actually sized correctly. */
static void pump(wire_t *w, ss_link_t *from, ss_link_t *to)
{
    uint8_t buf[128];
    size_t budget = (w->rate != 0) ? w->rate : WIRE_DEFAULT_RATE;

    while (budget > 0) {
        const size_t want = (budget < sizeof(buf)) ? budget : sizeof(buf);
        const size_t n = ss_link_tx_pull(from, buf, want);
        if (n == 0) break;
        budget -= n;
        if (w->cut) continue;    /* drained, but discarded — the peer is gone */
        for (size_t i = 0; i < n; i++) {
            w->delivered++;
            uint8_t b = buf[i];
            if (w->corrupt_at != 0 && w->delivered == w->corrupt_at) b ^= 0x40u;
            ss_link_rx_byte(to, b);
        }
    }
}

/* Bytes move before the main loops run, because on the real boards the UART
 * ISR fills the RX ring continuously and independently of poll(). */
static void step(wire_t *w, node_t *a, node_t *b, uint32_t t)
{
    pump(w, &a->link, &b->link);
    pump(w, &b->link, &a->link);
    ss_link_poll(&a->link, t);
    ss_link_poll(&b->link, t);
}

/* Advance the clock in ms_step increments for total_ms. Returns the end time. */
static uint32_t run_for(wire_t *w, node_t *a, node_t *b,
                        uint32_t t0, uint32_t total_ms, uint32_t ms_step)
{
    uint32_t t = t0;
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += ms_step) {
        t += ms_step;
        step(w, a, b, t);
    }
    return t;
}

/* --------------------------------------------------------------- tests */

static void test_handshake(void)
{
    printf("handshake\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);

    CHECK(!ss_link_is_up(&a.link), "must start down");

    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);

    CHECK(ss_link_is_up(&a.link), "A failed to come up");
    CHECK(ss_link_is_up(&b.link), "B failed to come up");
    CHECK(a.link.peer_version == SS_PROTOCOL_VERSION, "A recorded wrong version");
    CHECK(a.link.link_ups == 1, "A counted %u ups, expected 1", a.link.link_ups);
    CHECK(a.link.version_rejects == 0, "unexpected version reject");
}

static void test_app_frames(void)
{
    printf("application frames\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);
    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);

    const uint8_t lcd[] = "?f?x00?y0synthseqr";
    CHECK(ss_link_send(&a.link, SS_MSG_LCD_RAW, lcd, sizeof(lcd) - 1),
          "send failed on an up link");
    step(&w, &a, &b, 2);

    CHECK(b.rx_frames == 1, "B got %d frames, expected 1", b.rx_frames);
    CHECK(b.last.type == SS_MSG_LCD_RAW, "wrong type 0x%02X", b.last.type);
    CHECK(b.last.len == sizeof(lcd) - 1, "wrong length %u", b.last.len);
    CHECK(memcmp(b.last.payload, lcd, sizeof(lcd) - 1) == 0, "payload corrupted");

    /* Reverse direction: the CYD proposing an input event. */
    const uint8_t evt[] = {0x03, 0x7F};
    CHECK(ss_link_send(&b.link, SS_MSG_EVT_CONTROL, evt, sizeof(evt)),
          "reverse send failed");
    step(&w, &a, &b, 3);
    CHECK(a.rx_frames == 1, "A got %d frames, expected 1", a.rx_frames);
    CHECK(a.last.type == SS_MSG_EVT_CONTROL, "wrong reverse type");

    /* Zero-length application payloads are legal. */
    CHECK(ss_link_send(&a.link, SS_MSG_STORE_END, NULL, 0), "empty send failed");
    step(&w, &a, &b, 4);
    CHECK(b.rx_frames == 2 && b.last.len == 0, "empty frame not delivered");
}

static void test_send_refused_when_down(void)
{
    printf("sends refused while down\n");

    node_t a;
    node_init(&a);

    const uint8_t p[] = {1, 2, 3};
    CHECK(!ss_link_send(&a.link, SS_MSG_LCD_RAW, p, sizeof(p)),
          "send must fail before the handshake");
    CHECK(a.link.tx_dropped_down == 1, "drop not counted");
    CHECK(a.link.tx_frames == 0, "nothing should have been queued");
}

static void test_version_mismatch(void)
{
    printf("version mismatch\n");

    node_t a;
    node_init(&a);
    ss_link_poll(&a.link, 0);

    /* A peer built from an older protocol header. */
    uint8_t wire[SS_MAX_ENCODED];
    const uint8_t theirs = SS_PROTOCOL_VERSION + 42u;
    const size_t n = ss_frame_encode(SS_MSG_HELLO, 0, &theirs, 1,
                                     wire, sizeof(wire));
    for (size_t i = 0; i < n; i++) ss_link_rx_byte(&a.link, wire[i]);
    ss_link_poll(&a.link, 1);

    CHECK(!ss_link_is_up(&a.link), "must refuse to come up on version mismatch");
    CHECK(a.link.version_rejects == 1, "reject not counted (%u)",
          a.link.version_rejects);

    /* A NAK must have been queued, so the peer learns why it was refused
     * rather than just seeing silence. */
    uint8_t out[512];
    const size_t got = ss_link_tx_pull(&a.link, out, sizeof(out));
    ss_rx_t p;
    ss_rx_init(&p);
    ss_frame_t f;
    bool saw_nak = false;
    for (size_t i = 0; i < got; i++)
        if (ss_rx_byte(&p, out[i], &f))
            if (f.type == SS_MSG_NAK && f.len >= 1 &&
                f.payload[0] == SS_NAK_BAD_VERSION) saw_nak = true;
    CHECK(saw_nak, "expected a NAK(BAD_VERSION) in the output");
}

static void test_keepalive(void)
{
    printf("keepalive over a long idle\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);
    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);
    CHECK(ss_link_is_up(&a.link), "precondition: link up");

    /* Thirty seconds of no application traffic. Ping/pong alone must hold it
     * up — ten times the timeout, so a broken keepalive cannot pass by luck. */
    const uint32_t t = run_for(&w, &a, &b, 1, 30000, 100);
    (void)t;

    CHECK(ss_link_is_up(&a.link), "A dropped while idle");
    CHECK(ss_link_is_up(&b.link), "B dropped while idle");
    CHECK(a.link.link_downs == 0, "A bounced %u times", a.link.link_downs);
    CHECK(a.rx_frames == 0 && b.rx_frames == 0,
          "keepalive must not surface as application frames");
}

static void test_timeout_and_recovery(void)
{
    printf("timeout and recovery\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);
    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);
    CHECK(ss_link_is_up(&a.link), "precondition: link up");

    /* Unplug the CYD. */
    w.cut = true;
    uint32_t t = run_for(&w, &a, &b, 1, SS_LINK_TIMEOUT_MS + 500, 100);

    CHECK(!ss_link_is_up(&a.link), "A should have timed out");
    CHECK(!ss_link_is_up(&b.link), "B should have timed out");
    CHECK(a.link.link_downs == 1, "A counted %u downs, expected 1",
          a.link.link_downs);

    /* Sends must fail cleanly while it is gone, not queue up forever. */
    const uint8_t p[] = {9};
    CHECK(!ss_link_send(&a.link, SS_MSG_LCD_RAW, p, 1), "send must fail while down");

    /* Plug it back in. */
    w.cut = false;
    t = run_for(&w, &a, &b, t, SS_HELLO_INTERVAL_MS * 3, 50);

    CHECK(ss_link_is_up(&a.link), "A failed to recover");
    CHECK(ss_link_is_up(&b.link), "B failed to recover");
    CHECK(a.link.link_ups == 2, "A counted %u ups, expected 2", a.link.link_ups);

    /* And traffic flows again. */
    a.rx_frames = b.rx_frames = 0;
    CHECK(ss_link_send(&a.link, SS_MSG_LCD_RAW, p, 1), "send failed after recovery");
    t += 10;
    step(&w, &a, &b, t);
    CHECK(b.rx_frames == 1, "no traffic after recovery");
}

static void test_tx_backpressure(void)
{
    printf("tx backpressure\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);
    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);

    /* Fill the TX ring without ever draining it — a CYD that has stopped
     * reading. The Feather must shed frames rather than stall. */
    uint8_t big[SS_MAX_PAYLOAD];
    memset(big, 0xC3, sizeof(big));

    int queued = 0;
    for (int i = 0; i < 64; i++)
        if (ss_link_send(&a.link, SS_MSG_STORE_CHUNK, big, sizeof(big))) queued++;

    CHECK(queued > 0, "nothing fit at all");
    CHECK(queued < 64, "ring should have filled; queued all %d", queued);
    CHECK(a.link.tx_dropped_full == (uint32_t)(64 - queued),
          "drops %u != refusals %d", a.link.tx_dropped_full, 64 - queued);

    /* The critical property: frames that were accepted are intact. A partial
     * write would desynchronise the peer for every subsequent frame. */
    b.rx_frames = 0;
    for (int i = 0; i < 40; i++) step(&w, &a, &b, (uint32_t)(2 + i));

    CHECK(b.rx_frames == queued, "B received %d frames, expected %d",
          b.rx_frames, queued);
    CHECK(b.last.len == SS_MAX_PAYLOAD && b.last.payload[0] == 0xC3,
          "delivered payload corrupted");
    CHECK(b.link.parser.err_crc == 0 && b.link.parser.err_cobs == 0,
          "peer saw framing errors: crc=%u cobs=%u",
          b.link.parser.err_crc, b.link.parser.err_cobs);
}

static void test_rx_overflow(void)
{
    printf("rx overflow\n");

    node_t a;
    node_init(&a);

    /* Shove in more than the RX ring holds without polling. */
    int refused = 0;
    for (size_t i = 0; i < SS_RX_RING + 128u; i++)
        if (!ss_link_rx_byte(&a.link, 0x5A)) refused++;

    CHECK(refused > 0, "overflow should have been refused");
    CHECK(a.link.rx_dropped_full == (uint32_t)refused, "drops not counted");

    /* Still usable afterwards. The garbage has no delimiter, so it must be
     * closed with one before a frame can be parsed — resync happens at frame
     * boundaries and nowhere else. Without this the next frame is swallowed. */
    ss_link_poll(&a.link, 0);
    ss_link_rx_byte(&a.link, 0x00);

    uint8_t wire[SS_MAX_ENCODED];
    const uint8_t v = SS_PROTOCOL_VERSION;
    const size_t n = ss_frame_encode(SS_MSG_HELLO, 0, &v, 1, wire, sizeof(wire));
    for (size_t i = 0; i < n; i++) ss_link_rx_byte(&a.link, wire[i]);
    ss_link_poll(&a.link, 1);
    CHECK(ss_link_is_up(&a.link), "link unusable after an RX overflow");
}

static void test_corruption_survival(void)
{
    printf("corruption survival\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);
    step(&w, &a, &b, 0);
    step(&w, &a, &b, 1);

    /* Let the handshake traffic clear, so the corruption lands on the
     * application frame rather than on a trailing HELLO_ACK. */
    for (uint32_t t = 2; t < 6; t++) step(&w, &a, &b, t);

    /* Corrupt one byte of the next thing A transmits. */
    w.corrupt_at = w.delivered + 4;

    const uint8_t p1[] = {0xAA, 0xBB, 0xCC};
    ss_link_send(&a.link, SS_MSG_LCD_RAW, p1, sizeof(p1));
    step(&w, &a, &b, 6);

    CHECK(b.rx_frames == 0, "corrupted frame must not be delivered");
    /* One flip may register as either a CRC failure or, if the byte became
     * 0x00, a split frame — so count errors rather than pinning the kind. */
    CHECK(b.link.parser.err_crc + b.link.parser.err_cobs +
          b.link.parser.err_short >= 1, "corruption was not detected");

    /* The link must not have latched into a bad state. */
    w.corrupt_at = 0;
    const uint8_t p2[] = {0x11, 0x22};
    CHECK(ss_link_send(&a.link, SS_MSG_LCD_RAW, p2, sizeof(p2)), "send failed");
    step(&w, &a, &b, 7);

    CHECK(b.rx_frames == 1, "link did not recover from a corrupt frame");
    CHECK(b.last.len == 2 && b.last.payload[0] == 0x11, "recovered frame wrong");
    CHECK(ss_link_is_up(&b.link), "B should still be up");
}

static void test_clock_wraparound(void)
{
    printf("millis wraparound\n");

    wire_t w = {0};
    node_t a, b;
    node_init(&a);
    node_init(&b);

    /* Start ~2 seconds before the 32-bit millisecond counter wraps, which the
     * real device hits after roughly 49 days of uptime. */
    const uint32_t t0 = 0xFFFFFFFFu - 2000u;
    step(&w, &a, &b, t0);
    step(&w, &a, &b, t0 + 1);
    CHECK(ss_link_is_up(&a.link), "precondition: link up");

    /* Drive across the boundary. Signed or naive comparisons would either
     * time the link out or wedge the keepalive here. */
    uint32_t t = t0 + 1;
    for (int i = 0; i < 200; i++) { t += 100; step(&w, &a, &b, t); }

    CHECK(ss_link_is_up(&a.link), "A dropped across the wrap");
    CHECK(ss_link_is_up(&b.link), "B dropped across the wrap");
    CHECK(a.link.link_downs == 0, "A bounced across the wrap");

    const uint8_t p[] = {0x5A};
    b.rx_frames = 0;
    CHECK(ss_link_send(&a.link, SS_MSG_LCD_RAW, p, 1), "send failed after wrap");
    t += 10;
    step(&w, &a, &b, t);
    CHECK(b.rx_frames == 1, "no traffic after the wrap");
}

int main(void)
{
    printf("== synthseqr link transport ==\n");

    test_handshake();
    test_app_frames();
    test_send_refused_when_down();
    test_version_mismatch();
    test_keepalive();
    test_timeout_and_recovery();
    test_tx_backpressure();
    test_rx_overflow();
    test_corruption_survival();
    test_clock_wraparound();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
