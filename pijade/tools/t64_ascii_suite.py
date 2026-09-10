"""Measure the fork's printable-ASCII rule on the running firmware, on both entry points.

Discriminating, and each half fails on its own:

  * The five refused cases must come back as an error with the screen never redrawn.  A build
    that merely warned, or that rejected only after drawing the summary, fails here.
  * The three accepted cases must reach the confirmation screen, which this suite then declines,
    so a build that refused everything fails on them.  Their verdict is told apart from a refusal
    by which error comes back, not by a guess: "User declined" only exists past the screen.

Two of the cases are there for reasons measured on 2026-09-10 rather than imagined.  A message
carrying a NUL byte aborted the process at the snprintf length assertion in confirm_sign_message()
on both paths, so 'nul' guards a crash and not only a display defect.  'edges' holds exactly the
two bytes at the ends of the allowed range (0x20 and 0x7e), which an off-by-one in either
comparison would reject.  The complementary off-by-one, a ceiling one byte too high, is what 'del'
catches; removing the check from one path at a time was measured to fail only that path's rows.

The JSON fixtures (test_data/msg_bbb_*.json, msgfile_bbb_*.json) check the same rule through
pijade/tools/t58_suite.py.  They cannot see the screen; this suite exists for that half.

Run it inside the jade-dev container, from the repo root mounted at /jade:

    docker exec jade-dev sh -lc 'cd /jade && python3 pijade/tools/t64_ascii_suite.py'

It starts and stops its own daemon, so it needs the build_linux_nci_log build (real presses, LOG
enabled) to be current: `cmake --build /jade/build_linux_nci_log`.  Public test vectors only.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, '/jade/pijade/tools')
from jadectl import Jade

DAEMON = '/jade/build_linux_nci_log/libjade/libjade_daemon'
# The public test vector every upstream fixture uses (pijade/tools/rpcprobe.py).
MNEMONIC = ' '.join(['abandon'] * 11 + ['about'])
PATH = [44 + 2**31, 2**31, 2**31, 0, 0]
REFUSAL = 'Message contains a character the screen cannot show'

CASES = [
    ('lf', 'OK' + '\n' * 32 + 'NO', 'refuse'),
    ('tab', 'OK' + '\t' * 40 + 'NO', 'refuse'),
    ('nonascii', 'ode 5 TL: sifir mi, şifre mi?', 'refuse'),
    ('del', 'OK\x7fNO', 'refuse'),
    ('nul', 'OK\x00NO', 'refuse'),
    ('plain', 'hello', 'accept'),
    ('plain148', 'START' + 'x' * 140 + 'END', 'accept'),
    ('edges', ' ~', 'accept'),
]


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
        for name, msg, want in CASES:
            for kind in ('rpc', 'file'):
                settle()
                before = screen()
                params = ({'path': PATH, 'message': msg} if kind == 'rpc'
                          else {'message_file': 'signmessage m/44h/0h/0h/0/0 ascii:' + msg})
                rid = j.send('sign_message', params)
                # A refusal answers without drawing anything; an acceptance draws the summary.
                moved = False
                for _ in range(20):
                    time.sleep(0.25)
                    if screen() != before:
                        moved = True
                        break
                if moved:
                    j.btn('click')      # the summary opens on its Reject header button
                err = (j.recv_id(rid).get('error') or {}).get('message', '')
                ok = ((not moved and REFUSAL in err) if want == 'refuse'
                      else (moved and 'declined' in err.lower()))
                fails += 0 if ok else 1
                print('%-4s %-9s %-4s screen_redrawn=%-5s -> %s%s'
                      % ('PASS' if ok else 'FAIL', name, kind, moved, err[:58],
                         '' if ok else '   <== expected to ' + want))
        return fails
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log.close()


with tempfile.TemporaryDirectory() as work:
    failures = main(work)
print('T64 ASCII SUITE %s (%d failure(s))' % ('OK' if failures == 0 else 'BROKEN', failures))
sys.exit(1 if failures else 0)
