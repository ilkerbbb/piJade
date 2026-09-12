# -*- coding: utf-8 -*-
"""Item 78 (audit A2): the drilldown split boundary of sign_message, measured on both sides.

SIGN_MESSAGE_MAX_DISPLAY_LEN is MAX_DISPLAY_MESSAGE_LEN / 2 = 96.  A message of 96 bytes
takes the one-screen branch (tick button), 97 bytes takes the two-screen branch, whose
two screens carry the two snprintf calls changed by upstream 255cab8e: the first uses the
precision argument ("\n%.*s", the one whose type changed from size_t to int), the second
the tail pointer (msgtxt + SIGN_MESSAGE_MAX_DISPLAY_LEN).

The summary screen does NOT exercise either call, so it is not evidence on its own: the
route walks down into the message screen (down, click) and, on the two-screen branch, on
into 2/2 (click).  Every drawn frame is dumped, so the same run before and after the
change can be compared byte for byte.

No signature is produced: the tick on the last screen is never pressed; each request is
cancelled with KEY3.  Public BIP39 test vector only.
"""
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import menu_audit
from t58_message_len import (TEST_MNEMONIC, SIGN_PATH, DRAW_BUDGET, RESPONSE_BUDGET,
                             CBOR_RPC_USER_CANCELLED, message_of, start_daemon, stop_daemon, wait)

TAG = sys.argv[1] if len(sys.argv) > 1 else 'post'
# length -> how many message screens the branch draws
CASES = [(96, 1), (97, 2), (191, 2)]


def run_case(a, length, screens, errors):
    label = '%d bytes' % length
    before = hashlib.sha256(a.j.display_bytes()).digest()
    rid = a.j.send('sign_message', {'path': SIGN_PATH, 'message': message_of(length)})
    deadline = time.monotonic() + DRAW_BUDGET
    drawn = False
    while time.monotonic() < deadline:
        wait(0.1)
        if hashlib.sha256(a._read(deadline)).digest() != before:
            drawn = True
            break
    if not drawn:
        errors.append('%s: confirmation NOT drawn' % label)
        return
    a.settle()
    shots = [a.shot('%s_%d_sum' % (TAG, length))]

    # Summary -> message screen 1, then on into 2/2 where the branch draws one.
    # Every message screen is dumped, not just the last: screen 1 carries the precision
    # argument and screen 2 the tail pointer, so a single frame would leave one untested.
    # The tick that would sign sits on the last screen and is never pressed.
    try:
        a.j.press('down')
        for i in range(1, screens + 1):
            a.j.press('click')
            shots.append(a.shot('%s_%d_s%d' % (TAG, length, i)))
    except Exception as exc:
        errors.append('%s: navigation into the message screens failed: %s' % (label, exc))
        return

    a.j.btn('alt')
    r = a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
    if r is None:
        errors.append('%s: no response after KEY3' % label)
    elif 'result' in r:
        errors.append('%s: KEY3 produced a signature' % label)
    else:
        code = (r.get('error') or {}).get('code')
        if code != CBOR_RPC_USER_CANCELLED:
            errors.append('%s: cancel code %s, expected %d' % (label, code, CBOR_RPC_USER_CANCELLED))
        else:
            print('78 PASS: %s, %d message screen(s) drawn, cancelled; frames %s'
                  % (label, screens, ', '.join(shots)))
    a.settle()


def main():
    proc, log_file, sock, log = start_daemon('t78' + TAG)
    errors = []
    try:
        a = menu_audit.Audit(sock, log, 't78')
        a.seed(TEST_MNEMONIC)
        if a.findings:
            raise SystemExit('78: wallet setup failed: %s' % a.findings)
        if not a.settle():
            raise SystemExit('78: screen did not settle after wallet setup')
        for n, screens in CASES:
            run_case(a, n, screens, errors)
    finally:
        stop_daemon(proc, log_file)
    for h in errors:
        print('ERROR: %s' % h, file=sys.stderr)
    print('78 RESULT: %d error(s)' % len(errors))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
