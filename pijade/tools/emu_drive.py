"""Emulator display flow driver (written for T3.19B, reusable).

Why a separate tap(): jadectl.Jade.press() cannot detect settling on a fresh daemon and
fails with "display did not change" (measured 2026-08-27). This sends raw btn events,
verifies the change itself and retries dropped presses (SILENT loss, measured).

Pitfall (measured): on the SeedQR parts screen, right/left moves button focus in the
header, not the BLOCKS; advancing a block requires CLICKING the '>' button.

Usage:
    import sys; sys.argv = ['x', '/probe/sockB']
    exec(open('emu_drive.py').read())   # or import this file
    right(2); tap('click'); shot('screen')
"""
import hashlib, sys, time, jadectl, kbd

j = jadectl.Jade(sys.argv[1] if len(sys.argv) > 1 else '/probe/sockB')
DEADLINE = 3.0

def snap():
    return hashlib.sha256(j.display_bytes()).digest()

PRE_DELAY = 0.12
RETRY = 2

def tap(ev, expect_change=True, label=''):
    """Send a raw btn and verify the display change (press() cannot detect settling on this daemon)."""
    before = snap()
    for attempt in range(RETRY + 1):
        time.sleep(PRE_DELAY)
        assert j.btn(ev).get('result') is True, 'btn %s rejected' % ev
        deadline = time.time() + DEADLINE
        while time.time() < deadline:
            time.sleep(0.05)
            if snap() != before:
                if not expect_change:
                    raise SystemExit('%s: display SHOULD NOT HAVE CHANGED (%s)' % (ev, label))
                return True
        if not expect_change:
            return False
        # The press was dropped SILENTLY (measured). Resend the same event;
        # the unchanged display proves the first press was not applied.
        print('  ! %s dropped, resending (%d) [%s]' % (ev, attempt + 1, label))
    raise SystemExit('%s: display did not change within %g s, %d attempts (%s)' % (ev, DEADLINE, RETRY + 1, label))


def shot(name):
    j.shot(name)
    print('  shot %s' % name)

def right(n):
    for _ in range(n):
        tap('right')
