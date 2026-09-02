/*
 * Synthseqr v3 — link transport.
 *
 * Sits on top of ss_frame: ring buffers in both directions, the HELLO
 * handshake, and keepalive. Knows nothing about UARTs, Arduino, or time
 * sources — the caller pushes received bytes in, pulls bytes to transmit out,
 * and supplies the clock. That keeps the whole state machine testable on a
 * workstation, and keeps platform glue down to about twenty lines per board.
 *
 * THE RULE THIS EXISTS TO ENFORCE: nothing here blocks. The Feather runs a
 * 24 PPQN sequencer ISR, and a CYD that is absent, slow, or still booting must
 * never be able to stall it. Sends fail fast and are counted rather than
 * waiting for space.
 *
 * Threading: single-producer/single-consumer per direction.
 *   ss_link_rx_byte   - one context only (typically the UART RX ISR)
 *   ss_link_tx_pull   - one context only (main loop, or a TX-empty ISR)
 *   ss_link_send/poll - main loop only
 * On Cortex-M4 and Xtensa the aligned 16-bit index loads and stores are
 * atomic, so no critical sections are needed as long as that split holds.
 */

#ifndef SS_LINK_H
#define SS_LINK_H

#include "ss_frame.h"
#include "ss_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Both must be powers of two.
 *
 * RX is sized by how long the main loop may go without polling. At 1 Mbaud the
 * peer delivers ~100 bytes/ms, so 1024 bytes buys about 10 ms of slack — ample
 * next to the Feather's worst case, which is the ~1 ms blocking bit-bang of a
 * 33-pixel SK6812 refresh. 512 would leave only ~5 ms and no real margin.
 *
 * TX is sized to hold a few maximum-length frames so that bursts of display
 * updates do not immediately hit backpressure. */
#define SS_RX_RING 1024u
#define SS_TX_RING 1024u

#define SS_HELLO_INTERVAL_MS  250u   /* retry cadence while not yet up */
#define SS_PING_INTERVAL_MS  1000u   /* keepalive when otherwise idle */
#define SS_LINK_TIMEOUT_MS   3000u   /* silence this long means the peer is gone */

typedef enum {
    SS_LINK_DOWN = 0,   /* no peer, or the peer was lost */
    SS_LINK_UP          /* handshake complete, versions agree */
} ss_link_state_t;

/* Called from ss_link_poll for application frames. Link-management frames
 * (class 0x0_) are handled internally and never reach this. The frame is
 * borrowed — copy anything that must outlive the call. */
typedef void (*ss_link_frame_cb)(const ss_frame_t *frame, void *user);

typedef struct {
    /* receive */
    uint8_t  rx_ring[SS_RX_RING];
    volatile uint16_t rx_head, rx_tail;
    ss_rx_t  parser;

    /* transmit */
    uint8_t  tx_ring[SS_TX_RING];
    volatile uint16_t tx_head, tx_tail;

    /* state machine */
    ss_link_state_t state;
    uint8_t  tx_seq;
    uint8_t  peer_version;
    uint32_t last_rx_ms;
    uint32_t last_tx_ms;
    uint32_t last_hello_ms;
    bool     started;

    ss_link_frame_cb cb;
    void            *user;

    /* diagnostics — a link that is quietly degrading is otherwise invisible */
    uint32_t tx_frames;
    uint32_t tx_dropped_full;    /* TX ring had no room */
    uint32_t tx_dropped_down;    /* refused because the link was down */
    uint32_t rx_dropped_full;    /* RX ring overflowed; bytes lost */
    uint32_t version_rejects;
    uint32_t link_ups;
    uint32_t link_downs;
} ss_link_t;

void ss_link_init(ss_link_t *lk, ss_link_frame_cb cb, void *user);

/* Feed one received byte. Returns false if the RX ring was full, in which case
 * the byte is dropped and counted. Safe to call from an ISR. */
bool ss_link_rx_byte(ss_link_t *lk, uint8_t b);

/* Drain up to cap bytes of encoded output for the UART. Returns bytes written.
 * Safe to call from a TX ISR. */
size_t ss_link_tx_pull(ss_link_t *lk, uint8_t *out, size_t cap);

/* Queue an application frame. Returns false — immediately, never blocking — if
 * the link is down or the TX ring is full. Callers that care must retry;
 * callers sending superseded data (display updates) should simply drop it. */
bool ss_link_send(ss_link_t *lk, uint8_t type, const uint8_t *payload, size_t len);

/* Run the state machine: parse received bytes, dispatch frames, drive the
 * handshake and keepalive. Call every main-loop iteration. now_ms may be any
 * monotonic millisecond counter; wraparound is handled. */
void ss_link_poll(ss_link_t *lk, uint32_t now_ms);

static inline ss_link_state_t ss_link_state(const ss_link_t *lk) { return lk->state; }
static inline bool ss_link_is_up(const ss_link_t *lk) { return lk->state == SS_LINK_UP; }

/* Bytes currently queued for transmit. Lets the caller shed load before
 * offering more, e.g. skip a display refresh when the link is congested. */
size_t ss_link_tx_pending(const ss_link_t *lk);

#ifdef __cplusplus
}
#endif

#endif /* SS_LINK_H */
