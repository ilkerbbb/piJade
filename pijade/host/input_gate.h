#ifndef PIJADE_INPUT_GATE_H
#define PIJADE_INPUT_GATE_H

/*
 * BBB-AIRGAP: decides whether a button press should be discarded because the panel was not yet
 * showing the thing the press would act on.
 *
 * The property being protected: a press applies to what the user was READING when they made it.
 *
 * What the gate is allowed to know is the whole design. It cannot see what a click acts on: that
 * lives inside each screen's own loop, as a dice face, a script type, a brightness value or a PIN
 * digit, and three review rounds showed that trying to be told about each one is open ended, a
 * screen missed being a silent hole and a screen over-reported being a device that stops answering
 * its buttons. So the gate knows only what is observable at this boundary:
 *
 *   - when a panel write started and ended
 *   - which screen is current, as libjade_activity_generation(), a number that moves when Jade
 *     replaces the current screen and at no other time
 *   - how many jobs Jade's gui task has been given and how many it has drained, as
 *     libjade_jobs_posted() and libjade_jobs_drained()
 *
 * The job counters are what make this rule exact rather than a guess about timing. A write that
 * merely STARTED after a press need not SHOW that press: the gui task drains its repaint queue and
 * only then flushes, so a press arriving between the drain and the flush changes the selection
 * while the frame already composed still shows the old one. Timestamps cannot tell those apart.
 * The counters can: dispatching a press posts its job before libjade_input() returns, and the
 * queue is FIFO, so a frame flushed once the drain count has caught up carries that job. An
 * earlier design waited for a second write instead; a Codex round measured its cost, because on a
 * static screen the gui task flushes once and then has nothing more to draw, so every ordinary
 * navigation cost the user a further 250 ms of dropped clicks - the very symptom being fixed.
 *
 * The screen counter is still needed, because a screen can be replaced with no press behind it:
 * auto-scan leaves the camera loop the moment a QR decodes (main/camera.c:538) and the caller puts
 * up a confirm screen, so a press dispatched at that instant would land on a screen nobody read.
 *
 * The rule:
 *
 *   1. A dispatched press that posted at least one job, navigation included, arms a pending mark
 *      holding the job count read straight after the dispatch. Navigation is not judged by the
 *      gate, but it does change the panel, so it has to be waited on. A press that posted nothing
 *      changed nothing and arms nothing, so no-op navigation - a wheel press at a carousel limit -
 *      cannot make the device stop answering.
 *   2. A write whose drain count has reached the mark carries every job that press posted. It
 *      clears the mark and becomes the moment the panel last settled. BBB-AIRGAP: if completion
 *      beats mark publication, use the last completed write that advanced the drain count to
 *      repair that moment retrospectively; a later write with no drained work must not move it.
 *   3. A write carrying a screen different from the settled one also becomes that moment. A write
 *      that does neither leaves the settle point alone, which is what makes a camera preview frame
 *      cost nothing.
 *   4. An acting press is discarded when the panel has never settled, when it is older than the
 *      settle point, while a pending mark is still unmet, or when Jade's current screen is not the
 *      one that settled.
 *
 * SETTLE_TIMEOUT is insurance against a gui task that has stopped, not part of the rule. In normal
 * operation it never fires: a pending mark means a job is on the queue, a drained job forces a
 * flush (main/gui.c, gui_task: jobs_handled makes the flush unconditional), and that write clears
 * the mark. When the timeout does fire the gate has given up waiting, not concluded that the panel
 * settled; it releases the press so a stalled device is still usable.
 *
 * Five deliberate limits remain.
 *
 * A press whose consequence is posted by another Jade task is not covered. Clicking posts an event
 * (main/gui.c, select_action) and the screen's own task acts on it - digit entry choosing the next
 * random digit, a carousel updating its labels - after libjade_input() has returned, so the count
 * read at dispatch does not name those jobs and the mark can be cleared by a frame composed before
 * them. The window is one task switch plus one frame, tens of milliseconds, against a human
 * re-press of at least about a hundred. Covering it means waiting on wall-clock time again, which
 * is what cost 250 ms per navigation.
 *
 * The press's target is chosen when Jade runs it, not when the gate approves it: libjade_input()
 * calls gui_front_click(), which acts on current_activity as it stands at that moment, and the
 * panel lock cannot be held across a call into Jade. A swap landing in between sends the press to
 * the new screen. That window is a mutex release plus a function call.
 *
 * A press between two frames of one multi-frame update acts on a half drawn screen. Neither this
 * rule nor the interval rule it replaces covers that; closing it means batching the update inside
 * Jade.
 *
 * The end timestamp is the caller's completion observation, not a hardware one, so a press made
 * after the panel physically finished but before that reading can still be discarded.
 *
 * BBB-AIRGAP: if an unrelated job's frame completes after the press's own frame but before its
 * mark is published, retrospective repair takes that later drain-advancing completion. A press
 * between those completions is then discarded. The extra window is their completion-time
 * difference, bounded by the delay from the press's frame completion to mark publication; it
 * has no fixed wall-clock bound if the input thread is descheduled. Keeping the first qualifying
 * completion would require frame history, so we deliberately keep only the last one. Frames
 * that drain nothing add no extra window.
 *
 * This deliberately replaces the older rule "discard any press whose timestamp fell inside a panel
 * write". That rule discarded 33 presses in a single device run (measured on the device, 2026-09-09),
 * because the camera screen repaints continuously and a repaint shows nothing new.
 *
 * Every field is written and read under the caller's panel state lock.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * How long an acting press waits on a pending mark before the gate gives up on the gui task. Device
 * measurements: gui tick 50 ms, full-frame write 26 to 36 ms, so a mark met normally is cleared
 * inside about 90 ms. This is an order of magnitude above that on purpose: it is not a tuning knob
 * for comfort, it is the point at which the gate stops trusting that a frame is still coming.
 */
