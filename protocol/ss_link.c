#include "ss_link.h"

#include <string.h>

/* Unsigned subtraction, so a 32-bit millisecond counter wrapping at ~49 days
 * behaves correctly rather than pinning the link down forever. */
static inline uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }

/* ---------------------------------------------------------------- rings */

static inline uint16_t ring_used(uint16_t head, uint16_t tail, uint16_t size)
{
    return (uint16_t)((head - tail) & (size - 1u));
}

/* One slot is always left empty so full and empty stay distinguishable. */
static inline uint16_t ring_free(uint16_t head, uint16_t tail, uint16_t size)
{
    return (uint16_t)(size - 1u - ring_used(head, tail, size));
}

bool ss_link_rx_byte(ss_link_t *lk, uint8_t b)
{
    const uint16_t head = lk->rx_head;
    const uint16_t next = (uint16_t)((head + 1u) & (SS_RX_RING - 1u));
    if (next == lk->rx_tail) {
        lk->rx_dropped_full++;
        return false;
    }
    lk->rx_ring[head] = b;
    lk->rx_head = next;
    return true;
}

size_t ss_link_tx_pull(ss_link_t *lk, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint16_t tail = lk->tx_tail;
    while (n < cap && tail != lk->tx_head) {
        out[n++] = lk->tx_ring[tail];
        tail = (uint16_t)((tail + 1u) & (SS_TX_RING - 1u));
    }
    lk->tx_tail = tail;
    return n;
}

size_t ss_link_tx_pending(const ss_link_t *lk)
{
    return ring_used(lk->tx_head, lk->tx_tail, SS_TX_RING);
}

/* ---------------------------------------------------------------- send */

/* Queues a frame regardless of link state. Frames are all-or-nothing: a frame
 * that does not fit entirely is not written at all, because a partial frame
 * would corrupt the peer's parse of the next one. */
static bool queue_frame(ss_link_t *lk, uint8_t type,
                        const uint8_t *payload, size_t len)
{
    uint8_t wire[SS_MAX_ENCODED];
    const size_t n = ss_frame_encode(type, lk->tx_seq, payload, len,
                                     wire, sizeof(wire));
    if (n == 0) return false;

    if (ring_free(lk->tx_head, lk->tx_tail, SS_TX_RING) < n) {
        lk->tx_dropped_full++;
        return false;
    }

    uint16_t head = lk->tx_head;
    for (size_t i = 0; i < n; i++) {
        lk->tx_ring[head] = wire[i];
        head = (uint16_t)((head + 1u) & (SS_TX_RING - 1u));
    }
    lk->tx_head = head;

    lk->tx_seq++;
    lk->tx_frames++;
    return true;
}

bool ss_link_send(ss_link_t *lk, uint8_t type, const uint8_t *payload, size_t len)
{
    if (lk->state != SS_LINK_UP) {
        lk->tx_dropped_down++;
        return false;
    }
    return queue_frame(lk, type, payload, len);
}

/* ---------------------------------------------------------------- state */

static void go_down(ss_link_t *lk)
{
    if (lk->state == SS_LINK_DOWN) return;
    lk->state = SS_LINK_DOWN;
    lk->peer_version = 0;
    lk->link_downs++;
    /* The TX ring is deliberately not flushed. Doing so would race the
     * consumer, which may be a TX ISR, and stale frames are harmless: they
     * carry a valid CRC and the peer discards what it cannot use. */
}

static void go_up(ss_link_t *lk, uint8_t peer_version)
{
    if (lk->state == SS_LINK_UP) return;
    lk->state = SS_LINK_UP;
    lk->peer_version = peer_version;
    lk->link_ups++;
}

