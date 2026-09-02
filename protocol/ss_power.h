/*
 * Synthseqr v3 — soft power.
 *
 * S1 is a maintained rocker on the Feather's MISO (PB22, EXTINT[6]). It does
 * not break the 5V rail: J1 feeds both boards through D1/D2, so "off" has to be
 * a cooperative shutdown into a low-power state rather than a power cut. The
 * point of the exercise is the save window — the CYD owns storage, and yanking
 * the rail mid-write is exactly the corruption case this avoids.
 *
 * The Feather owns the switch and drives the sequence. The CYD only answers.
 *
 *   rocker off ──> quiesce (notes off) ──> PWR_SHUTDOWN ──> await PWR_SAVED
 *              ──> save_local ──> PWR_SLEEP ──> flush TX ──> ASLEEP
 *
 * THE RULE THIS EXISTS TO ENFORCE: a missing, wedged, or out-of-date CYD must
 * never prevent shutdown. Every wait has a deadline and every deadline ends in
 * powering down anyway. Losing the CYD's copy is bad; refusing to turn off is
 * worse, because the user's only recourse is pulling the barrel jack — which is
 * the failure mode we are trying to eliminate.
 *
 * No Arduino, no UART, no clock source: the caller supplies the debounced-or-not
 * switch level and now_ms, and performs the actual sleep when the state says to.
 */

#ifndef SS_POWER_H
#define SS_POWER_H

#include "ss_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A rocker bounces for a few ms. This is deliberately long: an accidental brush
 * against the switch should not tear down a running sequence. */
#define SS_PWR_DEBOUNCE_MS      80u

/* How long the CYD gets to persist before the Feather proceeds regardless.
 * Generous — an SD write behind a busy LVGL loop is not quick — but bounded. */
#define SS_PWR_SAVE_TIMEOUT_MS 3000u

/* Cap on draining the TX ring so PWR_SLEEP actually reaches the peer. A wedged
 * or absent UART consumer must not strand us here. */
#define SS_PWR_FLUSH_TIMEOUT_MS 250u

typedef enum {
    SS_PWR_RUNNING = 0,
    SS_PWR_QUIESCING,   /* sequencer stopped, waiting on the CYD's save */
    SS_PWR_FLUSHING,    /* PWR_SLEEP queued, draining it out of the TX ring */
    SS_PWR_ASLEEP       /* caller should now enter standby */
} ss_pwr_state_t;

/* All optional. Called from ss_power_poll, main-loop context only.
 *
 * quiesce runs first and must be fast and unconditional: stop the transport and
 * send all-notes-off. Skipping it leaves notes hanging on external gear with no
 * sequencer left running to release them.
 *
 * save_local writes the FlashAsEEPROM fallback, so a Feather that boots with no
 * CYD attached still comes back with its patterns. */
typedef struct {
    void (*quiesce)(void *user);
    void (*save_local)(void *user);
    void (*resume)(void *user);
} ss_power_hooks_t;

typedef struct {
    ss_link_t *link;
    ss_power_hooks_t hooks;
    void *user;

    ss_pwr_state_t state;

    /* debounce */
    bool sw_on;             /* accepted level */
    bool sw_raw;            /* last sampled level */
    uint32_t sw_since_ms;   /* when sw_raw last changed */
    bool started;

    uint32_t phase_ms;      /* entered the current timed phase */
    bool     req_sent;      /* PWR_SHUTDOWN actually reached the TX ring */

    /* diagnostics */
    uint8_t  last_save_status;
    bool     saw_saved;         /* CYD answered on the last shutdown */
    uint32_t shutdowns;
    uint32_t save_timeouts;
    uint32_t aborts;
} ss_power_t;

/* sw_on is the logical "powered on" level, already mapped from the pin. S1
 * closes to GND against an internal pull-up, so on the Feather this is
 * (digitalRead(PIN_SLIDE) == LOW) — or its negation, depending on which way the
 * rocker is fitted. Keep that mapping at the call site, not in here. */
void ss_power_init(ss_power_t *pw, ss_link_t *link,
                   const ss_power_hooks_t *hooks, void *user, bool sw_on);

/* Feed the raw switch level every main-loop iteration. Debounced internally. */
void ss_power_switch(ss_power_t *pw, bool sw_on, uint32_t now_ms);

/* Offer a received frame. Returns true if it belonged to the power class and
 * was consumed. Call from the link's frame callback. */
bool ss_power_on_frame(ss_power_t *pw, const ss_frame_t *frame);

/* Advance the sequence. Call every main-loop iteration, after ss_link_poll. */
void ss_power_poll(ss_power_t *pw, uint32_t now_ms);

static inline ss_pwr_state_t ss_power_state(const ss_power_t *pw) { return pw->state; }

/* True once it is safe to enter standby. The caller sleeps, and on wake keeps
 * calling ss_power_switch — the transition back to RUNNING is what fires the
 * resume hook and sends PWR_WAKE. */
static inline bool ss_power_should_sleep(const ss_power_t *pw)
{
    return pw->state == SS_PWR_ASLEEP;
}

#ifdef __cplusplus
}
#endif

#endif /* SS_POWER_H */
