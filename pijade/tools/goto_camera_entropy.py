"""Navigate to camera entropy and PROVE ARRIVAL WITH A DISPLAY HASH.

Blind navigation is unreliable on this daemon: retrying a dropped press applied twice and
moved the menu back home (measured 2026-09-03; the home hash that day, 0bd4bb6820db, appeared
twice during navigation). Therefore, there are NO retries here; each button only waits for
a display change. Verify arrival against a hash from a manually driven run. On mismatch,
do not measure: a counter measured on the wrong screen silently misleads.

Usage: goto_camera_entropy.py <sock>

Hashes belong to a fresh daemon for an uninitialized device with a 240x240 display; recapture
them if screen dimensions or text change. An incorrect hash fails explicitly instead of
silently drifting; that is intentional.
"""
import hashlib, sys, time
sys.path.insert(0, '/jade/pijade/tools')
import jadectl

HOME = '646284f83f1b'
CAMERA   = '95a47fb910f0'
# An instruction screen sits between home and Setup Type ("For setup instructions visit
# blockstream.com/jade"). The manually driven run skipped it on the first press, omitted
# from the sequence; that missing press sent navigation to QR scanning (measured 2026-09-03).
SEQUENCE = ['click',   # home -> instructions
        'click',   # instructions -> Setup Type
        'down', 'click',            # Advanced Setup
        'click',                    # warning -> Create/Restore
        'click',                    # -> Entropy Source
        'down', 'down', 'click',    # -> Camera
        'down', 'click']            # -> 24 Words -> camera screen

j = jadectl.Jade(sys.argv[1])
def h():
    return hashlib.sha256(j.display_bytes()).hexdigest()[:12]

initial = h()
if initial != HOME:
    raise SystemExit('START is not home: %s (expected a fresh daemon)' % initial)

for i, button in enumerate(SEQUENCE):
    before = h()
    assert j.btn(button).get('result') is True, 'btn %s rejected' % button
    last = time.time() + 3.0
    while time.time() < last and h() == before:
        time.sleep(0.05)
    if h() == before:
        raise SystemExit('step %d (%s): display did not change' % (i + 1, button))

# Arrival is not the frame immediately after the last press: the camera first draws an
# intermediate frame with an empty progress bar (measured 2026-09-09: 57a62e352f15), then
# settles. Wait for the settled state to avoid failure even at the correct destination.
last = h()
wait_until = time.time() + 5.0
while time.time() < wait_until and last != CAMERA:
    time.sleep(0.2)
    last = h()
if last != CAMERA:
    j.shot('wrong_destination')
    raise SystemExit('WRONG DESTINATION: %s expected %s; /probe/wrong_destination.rgb565' % (last, CAMERA))
print('  camera entropy screen VERIFIED (%s)' % last)