#define INPUT_GATE_SETTLE_TIMEOUT_NS UINT64_C(1000000000)

/* Why a press was discarded, for the drop log. */
typedef enum {
    INPUT_GATE_ALLOW = 0,
    INPUT_GATE_DROP_NO_FRAME, /* nothing has ever been written, or the clock reading is unusable */
    INPUT_GATE_DROP_OLDER, /* the press predates the frame now on the panel */
    INPUT_GATE_DROP_PENDING, /* an earlier press's work has not reached the panel yet */
    INPUT_GATE_DROP_SCREEN, /* the screen changed and its frame has not been written */
} input_gate_verdict_t;

typedef struct {
    /*
     * A dispatched press whose work is not on the panel yet. 'pending_jobs' is the job count read
     * straight after the dispatch; the mark is met by the first write that has drained that far.
     * 'pending_since_ns' feeds the timeout above and nothing else.
     */
    uint32_t pending_jobs;
    uint64_t pending_since_ns;
    bool have_pending;

    /* The write now in flight: the screen and the drain count it is carrying. */
    uint64_t active_start_ns;
    uint32_t active_gen;
    uint32_t active_drained;
    bool have_active;

    /* BBB-AIRGAP: the last COMPLETED write that advanced the drain count, for a mark published
     * after its frame. Live drained counts and in-flight writes cannot prove panel completion. */
    uint32_t completed_drained;
    uint64_t drain_advanced_ns;
    uint32_t drain_advanced_gen;
    bool have_completed;

    /* When the panel last showed something new, and which screen that was. */
    uint64_t settled_ns;
    uint32_t settled_gen;
    bool have_settled;

    /* Bookkeeping for the drop log only. */
    uint64_t last_start_ns;
    uint64_t last_end_ns;
} input_gate_t;

/*
 * Record that a press has been handed to Jade and posted work. Called for EVERY dispatched press
 * that posted at least one job, navigation included, immediately AFTER libjade_input() returns:
 * the jobs the press produced have to be on the queue before their count is read, or the mark
 * would name work that has not been posted and a frame composed before the press would meet it.
 *
 * 'posted' is libjade_jobs_posted() read at that same moment. 'dispatch_ns' is only the start of
 * the timeout window.
 */
static inline void input_gate_input_dispatched(
    input_gate_t* const gate, const uint64_t dispatch_ns, const uint32_t posted)
{
    gate->pending_jobs = posted;
    gate->pending_since_ns = dispatch_ns;
    gate->have_pending = true;

    /* BBB-AIRGAP: the GUI can finish before the input thread acquires the panel lock. Clear an
     * already-met mark AND repair settlement, so presses made during its frame stay stale.
     * Never undo a newer settlement, including a screen change with no additional drained work. */
    if (gate->have_completed && (int32_t)(gate->completed_drained - posted) >= 0) {
        gate->have_pending = false;
        if (!gate->have_settled || gate->drain_advanced_ns > gate->settled_ns) {
            gate->settled_ns = gate->drain_advanced_ns;
            gate->settled_gen = gate->drain_advanced_gen;
            gate->have_settled = true;
        }
    }
}

