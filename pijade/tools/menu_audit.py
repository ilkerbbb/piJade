"""Menu auditor: navigate emulator screens, save each one, and look for errors.

Measure two dimensions:
  1. layout - save screenshots to disk; visually inspect clipping and overflow
  2. functionality - after each step, check daemon health and new log errors

Usage (inside the container):
  python3 menu_audit.py <socket> <daemon.log> <output-prefix> <step...>
Steps use the same language as jadectl: btn:left | btn:right | btn:up | btn:down | btn:first |
btn:alt | btn:click | wait:<seconds> | shot:<name>
btn: verify the screen ACTUALLY changes after every press; abort if it does not.
  still:<button> - a press expected to leave the screen UNCHANGED (e.g. emulator `Flip Orientation`).
                  An inverse assertion: a screen change also produces a finding.
                  Cannot be used on a SELF-ANIMATING screen: multipart BC-UR QR codes
                  (e.g. fullscreen xpub codes) cycle between frames without presses,
                  so `still` always produces a finding. The criterion there is a SET
                  of frames, not frame equality; compare the hash sets observed
                  before and after the presses.
Additionally:
  scan:<name>:<n> - press 'next' n times on the current screen, taking a screenshot each time;
                   never click, so a destructive menu item cannot run accidentally.
  seed:<mnemonic> - set up a wallet with debug_set_mnemonic; click its confirmation screen.
  seedpp:<mnemonic>|<passphrase> - the same with a bip39 passphrase. Wallet menus differ
                   with and without a passphrase (no SeedQR export with one), so this
                   is needed to measure that distinction.

Idle dimming trap (measured 2026-08-28, threshold became configurable on 2026-09-03):
the idle timer dims the screen after UI inactivity (decision at `idletimer.c:289-291`,
`WARN:idletimer.c:291`). The threshold now comes from storage, NOT a build constant
(`storage_get_screen_timeout()`; screen `Preferences > Screen Timeout`); default
`DEFAULT_SCREEN_TIMEOUT_SECS` = 60 seconds (`main/idletimer.c:20`). The log's
`timeout period: 600000` is a DIFFERENT threshold (`DEFAULT_IDLE_TIMEOUT_SECS`, full
idle lock), not dimming. The old "between 90-150 seconds" window no longer applies:
the wake calculation includes the projected dimming time (`idletimer.c:302-309`),
so dimming occurs just after the configured value (measured 2026-09-03: 32 seconds
with a 30 second setting). Changes DURING sleep also wake the timer (`idletimer_recheck()`),
so shortening the setting or waking a dimmed screen does not wait for the current sleep
(measured 2026-09-03: 31 seconds when changing from 10 minutes to 30 seconds, and 33 seconds
to the second dimming after wake; both paths took up to 60 seconds before the fix).
`Disabled` prevents dimming (measured: idle for 80 seconds, no dimming log line).
The FIRST press after dimming only restores the screen; its event is never sent
(`gui_front_click()` first calls `idletimer_register_activity(true)`, `main/gui.c:2568`,
which returns `true` on a dimmed screen, `main/idletimer.c:154`). Emulator dimming does
not touch the framebuffer, so the change check correctly reports "screen did not change"
and stops; the press was swallowed, not dropped. After long pauses, wake a screen without
selectable buttons with `still:first`: `gui_select_first()` returns early on
`!current_activity->selectables` (`main/gui.c:2701`), keeping the frame identical in both cases."""
import hashlib
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jadectl

OUT = jadectl.OUT

# Polling budget for confirmation-screen drawing. Measure elapsed time, not ITERATIONS:
# every poll makes an RPC that can block until the socket timeout (20 s), so an
# iteration count does not bound real elapsed time.
SEED_POLL_INTERVAL = 0.1
SEED_DEADLINE = 6.0

# Grace period before the first read. Measured (2026-08-26): if drawing is QUEUED but not
# started, two consecutive reads return the same OLD frame and falsely confirm settling.
# The auditor needs this because seed() establishes an ASYNCHRONOUS baseline without a
# button; a wrong baseline mistakes normal drawing for confirmation and clicks the wrong screen.
SETTLE_MIN_WAIT = 0.15

# Minimum timeout for one polling read. Use the floor when the remaining budget is
# shorter; the budget can overrun by at most this amount, but a healthy response is
# not mistaken for a timeout because too little time remains.
POLL_TIMEOUT_FLOOR = 0.2

# Timeout for the cheap daemon health probe after each step. get_display_size returns
# one small object rather than a full frame like get_display_bytes, so its per-step
# cost is negligible.
HEALTH_TIMEOUT = 2.0


class AuditAbort(Exception):
    """Raised when further audit steps would be meaningless."""


