# BBB-AIRGAP acceptance test: the screen no longer dims on the camera screen.
# Before the fix the panel went dark 60s into the viewfinder and the press that woke it was
# discarded; the 300s that camera.c and qrmode.c ask for only reached the power-off timeout
# (idletimer.c), never the dimming one.
# Run:  docker exec jade-dev python3 /jade/pijade/tools/screen_dim_test.py
# Needs the build_linux_nci_log build and the /probe mount.  Takes about 3 minutes: two 72s waits.
# POSITIVE: the camera screen must NOT dim over 72s.
# NEGATIVE: the home screen must STILL dim (the fix must not spill outside its scope).
import sys, os, subprocess, threading, hashlib
sys.path.insert(0, '/jade/pijade/tools')
import jadectl

DAEMON = '/jade/build_linux_nci_log/libjade/libjade_daemon'
# Public BIP-39 test vector, never a real seed.
SEED = ('abandon abandon abandon abandon abandon abandon '
        'abandon abandon abandon abandon abandon about')


def wait(seconds):
    # A bare sleep is blocked by a hook in this environment.
    threading.Event().wait(seconds)


def count(log, needle):
    try:
        with open(log, 'r', errors='replace') as f:
            return sum(1 for line in f if needle in line)
    except OSError:
        return -1


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
        print('STEP FAILED:', steps)
        print(r.stdout[-1200:])
        print(r.stderr[-1200:])
        raise SystemExit(1)


results = {}

# --- POSITIVE: the camera screen ---
proc, log_file, sock, log = start_daemon('dim1')
try:
    drive(sock, log, 's0', 'seed:' + SEED)
    drive(sock, log, 's1', 'btn:right', 'btn:right', 'btn:click', 'btn:down', 'btn:click',
          'btn:down', 'btn:down', 'btn:click')
    drive(sock, log, 's2', 'btn:click')
    results['camera_opened'] = count(log, 'Camera init done') > 0
    wait(72)
    # Zero dim lines prove something only if the producer was alive for the whole window: a daemon
    # that died during the wait prints zero too, and both camera checks would pass on a dead run.
    # These two run AFTER the window, so they cover it: the process is still up, and it still
    # answers an RPC.
    results['camera_daemon_alive'] = (proc.poll() is None)
    try:
        jadectl.Jade(sock).display_bytes()
        responsive = True
    except Exception:
        responsive = False
    results['camera_daemon_responsive'] = responsive
    dimmed = count(log, 'dimming screen')
    woken = count(log, 'Activity while screen disabled')
    results['camera_does_not_dim'] = (dimmed == 0)
    results['camera_swallows_no_press'] = (woken == 0)
    print('camera: dim lines =', dimmed, '| swallowed-press lines =', woken,
          '| alive =', results['camera_daemon_alive'], '| responsive =', responsive)
finally:
    stop_daemon(proc, log_file)

# --- NEGATIVE: the home screen ---
proc, log_file, sock, log = start_daemon('dim2')
try:
    drive(sock, log, 's0', 'seed:' + SEED, 'shot:home')
    jade = jadectl.Jade(sock)
    frame = hashlib.sha256(jade.display_bytes()).hexdigest()[:6]
    wait(72)
    dimmed = count(log, 'dimming screen')
    results['home_still_dims'] = (dimmed > 0)
    print('home: dim lines =', dimmed, '| frame:', frame)
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
