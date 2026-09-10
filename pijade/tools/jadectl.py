"""libjade daemon driver: press buttons, capture screens, send RPCs.
Usage (inside the container): python3 jadectl.py <socket> <command...>
Commands: btn:left | btn:right | btn:up | btn:down | btn:first | btn:alt | btn:click |
          shot:<name> | wait:<seconds> | rpc:<method> | camfile:<path>[:<count>] | camfiles:<prefix>[:<rounds>]"""
import cbor2, contextlib, hashlib, io, socket, sys, time

import glob
import os
# Wait for the screen to ACTUALLY change after every button press. A fixed sleep is
# unreliable: measured (2026-08-27), a 0.03 s sleep loses one in 20 clicks, SILENTLY.
# When the wheel position drifts, the next click lands on '<' and undoes a roll, so
# only 35 of 52 inputs counted. The same bug affects menu navigation: a lost click
# shifts the step count and measurement silently occurs on the WRONG screen (measured
# 2026-08-27: missing the Advanced Setup warning sent dice input to the keyboard).
# PRESS_DELAY remains the minimum; the wait is added on top.
PRESS_DELAY = float(os.environ.get('PRESS_DELAY', '0.03'))
SETTLE_DEADLINE = 2.5
SETTLE_POLL = 0.02
# Minimum timeout for a single polling read. Use this floor when the remaining budget
# is shorter; the budget can overrun by at most this amount, but a healthy response is
# not mistaken for a timeout because too little time remains. Same rationale as menu_audit.POLL_TIMEOUT_FLOOR.
READ_TIMEOUT_FLOOR = 0.2
# Characters on the dice screen wheel, in screen order.
DICE_CHARS = '123456<'
CAM_DELAY = float(os.environ.get('CAM_DELAY','0.06'))
# Camera frames are QVGA grayscale; their size is a C-side CONTRACT, not an assumption here:
# esp_camera_init() asserts FRAMESIZE_QVGA (libjade/esp_camera.c:52), and
# libjade_push_camera_frame() rejects frames of a different length (esp_camera.c:35).
# A wrong size therefore returns RpcError instead of being silently swallowed. The daemon
# has no RPC to query this size; if the constant changes, this code must change too.
CAM_FRAME_W, CAM_FRAME_H = 320, 240
CAM_FRAME_SIZE = CAM_FRAME_W * CAM_FRAME_H
OUT = '/probe'

class RpcError(Exception):
    pass