class Audit:
    def __init__(self, sock, logpath, prefix):
        self.j = jadectl.Jade(sock)
        self.logpath = logpath
        self.prefix = prefix
        self.log_seen = self._logsize()
        self.findings = []
        self.shots = []

    def _logsize(self):
        try:
            return os.path.getsize(self.logpath)
        except OSError:
            return 0

    def check(self, where):
        """After a step: check for new log errors and a live connection.

        The probe is required: `wait:` makes no RPC, so an audit ending in `wait:` could
        print "no errors in log" without noticing a dead daemon. Crashes do not always
        write a recognized keyword; the connection itself is the only reliable criterion."""
        size = self._logsize()
        if size > self.log_seen:
            with open(self.logpath, 'r', errors='replace') as f:
                f.seek(self.log_seen)
                fresh = f.read()
            self.log_seen = size
            for line in fresh.splitlines():
                if any(k in line for k in ('ERROR', 'ERR:', 'assert', 'Assert', 'abort', 'FATAL')):
                    self.findings.append((where, line.strip()))
        try:
            self.j.rpc('libjade_request', {'request': 'get_display_size'}, timeout=HEALTH_TIMEOUT)
        except socket.timeout:
            raise AuditAbort('daemon did not respond within %g s (%s); audit aborted'
                             % (HEALTH_TIMEOUT, where))
        except (jadectl.RpcError, OSError) as exc:
            raise AuditAbort('daemon disconnected (%s): %s' % (where, str(exc)))

    def _read(self, deadline):
        """Read the screen once within the remaining budget; abort if the daemon stops responding."""
        remaining = max(deadline - time.monotonic(), POLL_TIMEOUT_FLOOR)
        try:
            return self.j.display_bytes(timeout=remaining)
        except socket.timeout:
            raise AuditAbort('screen read did not respond within %g s; audit aborted' % remaining)
        except (jadectl.RpcError, OSError) as exc:
            raise AuditAbort('screen read could not complete: %s' % str(exc))

    def settle(self):
        """Wait for screen drawing to settle; return True if it does.

        Delegate to jadectl.settle(): separate implementations made different decisions
        for the same screen when their budgets and polling intervals diverged.
        Only the audit contract remains here: record a finding and return False instead
        of raising, because the caller (seed) handles failure to settle itself."""
        try:
            self.j.settle(time.monotonic() + jadectl.SETTLE_DEADLINE, SETTLE_MIN_WAIT)
            return True
        except jadectl.RpcError as exc:
            self.findings.append(('settle', str(exc)))
            return False

    def _press_still(self, which, source):
        """A press expected to leave the screen UNCHANGED; an inverse assertion.

        Such presses exist and were measured: `Settings > Display > Flip Orientation`
        repaints the same menu with `gui_repaint()` (`main/process/dashboard.c:1952`), but
        without emulator panel rotation the frame stays byte-identical. Checking this
        with `btn:` would wait 2.5 s and treat a legitimate press as an error.

        State the expectation EXPLICITLY in the steps (`still:click`); automatic relaxation
        would make dropped presses indistinguishable from legitimate stillness. Each
        `still:` step must have a visually justifiable reason."""
        try:
            self.j.press(which, expect_change=False)
        except jadectl.RpcError as exc:
            self.findings.append((source, str(exc)))
            raise AuditAbort('%s: still press could not be verified' % source)
        except OSError as exc:
            self.findings.append((source, repr(exc)))
            raise AuditAbort('%s: still press could not complete' % source)

    def _press(self, which, source):
        """Press a button and verify the screen CHANGED; otherwise abort the audit.

        Response validation alone is insufficient: the daemon can accept an event
        (result:true) without changing the screen. The old version only called `settle()`
        in that case; settling verifies stability, not change. On a stable UNCHANGED
        screen it returns True and the audit silently continues on the wrong screen.
        The silent loss shifts every subsequent step.

        Delegate verification to jadectl.press(), the single source of screen-settling
        logic, eliminating the auditor's duplicate settling implementation."""
        try:
            self.j.press(which)
        except jadectl.RpcError as exc:
            self.findings.append((source, str(exc)))
            raise AuditAbort('%s: button step could not be verified' % source)
        except OSError as exc:
            self.findings.append((source, repr(exc)))
            raise AuditAbort('%s: button step could not complete' % source)

    def shot(self, name):
        full = f'{self.prefix}_{name}'
        self.j.shot(full)
        self.shots.append(full)
        return full

    def step(self, cmd):
        kind, _, arg = cmd.partition(':')
        if kind == 'btn':
            self._press(arg, 'btn')
        elif kind == 'still':
            self._press_still(arg, 'still')
        elif kind == 'wait':
            time.sleep(float(arg))
        elif kind == 'shot':
            self.shot(arg)
        elif kind == 'scan':
            name, _, count = arg.partition(':')
            n = int(count or 6)
            for i in range(n):
                self.shot(f'{name}{i:02d}')
                self._press('right', 'scan')
        elif kind == 'seed':
            self.seed(arg)
        elif kind == 'seedpp':
            mnemonic, sep, passphrase = arg.partition('|')
            if not sep or not passphrase:
                raise SystemExit(f'seedpp: expected "<mnemonic>|<passphrase>": {cmd}')
            self.seed(mnemonic, passphrase)
        else:
            raise SystemExit(f'unknown step: {cmd}')
        self.check(cmd)

    def seed(self, mnemonic, passphrase=None):
        """debug_set_mnemonic needs screen confirmation (debug_set_mnemonic.c:105 await_message).
        Send the request without waiting for its response, wait for and click the
        confirmation screen, then collect the response by id. Public test vectors only.

        No threads are needed: reading one socket from two threads allowed responses
        to become mixed up. Since jadectl now matches responses by id, one thread suffices."""
        # Start measurement from a stable baseline: drawing during startup could look like a
        # screen change even if confirmation never appeared (measured 2026-08-26). Send NO request
        # if settling fails; otherwise the first pixel change could be mistaken for confirmation
        # and the click would land on the wrong screen.
        if not self.settle():
            raise AuditAbort('seed: request not sent because screen did not settle')
        before = hashlib.sha256(self.j.display_bytes()).digest()
        try:
            params = {'mnemonic': mnemonic}
            if passphrase:
                params['passphrase'] = passphrase
            rid = self.j.send('debug_set_mnemonic', params)
        except (jadectl.RpcError, OSError) as exc:  # a connection error is also a finding
            self.findings.append(('seed', repr(exc)))
            raise AuditAbort('seed: request could not be sent; audit aborted')

        deadline = time.monotonic() + SEED_DEADLINE
        drawn = False
        while time.monotonic() < deadline:
            time.sleep(SEED_POLL_INTERVAL)
            try:
                if hashlib.sha256(self._read(deadline)).digest() != before:
                    drawn = True
                    break
            except AuditAbort:
                raise

        if not drawn:
            # Two cases: (a) the request completed without showing confirmation (e.g. an invalid
            # mnemonic is rejected immediately), or (b) it is still pending. In (a), collect and
            # report the actual response; in (b), the daemon is waiting on confirmation, so abort
            # before subsequent steps act on that screen.
            r = self.j.try_recv_id(rid)
            if r is not None:
                self.findings.append(('seed', 'confirmation screen was not drawn; request completed: %s'
                                      % str(r)[:160]))
                return
            raise AuditAbort('seed: request still pending after %g s (id=%s); audit aborted'
                             % (SEED_DEADLINE, rid))

        try:
            self._press('click', 'seed')
            r = self.j.recv_id(rid)
        except AuditAbort:
            raise
        except (jadectl.RpcError, OSError) as exc:
            self.findings.append(('seed', repr(exc)))
            raise AuditAbort('seed: response could not be received; audit aborted')
        if not isinstance(r, dict) or 'error' in r or not r.get('result'):
            self.findings.append(('seed', 'unexpected response: %s' % str(r)[:160]))