static void handle_internal(ss_link_t *lk, const ss_frame_t *f, uint32_t now_ms)
{
    switch (f->type) {
    case SS_MSG_HELLO: {
        if (f->len < 1) return;
        const uint8_t theirs = f->payload[0];
        if (theirs != SS_PROTOCOL_VERSION) {
            /* Fail loudly. The boards are flashed independently, so silent
             * version skew would surface much later as inexplicable corruption. */
            const uint8_t reason = SS_NAK_BAD_VERSION;
            queue_frame(lk, SS_MSG_NAK, &reason, 1);
            lk->version_rejects++;
            go_down(lk);
            return;
        }
        const uint8_t mine = SS_PROTOCOL_VERSION;
        queue_frame(lk, SS_MSG_HELLO_ACK, &mine, 1);
        lk->last_tx_ms = now_ms;
        go_up(lk, theirs);
        return;
    }

    case SS_MSG_HELLO_ACK: {
        if (f->len < 1) return;
        const uint8_t theirs = f->payload[0];
        if (theirs != SS_PROTOCOL_VERSION) {
            lk->version_rejects++;
            go_down(lk);
            return;
        }
        go_up(lk, theirs);
        return;
    }

    case SS_MSG_PING:
        queue_frame(lk, SS_MSG_PONG, NULL, 0);
        lk->last_tx_ms = now_ms;
        return;

    case SS_MSG_PONG:
        return;   /* liveness already recorded via last_rx_ms */

    case SS_MSG_NAK:
        if (f->len >= 1 && f->payload[0] == SS_NAK_BAD_VERSION) {
            lk->version_rejects++;
            go_down(lk);
        }
        return;

    default:
        return;
    }
}

void ss_link_init(ss_link_t *lk, ss_link_frame_cb cb, void *user)
{
    memset(lk, 0, sizeof(*lk));
    ss_rx_init(&lk->parser);
    lk->cb = cb;
    lk->user = user;
    lk->state = SS_LINK_DOWN;
}

void ss_link_poll(ss_link_t *lk, uint32_t now_ms)
{
    /* Seed the timers on the first poll rather than in init, so a link created
     * long before the main loop starts does not immediately look timed out. */
    if (!lk->started) {
        lk->started = true;
        lk->last_rx_ms = now_ms;
        lk->last_tx_ms = now_ms;
        lk->last_hello_ms = now_ms - SS_HELLO_INTERVAL_MS;  /* HELLO on this poll */
    }

    /* Drain everything received since the last poll. Bounded by the ring size,
     * so this cannot become an unbounded stall. */
    ss_frame_t f;
    while (lk->rx_tail != lk->rx_head) {
        const uint8_t b = lk->rx_ring[lk->rx_tail];
        lk->rx_tail = (uint16_t)((lk->rx_tail + 1u) & (SS_RX_RING - 1u));

        if (!ss_rx_byte(&lk->parser, b, &f)) continue;

        lk->last_rx_ms = now_ms;

        if ((f.type & 0xF0u) == 0x00u) {
            handle_internal(lk, &f, now_ms);
        } else if (lk->state == SS_LINK_UP && lk->cb != NULL) {
            lk->cb(&f, lk->user);
        }
        /* Application frames arriving while down are discarded: without a
         * completed handshake we cannot trust the peer's protocol version. */
    }

    if (lk->state == SS_LINK_UP) {
        if (elapsed(now_ms, lk->last_rx_ms) >= SS_LINK_TIMEOUT_MS) {
            go_down(lk);
        } else if (elapsed(now_ms, lk->last_tx_ms) >= SS_PING_INTERVAL_MS) {
            if (queue_frame(lk, SS_MSG_PING, NULL, 0)) lk->last_tx_ms = now_ms;
        }
    }

    if (lk->state == SS_LINK_DOWN &&
        elapsed(now_ms, lk->last_hello_ms) >= SS_HELLO_INTERVAL_MS) {
        const uint8_t mine = SS_PROTOCOL_VERSION;
        if (queue_frame(lk, SS_MSG_HELLO, &mine, 1)) {
            lk->last_tx_ms = now_ms;
        }
        /* Advance regardless, so a full TX ring cannot turn the retry into a
         * tight loop that starves the rest of the main loop. */
        lk->last_hello_ms = now_ms;
    }
}
