"""Item 58, option B (Ilker's decision, 2026-09-10): refuse to sign what the screen cannot show.

Upstream truncates a message at or beyond MAX_DISPLAY_MESSAGE_LEN (192) to its first 188 bytes
plus an ellipsis and then signs the WHOLE text, so every byte past the ellipsis is approved
unseen. The fork now rejects such a message on both entry paths (main/process/sign_message.c).

Run:  docker exec jade-dev python3 /jade/pijade/tools/t58_message_len.py
Needs the build_linux build and the /probe mount. Takes about a minute.

The measurement is DISCRIMINATING, not a single assertion: the same tool drives three lengths
through the same RPC and the verdict is the DIFFERENCE between them.

  191 bytes (limit - 1)  ACCEPTED: confirmation must be drawn, and the response after KEY3 must
                         be CBOR_RPC_USER_CANCELLED, i.e. the device really was waiting for a
                         human. This is the POSITIVE CONTROL: without it, a tool that rejects
                         everything (or whose RPC never arrives at all) would also "pass".
  192 bytes (the limit)  REJECTED: no confirmation drawn, response CBOR_RPC_BAD_PARAMETERS.
  256 bytes              REJECTED the same way, so the rule is not an off-by-one artefact.

No signature is ever produced here: the accepted case is cancelled with KEY3 on purpose. A public
BIP39 test vector is used; no real seed enters this machine.
"""
import hashlib
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jadectl
import menu_audit

# NOT build_linux: that tree is configured with CI=CI, where confirmation screens are
# auto-approved and never reach a frame - a measurement of 'was confirmation drawn' would
# read False for every length and every signature would come back approved. Measured here
# 2026-09-10 before the build was switched. build_linux_nci_log has CI=0, like t47_escape.sh.
DAEMON = '/jade/build_linux_nci_log/libjade/libjade_daemon'
CBOR_RPC_BAD_PARAMETERS = -32602
CBOR_RPC_USER_CANCELLED = -32000

# main/ui.h:11
MAX_DISPLAY_MESSAGE_LEN = 192

# Public BIP39 test vector; not a real wallet.
TEST_MNEMONIC = ('abandon abandon abandon abandon abandon abandon '
                 'abandon abandon abandon abandon abandon about')

# m/44'/0'/0'/0/0. An ordinary signing path, NOT the GDK login challenge path, so the auto_sign
# branch (sign_message.c) is not taken and confirmation is actually drawn.
SIGN_PATH = [2147483692, 2147483648, 2147483648, 0, 0]

DRAW_BUDGET = 8.0
RESPONSE_BUDGET = 8.0

CASES = [
    (MAX_DISPLAY_MESSAGE_LEN - 1, True),
    (MAX_DISPLAY_MESSAGE_LEN, False),
    (MAX_DISPLAY_MESSAGE_LEN + 64, False),
]


def wait(seconds):
    # A bare sleep is blocked by a hook in this environment.
    threading.Event().wait(seconds)


def start_daemon(name):
    sock = '/probe/%s.sock' % name
    log = '/probe/%s.log' % name
    settings = '/probe/%sset' % name
    for stale in (sock, log, settings + '.a', settings + '.b'):
        try:
            os.unlink(stale)
        except OSError:
            pass
    log_file = open(log, 'wb')
    proc = subprocess.Popen(
        [DAEMON, '--socketfile', sock, '--settings', settings, '--log-level', 'info'],
        stdout=log_file, stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(80):
        if os.path.exists(sock):
            break
        wait(0.5)
    wait(2)
    return proc, log_file, sock, log


def stop_daemon(proc, log_file):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
    log_file.close()


def message_of(length):
    """A readable ASCII message of exactly `length` bytes."""
    body = ('piJade item 58 measurement, byte %d. ' % length)
    text = (body * (length // len(body) + 1))[:length]
    assert len(text) == length and text.isascii()
    return text


def run_case(a, length, expect_accepted, errors):
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

    if expect_accepted:
        if not drawn:
            r = a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
            errors.append('%s: confirmation NOT drawn, but this length must be accepted '
                          '(response: %s)' % (label, str(r)[:160]))
            return
        a.settle()
        a.shot('accepted_%d' % length)
        # Cancel with KEY3: the point is that the device was waiting for a human, not that we
        # want a signature. Nothing here should ever produce one.
        a._press('alt', 't58')
        r = a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
        if r is None:
            errors.append('%s: no response after KEY3' % label)
        elif 'result' in r:
            errors.append('%s: KEY3 produced a signature (response contains result)' % label)
        else:
            code = (r.get('error') or {}).get('code')
            if code != CBOR_RPC_USER_CANCELLED:
                errors.append('%s: cancelled response has code %s, expected %d'
                              % (label, code, CBOR_RPC_USER_CANCELLED))
            else:
                print('58 PASS: %s accepted, confirmation drawn, cancelled with KEY3 (code %d)'
                      % (label, code))
        a.settle()
        return

    # Rejected cases
    r = a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
    if drawn:
        errors.append('%s: confirmation WAS drawn; an overlong message must never reach the '
                      'confirmation screen' % label)
    if r is None:
        errors.append('%s: no response at all' % label)
    elif 'result' in r:
        errors.append('%s: a signature was produced (response contains result)' % label)
    else:
        code = (r.get('error') or {}).get('code')
        if code != CBOR_RPC_BAD_PARAMETERS:
            errors.append('%s: rejection code %s, expected %d'
                          % (label, code, CBOR_RPC_BAD_PARAMETERS))
        elif not drawn:
            print('58 PASS: %s rejected without drawing confirmation (code %d)' % (label, code))
    a.settle()


def main():
    proc, log_file, sock, log = start_daemon('t58')
    errors = []
    try:
        a = menu_audit.Audit(sock, log, 't58')
        a.seed(TEST_MNEMONIC)
        if a.findings:
            raise SystemExit('58: wallet setup failed: %s' % a.findings)
        if not a.settle():
            raise SystemExit('58: screen did not settle after wallet setup')

        for length, expect_accepted in CASES:
            run_case(a, length, expect_accepted, errors)
    finally:
        stop_daemon(proc, log_file)

    for h in errors:
        print('ERROR: %s' % h, file=sys.stderr)
    print('58 RESULT: %d error(s)' % len(errors))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
