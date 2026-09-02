#include "ss_power.h"

#include <string.h>

static inline uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }

static void call(void (*fn)(void *), void *user) { if (fn) fn(user); }

void ss_power_init(ss_power_t *pw, ss_link_t *link,
                   const ss_power_hooks_t *hooks, void *user, bool sw_on)
{
    memset(pw, 0, sizeof(*pw));
    pw->link = link;
    if (hooks) pw->hooks = *hooks;
    pw->user = user;
    pw->state = SS_PWR_RUNNING;
    /* Seed from the actual switch position. Booting with the rocker already off
     * must not look like a fresh off-transition, and must not come up running:
     * the first poll settles it into shutdown. */
    pw->sw_on = sw_on;
    pw->sw_raw = sw_on;
}

void ss_power_switch(ss_power_t *pw, bool sw_on, uint32_t now_ms)
{
    if (!pw->started) {
        pw->started = true;
        pw->sw_since_ms = now_ms;
    }
    if (sw_on != pw->sw_raw) {
        pw->sw_raw = sw_on;
        pw->sw_since_ms = now_ms;
        return;
    }
    if (sw_on != pw->sw_on &&
        elapsed(now_ms, pw->sw_since_ms) >= SS_PWR_DEBOUNCE_MS) {
        pw->sw_on = sw_on;
    }
}

bool ss_power_on_frame(ss_power_t *pw, const ss_frame_t *frame)
{
    if ((frame->type & 0xF0u) != 0x60u) return false;

    if (frame->type == SS_MSG_PWR_SAVED) {
        /* Only meaningful while we are actually waiting for it. A late reply
         * from a previous cycle must not shortcut a later one. */
        if (pw->state == SS_PWR_QUIESCING) {
            pw->last_save_status = (frame->len >= 1) ? frame->payload[0]
                                                     : SS_PWR_SAVE_FAILED;
            pw->saw_saved = true;
        }
    }
    return true;
}

/* Queue a power frame. Returns whether it was accepted; most callers here are
 * on a path that must complete regardless of whether the peer can be reached. */
static bool notify(ss_power_t *pw, uint8_t type)
{
    return ss_link_send(pw->link, type, NULL, 0);
}

static void begin_shutdown(ss_power_t *pw, uint32_t now_ms)
{
    pw->shutdowns++;
    pw->saw_saved = false;
    pw->last_save_status = SS_PWR_SAVE_FAILED;

    /* Before anything else, and before any timeout can apply: silence the
     * instrument. Everything after this point is allowed to be slow. */
    call(pw->hooks.quiesce, pw->user);

    pw->req_sent = notify(pw, SS_MSG_PWR_SHUTDOWN);
    pw->state = SS_PWR_QUIESCING;
    pw->phase_ms = now_ms;
}

static void finish_shutdown(ss_power_t *pw, uint32_t now_ms)
{
    call(pw->hooks.save_local, pw->user);
    notify(pw, SS_MSG_PWR_SLEEP);
    pw->state = SS_PWR_FLUSHING;
    pw->phase_ms = now_ms;
}

static void wake(ss_power_t *pw, uint8_t type)
{
    notify(pw, type);
    call(pw->hooks.resume, pw->user);
    pw->state = SS_PWR_RUNNING;
}

void ss_power_poll(ss_power_t *pw, uint32_t now_ms)
{
    switch (pw->state) {
    case SS_PWR_RUNNING:
        if (!pw->sw_on) begin_shutdown(pw, now_ms);
        return;

    case SS_PWR_QUIESCING:
        if (pw->sw_on) {
            /* Flipped back before we committed. Nothing has been persisted or
             * slept, so the sequencer can simply come back up. */
            pw->aborts++;
            wake(pw, SS_MSG_PWR_ABORT);
            return;
        }
        if (pw->saw_saved) {
            finish_shutdown(pw, now_ms);
        } else if (!ss_link_is_up(pw->link)) {
            /* No peer to wait for. Standalone operation is a supported mode,
             * not an error, so this is not counted as a timeout. */
            finish_shutdown(pw, now_ms);
        } else if (!pw->req_sent) {
            /* The request never made it out — the link was down when the rocker
             * moved, which is exactly what happens when the board is powered up
             * with the rocker already off. Now that there is a peer, ask it,
             * and give it the full window from the point it could have heard.
             * req_sent latches, so this restart happens at most once. */
            if (notify(pw, SS_MSG_PWR_SHUTDOWN)) {
                pw->req_sent = true;
                pw->phase_ms = now_ms;
            }
        } else if (elapsed(now_ms, pw->phase_ms) >= SS_PWR_SAVE_TIMEOUT_MS) {
            pw->save_timeouts++;
            finish_shutdown(pw, now_ms);
        }
        return;

    case SS_PWR_FLUSHING:
        if (pw->sw_on) {
            /* PWR_SLEEP may already be on the wire; PWR_WAKE follows it and the
             * CYD applies them in order, so it lands back awake either way. */
            pw->aborts++;
            wake(pw, SS_MSG_PWR_WAKE);
            return;
        }
        if (ss_link_tx_pending(pw->link) == 0 ||
            elapsed(now_ms, pw->phase_ms) >= SS_PWR_FLUSH_TIMEOUT_MS) {
            pw->state = SS_PWR_ASLEEP;
        }
        return;

    case SS_PWR_ASLEEP:
        if (pw->sw_on) wake(pw, SS_MSG_PWR_WAKE);
        return;
    }
}
