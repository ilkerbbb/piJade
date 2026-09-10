"""Measure that a message cannot be signed from a screen that has not drawn it.

The summary lists Message, Hash and Path as rows and scrolls a row's text only while that row is
selected, so a tick on the summary could sign text the screen had not finished drawing.  Upstream
puts one there; this fork does not.  The only accept is on the message screen itself, and where the
message needs two screens it is on the second, so everything signed has been drawn first.

Each case drives the real firmware and checks whether a signature comes back. Accepting walks
also require the exact pixels of every message page, in order, before the final press:

  short_reject   summary, open the message, press back      -> no signature
  short_accept   summary, open the message, press the tick  -> signature
  long_partial   summary, open page 1, return to summary    -> no signature
  long_page1     summary, open page 1, press next           -> no signature (page 2 not accepted)
  long_accept    summary, page 1, next, tick                -> signature

The suite fails in both directions: a build that put the accept back on the summary would sign in
the reject cases, and a build that lost the accept altogether would fail the accept cases.
That second half is not hypothetical.  Upstream leaves page 2's tick raising NEXT, which walks back
to the summary, so removing the summary's accept on its own left a long message with no way to
sign at all; measured here on 2026-09-10 before page 2's button was bound to accept.

Run it inside the jade-dev container, from the repo root mounted at /jade:

    docker exec jade-dev sh -lc 'cd /jade && python3 pijade/tools/t65_show_before_sign.py'

It starts and stops its own daemon, so build_linux_nci_log must be current (real presses, not the
CI build, whose unattended mode answers every screen with its default event).  Public vectors only.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, '/jade/pijade/tools')
from jadectl import Jade
from t51_measure import font

DAEMON = '/jade/build_linux_nci_log/libjade/libjade_daemon'
MNEMONIC = ' '.join(['abandon'] * 11 + ['about'])
PATH = [44 + 2**31, 2**31, 2**31, 0, 0]
SHORT = 'hello'
# over MAX_DISPLAY_MESSAGE_LEN / 2, so the firmware splits it into Message (1/2) and (2/2)
LONG = 'START' + 'x' * 140 + 'END'
# the split itself: main/ui/sign_message.c takes one screen at 96 bytes and two at 97, and the
# boundary is where a message would most easily lose its tail without anyone noticing
ONE_SCREEN = 'A' * 95 + 'Z'
TWO_SCREEN = 'A' * 96 + 'Z'
# BBB-AIRGAP: W has the largest advance in the display font. Exercise the last allowed byte
# with the widest text, so clipping a page cannot hide behind the shorter existing vectors.
MAX_WIDE = 'W' * 190 + 'Z'

# name, message, entry point, presses from the summary, whether a signature must come back
CASES = [
    ('short_reject', SHORT, 'rpc', ['right', 'click', 'click'], False),
    ('short_accept', SHORT, 'rpc', ['right', 'click', 'right', 'click'], True),
    ('long_partial', LONG, 'rpc', ['right', 'click', 'left', 'click'], False),
    ('long_page1', LONG, 'rpc', ['right', 'click'], False),
    ('long_accept', LONG, 'rpc', ['right', 'click', 'click', 'click'], True),
    # the QR reader's own entry point, which is the only one the device has in the field
    ('file_reject', SHORT, 'file', ['right', 'click', 'click'], False),
    ('file_accept', SHORT, 'file', ['right', 'click', 'right', 'click'], True),
    ('file_long_accept', LONG, 'file', ['right', 'click', 'click', 'click'], True),
    # at 96 bytes one screen carries the message, so the single screen accept must work
    ('boundary96_accept', ONE_SCREEN, 'rpc', ['right', 'click', 'right', 'click'], True),
    # at 97 the message is split, so the presses that sign a one screen message must not sign
    ('boundary97_reject', TWO_SCREEN, 'rpc', ['right', 'click', 'right', 'click'], False),
    ('boundary97_accept', TWO_SCREEN, 'rpc', ['right', 'click', 'click', 'click'], True),
    ('max191_accept', MAX_WIDE, 'rpc', ['right', 'click', 'click', 'click'], True),
    # a last glyph whose ink reaches past the text area but stays on the panel
    ('overhang_accept', 'iiiiiWWWWWWWWWWf', 'rpc', ['right', 'click', 'right', 'click'], True),
    ('file_max191_accept', MAX_WIDE, 'file', ['right', 'click', 'click', 'click'], True),
]


def message_body(page):
    """BBB-AIRGAP: expected white glyphs on black below the 48-pixel header, at 240x240.

    Reuse the font decoder already used by T51. Layout follows display_print_in_area():
    two pixels of side padding, a leading newline, and wrapping by glyph advance. Fail if
    ANY glyph would be clipped, instead of reproducing a truncated renderer's output.
    """
    assert 0 < len(page) <= 96
    glyphs, data = font()
    height = max(g[1] + g[2] for g in glyphs.values())
    body = bytearray(240 * (240 - 48) * 2)
    x, y = 2, height
    for char in page:
        width, rows, dy, dx, advance, start = glyphs[char]
        if x + advance > 238:
            x, y = 2, y + height
        assert y <= 192 - height, 'message exceeds the visible body'
        for row in range(rows):
            for col in range(width):
                bit = row * width + col
                if data[start + bit // 8] & (0x80 >> (bit % 8)):
                    px, py = x + dx + col, y + dy + row
                    # The firmware wraps on the advance (display.c, TFT_X + xDelta against the
                    # area edge) but paints ink against the display itself (is_within_limits), so
                    # a glyph wider than its advance may legitimately reach past the area and is
                    # dropped only where it leaves the panel.  Asserting on the area instead was
                    # measured to reject text the device draws in full: "iiiiiWWWWWWWWWWf" ends
                    # with an f that starts at column 232 and inks through 238.  That case is in
                    # the table below so this stays measured rather than argued.
                    if not (0 <= px < 240 and 0 <= py < 192):
                        continue
                    offset = (py * 240 + px) * 2
                    body[offset:offset + 2] = b'\xff\xff'
        x += max(width, advance) + 1
    return bytes(body)


def pages_drawn(message, frames):
    # BBB-AIRGAP: whole-screen hashes also count selection changes and the return screen.
    # Match each complete page's body in order, excluding the frame AFTER the accepting press.
    # A blank, truncated, repeated or skipped page must fail even if its header changes.
    expected = [message_body(message[i:i + 96]) for i in range(0, len(message), 96)]
    page = 0
    for frame in frames[:-1]:
        assert len(frame) == 240 * 240 * 2, 'expected the device display geometry'
        if frame[48 * 240 * 2:] == expected[page]:
            page += 1
            if page == len(expected):
                return True
    return False


def main(work):
    sock = os.path.join(work, 'sock')
    log = open(os.path.join(work, 'daemon.log'), 'wb')
    daemon = subprocess.Popen([DAEMON, '--socketfile', sock, '--settings',
                               os.path.join(work, 'settings'), '--log-level', 'info'],
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            if os.path.exists(sock):
                break
            time.sleep(0.2)
        j = Jade(sock)
        assert (j.w, j.h) == (240, 240), 'T65 requires the device display geometry'
        # Every read is bounded.  A blocking one was measured to hang the whole suite under the
        # mutation that puts the accept back on the summary: presses meant for the message screen
        # land on the home screen instead, and a suite that hangs is not a guard.
        j.s.settimeout(6.0)

        def screen():
            return hashlib.sha256(j.display_bytes()).digest()

        def settle(timeout=6.0):
            last = None
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                cur = screen()
                if cur == last:
                    return
                last = cur
                time.sleep(0.4)

        settle()
        before = screen()
        rid = j.send('debug_set_mnemonic', {'mnemonic': MNEMONIC, 'temporary_wallet': True})
        for _ in range(40):
            time.sleep(0.3)
            if screen() != before:
                break
        j.btn('click')
        assert 'error' not in j.recv_id(rid), 'the test wallet would not load'

        fails = 0
        for name, message, entry, presses, want_signature in CASES:
            j.btn('alt')            # KEY3, back to the home screen, so every case starts alike
            settle()
            before = screen()
            params = ({'path': PATH, 'message': message} if entry == 'rpc'
                      else {'message_file': 'signmessage m/44h/0h/0h/0/0 ascii:' + message})
            rid = j.send('sign_message', params)
            for _ in range(20):
                time.sleep(0.25)
                if screen() != before:
                    break
            time.sleep(0.6)
            # BBB-AIRGAP: keep pixels, not just hashes, to check message content before signing.
            seen = [j.display_bytes()]
            for press in presses:
                j.btn(press)
                time.sleep(0.6)
                seen.append(j.display_bytes())
            distinct = len(set(seen))
            try:
                signed = 'result' in j.recv_id(rid)
            except Exception:
                signed = False
            # BBB-AIRGAP: retain the old distinct-screen guard and also require the actual text.
            pages = 1 if len(message) <= 96 else 2
            drawn = distinct >= 1 + pages and pages_drawn(message, seen)
            ok = signed == want_signature and (drawn or not want_signature)
            fails += 0 if ok else 1
            print('%-4s %-18s %-4s %-24s signed=%-5s screens=%d drawn=%-5s%s'
                  % ('PASS' if ok else 'FAIL', name, entry, ','.join(presses), signed, distinct, drawn,
                     '' if ok else '   <== expected signed=%s over %d screens'
                     % (want_signature, 1 + pages)))
            if not signed:
                # KEY3 declines from wherever the presses left us, so the next case starts clean.
                j.btn('alt')
                try:
                    j.recv_id(rid)
                except Exception:
                    pass
        return fails
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as work:
        failures = main(work)
    print('T65 SHOW BEFORE SIGN %s (%d failure(s))' % ('OK' if failures == 0 else 'BROKEN', failures))
    sys.exit(1 if failures else 0)
