/*
 * Native tests for the Synthseqr v3 soft-power sequence.
 *
 * A Feather running ss_power against a CYD that answers the way the real one
 * will, over the same wire model as test_link. The cases that matter are the
 * unhappy ones: the CYD silent, absent, or slow. None of them may prevent the
 * instrument from turning off.
 */

#include "../../protocol/ss_power.h"

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
    ss_power_t pw;
    char       log[64];      /* Q quiesce, S save_local, R resume */
    size_t     log_n;
    uint32_t   quiesce_at;
    uint32_t   now;
} feather_t;

typedef struct {
    ss_link_t link;
    bool      respond;
    uint32_t  delay_ms;
    bool      pending;
    uint32_t  due_ms;
    uint8_t   status;
    int       n_shutdown, n_sleep, n_wake, n_abort;
    bool      asleep;
} cyd_t;

static void logc(feather_t *f, char c)
{
    if (f->log_n + 1 < sizeof(f->log)) f->log[f->log_n++] = c;
}

static void h_quiesce(void *u)
{
    feather_t *f = (feather_t *)u;
    logc(f, 'Q');
    f->quiesce_at = f->now;
}
static void h_save_local(void *u) { logc((feather_t *)u, 'S'); }
static void h_resume(void *u)     { logc((feather_t *)u, 'R'); }

static const ss_power_hooks_t HOOKS = { h_quiesce, h_save_local, h_resume };

static void feather_frame(const ss_frame_t *fr, void *user)
{
    feather_t *f = (feather_t *)user;
    ss_power_on_frame(&f->pw, fr);
}

static void cyd_frame(const ss_frame_t *fr, void *user)
{
    cyd_t *c = (cyd_t *)user;
    switch (fr->type) {
    case SS_MSG_PWR_SHUTDOWN:
        c->n_shutdown++;
        if (c->respond) { c->pending = true; c->due_ms = c->delay_ms; }
        break;
    case SS_MSG_PWR_SLEEP: c->n_sleep++; c->asleep = true;  break;
    case SS_MSG_PWR_WAKE:  c->n_wake++;  c->asleep = false; break;
    case SS_MSG_PWR_ABORT: c->n_abort++; c->asleep = false; break;
    default: break;
    }
}

typedef struct { bool cut; size_t rate; } wire_t;

static void pump(wire_t *w, ss_link_t *from, ss_link_t *to)
{
    uint8_t buf[128];
    size_t budget = (w->rate != 0) ? w->rate : 256u;
    while (budget > 0) {
        const size_t want = (budget < sizeof(buf)) ? budget : sizeof(buf);
        const size_t n = ss_link_tx_pull(from, buf, want);
        if (n == 0) break;
        budget -= n;
        if (w->cut) continue;
        for (size_t i = 0; i < n; i++) ss_link_rx_byte(to, buf[i]);
    }
}

/* One millisecond of both boards. Order mirrors the real main loops: bytes are
 * already moving under interrupt, then link, then application. */
static void step(wire_t *w, feather_t *f, cyd_t *c, bool sw_on, uint32_t t)
{
    f->now = t;
    pump(w, &f->link, &c->link);
    pump(w, &c->link, &f->link);

    ss_link_poll(&f->link, t);
    ss_link_poll(&c->link, t);

    if (c->pending) {
        if (c->due_ms == 0) {
            ss_link_send(&c->link, SS_MSG_PWR_SAVED, &c->status, 1);
            c->pending = false;
        } else {
            c->due_ms--;
        }
    }

    ss_power_switch(&f->pw, sw_on, t);
    ss_power_poll(&f->pw, t);
}

/* Bring both ends up and settle the handshake. Returns the next timestamp. */
static uint32_t bring_up(wire_t *w, feather_t *f, cyd_t *c, uint32_t t0)
{
    memset(f, 0, sizeof(*f));
    memset(c, 0, sizeof(*c));
    c->respond = true;
    c->status  = SS_PWR_SAVE_OK;

    ss_link_init(&f->link, feather_frame, f);
    ss_link_init(&c->link, cyd_frame, c);
    ss_power_init(&f->pw, &f->link, &HOOKS, f, true);

    uint32_t t = t0;
    for (int i = 0; i < 600; i++, t++) step(w, f, c, true, t);
    return t;
}

