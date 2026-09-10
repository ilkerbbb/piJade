"""Item 47, N6 check: KEY3 does NOT produce a signature on signing confirmation.

Ilker's second constraint: the escape key on PIN entry or signing confirmation must
not silently cancel or accidentally CONFIRM. A frame alone cannot show whether a
signature was produced when leaving confirmation; the criterion is the RPC RESPONSE:

  1. Set up a wallet with a public test vector (no real seed enters this machine).
  2. Capture the dashboard anchor.
  3. Send sign_message ASYNCHRONOUSLY without waiting for its response: the device
     draws confirmation and waits (main/process/sign_message.c:252 confirm_sign_message).
  4. Verify confirmation drawing through a frame change.
  5. Press KEY3.
  6. Collect the response by id. PASS: an ERROR with code CBOR_RPC_USER_CANCELLED
     (-32000, main/utils/cbor_rpc.h:20). A response containing 'result', a signature, FAILS.
  7. Compare the frame with the dashboard anchor: escape must return to the dashboard.

Run (inside the container):
  python3 t47_signature.py <socket> <daemon.log> <prefix>
Exit 0 = pass, 1 = fail."""
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jadectl
import menu_audit

CBOR_RPC_USER_CANCELLED = -32000

# Public bip39 test vector; not a real wallet.
TEST_MNEMONIC = ('abandon abandon abandon abandon abandon abandon '
                 'abandon abandon abandon abandon abandon about')

# m/44'/0'/0'/0/0, an ordinary signing path, NOT a GDK login challenge. The
# auto_sign branch (sign_message.c:247) is not taken and confirmation is actually drawn.
SIGN_PATH = [2147483692, 2147483648, 2147483648, 0, 0]
SIGN_MESSAGE = 'piJade escape test'

CONFIRMATION_BUDGET = 8.0
RESPONSE_BUDGET = 8.0


def main():
    if len(sys.argv) != 4:
        raise SystemExit('usage: t47_signature.py <socket> <daemon.log> <prefix>')
    sock, logpath, prefix = sys.argv[1:4]

    a = menu_audit.Audit(sock, logpath, prefix)
    errors = []

    a.seed(TEST_MNEMONIC)
    if a.findings:
        raise SystemExit('N6: wallet setup failed: %s' % a.findings)

    if not a.settle():
        raise SystemExit('N6: screen did not settle before dashboard anchor capture')
    dashboard = a.shot('dashboard')

    before = hashlib.sha256(a.j.display_bytes()).digest()
    rid = a.j.send('sign_message', {'path': SIGN_PATH, 'message': SIGN_MESSAGE})

    deadline = time.monotonic() + CONFIRMATION_BUDGET
    drawn = False
    while time.monotonic() < deadline:
        time.sleep(0.1)
        if hashlib.sha256(a._read(deadline)).digest() != before:
            drawn = True
            break
    if not drawn:
        r = a.j.try_recv_id(rid)
        raise SystemExit('N6: confirmation screen not drawn (request result: %s)' % str(r)[:200])
    a.settle()
    a.shot('confirmation')

    # KEY3. Also verify screen change: returning to the dashboard must change the frame.
    a._press('alt', 'N6')

    r = a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
    if r is None:
        errors.append('N6: no sign_message response after KEY3; request still pending')
    elif 'result' in r:
        # A signature was produced. Do not print its contents; its presence already means failure.
        errors.append('N6: KEY3 PRODUCED a signature (response contains result)')
    else:
        code = (r.get('error') or {}).get('code')
        if code != CBOR_RPC_USER_CANCELLED:
            errors.append('N6: response is not a rejection: code %s' % code)
        else:
            print('47 PASS: N6 KEY3 produced no signature at signing confirmation (code %d)' % code)

    a.settle()
    after = a.shot('after')
    path = lambda name: os.path.join(jadectl.OUT, name + '.rgb565')
    with open(path(dashboard), 'rb') as f1, open(path(after), 'rb') as f2:
        if f1.read() == f2.read():
            print('47 PASS: N6 returned to dashboard from signing confirmation')
        else:
            errors.append('N6: did not return to dashboard after KEY3')

    for h in errors:
        print('ERROR: %s' % h, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
