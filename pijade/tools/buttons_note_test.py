# BBB-AIRGAP acceptance test: the buttons check names the button you just pressed.
# The screen used to carry one fixed remark, so a green mark told you a button was seen but never
# what it is for.  The line under the marks is now rewritten on every press (IO_TEST_NOTE_*,
# main/ui.h), and it opens on KEY3 because pressing KEY3 leaves the screen.
# Run:  docker exec jade-dev python3 /jade/pijade/tools/buttons_note_test.py
# Needs the build_linux build and the /probe mount.  Takes about a minute.
# POSITIVE: every one of the six inputs must leave a different line behind, and the opening line
# must differ from all of them (that one belongs to KEY3, which never gets a press of its own).
# NEGATIVE: pressing the same button twice must NOT change the line again, so a test that passes
# by repainting anything at all is ruled out.
# ALSO: each line must be ONE line.  Hashing alone would accept the bug this screen actually had,
# where the new text landed on top of the old one instead of replacing it, because two lines piled
# on each other still hash differently every time.  Measured on the emulator: a single line lights
# 615-722 pixels in the note area, a pile lights 1233-1463, so the ceiling below separates them
# with room on both sides.
import sys, os, subprocess, threading, hashlib
sys.path.insert(0, '/jade/pijade/tools')
import jadectl

DAEMON = '/jade/build_linux/libjade/libjade_daemon'
# Public BIP-39 test vector, never a real seed.
SEED = ('abandon abandon abandon abandon abandon abandon '
        'abandon abandon abandon abandon abandon about')

WIDTH = HEIGHT = 240
# The note is the bottom slice of the outer vsplit 20/56/24 (make_io_test_buttons_activity(),
# main/ui/dashboard.c), so it starts where the body ends.  Hashing only these rows keeps the marks
# out of the measurement: a green mark changes the body, and would otherwise make every press look
# like it rewrote the line.
NOTE_TOP = HEIGHT * 76 // 100

# Options > Info > I/O Test > Buttons.  The menus open with the selection on the header button, so
# each list needs one press more than its row index.
NAV = (['btn:right', 'btn:right', 'btn:click']          # home carousel -> Options
       + ['btn:down'] * 7 + ['btn:click']                # Options -> Info
       + ['btn:down'] * 3 + ['btn:click']                # Info -> I/O Test
       + ['btn:down'] * 2 + ['btn:click'])               # I/O Test -> Buttons


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


def drive(sock, log, *steps):
    r = subprocess.run(['python3', '/jade/pijade/tools/menu_audit.py', sock, log] + list(steps),
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('NAVIGATION FAILED:', steps)
        print(r.stdout[-1200:])
        print(r.stderr[-800:])
        raise SystemExit(1)


# A single line of this font lights well under this many pixels; two lines drawn over each other
# light well over it.  Both ends were measured, see the header.
ONE_LINE_CEILING = 1000


def note_hash(frame):
    row = NOTE_TOP * WIDTH * 2
    return hashlib.sha256(frame[row:]).hexdigest()[:8]


def note_pixels(frame):
    lit = 0
    for y in range(NOTE_TOP, HEIGHT):
        row = y * WIDTH * 2
        for x in range(WIDTH):
            i = row + x * 2
            v = (frame[i] << 8) | frame[i + 1]
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            if r > 180 and g > 180 and b > 180:
                lit += 1
    return lit


results = {}
proc, log_file, sock, log = start_daemon('note')
try:
    drive(sock, log, 'nav', 'seed:' + SEED, *NAV)
    jade = jadectl.Jade(sock)

    frames = {'opening (KEY3)': jade.display_bytes()}
    for key in ('right', 'left', 'up', 'down', 'first', 'click'):
        jade.press(key)
        frames[key] = jade.display_bytes()

    lines = {}
    piled = []
    for name, frame in frames.items():
        lit = note_pixels(frame)
        lines[name] = note_hash(frame)
        print('%-16s note %s  %4d lit' % (name, lines[name], lit))
        if lit >= ONE_LINE_CEILING:
            piled.append(name)
    results['every press writes its own line'] = len(set(lines.values())) == len(lines)
    results['each line replaces the last one'] = not piled
    if piled:
        print('PILED ON TOP OF THE PREVIOUS LINE:', piled)

    # NEGATIVE: the same input again must leave the panel byte for byte where it is.  press()
    # raises if the screen changes, so this is an assertion and not a skipped check.
    before = note_hash(jade.display_bytes())
    jade.press('click', expect_change=False)
    after = note_hash(jade.display_bytes())
    print('%-16s note %s -> %s' % ('click again', before, after))
    results['repeating a press does not rewrite the line'] = (before == after)
finally:
    stop_daemon(proc, log_file)

print('--- SUMMARY ---')
failures = 0
for name, ok in results.items():
    print(name, '=', ok)
    if not ok:
        failures += 1
print('FAILURES:', failures)
raise SystemExit(1 if failures else 0)