static void run(wire_t *w, feather_t *f, cyd_t *c, bool sw, int ms, uint32_t *t)
{
    for (int i = 0; i < ms; i++, (*t)++) step(w, f, c, sw, *t);
}

/* ------------------------------------------------------------- tests */

static void test_normal_shutdown(void)
{
    printf("normal shutdown\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);

    CHECK(ss_link_is_up(&f.link), "link should be up before shutdown");
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "should start running");

    run(&w, &f, &c, false, 500, &t);

    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "should reach ASLEEP");
    CHECK(ss_power_should_sleep(&f.pw), "should report safe to sleep");
    CHECK(strcmp(f.log, "QS") == 0, "hooks should run quiesce then save, got \"%s\"", f.log);
    CHECK(c.n_shutdown == 1, "CYD should see one shutdown, saw %d", c.n_shutdown);
    CHECK(c.n_sleep == 1, "CYD should see one sleep, saw %d", c.n_sleep);
    CHECK(c.asleep, "CYD should be asleep");
    CHECK(f.pw.saw_saved, "should have recorded the CYD's save");
    CHECK(f.pw.last_save_status == SS_PWR_SAVE_OK, "status should be OK");
    CHECK(f.pw.save_timeouts == 0, "no timeout expected, got %u",
          (unsigned)f.pw.save_timeouts);
}

static void test_quiesce_is_immediate(void)
{
    printf("quiesce precedes every wait\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);
    c.delay_ms = 900;             /* slow save */

    const uint32_t off_at = t + SS_PWR_DEBOUNCE_MS;
    run(&w, &f, &c, false, 2000, &t);

    /* Notes must stop as soon as the switch is believed, not after the CYD
     * finishes writing — otherwise a slow save holds a note on. */
    CHECK(f.quiesce_at <= off_at + 2,
          "quiesce at %u, expected by %u", (unsigned)f.quiesce_at, (unsigned)off_at);
    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "should still complete");
    CHECK(strcmp(f.log, "QS") == 0, "expected \"QS\", got \"%s\"", f.log);
}