def main():
    sock, logpath, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    try:
        a = Audit(sock, logpath, prefix)
    except (jadectl.RpcError, OSError) as exc:
        print('0 screenshots: ')
        print('1 FINDINGS:')
        print('  [abort] audit could not start: %s' % str(exc))
        sys.exit(1)
    aborted = False
    try:
        for cmd in sys.argv[4:]:
            a.step(cmd)
    except AuditAbort as exc:
        a.findings.append(('abort', str(exc)))
        aborted = True
    except (jadectl.RpcError, OSError) as exc:
        a.findings.append(('abort', 'step could not complete: %s' % str(exc)))
        aborted = True
    print(f'{len(a.shots)} screenshots: ' + ' '.join(a.shots))
    if a.findings:
        print(f'{len(a.findings)} FINDINGS:')
        for where, line in a.findings:
            print(f'  [{where}] {line}')
    elif a._logsize() == 0:
        # An empty log means there was nothing to inspect, not that no errors occurred:
        # libjade builds with LOG=OFF by default (libjade/CMakeLists.txt), compiling out ESP_LOG*
        # calls so the daemon never writes logs. Report this silent audit explicitly; the
        # connection probe (rpc in check()) still applies.
        print('no errors in log (WARNING: log is empty, likely a LOG=OFF build; '
              'evidence comes only from the connection probe and frames)')
    else:
        print('no errors in log')
    if aborted or a.findings:
        # Some steps never ran, or an error was logged. Exit code 0 would make a shell using
        # `set -e`, or CI, treat an audit with findings as successful.
        sys.exit(1)


if __name__ == '__main__':
    main()