/*
 * A panel write is starting, carrying the screen Jade reports for the frame it hands over and the
 * number of jobs Jade had drained when it composed that frame.
 */
static inline void input_gate_write_begin(
    input_gate_t* const gate, const uint64_t start_ns, const uint32_t screen_gen, const uint32_t drained)
{
    gate->active_start_ns = start_ns;
    gate->active_gen = screen_gen;
    gate->active_drained = drained;
    gate->have_active = true;
}

/* The write has finished. Pass UINT64_MAX when the completion clock reading is unusable. */
static inline void input_gate_write_end(input_gate_t* const gate, const uint64_t end_ns)
{
    gate->last_start_ns = gate->active_start_ns;
    gate->last_end_ns = end_ns;

    /* BBB-AIRGAP: remember completion even when no mark has arrived yet. Counts arrive in FIFO
     * order and may wrap; equality means this write drained nothing new and must not move it. */
    if (!gate->have_completed || gate->active_drained != gate->completed_drained) {
        gate->completed_drained = gate->active_drained;
        gate->drain_advanced_ns = end_ns;
        gate->drain_advanced_gen = gate->active_gen;
        gate->have_completed = true;
    }

    /* Rule 2. Signed difference because both counters wrap at 32 bits; a bare >= would treat the
     * wrap as the mark suddenly being far in the future and stop settling. */
    const bool meets_pending
        = gate->have_pending && (int32_t)(gate->active_drained - gate->pending_jobs) >= 0;
    const bool new_screen = !gate->have_settled || gate->active_gen != gate->settled_gen;

    /* Rule 3. */
    if (meets_pending || new_screen) {
        gate->settled_ns = end_ns;
        gate->settled_gen = gate->active_gen;
        gate->have_settled = true;
    }

    if (meets_pending) {
        gate->have_pending = false;
    }

    gate->have_active = false;
    gate->active_start_ns = 0;
}

/*
 * Rule 4, for a press that ACTS. Navigation must not be passed through here: it moves a selection
 * or a value that the user then reads before acting, and discarding it is what makes a device feel
 * like it needs two or three pushes.
 *
 * 'current_gen' is libjade_activity_generation() read at press time. It is what closes the window
 * between Jade making a screen current and the frame for it reaching the panel.
 */
static inline input_gate_verdict_t input_gate_judge(
    const input_gate_t* const gate, const uint64_t press_ns, const uint32_t current_gen)
{
    if (gate->last_end_ns == UINT64_MAX || !gate->have_settled) {
        return INPUT_GATE_DROP_NO_FRAME;
    }
    if (press_ns < gate->settled_ns) {
        return INPUT_GATE_DROP_OLDER;
    }
    if (gate->have_pending && press_ns < gate->pending_since_ns + INPUT_GATE_SETTLE_TIMEOUT_NS) {
        return INPUT_GATE_DROP_PENDING;
    }
    if (current_gen != gate->settled_gen) {
        return INPUT_GATE_DROP_SCREEN;
    }
    return INPUT_GATE_ALLOW;
}

/* The verdict as it appears in the drop log. */
static inline const char* input_gate_verdict_name(const input_gate_verdict_t verdict)
{
    switch (verdict) {
    case INPUT_GATE_ALLOW:
        return "allow";
    case INPUT_GATE_DROP_NO_FRAME:
        return "no-frame";
    case INPUT_GATE_DROP_OLDER:
        return "older";
    case INPUT_GATE_DROP_PENDING:
        return "pending";
    case INPUT_GATE_DROP_SCREEN:
        return "screen";
    }
    return "?";
}

/*
 * Duration of the last COMPLETED write, in microseconds, or 0 when there is none or its clock
 * readings are unusable. Not the duration of a write still in flight: that one has no end yet.
 */
static inline uint64_t input_gate_last_write_us(const input_gate_t* const gate)
{
    if (gate->last_end_ns == UINT64_MAX || gate->last_end_ns < gate->last_start_ns) {
        return 0;
    }
    return (gate->last_end_ns - gate->last_start_ns) / 1000;
}

#endif /* PIJADE_INPUT_GATE_H */
