"""T3.15 digit entry driver (inside a container, extending menu_audit).

Usage: python3 /probe/t315_drive.py <sock> <daemon.log> <prefix> <steps...>
All menu_audit steps remain valid (seed:, btn:, still:, shot:, wait:). Additional steps:
  calib:<name>     rotate the selected digit box right 11 times and crop each frame;
                    write crop hash order to /probe/t315_templates.json (awaits anchor)
  anchor:<i>:<ch>  character in calibration frame i (read visually) -> complete templates
  read              print the selected box's character
  enter:<digits>   enter digits mechanically in order (read, rotate by shortest path, click)
  wheel:<ch>       rotate selected box to the given character, DO NOT CLICK
Digit boxes: main/ui/digit_entry.c make_digit_entry_activity, 6 x 35 px, left margin (W-210)/2.
Wheel order: ENTRY_CHARS = 0..9 '|' (11 values for PIN; right = +1, left = -1, mod 11).
"""
import hashlib, json, os, sys
sys.path.insert(0, '/jade/pijade/tools')
import jadectl, menu_audit

WHEEL = '0123456789|'
TPL = '/probe/t315_templates.json'
BOX_W = 35


class Drive(menu_audit.Audit):
    def __init__(self, *a):
        super().__init__(*a)
        self.tpl = json.load(open(TPL)) if os.path.exists(TPL) else {'rows': None, 'map': {}, 'order': []}
        self.pos = 0  # selected digit (0-5); updated after enter/abandon

    # --- crop ---
    def _frame(self):
        return self.j.display_bytes()

    def _crop(self, data, pos, rows):
        w = self.j.w
        x0 = (w - 6 * BOX_W) // 2 + pos * BOX_W
        out = bytearray()
        for y in range(rows[0], rows[1] + 1):
            off = (y * w + x0) * 2
            out += data[off:off + BOX_W * 2]
        return bytes(out)

    def _diff_rows(self, a, b, pos):
        w = self.j.w
        x0 = (w - 6 * BOX_W) // 2 + pos * BOX_W
        rows = []
        for y in range(self.j.h):
            off = (y * w + x0) * 2
            if a[off:off + BOX_W * 2] != b[off:off + BOX_W * 2]:
                rows.append(y)
        return rows

    def calib(self, name):
        frames = []
        for i in range(len(WHEEL)):
            self.j.settle(__import__('time').monotonic() + jadectl.SETTLE_DEADLINE)
            data = self._frame()
            frames.append(data)
            with open(f'/probe/{self.prefix}_{name}{i:02d}.rgb565', 'wb') as f:
                f.write(data)
            self.shots.append(f'{self.prefix}_{name}{i:02d}')
            self._press('right', 'calib')
            self.check('calib')
        # After 11 presses the value should return to the start.
        if self._frame() != frames[0]:
            self.findings.append(('calib', 'screen did not return to first frame after 11 right presses'))
        allrows = set()
        for k in range(1, len(frames)):
            allrows.update(self._diff_rows(frames[0], frames[k], self.pos))
        if not allrows:
            raise menu_audit.AuditAbort('calib: no pixels changed inside box')
        rows = [min(allrows) - 2, max(allrows) + 2]
        hashes = [hashlib.sha256(self._crop(f, self.pos, rows)).hexdigest()[:16] for f in frames]
        if len(set(hashes)) != len(hashes):
            raise menu_audit.AuditAbort('calib: the 11 crop hashes are not unique')
        self.tpl = {'rows': rows, 'map': {}, 'order': hashes}
        json.dump(self.tpl, open(TPL, 'w'))
        print(f'calib: rows {rows}, 11 unique crops; awaiting anchor ({name}00..{name}10)')

    def anchor(self, i, ch):
        order = self.tpl['order']
        if not order:
            raise menu_audit.AuditAbort('anchor: run calib first')
        base = WHEEL.index(ch)
        self.tpl['map'] = {order[k]: WHEEL[(base + (k - int(i))) % len(WHEEL)] for k in range(len(order))}
        json.dump(self.tpl, open(TPL, 'w'))
        print('anchor: template map', ''.join(self.tpl['map'][h] for h in order))

    def read(self):
        if not self.tpl.get('map'):
            raise menu_audit.AuditAbort('read: no templates (calib + anchor)')
        self.j.settle(__import__('time').monotonic() + jadectl.SETTLE_DEADLINE)
        h = hashlib.sha256(self._crop(self._frame(), self.pos, self.tpl['rows'])).hexdigest()[:16]
        ch = self.tpl['map'].get(h)
        if ch is None:
            raise menu_audit.AuditAbort(f'read: digit {self.pos} crop missing from templates ({h})')
        return ch

    def wheel(self, target):
        cur = self.read()
        n = len(WHEEL)
        right = (WHEEL.index(target) - WHEEL.index(cur)) % n
        left = (WHEEL.index(cur) - WHEEL.index(target)) % n
        which, steps = ('right', right) if right <= left else ('left', left)
        for _ in range(steps):
            self._press(which, 'wheel')
        got = self.read()
        if got != target:
            raise menu_audit.AuditAbort(f'wheel: target {target!r}, read {got!r}')
        print(f'wheel: digit {self.pos}: {cur!r} -> {target!r} ({which} x{steps})')

    def enter(self, digits):
        for d in digits:
            self.wheel(d)
            self._press('click', 'enter')
            self.check('enter')
            self.pos = 0 if self.pos == 5 else self.pos + 1
        print(f'enter: {len(digits)} digits entered, pos={self.pos}')

    def step(self, cmd):
        kind, _, arg = cmd.partition(':')
        if kind == 'calib':
            self.calib(arg)
        elif kind == 'anchor':
            i, _, ch = arg.partition(':')
            self.anchor(i, ch)
        elif kind == 'read':
            print(f'read: digit {self.pos} = {self.read()!r}')
        elif kind == 'enter':
            self.enter(arg)
        elif kind == 'wheel':
            self.wheel(arg)
        elif kind == 'pos':
            self.pos = int(arg)
        else:
            super().step(cmd)
            return
        self.check(cmd)


def main():
    sock, logpath, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    a = Drive(sock, logpath, prefix)
    aborted = False
    try:
        for cmd in sys.argv[4:]:
            a.step(cmd)
    except menu_audit.AuditAbort as exc:
        a.findings.append(('abort', str(exc))); aborted = True
    except (jadectl.RpcError, OSError) as exc:
        a.findings.append(('abort', 'step could not complete: %s' % exc)); aborted = True
    print(f'{len(a.shots)} screenshots: ' + ' '.join(a.shots))
    if a.findings:
        print(f'{len(a.findings)} FINDINGS:')
        for w, l in a.findings:
            print(f'  [{w}] {l}')
    else:
        print('no errors in log')
    sys.exit(1 if (aborted or a.findings) else 0)


if __name__ == '__main__':
    main()
