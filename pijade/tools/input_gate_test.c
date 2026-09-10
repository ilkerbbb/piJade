/*
 * BBB-AIRGAP: standalone check of the input gate rule in pijade/host/input_gate.h.
 *
 * Build and run:
 *   cc -Wall -Wextra -Werror -O1 -o /tmp/input_gate_test pijade/tools/input_gate_test.c
 *   /tmp/input_gate_test
 *
 * Every case the rule exists for is a fixture below, written as a timeline of what the host
 * observed. The fixtures are then replayed against deliberately WRONG versions of the rule, one
 * clause broken at a time; a wrong version that still passes every fixture would mean the fixtures
 * do not actually pin the rule down. The wrong versions are listed with the fixture that catches
 * each, so a fixture deleted in future shows up as a mutation that survives.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "../host/input_gate.h"

static int failures = 0;

static void check(const char* const what, const int got, const int expected)
{
    if (got == expected) {
        printf("PASS: %s\n", what);
        return;
    }
    printf("FAIL: %s (result %d, expected %d)\n", what, got, expected);
    ++failures;
}

/* Timelines are written in milliseconds; the gate works in nanoseconds. */
#define MS UINT64_C(1000000)

/* Screen numbers. The counter never repeats, so returning to a screen gets a new number. */
#define SCREEN_A 7
#define SCREEN_B 8
#define SCREEN_A_AGAIN 9

typedef enum {
    EV_NONE, /* end of the list */
    EV_DISPATCH, /* a press was handed to Jade at t and had posted 'jobs' by the time it returned */
    EV_DISPATCH_NOOP, /* a press was handed over and posted nothing, so the host arms nothing */
    EV_BEGIN, /* a panel write started at t, carrying screen 'gen', composed after 'jobs' drained */
    EV_END, /* that write finished at t */
    EV_END_NO_CLOCK, /* it finished but the clock reading was unusable */
    EV_ASK, /* an acting press made at t, with gen current: expect this verdict */
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint64_t t;
    uint32_t gen;
    uint32_t jobs;
    input_gate_verdict_t expect;
} ev_t;

#define MAX_EVENTS 18

typedef struct {
    const char* name;
    ev_t events[MAX_EVENTS];
} fixture_t;

/* BBB-AIRGAP: dispatch is replaceable too, to prove retrospective settlement is required. */
typedef void (*dispatch_fn)(input_gate_t*, uint64_t, uint32_t);
typedef void (*write_end_fn)(input_gate_t*, uint64_t);
typedef input_gate_verdict_t (*judge_fn)(const input_gate_t*, uint64_t, uint32_t);

typedef struct {
    const char* name;
    dispatch_fn dispatch;
    write_end_fn write_end;
    judge_fn judge;
    const char* caught_by; /* the fixture expected to reject this wrong version */
} rule_t;

/*
 * The fixtures. Each is the story of one thing the gate exists to get right.
 */