class Jade:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(20)
        self.s.connect(path)
        self.n = 0
        self.buf = b''   # bytes left in the stream; incomplete objects wait here
        self.inbox = {}  # id -> response whose turn has not come yet
        self.wheel = 0  # dice wheel position; preserved across dice() calls
        # Display size is now a build option (--display WxH); query it instead of assuming a constant.
        size = self.rpc('libjade_request', {'request': 'get_display_size'}).get('result', {})
        self.w, self.h = size.get('width'), size.get('height')

    def _read_obj(self):
        """Read EXACTLY ONE CBOR object from the stream; keep remaining bytes buffered.

        BBB-AIRGAP: the old version used `buf += recv(); cbor2.loads(buf)`. That pattern
        SILENTLY swallows the second response when two arrive in one recv (measured 2026-08-26:
        cbor2.loads(a+b) returns only a without raising). Because the loss is silent, the
        next call either mistakes a shifted response for its own or times out after 20 s.
        CBORDecoder + tell() gives an exact object boundary, preserving the remaining bytes."""
        timeout = self.s.gettimeout()
        deadline = None if timeout is None else time.monotonic() + timeout
        try:
            while True:
                if self.buf:
                    bio = io.BytesIO(self.buf)
                    try:
                        obj = cbor2.CBORDecoder(bio).decode()
                    except cbor2.CBORDecodeEOF:
                        pass  # object is still incomplete; more bytes needed
                    else:
                        self.buf = self.buf[bio.tell():]
                        return obj
                if deadline is not None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise socket.timeout('CBOR response timed out')
                    self.s.settimeout(remaining)
                chunk = self.s.recv(262144)
                if not chunk:
                    raise RpcError('daemon closed the connection')
                self.buf += chunk
        finally:
            if timeout is not None:
                self.s.settimeout(timeout)

    def send(self, method, params=None):
        """Send the request and return its id; do NOT wait for the response.

        Methods needing screen confirmation (e.g. debug_set_mnemonic, main/process/
        debug_set_mnemonic.c:105 await_message) respond only after a button press.
        wire.c:95 handles `libjade_request` IMMEDIATELY on the wire thread, so buttons
        can be pressed while a request is pending; this requires the send/recv_id split."""
        self.n += 1
        rid = str(self.n)
        m = {"id": rid, "method": method}
        if params is not None:
            m["params"] = params
        self.s.sendall(cbor2.dumps(m))
        return rid

    def recv_id(self, rid):
        """Return the response for the given id; hold intervening responses in inbox."""
        if rid in self.inbox:
            return self.inbox.pop(rid)
        while True:
            obj = self._read_obj()
            got = obj.get('id') if isinstance(obj, dict) else None
            if got == rid:
                return obj
            if got is None:
                raise RpcError('message without id: %s' % str(obj)[:160])
            self.inbox[got] = obj

    @contextlib.contextmanager
    def _timeout(self, seconds):
        """Temporarily change the socket timeout for a single read.

        Polling loops measure budgets by elapsed time; a fixed 20 s socket timeout
        makes that budget meaningless on one slow response. A timeout does not corrupt
        the buffer: _read_obj keeps the remaining bytes in self.buf."""
        previous = self.s.gettimeout()
        if seconds is not None:
            self.s.settimeout(seconds)
        try:
            yield
        finally:
            self.s.settimeout(previous)

    def try_recv_id(self, rid, timeout=0.5):
        """Wait briefly for rid's response; return None if it does not arrive.

        This distinguishes a request that is still pending from one that has already
        completed: the response may be in inbox or waiting unread in the socket buffer."""
        if rid in self.inbox:
            return self.inbox.pop(rid)
        with self._timeout(timeout):
            try:
                return self.recv_id(rid)
            except socket.timeout:
                return None

    def rpc(self, method, params=None, timeout=None):
        rid = self.send(method, params)
        with self._timeout(timeout):
            return self.recv_id(rid)

    def btn(self, which, timeout=None):
        """Send the button event and verify that the daemon ACCEPTED it.

        Response validation belongs here because a failed send_input means the same
        thing to every caller: no press occurred. When validation was left to callers,
        only menu_audit checked it; the others continued silently."""
        try:
            r = self.rpc('libjade_request',
                         {'request': 'send_input', 'event': which}, timeout=timeout)
        except OSError as exc:
            if timeout is None:
                raise
            raise RpcError('button RPC call failed: %s' % exc) from exc
        if not isinstance(r, dict) or 'error' in r or r.get('result') is not True:
            raise RpcError('button %r rejected; response: %s' % (which, str(r)[:160]))
        return r

    def _digest(self, deadline):
        """Read the screen once within the remaining budget; return its digest.

        Without a budget for display_bytes, a read can block until the socket's 20 s
        timeout, making the 2 s deadline here meaningless."""
        remaining = max(deadline - time.monotonic(), READ_TIMEOUT_FLOOR)
        try:
            data = self.display_bytes(timeout=remaining)
        except socket.timeout as exc:
            # Give callers a uniform error: this helper returns a digest or raises RpcError.
            raise RpcError('screen read did not respond within %g s' % remaining) from exc
        except OSError as exc:
            raise RpcError('screen read failed: %s' % exc) from exc
        return hashlib.sha256(data).digest()

    def settle(self, deadline, first_wait=0.0):
        """Wait for screen drawing to settle and return the settled screen's digest.

        One read is insufficient: a frame captured during drawing may be an intermediate
        state. Using that frame as the baseline lets remaining drawing produce a difference
        and a false confirmation even if the next event is NEVER processed.

        first_wait: time to wait BEFORE the first read. A case requiring a positive value
        was measured (2026-08-26, menu_audit): if drawing is QUEUED but has NOT STARTED,
        two consecutive reads return the same old frame and falsely confirm settling.
        This is relatively harmless for a caller that triggers its own change with a
        direction key, but a caller establishing an asynchronous baseline (menu_audit.seed)
        uses the wrong baseline and mistakes the next normal drawing for its target screen.
        The default is 0 because adding 150 ms to every press would slow keyboard entry excessively."""
        if first_wait:
            time.sleep(min(first_wait, max(deadline - time.monotonic(), 0)))
        prev = self._digest(deadline)
        while time.monotonic() < deadline:
            time.sleep(SETTLE_POLL)
            cur = self._digest(deadline)
            if cur == prev:
                return cur
            prev = cur
        raise RpcError('screen did not settle within %g s' % SETTLE_DEADLINE)

    def press(self, event, before_send=None, expect_change=True):
        """Press a button and verify that the screen changes, or stays unchanged, as expected.

        expect_change=False is for presses expected to leave the screen UNCHANGED. It is
        an inverse ASSERTION, not an escape hatch: a change also raises RpcError.
        Such presses exist and have been measured. For example, `Settings > Display > Flip Orientation`
        repaints the same menu with `gui_repaint()` in the emulator (`main/process/dashboard.c:1952`),
        but without panel rotation the frame stays byte-identical. The expectation must be
        EXPLICIT in the step sequence to distinguish a dropped press from legitimate stillness.

        Measure instead of sleeping for a fixed duration. Blind counting fails in two places:
        a drifting dice wheel counter makes the next click hit '<' and undo a roll; a lost
        menu click shifts the whole step sequence, silently measuring the wrong screen.
        Both losses are silent, so do not advance until the screen change is verified.

        The budget applies per button in two phases: first a stable baseline (previous
        drawing must finish), then a different and again stable result.

        before_send: called AFTER the baseline settles, IMMEDIATELY BEFORE sending the button.
        Invalidate caller state here; doing so earlier would needlessly corrupt it if the
        baseline failed to settle and no button was sent."""
        baseline = self.settle(time.monotonic() + SETTLE_DEADLINE)
        deadline = time.monotonic() + SETTLE_DEADLINE
        remaining = max(deadline - time.monotonic(), READ_TIMEOUT_FLOOR)
        if before_send is not None:
            before_send()
        self.btn(event, timeout=remaining)
        time.sleep(PRESS_DELAY)
        while time.monotonic() < deadline:
            if self._digest(deadline) != baseline:
                if not expect_change:
                    raise RpcError('after %r, expected no change in screen, but it changed' % event)
                # Let the new screen settle so the next call does not use an intermediate frame as its baseline.
                return self.settle(time.monotonic() + SETTLE_DEADLINE)
            time.sleep(SETTLE_POLL)
        if not expect_change:
            return baseline
        raise RpcError('after %r, screen did not change within %g s' % (event, SETTLE_DEADLINE))

    def _press_and_wait(self, event):
        """Dice-screen wrapper around press(); track the wheel position.

        Mark the position unknown only when sending begins, and update it only after
        verification completes. An interrupted move leaves the position uncertain; otherwise
        a caller reusing the object could compute the wrong shortcut, land on '<', and
        erase the previous roll."""
        step = {'left': -1, 'right': 1}.get(event)
        previous = self.wheel
        if step is not None and previous is None:
            raise RpcError('dice entry: wheel position unknown; restart the dice screen')

        def override():
            if step is not None:
                self.wheel = None

        self.press(event, override)
        if step is not None:
            self.wheel = (previous + step) % len(DICE_CHARS)

    def dice(self, rolls):
        """Enter the given rolls on the dice screen; the wheel cycles through '123456<'."""
        chars = DICE_CHARS
        if self.wheel is None:
            raise RpcError('dice entry: wheel position unknown; restart the dice screen')
        cur = self.wheel
        for ch in rolls:
            target = chars.index(ch)
            delta = (target - cur) % len(chars)
            # Update the position after EVERY verified move. With a batch update, if one move
            # succeeded and the next failed, the device selection would advance while the tool's
            # counter stayed behind. A caller reusing the object would compute the wrong shortcut,
            # land on '<', and erase the previous roll.
            if delta <= len(chars) // 2:
                for _ in range(delta):
                    self._press_and_wait('right')
                    cur = (cur + 1) % len(chars)
                    self.wheel = cur
            else:
                for _ in range(len(chars) - delta):
                    self._press_and_wait('left')
                    cur = (cur - 1) % len(chars)
                    self.wheel = cur
            self._press_and_wait('click')
        return len(rolls)

    def cam(self, spec):
        """Send synthetic grayscale frames. spec = '<count>:<seed>'; frames are deterministic."""
        count, _, seed = spec.partition(':')
        count = int(count)
        seed = int(seed or 0)
        size = CAM_FRAME_SIZE
        for i in range(count):
            # deterministic pattern, but actually different between frames
            frame = hashlib.sha256(f'{seed}:{i}'.encode()).digest() * (size // 32)
            self.rpc('libjade_request', {'request': 'set_camera_bytes', 'bytes': frame})
            time.sleep(CAM_DELAY)
        return count

    def camfile(self, spec):
        """Repeatedly send a grayscale frame from disk. spec = '<path>[:<count>]'.

        cam()/flatcam() produce synthetic patterns; scanning a real QR requires a frame
        written by screen_qr_to_camera.py. The scanner retries on every frame, so send
        the same frame several times."""
        path, _, count = spec.rpartition(':')
        if not path:  # without ':', rpartition puts the path on the right
            path, count = count, ''
        count = int(count or 12)
        frame = open(path, 'rb').read()
        if len(frame) != CAM_FRAME_SIZE:
            raise RpcError('frame is %d bytes; expected %d' % (len(frame), CAM_FRAME_SIZE))
        for _ in range(count):
            self.rpc('libjade_request', {'request': 'set_camera_bytes', 'bytes': frame})
            time.sleep(CAM_DELAY)
        return count

    def camfiles(self, spec):
        """Push <prefix>-NN.gray frames in order, cycling `rounds` times, so a multi-part BC-UR can
        be scanned. camfile repeats one frame; the fountain decoder needs different parts in a row.
        spec = '<prefix>[:<rounds>]'."""
        prefix, _, rounds = spec.rpartition(':')
        if not prefix:
            prefix, rounds = rounds, ''
        rounds = int(rounds or 3)
        if rounds < 1:
            raise RpcError('rounds must be >= 1: %d' % rounds)
        paths = sorted(glob.glob(prefix + '-[0-9][0-9].gray'))
        if not paths:
            raise RpcError('no frames: %s-NN.gray' % prefix)
        frames = [open(p, 'rb').read() for p in paths]
        for f in frames:
            if len(f) != CAM_FRAME_SIZE:
                raise RpcError('frame is %d bytes, expected %d' % (len(f), CAM_FRAME_SIZE))
        for _ in range(rounds):
            for f in frames:
                self.rpc('libjade_request', {'request': 'set_camera_bytes', 'bytes': f})
                time.sleep(CAM_DELAY)
        return rounds * len(frames)

    def flatcam(self, spec):
        """Send flat (unvarying) frames; each frame has one tone, increasing between frames."""
        count, _, start = spec.partition(':')
        count = int(count); start = int(start or 0)
        size = CAM_FRAME_SIZE
        for i in range(count):
            frame = bytes([(start + i) & 0xff]) * size
            self.rpc('libjade_request', {'request': 'set_camera_bytes', 'bytes': frame})
            time.sleep(CAM_DELAY)
        return count

    def display_bytes(self, timeout=None):
        """Return the screen's raw RGB565 contents without writing to disk.

        Needed to poll screen changes: a fixed sleep risks pressing a button before
        drawing is ready. If provided, timeout limits this single read so polling
        budgets actually constrain elapsed time."""
        r = self.rpc('libjade_request', {'request': 'get_display_bytes'}, timeout=timeout)
        if not isinstance(r, dict) or 'error' in r or not isinstance(r.get('result'), bytes):
            raise RpcError('get_display_bytes unexpected response: %s' % str(r)[:160])
        data = r['result']
        expected = self.w * self.h * 2
        if len(data) != expected:
            raise RpcError('display %dx%d expected %d bytes, received %d' % (
                self.w, self.h, expected, len(data)))
        return data

    def shot(self, name):
        data = self.display_bytes()
        with open(f'{OUT}/{name}.rgb565', 'wb') as f:
            f.write(data)
        return '%d bytes (%dx%d)' % (len(data), self.w, self.h)

def main():
    j = Jade(sys.argv[1])
    for cmd in sys.argv[2:]:
        kind, _, arg = cmd.partition(':')
        if kind == 'btn':
            print(f'{cmd} ->', j.btn(arg).get('result'))
        elif kind == 'shot':
            print(f'{cmd} -> {j.shot(arg)}')
        elif kind == 'wait':
            time.sleep(float(arg))
        elif kind == 'flatcam':
            print(f'{cmd} -> {j.flatcam(arg)} flat frames pushed')
        elif kind == 'cam':
            print(f'{cmd} -> {j.cam(arg)} frames pushed')
        elif kind == 'camfile':
            print(f'{cmd} -> {j.camfile(arg)} frames pushed')
        elif kind == 'camfiles':
            print(f'{cmd} -> {j.camfiles(arg)} frames pushed')
        elif kind == 'dice':
            print(f'dice -> {j.dice(arg)} rolls entered')
        elif kind == 'rpc':
            r = j.rpc(arg)
            print(f'{cmd} ->', str(r)[:200])
        else:
            raise SystemExit(f'unknown command: {cmd}')


if __name__ == '__main__':
    main()