static void test_debounce(void)
{
    printf("debounce rejects a brief blip\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);

    run(&w, &f, &c, false, SS_PWR_DEBOUNCE_MS / 2, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "blip must not start shutdown");
    run(&w, &f, &c, true, 200, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "still running after blip");
    CHECK(f.log_n == 0, "no hooks should have fired, got \"%s\"", f.log);
    CHECK(c.n_shutdown == 0, "CYD should not have been told to save");
}

static void test_cyd_silent(void)
{
    printf("silent CYD times out but still sleeps\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);
    c.respond = false;

    run(&w, &f, &c, false, SS_PWR_DEBOUNCE_MS + 100, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_QUIESCING, "should be waiting on the save");

    run(&w, &f, &c, false, SS_PWR_SAVE_TIMEOUT_MS + 500, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "must sleep despite no reply");
    CHECK(f.pw.save_timeouts == 1, "should count one timeout, got %u",
          (unsigned)f.pw.save_timeouts);
    CHECK(!f.pw.saw_saved, "should not claim a save happened");
    CHECK(strcmp(f.log, "QS") == 0, "local save must still run, got \"%s\"", f.log);
}

static void test_no_cyd_at_all(void)
{
    printf("absent CYD sleeps promptly\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);

    w.cut = true;
    run(&w, &f, &c, true, SS_LINK_TIMEOUT_MS + 200, &t);
    CHECK(!ss_link_is_up(&f.link), "link should have dropped");

    const uint32_t t_off = t;
    run(&w, &f, &c, false, SS_PWR_DEBOUNCE_MS + 400, &t);

    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "should sleep without a peer");
    /* Standalone is a supported mode, so this is not a timeout. And it must not
     * burn the full save window waiting for a peer that is known to be gone. */
    CHECK(f.pw.save_timeouts == 0, "absent peer is not a timeout, got %u",
          (unsigned)f.pw.save_timeouts);
    CHECK(t - t_off < SS_PWR_SAVE_TIMEOUT_MS, "should not wait the full window");
}

static void test_abort_mid_shutdown(void)
{
    printf("rocker back on mid-save aborts\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);
    c.delay_ms = 5000;            /* never lands within the window */

    run(&w, &f, &c, false, SS_PWR_DEBOUNCE_MS + 50, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_QUIESCING, "should be quiescing");

    run(&w, &f, &c, true, SS_PWR_DEBOUNCE_MS + 200, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "should return to running");
    CHECK(strcmp(f.log, "QR") == 0, "expected quiesce then resume, got \"%s\"", f.log);
    CHECK(c.n_abort == 1, "CYD should see one abort, saw %d", c.n_abort);
    CHECK(c.n_sleep == 0, "CYD must not have been told to sleep");
    CHECK(f.pw.aborts == 1, "should count the abort");
}

static void test_wake_from_asleep(void)
{
    printf("wake from asleep\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);

    run(&w, &f, &c, false, 500, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "asleep first");

    run(&w, &f, &c, true, 500, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "should resume");
    CHECK(!ss_power_should_sleep(&f.pw), "should no longer ask to sleep");
    CHECK(strcmp(f.log, "QSR") == 0, "expected \"QSR\", got \"%s\"", f.log);
    CHECK(c.n_wake == 1, "CYD should see one wake, saw %d", c.n_wake);
    CHECK(!c.asleep, "CYD should be awake again");
}

static void test_boot_with_rocker_off(void)
{
    printf("boot with the rocker already off\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    memset(&f, 0, sizeof(f)); memset(&c, 0, sizeof(c));
    c.respond = true; c.status = SS_PWR_SAVE_OK;

    ss_link_init(&f.link, feather_frame, &f);
    ss_link_init(&c.link, cyd_frame, &c);
    ss_power_init(&f.pw, &f.link, &HOOKS, &f, false);   /* rocker off at boot */

    uint32_t t = 1000;
    run(&w, &f, &c, false, 1000, &t);

    /* Plugging in with the rocker off must not bring the instrument up. The
     * initial level is seeded in init, so this is not treated as a fresh
     * off-transition needing a debounce. */
    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "should settle straight to asleep");
    CHECK(f.pw.shutdowns == 1, "one shutdown, got %u", (unsigned)f.pw.shutdowns);

    run(&w, &f, &c, true, 500, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "should come up when switched on");
}

static void test_cycle_repeats(void)
{
    printf("repeated off/on cycles\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 1000);

    for (int i = 0; i < 3; i++) {
        run(&w, &f, &c, false, 400, &t);
        CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "cycle %d should sleep", i);
        run(&w, &f, &c, true, 400, &t);
        CHECK(ss_power_state(&f.pw) == SS_PWR_RUNNING, "cycle %d should wake", i);
    }
    CHECK(f.pw.shutdowns == 3, "should count three shutdowns, got %u",
          (unsigned)f.pw.shutdowns);
    CHECK(c.n_sleep == 3 && c.n_wake == 3,
          "CYD should see 3 sleeps and 3 wakes, saw %d/%d", c.n_sleep, c.n_wake);
}

static void test_millis_wrap(void)
{
    printf("shutdown across the millis wrap\n");
    wire_t w = {0}; feather_t f; cyd_t c;
    uint32_t t = bring_up(&w, &f, &c, 0xFFFFFFFFu - 4000u);
    c.respond = false;            /* force the timeout path across the wrap */

    run(&w, &f, &c, false, SS_PWR_SAVE_TIMEOUT_MS + 1000, &t);
    CHECK(ss_power_state(&f.pw) == SS_PWR_ASLEEP, "wrap must not strand shutdown");
    CHECK(f.pw.save_timeouts == 1, "timeout should fire exactly once, got %u",
          (unsigned)f.pw.save_timeouts);
}

int main(void)
{
    test_normal_shutdown();
    test_quiesce_is_immediate();
    test_debounce();
    test_cyd_silent();
    test_no_cyd_at_all();
    test_abort_mid_shutdown();
    test_wake_from_asleep();
    test_boot_with_rocker_off();
    test_cycle_repeats();
    test_millis_wrap();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