static const fixture_t FIXTURES[] = {
    /* Nothing has been drawn yet, so nothing can have been read. */
    { "BOOT_BEFORE_ANY_FRAME",
        { { EV_ASK, 5 * MS, SCREEN_A, 0, INPUT_GATE_DROP_NO_FRAME }, { EV_NONE, 0, 0, 0, 0 } } },

    /* An unusable completion clock reading is not evidence of anything; refuse to act on it. */
    { "CLOCK_READING_UNUSABLE",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 0, 0 }, { EV_END_NO_CLOCK, 0, 0, 0, 0 },
            { EV_ASK, 50 * MS, SCREEN_A, 0, INPUT_GATE_DROP_NO_FRAME }, { EV_NONE, 0, 0, 0, 0 } } },

    /* The plain case: the screen has been on the panel a while and nothing is happening. */
    { "IDLE_SCREEN_HONOURS_PRESS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 0, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 200 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* A press made while the frame it would act on was still being written. */
    { "PRESS_BEFORE_ITS_FRAME_FINISHED",
        { { EV_BEGIN, 100 * MS, SCREEN_B, 0, 0 }, { EV_END, 135 * MS, SCREEN_B, 0, 0 },
            { EV_ASK, 120 * MS, SCREEN_B, 0, INPUT_GATE_DROP_OLDER }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * The reported fault. The camera screen repaints continuously; none of those frames shows
     * anything the user has to read before acting, so none of them may cost a press.
     * A run on the device dropped 33 presses here under the rule this one replaces.
     */
    { "CAMERA_PREVIEW_FRAMES_COST_NOTHING",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 4, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 5, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 110 * MS, SCREEN_A, 6, 0 }, { EV_END, 145 * MS, SCREEN_A, 0, 0 },
            /* Made right after the first frame, then left in the queue while two more preview
             * frames went by. This is the shape of the 33 drops: under a rule where any frame
             * counts as something new to read, this press is stale by the time it is judged. */
            { EV_ASK, 50 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW },
            { EV_ASK, 150 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW },
            /* And one made while a preview frame was being written, which the interval rule this
             * one replaces discarded outright. */
            { EV_BEGIN, 160 * MS, SCREEN_A, 7, 0 },
            { EV_ASK, 170 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * The regression a Codex round measured against the design that waited for a SECOND write.
     * On a static screen the gui task drains, flushes once, and then has nothing left to draw, so
     * that second write never comes and every ordinary navigation cost the user a further 250 ms
     * of dropped clicks. Here one frame carries the work and the click straight after it is taken.
     */
    { "NAVIGATION_ONE_FRAME_THEN_CLICK",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 50 * MS, 0, 6, 0 }, { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 },
            { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 96 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: settle the SAME screen first, then publish only after the work's frame has
     * completed. Skipping the mark alone would wrongly allow the press made during that frame. */
    { "COMPLETION_BEFORE_MARK_REPAIRS_SETTLEMENT",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 59 * MS, 0, 6, 0 },
            { EV_ASK, 80 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: a later completion with unchanged drainage must not extend the repair. */
    { "LATE_MARK_IGNORES_NO_DRAIN_COMPLETION",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 110 * MS, SCREEN_A, 6, 0 }, { EV_END, 145 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 59 * MS, 0, 6, 0 },
            { EV_ASK, 80 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: deliberately use the LAST drain-advancing completion, even for unrelated work.
     * A subsequent in-flight frame proves that its drain count cannot stand in for completion. */
    { "LATE_MARK_USES_LAST_DRAIN_ADVANCE",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 110 * MS, SCREEN_A, 7, 0 }, { EV_END, 145 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 160 * MS, SCREEN_A, 8, 0 }, { EV_DISPATCH, 59 * MS, 0, 6, 0 },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 146 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: a qualifying count in flight is not a qualifying COMPLETION. */
    { "MARK_WAITS_FOR_IN_FLIGHT_WORK",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_DISPATCH, 59 * MS, 0, 6, 0 },
            { EV_ASK, 80 * MS, SCREEN_A, 0, INPUT_GATE_DROP_PENDING },
            { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 80 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: navigation posts two repaints; completion of only the first cannot meet the
     * count read after dispatch returns, even if that completion beats mark publication. */
    { "LATE_MARK_COVERS_BOTH_NAVIGATION_JOBS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 59 * MS, 0, 7, 0 },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_DROP_PENDING },
            { EV_BEGIN, 110 * MS, SCREEN_A, 7, 0 }, { EV_END, 145 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 120 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 146 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* BBB-AIRGAP: retrospective repair must compare wrapped job counts just like write_end. */
    { "LATE_MARK_JOBS_COUNTER_WRAPS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, UINT32_MAX - 1, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_A, 0, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 59 * MS, 0, UINT32_MAX, 0 },
            { EV_ASK, 80 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * Why the mark is a job count and not a timestamp. The gui task had already drained its queue
     * and was about to flush when the press arrived, so the write that follows the press in TIME
     * still shows the old selection. Its drain count says so, and the mark survives it.
     */
    { "FRAME_DRAINED_BEFORE_POST",
        { /* This write is already in flight, composed after 5 jobs, when the press arrives. */
            { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_DISPATCH, 20 * MS, 0, 6, 0 },
            /* It ends after the press but shows none of it, and its drain count says so. */
            { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 46 * MS, SCREEN_A, 0, INPUT_GATE_DROP_PENDING },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 96 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * A wheel press at a carousel limit changes nothing and queues no repaint. Under a rule that
     * armed on every press it would suppress every click until a timeout, which on a screen with
     * nothing else to draw is a device that has stopped answering. Posting nothing arms nothing.
     */
    { "NO_OP_NAVIGATION_MUST_NOT_LOCK_UP",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH_NOOP, 50 * MS, 0, 5, 0 },
            { EV_ASK, 51 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * Two presses in a row, the second made before the first one's frame arrived. PIN entry is the
     * screen that matters: a click there advances to a fresh digit, and a second click taken early
     * would commit a digit the user never saw.
     */
    { "SECOND_CLICK_BEFORE_NEW_DIGIT_IS_DRAWN",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 50 * MS, 0, 6, 0 },
            { EV_ASK, 55 * MS, SCREEN_A, 0, INPUT_GATE_DROP_PENDING },
            { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * The known limit, measured rather than described. Clicking posts an event and the screen's own
     * task acts on it later, so the count read at dispatch does not name the job that consequence
     * posts. The first frame meets the mark and a press during the NEXT write, the one actually
     * carrying the new digit, is allowed. Covering this means going back to waiting on wall-clock
     * time, which is what cost 250 ms per navigation.
     */
    { "CLICK_CONSEQUENCE_POSTED_BY_SCREEN_TASK",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 50 * MS, 0, 6, 0 }, { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 },
            { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            /* The screen's task now posts job 7 and the frame for it is still being written. */
            { EV_BEGIN, 110 * MS, SCREEN_A, 7, 0 },
            { EV_ASK, 115 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* A frame that took much longer than expected still counts when it finally arrives. */
    { "LATE_FRAME_STILL_COUNTS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 50 * MS, 0, 6, 0 }, { EV_BEGIN, 60 * MS, SCREEN_A, 6, 0 },
            { EV_END, 900 * MS, SCREEN_A, 0, 0 },
            /* Older than the frame that finally showed the change. */
            { EV_ASK, 800 * MS, SCREEN_A, 0, INPUT_GATE_DROP_OLDER },
            { EV_ASK, 950 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* Jade has made a new screen current but its frame has not been written. */
    { "SCREEN_MADE_CURRENT_BUT_NOT_WRITTEN",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 50 * MS, SCREEN_B, 0, INPUT_GATE_DROP_SCREEN }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * Why the screen counter is still needed once the mark is a job count. Auto-scan leaves the
     * camera loop the moment a QR decodes (main/camera.c:538) and the caller puts up a confirm
     * screen. No press was dispatched, so nothing was armed; only the screen number catches it.
     */
    { "QR_DECODE_SWAPS_SCREEN_WITH_NO_PRESS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 50 * MS, SCREEN_B, 0, INPUT_GATE_DROP_SCREEN },
            { EV_BEGIN, 60 * MS, SCREEN_B, 6, 0 }, { EV_END, 95 * MS, SCREEN_B, 0, 0 },
            { EV_ASK, 100 * MS, SCREEN_B, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* Going back to a screen gives it a new number, so its frame has to be waited for again. */
    { "RETURN_TO_A_SCREEN_IS_A_NEW_SCREEN",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_BEGIN, 60 * MS, SCREEN_B, 6, 0 }, { EV_END, 95 * MS, SCREEN_B, 0, 0 },
            { EV_ASK, 100 * MS, SCREEN_A_AGAIN, 0, INPUT_GATE_DROP_SCREEN },
            { EV_BEGIN, 110 * MS, SCREEN_A_AGAIN, 7, 0 }, { EV_END, 145 * MS, SCREEN_A_AGAIN, 0, 0 },
            { EV_ASK, 150 * MS, SCREEN_A_AGAIN, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* A press at exactly the completion instant is not older than it. */
    { "PRESS_EXACTLY_AT_FRAME_END",
        { { EV_BEGIN, 10 * MS, SCREEN_A, 5, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 45 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* Screen zero and time zero are ordinary values, not "unset". */
    { "ZERO_SCREEN_AND_ZERO_TIME",
        { { EV_BEGIN, 0, 0, 0, 0 }, { EV_END, 0, 0, 0, 0 },
            { EV_ASK, 0, 0, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },

    /* The screen counter is 32 bits and wraps; equality has to keep working across the wrap. */
    { "SCREEN_COUNTER_WRAPS",
        { { EV_BEGIN, 10 * MS, UINT32_MAX, 5, 0 }, { EV_END, 45 * MS, 0, 0, 0 },
            { EV_ASK, 50 * MS, UINT32_MAX, 0, INPUT_GATE_ALLOW },
            { EV_ASK, 51 * MS, 0, 0, INPUT_GATE_DROP_SCREEN }, { EV_NONE, 0, 0, 0, 0 } } },

    /*
     * The job counters wrap too. A press marked just below the wrap must still be met by a frame
     * drained just above it; comparing the raw values would see the frame as far behind the mark
     * and never settle again.
     */
    { "JOBS_COUNTER_WRAPS",
        { { EV_BEGIN, 10 * MS, SCREEN_A, UINT32_MAX - 1, 0 }, { EV_END, 45 * MS, SCREEN_A, 0, 0 },
            { EV_DISPATCH, 50 * MS, 0, UINT32_MAX, 0 },
            { EV_ASK, 55 * MS, SCREEN_A, 0, INPUT_GATE_DROP_PENDING },
            { EV_BEGIN, 60 * MS, SCREEN_A, 0, 0 }, { EV_END, 95 * MS, SCREEN_A, 0, 0 },
            { EV_ASK, 100 * MS, SCREEN_A, 0, INPUT_GATE_ALLOW }, { EV_NONE, 0, 0, 0, 0 } } },
};

/*
 * The wrong versions of the rule. Each breaks exactly one thing.
 */

/* Every write settles the panel: the camera preview costs a press again. */
static void we_every_write_settles(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    g->settled_ns = end_ns;
    g->settled_gen = g->active_gen;
    g->have_settled = true;
    g->have_pending = false;
    g->have_active = false;
    g->active_start_ns = 0;
}

/* The old interval rule this design replaces: a press inside a write is discarded outright. */
static void we_old_interval_rule(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    g->settled_ns = end_ns;
    g->settled_gen = g->active_gen;
    g->have_settled = true;
    g->have_pending = false;
    g->have_active = false;
    g->active_start_ns = 0;
}

static input_gate_verdict_t wj_old_interval_rule(
    const input_gate_t* const g, const uint64_t press_ns, const uint32_t current_gen)
{
    if (g->have_active && press_ns >= g->active_start_ns) {
        return INPUT_GATE_DROP_OLDER;
    }
    return input_gate_judge(g, press_ns, current_gen);
}

/* Any write clears a pending mark, whatever it was composed from. */
static void we_any_write_clears_pending(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    const bool new_screen = !g->have_settled || g->active_gen != g->settled_gen;
    if (g->have_pending || new_screen) {
        g->settled_ns = end_ns;
        g->settled_gen = g->active_gen;
        g->have_settled = true;
    }
    g->have_pending = false;
    g->have_active = false;
    g->active_start_ns = 0;
}

/* The mark is never met, so it only ever expires. */
static void we_pending_never_met(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    if (!g->have_settled || g->active_gen != g->settled_gen) {
        g->settled_ns = end_ns;
        g->settled_gen = g->active_gen;
        g->have_settled = true;
    }
    g->have_active = false;
    g->active_start_ns = 0;
}

/* Unsigned comparison of the job counters, which breaks across the wrap. */
static void we_bare_unsigned_compare(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    const bool meets_pending = g->have_pending && g->active_drained >= g->pending_jobs;
    const bool new_screen = !g->have_settled || g->active_gen != g->settled_gen;
    if (meets_pending || new_screen) {
        g->settled_ns = end_ns;
        g->settled_gen = g->active_gen;
        g->have_settled = true;
    }
    if (meets_pending) {
        g->have_pending = false;
    }
    g->have_active = false;
    g->active_start_ns = 0;
}

/* A new screen's frame does not become the settle point. */
static void we_screen_change_not_settled(input_gate_t* const g, const uint64_t end_ns)
{
    g->last_start_ns = g->active_start_ns;
    g->last_end_ns = end_ns;
    const bool meets_pending
        = g->have_pending && (int32_t)(g->active_drained - g->pending_jobs) >= 0;
    if (meets_pending) {
        g->settled_ns = end_ns;
        g->settled_gen = g->active_gen;
        g->have_settled = true;
        g->have_pending = false;
    }
    g->have_active = false;
    g->active_start_ns = 0;
}

/* No frame has ever been written, but act anyway. */
static input_gate_verdict_t wj_no_frame_check(
    const input_gate_t* const g, const uint64_t press_ns, const uint32_t current_gen)
{
    if (press_ns < g->settled_ns) {
        return INPUT_GATE_DROP_OLDER;
    }
    if (g->have_pending && press_ns < g->pending_since_ns + INPUT_GATE_SETTLE_TIMEOUT_NS) {
        return INPUT_GATE_DROP_PENDING;
    }
    if (current_gen != g->settled_gen) {
        return INPUT_GATE_DROP_SCREEN;
    }
    return INPUT_GATE_ALLOW;
}

/* A press older than the settle point is taken anyway. */
static input_gate_verdict_t wj_no_older_check(
    const input_gate_t* const g, const uint64_t press_ns, const uint32_t current_gen)
{
    if (g->last_end_ns == UINT64_MAX || !g->have_settled) {
        return INPUT_GATE_DROP_NO_FRAME;
    }
    if (g->have_pending && press_ns < g->pending_since_ns + INPUT_GATE_SETTLE_TIMEOUT_NS) {
        return INPUT_GATE_DROP_PENDING;
    }
    if (current_gen != g->settled_gen) {
        return INPUT_GATE_DROP_SCREEN;
    }
    return INPUT_GATE_ALLOW;
}

/* A pending mark does not hold a press back. */
static input_gate_verdict_t wj_no_pending_check(
    const input_gate_t* const g, const uint64_t press_ns, const uint32_t current_gen)
{
    if (g->last_end_ns == UINT64_MAX || !g->have_settled) {
        return INPUT_GATE_DROP_NO_FRAME;
    }
    if (press_ns < g->settled_ns) {
        return INPUT_GATE_DROP_OLDER;
    }
    if (current_gen != g->settled_gen) {
        return INPUT_GATE_DROP_SCREEN;
    }
    return INPUT_GATE_ALLOW;
}

/* The screen under the user is not checked at press time. */
static input_gate_verdict_t wj_no_screen_check(
    const input_gate_t* const g, const uint64_t press_ns, const uint32_t current_gen)
{
    (void)current_gen;
    if (g->last_end_ns == UINT64_MAX || !g->have_settled) {
        return INPUT_GATE_DROP_NO_FRAME;
    }
    if (press_ns < g->settled_ns) {
        return INPUT_GATE_DROP_OLDER;
    }
    if (g->have_pending && press_ns < g->pending_since_ns + INPUT_GATE_SETTLE_TIMEOUT_NS) {
        return INPUT_GATE_DROP_PENDING;
    }
    return INPUT_GATE_ALLOW;
}

/* BBB-AIRGAP: this wrong rule clears a late mark but omits retrospective settle-point repair. */
static void wd_skip_retrospective_repair(input_gate_t* const g, const uint64_t dispatch_ns, const uint32_t posted)
{
    const uint64_t settled_ns = g->settled_ns;
    const uint32_t settled_gen = g->settled_gen;
    const bool have_settled = g->have_settled;
    input_gate_input_dispatched(g, dispatch_ns, posted);
    g->settled_ns = settled_ns;
    g->settled_gen = settled_gen;
    g->have_settled = have_settled;
}

static const rule_t RIGHT_RULE = { "the rule", input_gate_input_dispatched, input_gate_write_end, input_gate_judge, NULL };

static const rule_t WRONG_RULES[] = {
    { "every write settles", input_gate_input_dispatched, we_every_write_settles, input_gate_judge,
        "CAMERA_PREVIEW_FRAMES_COST_NOTHING" },
    { "old interval rule", input_gate_input_dispatched, we_old_interval_rule, wj_old_interval_rule,
        "CAMERA_PREVIEW_FRAMES_COST_NOTHING" },
    { "every write clears the mark", input_gate_input_dispatched, we_any_write_clears_pending, input_gate_judge,
        "FRAME_DRAINED_BEFORE_POST" },
    { "mark is never met", input_gate_input_dispatched, we_pending_never_met, input_gate_judge,
        "NAVIGATION_ONE_FRAME_THEN_CLICK" },
    { "unsigned count comparison", input_gate_input_dispatched, we_bare_unsigned_compare, input_gate_judge,
        "JOBS_COUNTER_WRAPS" },
    { "screen change does not settle", input_gate_input_dispatched, we_screen_change_not_settled, input_gate_judge,
        "QR_DECODE_SWAPS_SCREEN_WITH_NO_PRESS" },
    { "trusts empty state", input_gate_input_dispatched, input_gate_write_end, wj_no_frame_check,
        "BOOT_BEFORE_ANY_FRAME" },
    { "ignores older press", input_gate_input_dispatched, input_gate_write_end, wj_no_older_check,
        "PRESS_BEFORE_ITS_FRAME_FINISHED" },
    { "ignores pending mark", input_gate_input_dispatched, input_gate_write_end, wj_no_pending_check,
        "FRAME_DRAINED_BEFORE_POST" },
    { "ignores current screen", input_gate_input_dispatched, input_gate_write_end, wj_no_screen_check,
        "SCREEN_MADE_CURRENT_BUT_NOT_WRITTEN" },
    { "clears late mark without settling", wd_skip_retrospective_repair, input_gate_write_end, input_gate_judge,
        "COMPLETION_BEFORE_MARK_REPAIRS_SETTLEMENT" },
};

/* Replay one fixture against one rule. Returns the number of ASK events that came out wrong. */
static int replay(const fixture_t* const f, const rule_t* const rule, const bool report)
{
    input_gate_t gate;
    memset(&gate, 0, sizeof(gate));
    int wrong = 0;

    for (size_t i = 0; i < MAX_EVENTS && f->events[i].kind != EV_NONE; ++i) {
        const ev_t* const e = &f->events[i];
        switch (e->kind) {
        case EV_DISPATCH:
            rule->dispatch(&gate, e->t, e->jobs);
            break;
        case EV_DISPATCH_NOOP:
            /* The host reads the job count either side of the dispatch and finds them equal, so
             * it arms nothing. Modelled by doing nothing, which is what the host does. */
            break;
        case EV_BEGIN:
            input_gate_write_begin(&gate, e->t, e->gen, e->jobs);
            break;
        case EV_END:
            rule->write_end(&gate, e->t);
            break;
        case EV_END_NO_CLOCK:
            rule->write_end(&gate, UINT64_MAX);
            break;
        case EV_ASK: {
            const input_gate_verdict_t got = rule->judge(&gate, e->t, e->gen);
            if (got != e->expect) {
                ++wrong;
                if (report) {
                    printf("FAIL: %s, event %zu: got %s, expected %s\n", f->name, i,
                        input_gate_verdict_name(got), input_gate_verdict_name(e->expect));
                    ++failures;
                }
            }
            break;
        }
        case EV_NONE:
            break;
        }
    }
    return wrong;
}

int main(void)
{
    const size_t num_fixtures = sizeof(FIXTURES) / sizeof(FIXTURES[0]);
    const size_t num_wrong = sizeof(WRONG_RULES) / sizeof(WRONG_RULES[0]);

    printf("--- the rule, %zu scenarios ---\n", num_fixtures);
    for (size_t i = 0; i < num_fixtures; ++i) {
        const int wrong = replay(&FIXTURES[i], &RIGHT_RULE, true);
        if (!wrong) {
            printf("PASS: %s\n", FIXTURES[i].name);
        }
    }

    printf("\n--- wrong rule versions, %zu total ---\n", num_wrong);
    for (size_t i = 0; i < num_wrong; ++i) {
        const rule_t* const wrongrule = &WRONG_RULES[i];
        int rejected_by_named = 0;
        int rejected_by_any = 0;
        for (size_t j = 0; j < num_fixtures; ++j) {
            const int wrong = replay(&FIXTURES[j], wrongrule, false);
            if (!wrong) {
                continue;
            }
            ++rejected_by_any;
            if (!strcmp(FIXTURES[j].name, wrongrule->caught_by)) {
                rejected_by_named = 1;
            }
        }
        char what[160];
        snprintf(what, sizeof(what), "\"%s\" version rejected by %s", wrongrule->name,
            wrongrule->caught_by);
        check(what, rejected_by_named, 1);
        if (!rejected_by_any) {
            printf("      (no scenario rejected this version)\n");
        }
    }

    printf("\nTOTAL ERRORS: %d\n", failures);
    return failures ? 1 : 0;
}
