"""Regression tests for menu_audit button-step verification.

Covered bug (D4, 2026-08-27): `_btn` sent a raw `btn()` and only called `settle()`.
Settling verifies stability, not change; a dropped press leaves a stable UNCHANGED
screen, `settle()` returns True, and the audit silently continues on the wrong screen.

The fake device extends jadectl.Jade and replaces only transport, so press()/settle()
run their REAL logic and the test does not merely verify a copy."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jadectl
import menu_audit


class FakeJade(jadectl.Jade):
    """Keep the display in memory instead of a socket. changes=False accepts presses but freezes the screen."""

    def __init__(self, changes=True, accepted=True):
        self.frame = b'\x00' * 16
        self.changes = changes
        self.accepted = accepted
        self.presses = 0

    def display_bytes(self, timeout=None):
        return self.frame

    def btn(self, which, timeout=None):
        if not self.accepted:
            raise jadectl.RpcError('button %r rejected; response: %s' % (which, "{'result': False}"))
        self.presses += 1
        if self.changes:
            self.frame = bytes([self.presses % 251]) * 16
        return {'result': True}

    def rpc(self, method, params=None, timeout=None):
        return {'result': {'w': 240, 'h': 240}}

    def shot(self, name):
        return name


def auditor(j, tmp):
    a = menu_audit.Audit.__new__(menu_audit.Audit)
    a.j = j
    a.logpath = tmp
    a.prefix = 'T'
    a.log_seen = 0
    a.findings = []
    a.shots = []
    return a


class ButtonStep(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.menu_audit_test.log')
        open(self.tmp, 'w').close()

    def tearDown(self):
        os.path.exists(self.tmp) and os.remove(self.tmp)

    def test_step_passes_when_screen_changes(self):
        a = auditor(FakeJade(changes=True), self.tmp)
        a.step('btn:left')
        self.assertEqual(a.findings, [])

    def test_audit_aborts_when_screen_does_not_change(self):
        """The old version SILENTLY passed this case: the press was accepted but the screen did not change."""
        a = auditor(FakeJade(changes=False), self.tmp)
        with self.assertRaises(menu_audit.AuditAbort):
            a.step('btn:left')
        self.assertTrue(a.findings, 'unchanged screen should have produced a finding')
        # Assert against the actual jadectl message substring.
        self.assertIn('did not change', a.findings[0][1])

    def test_rejected_press_produces_finding(self):
        a = auditor(FakeJade(accepted=False), self.tmp)
        with self.assertRaises(menu_audit.AuditAbort):
            a.step('btn:click')
        self.assertIn('rejected', a.findings[0][1])

    def test_still_passes_on_unchanged_screen(self):
        """Measured legitimate case: Flip Orientation leaves the same frame in the emulator."""
        a = auditor(FakeJade(changes=False), self.tmp)
        a.step('still:click')
        self.assertEqual(a.findings, [])

    def test_still_produces_finding_when_screen_changes(self):
        """Inverse assertion: a change is also an error when stillness is expected."""
        a = auditor(FakeJade(changes=True), self.tmp)
        with self.assertRaises(menu_audit.AuditAbort):
            a.step('still:click')
        self.assertIn('expected no change', a.findings[0][1])

    def test_scan_loop_also_verifies(self):
        """The scan: branch also delegates to press(), catching dropped presses during carousel scans."""
        a = auditor(FakeJade(changes=False), self.tmp)
        with self.assertRaises(menu_audit.AuditAbort):
            a.step('scan:x:3')
        self.assertEqual(a.findings[0][0], 'scan')


if __name__ == '__main__':
    unittest.main(verbosity=2)
