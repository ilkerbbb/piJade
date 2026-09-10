"""Item 47, N11-N13 checks: KEY3 does not CONFIRM destructive paths.

In the closing round (2026-09-08), three escape decisions began reading the PRESS
that closed the screen instead of `gui_escape_pending()` (`await_message_escaped()`,
`main/ui/dialogs.c`). That branch was absent from t47 frame measurements: every path
there closed the screen with "Continue", so the new branch never ran. This script
closes the gap using N6's criterion, the RPC RESPONSE rather than the frame:

  N11 KEY3 does NOT SET UP a wallet at debug_set_mnemonic warning (main/process/debug_set_mnemonic.c:110)
  N12 KEY3 does NOT ERASE a wallet at debug_clean_reset warning (main/process/debug_clean.c:28)
  N13 KEY3 does NOT GIVE THE HOST an address at its warning (main/process/get_receive_address.c:271)

Each check has a POSITIVE CONTROL: would the destructive action ACTUALLY happen
if the same path used Continue? Without it, all three measurements could be blind;
CBOR_RPC_USER_CANCELLED might be returned even if the RPC never reached the screen.

The order deliberately hands state from one check to the next:
  N11 (uninitialized device) -> positive control SETS UP the wallet
  N12 (initialized device) -> positive control ERASES it; frame returns to N11's uninitialized anchor
  N13 (set up again) -> positive control gets the address, then the KEY3 branch is measured

Run (inside the container):
  python3 t47_destructive.py <socket> <daemon.log> <prefix>
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

# m/84'/0'/0'/2/0, branch index 2, neither receive (0) nor change (1).
# wallet_is_expected_singlesig_path() returns false on both calls
# (main/process/get_receive_address.c:189-192), drawing "Warning: Unusual path".
# This is the sole warning condition; without the warning there is no branch to
# measure, which the script records as a finding.
UNUSUAL_PATH = [2147483732, 2147483648, 2147483648, 2, 0]
ADDRESS_PARAMS = {'network': 'mainnet', 'variant': 'wpkh(k)', 'path': UNUSUAL_PATH}

DRAW_BUDGET = 8.0
RESPONSE_BUDGET = 8.0


def path(name):
    return os.path.join(jadectl.OUT, name + '.rgb565')


def equal(a, b):
    with open(path(a), 'rb') as f1, open(path(b), 'rb') as f2:
        return f1.read() == f2.read()


class Measurement:
    def __init__(self, a):
        self.a = a
        self.errors = []

    def report(self, msg):
        self.errors.append(msg)

    def passed(self, msg):
        print('47 PASS: %s' % msg)

    def request_and_screen(self, name, method, params):
        """Send the request and verify screen drawing; return the id.

        Pressing before drawing is verified would put every subsequent step on the wrong screen."""
        a = self.a
        if not a.settle():
            raise menu_audit.AuditAbort('%s: screen did not settle before request' % name)
        before = hashlib.sha256(a.j.display_bytes()).digest()
        rid = a.j.send(method, params)

        deadline = time.monotonic() + DRAW_BUDGET
        while time.monotonic() < deadline:
            time.sleep(0.1)
            if hashlib.sha256(a._read(deadline)).digest() != before:
                a.settle()
                return rid
        r = a.j.try_recv_id(rid)
        raise menu_audit.AuditAbort(
            '%s: screen not drawn (request result: %s)' % (name, str(r)[:200]))

    def response(self, name, rid):
        r = self.a.j.try_recv_id(rid, timeout=RESPONSE_BUDGET)
        if r is None:
            self.report('%s: no response; request still pending' % name)
        return r

    def rejected(self, name, rid, assertion):
        """KEY3 criterion: the response must be a REJECTION with code USER_CANCELLED."""
        r = self.response(name, rid)
        if r is None:
            return
        if 'result' in r:
            # Do not print the contents; their presence already means failure.
            self.report('%s: KEY3 CONFIRMED (response contains result)' % name)
            return
        code = (r.get('error') or {}).get('code')
        if code != CBOR_RPC_USER_CANCELLED:
            self.report('%s: response is not a rejection: code %s' % (name, code))
            return
        self.passed('%s (code %d)' % (assertion, code))

    def accepted(self, name, rid, assertion):
        """Positive-control criterion: the same path with Continue ACTUALLY performs the action."""
        r = self.response(name, rid)
        if r is None:
            return
        if 'result' not in r:
            self.report('%s: positive control returned no result: %s' % (name, str(r)[:160]))
            return
        self.passed(assertion)


def main():
    if len(sys.argv) != 4:
        raise SystemExit('usage: t47_destructive.py <socket> <daemon.log> <prefix>')
    sock, logpath, prefix = sys.argv[1:4]

    a = menu_audit.Audit(sock, logpath, prefix)
    o = Measurement(a)

    # ------------------------------------------------ N11: debug wallet is not set up
    if not a.settle():
        raise SystemExit('N11: could not capture uninitialized dashboard anchor')
    uninitialized = a.shot('uninitialized')

    rid = o.request_and_screen('N11', 'debug_set_mnemonic', {'mnemonic': TEST_MNEMONIC})
    a.shot('n11_warning')
    a._press('alt', 'N11')
    o.rejected('N11', rid, 'N11 KEY3 did not set up wallet at debug wallet warning')

    a.settle()
    n11_after = a.shot('n11_after')
    if equal(uninitialized, n11_after):
        o.passed('N11 device stayed uninitialized (dashboard matches anchor exactly)')
    else:
        o.report('N11: dashboard changed after KEY3; wallet may have been set up')

    # Positive control: the same request with Continue ACTUALLY sets up the wallet.
    a.seed(TEST_MNEMONIC)
    if a.findings:
        raise SystemExit('N11 positive control: wallet setup failed: %s' % a.findings)
    a.settle()
    initialized = a.shot('initialized')
    if equal(uninitialized, initialized):
        o.report('N11 positive control: wallet setup did not change dashboard; criterion has no baseline')
    else:
        o.passed('N11 positive control: Continue actually set up wallet (dashboard changed)')

    # ------------------------------------------------- N12: debug wipe does not erase
    rid = o.request_and_screen('N12', 'debug_clean_reset', None)
    a.shot('n12_warning')
    a._press('alt', 'N12')
    o.rejected('N12', rid, 'N12 KEY3 did not erase wallet at debug wipe warning')

    a.settle()
    n12_after = a.shot('n12_after')
    if equal(initialized, n12_after):
        o.passed('N12 wallet remained (dashboard matches initialized anchor exactly)')
    else:
        o.report('N12: dashboard changed after KEY3; wipe may have run')

    # Positive control: the same request with Continue ACTUALLY erases the wallet.
    rid = o.request_and_screen('N12+', 'debug_clean_reset', None)
    a._press('click', 'N12+')
    o.accepted('N12+', rid, 'N12 positive control: Continue actually ran the wipe')
    a.settle()
    erased = a.shot('n12_erased')
    if equal(uninitialized, erased):
        o.passed('N12 positive control: dashboard returned to uninitialized anchor after wipe')
    else:
        o.report('N12 positive control: dashboard did not return to uninitialized anchor after wipe')

    # ------------------------------------------------- N13: address is not given to the host
    a.seed(TEST_MNEMONIC)
    if a.findings:
        raise SystemExit('N13: wallet setup failed on retry: %s' % a.findings)
    a.settle()

    # Positive control FIRST: prove the path through the address screen is correct.
    # The subsequent KEY3 measurement is meaningful only if this passes.
    # The tuple carries the check and assertion. The old version distinguished the branch
    # with `measurement is o.accepted`; bound methods are recreated on each access, so
    # that comparison ALWAYS returned False and reported the positive control with the
    # wrong sentence (measured).
    rounds = (
        ('N13+', 'click', o.accepted, 'N13 positive control: Continue actually returned the address'),
        ('N13', 'alt', o.rejected, 'N13 KEY3 did not give the host an address at the address warning'),
    )
    for label, button, measurement, assertion in rounds:
        rid = o.request_and_screen(label, 'get_receive_address', ADDRESS_PARAMS)
        # The address screen is one page (bech32 42 < MAX_DISPLAY_ADDRESS_LEN 96,
        # main/ui/confirm_address.c:49), initially selecting REJECT; confirmation 'S' is one step right.
        a._press('right', label)
        a._press('click', label)   # accept -> warning screen
        a.settle()
        a.shot('%s_warning' % label.replace('+', 'p').lower())
        a._press(button, label)
        measurement(label, rid, assertion)

    for h in o.errors:
        print('ERROR: %s' % h, file=sys.stderr)
    for f in a.findings:
        print('ERROR: %s' % (f,), file=sys.stderr)
    return 1 if (o.errors or a.findings) else 0


if __name__ == '__main__':
    sys.exit(main())
