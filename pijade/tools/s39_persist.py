#!/usr/bin/env python3
"""BBB-AIRGAP: emulator measurement of a PERSISTED SLIP-0039 wallet (run 5, 2026-09-14).

s39_recover.py measures the recovery itself and stops at a temporary wallet.  This runner
measures what happens after it: the same 33 word share is restored through auth_user on an
UNINITIALISED device, so the wallet is saved behind a PIN, and the daemon is then restarted
against the same settings file and unlocked again.

What is under test is the C2 design note section 3.2.  A SLIP-0039 backup yields a master
secret rather than a recovery phrase, so there is no mnemonic entropy to cache;
keychain_store() writes that master secret instead, behind a tag byte
(main/keychain.c:859-868), and keychain_load() derives the wallet back from it
(main/keychain.c:930-944).  Four things follow, and each is measured here rather than
assumed: the wallet survives a restart, the PIN alone brings it back, no passphrase is
asked for the second time, because in SLIP-0039 the passphrase decrypted the master secret
instead of being kept as a second factor, and the wallet's own menu no longer offers
'Backup', because every backup screen draws words and this wallet has none behind it.  The
'Backup' row follows the slot's entropy and not the wallet's seed, so it stays away even
though the wallet now comes back carrying one.

Identity is measured with get_xpub rather than by comparing screens: two daemons drawing
the same fingerprint is weaker evidence than the same extended key coming back over the
wire.  The ROOT key is asked for - an empty path, which main/process/get_xpubs.c:22 allows -
so the answer can be checked against the vector's own published xprv with no derivation in
between.  The negative control is the same call on the restarted daemon BEFORE the PIN: it
must be refused as hw locked, or 'the key came back' would say nothing about storage.

The pinserver is answered in this process, the way the C2d-1 probe measured, and its records
are held in a dict so that nothing about this wallet reaches the filesystem except the
emulator's own settings file.  The PIN is never known here: the wheel starts on a random
digit (main/ui/digit_entry.c:140) and this runner recognises the PICTURE of a digit rather
than its value, which is all it takes to type the same unknown PIN three times.

Usage: s39_persist.py <daemon-binary> <socket> <settings-prefix> <frame-directory> <tag>
"""
import base64
import hashlib
import io
import json
import os
import subprocess
import sys
import time

import jadectl
import kbd
import s39_recover
from s39_recover import press, press_slow, PASSPHRASE_VECTOR, SLOW_DEADLINE, VECTOR_PASSPHRASE

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

import wallycore as wally  # noqa: E402  (the repository root has to be on the path first)
from pinserver.pindb import PINDb  # noqa: E402
from pinserver.server import PINServerECDHv2  # noqa: E402

# The oracle's own key pair, as shipped with the pinserver submodule.  Absolute rather than
# relative: pinserver/server.py reads this name from the working directory, and the working
# directory here is not the repository root.
SERVER_PRIVATE_KEY = os.path.join(REPO, 'server_private_key.key')
SERVER_PUBLIC_KEY = os.path.join(REPO, 'server_public_key.pub')
PINServerECDHv2.STATIC_SERVER_PRIVATE_KEY_FILE = SERVER_PRIVATE_KEY

# Where the device is told the oracle lives.  Nothing dials these: the request comes back to
# this process as an out-message and is answered here.  Same pair the C2d-1 probe used.
TEST_URL = 'https://this.is.a.test.url.com'
TEST_ONION = 'http://we.dont.know.our.onion.but.this.string.is.about.the.right.size'

# get_xpub re-stamps the version bytes for the network it is asked for (main/wallet.c:1331),
# and keychain_derive_from_seed() builds the key with the mainnet private version
# (main/keychain.c:580), so mainnet is the network in which the device's answer and the
# vector's published xprv are the same string.
NETWORK = 'mainnet'

# A wallet menu cannot hold more rows than the table it is built in
# (main/process/dashboard.c:3395), and the walk also stops on the title bar's back arrow;
# a walk that passes this many stops without coming back to where it started is lost, not counting.
MAX_WALLET_STOPS = 9

# The 256 bit vector, whose single 33 word share is the one s39_recover.py's 'long' and
# 'pass' cases feed; its master secret is 32 bytes, which is what a seed wallet needs.
SHARE_FRAME = 's39v19-01.gray'

# The digit entry's geometry on a 240x240 panel, from main/ui/digit_entry.c:177-206 and the
# 20% title bar of main/ui/dialogs.c:9.  The horizontal figures are exact: the six boxes are
# an absolute 35 px split and the left pad is the integer half of what is left over.  The
# vertical ones are trimmed by a couple of pixels at each end so that the relative splits'
# rounding cannot move the crop off the box it measures.
W = H = 240
CELLS = 6
CELL_W = 35
CELL_X0 = (W - CELLS * CELL_W) // 2
_BODY_TOP = H * 20 // 100
_BODY_H = H - _BODY_TOP
_ROW_TOP = _BODY_TOP + _BODY_H * 10 // 100
_ROW_H = _BODY_H * 75 // 100
_TOPPAD = 20
CELL_TOP = _ROW_TOP + _TOPPAD
CELL_H = _ROW_H - _TOPPAD - (_TOPPAD + 8)
CROP_TRIM = 2

# main/ui/digit_entry.c:9 - ten digits and backspace are reachable on a PIN wheel (enter is
# not: digit_entry_allows_enter() is false for DIGIT_ENTRY_PIN), so this many steps visit
# every value there is.
WHEEL_VALUES = 11

# A pinserver roundtrip and a wallet derivation both take longer than a redraw.
PIN_DEADLINE = 60.0


class MemoryStorage(object):
    """The four calls PINDb makes, backed by a dict.

    pinserver/pindb.py picks FileStorage at import time when REDIS_HOST is unset, and that
    writes a <hex>.pin record into the working directory.  The record is the encrypted half
    of a wallet key; this measurement has no reason to leave one on disk.

    'Missing' has to be raised as FileNotFoundError and not as the dict's own KeyError:
    PINDb.set_pin reads any existing record for the pubkey and treats exactly that exception
    as 'no record yet' (pinserver/pindb.py:283).  A KeyError escapes instead and fails the
    very first PIN ever set."""

    _store = {}

    @classmethod
    def get(cls, key):
        if key not in cls._store:
            raise FileNotFoundError(key.hex())
        return cls._store[key]

    @classmethod
    def set(cls, key, data):
        cls._store[key] = data

    @classmethod
    def exists(cls, key):
        return key in cls._store

    @classmethod
    def remove(cls, key):
        if key not in cls._store:
            raise FileNotFoundError(key.hex())
        del cls._store[key]


def serve(result, handler):
    """Answer the one http_request the device makes, the way the companion app would."""
    req = result['http_request']
    data = base64.b64decode(req['params']['data']['data'].encode())
    cke, replay_counter, payload = data[:33], data[33:37], data[37:]
    server = PINServerECDHv2(replay_counter, cke)
    encrypted = server.call_with_payload(cke, payload, handler)
    return req['on-reply'], base64.b64encode(encrypted).decode()


def start_daemon(binary, sock, settings, log):
    """Start the emulator and wait for its socket.

    The settings files are NOT removed here: the second half of this measurement exists to
    read what the first half wrote, and a runner that tidied up between them would measure
    a fresh device twice."""
    if os.path.exists(sock):
        os.remove(sock)
    handle = io.open(log, 'ab')
    proc = subprocess.Popen([binary, '--socketfile', sock, '--settings', settings,
                             '--log-level', 'info'], stdout=handle, stderr=handle)
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if os.path.exists(sock):
            return proc, handle
        if proc.poll() is not None:
            handle.close()
            raise SystemExit('daemon exited before opening its socket; see %s' % log)
        time.sleep(0.2)
    proc.kill()
    handle.close()
    raise SystemExit('daemon did not open %s within 30 s' % sock)


def stop_daemon(proc, handle):
    """Stop the emulator.  Settings reach disk as they change (libjade/daemon.c:374), so
    there is nothing to flush here and a daemon that ignores the signal is simply killed."""
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=15)
    handle.close()


def pin_cell(px, index):
    """One digit box of the entry screen, as pixels.

    The whole box is taken rather than the glyph alone: every selected box draws the same
    fill and the same two arrows, so the only thing that can differ between two selected
    boxes is the digit on them."""
    x0 = CELL_X0 + index * CELL_W
    return tuple(px[y * W + x]
                 for y in range(CELL_TOP + CROP_TRIM, CELL_TOP + CELL_H - CROP_TRIM)
                 for x in range(x0, x0 + CELL_W))


def is_pin_entry(px):
    """Is this the six box digit entry, with the first box selected and the rest empty?

    This doubles as the positive control for the crop above: if the boxes were not where
    CELL_TOP and CELL_X0 say they are, the six crops would not fall into 'one different,
    five identical' and the caller would fail by name instead of measuring the wrong thing."""
    boxes = [pin_cell(px, i) for i in range(CELLS)]
    return boxes[0] != boxes[1] and all(box == boxes[1] for box in boxes[2:])


def wait_for_pin_entry(j, note, budget=PIN_DEADLINE):
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        if is_pin_entry(kbd.pixels(j)):
            return
        time.sleep(0.2)
    raise SystemExit('%s: no digit entry screen within %g s' % (note, budget))


def wait_for_change(j, baseline, note, budget=SLOW_DEADLINE):
    """Wait for a screen that arrives on its own rather than after a press.

    settle() cannot be used for this: with no press to hang the wait on it would take the
    OLD frame as its baseline and report the screen as settled before the new one is drawn."""
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        time.sleep(0.05)
        if hashlib.sha256(j.display_bytes()).digest() != baseline:
            return j.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.3)
    raise SystemExit('%s: the screen did not change within %g s' % (note, budget))


def wheel_control(j):
    """The crop follows the wheel: one step changes the selected box and nothing else.

    Without this the run could pass with a crop that happens to separate a selected box from
    an empty one while being blind to the digit drawn on it - which is the one thing the PIN
    entry below depends on.  Run after the picture is recorded, never before: see enter_pin."""
    before = kbd.pixels(j)
    press(j, 'right', 'digit wheel: one step')
    after = kbd.pixels(j)
    if pin_cell(before, 0) == pin_cell(after, 0):
        raise SystemExit('digit wheel: the selected box did not change on a wheel step')
    if pin_cell(before, CELLS - 1) != pin_cell(after, CELLS - 1):
        raise SystemExit('digit wheel: a box other than the selected one changed')
    print('control: one wheel step changes the selected box and no other')


def enter_pin(j, glyph, note):
    """Type the six digit PIN whose every digit carries the given picture.

    The value is not known and does not need to be: the wheel is turned until the box shows
    the picture that was recorded on the first screen, so the same digit is committed in
    every position and on every screen.

    The picture has to be taken from a box as it OPENS, not from one the wheel has been
    turned on: an opening box always carries a digit (main/ui/digit_entry.c:140 draws from
    the ten digits), while one step past '9' is the backspace, and clicking the backspace on
    the first box abandons the whole entry (main/ui/digit_entry.c:350-352).  Measured
    2026-09-14: a picture recorded one step after the box opened was the backspace one run in
    ten, and the entry it abandoned looked from here like a box that never showed the digit
    again."""
    for index in range(CELLS):
        for _ in range(WHEEL_VALUES):
            if pin_cell(kbd.pixels(j), index) == glyph:
                break
            press(j, 'right', '%s: digit %d wheel' % (note, index + 1))
        else:
            raise SystemExit('%s: digit %d never showed the recorded digit' % (note, index + 1))
        press(j, 'click', '%s: digit %d' % (note, index + 1))


def pin_roundtrip(j, rid, handler, note):
    """Answer the single pinserver request the device makes, and take the reply that follows.

    main/process/pinclient.c:505 sends the request as the REPLY to the message being handled,
    so the pending auth_user id carries it; the device's real answer comes back as the reply
    to the 'pin' message sent here."""
    reply = j.try_recv_id(rid, timeout=PIN_DEADLINE)
    result = reply.get('result') if isinstance(reply, dict) else None
    if not isinstance(result, dict) or 'http_request' not in result:
        raise SystemExit('%s: expected an http_request, got %s' % (note, str(reply)[:200]))
    on_reply, encrypted = serve(result, handler)
    final = j.rpc(on_reply, {'data': encrypted}, timeout=PIN_DEADLINE)
    if not isinstance(final, dict) or final.get('result') is not True:
        raise SystemExit('%s: the oracle roundtrip was rejected: %s' % (note, str(final)[:200]))
    print('%s: oracle roundtrip accepted, auth_user returned true' % note)


def set_test_oracle(j, pubkey):
    """Point the device at the oracle in this process.

    The confirmation screen opens on its back arrow, so the tick one step to its right is the
    answer.  This is sent ONCE, before the wallet exists; the second half does not repeat it,
    which is how the settings file is shown to have carried the override across the restart."""
    baseline = j.settle(time.monotonic() + SLOW_DEADLINE)
    rid = j.send('update_pinserver', {'urlA': TEST_URL, 'urlB': TEST_ONION, 'pubkey': pubkey})
    wait_for_change(j, baseline, 'Confirm Oracle screen')
    press(j, 'right', 'Confirm Oracle: move to the tick')
    press(j, 'click', 'Confirm Oracle: tick')
    reply = j.try_recv_id(rid, timeout=15)
    if not isinstance(reply, dict) or reply.get('result') is not True:
        raise SystemExit('update_pinserver rejected: %s' % str(reply)[:200])


# The walk from the welcome screen auth_user opens on an uninitialised device down to the
# share scanner.  Each step is named so that a changed menu fails by name rather than sending
# the next press to whatever screen happens to be in front.  This is NOT the walk
# s39_recover.py takes: that one starts at Options > Temporary Signer, where 'Scan QR' is the
# initial selection of the Restore Wallet menu; here the wallet is persistent and the initial
# selection is '12 Words' (main/ui/mnemonic.c:90).
SETUP_STEPS = (
    ('click', 'welcome screen: Continue'),
    ('click', 'Setup Type: Begin Setup'),
    ('down', 'Setup Method: Restore Wallet row'),
    ('click', 'Setup Method: Restore Wallet'),
    ('down', 'Restore Wallet: 24 Words row'),
    ('down', 'Restore Wallet: Scan QR row'),
    ('down', 'Restore Wallet: Split Backup row'),
    ('click', 'Restore Wallet: Split Backup'),
    ('down', 'Split Backup: SLIP39 row'),
    ('click', 'Split Backup: SLIP39'),
    ('down', 'Entry Method: Scan QR row'),
    ('click', 'Entry Method: Scan QR'),
)


def restore_share(j, frame_dir, tag):
    """Feed the 33 word share and give it the passphrase the published vectors carry."""
    for event, note in SETUP_STEPS:
        press(j, event, note)
    j.shot('%s_scanner' % tag)
    s39_recover.feed(j, os.path.join(frame_dir, SHARE_FRAME))
    time.sleep(1.0)
    j.shot('%s_scanned' % tag)

    # 'Enter' sits to the right of 'Skip', which is the initial selection
    # (main/process/mnemonic.c:2343).
    press(j, 'right', 'passphrase question: move to Enter')
    press(j, 'click', 'passphrase question: Enter')
    if kbd.ascii_selected(kbd.pixels(j), 0) is None:
        raise SystemExit('passphrase: the keyboard did not open')
    kbd.ascii_text(j, VECTOR_PASSPHRASE)
    # Confirm Passphrase: 'Yes' sits to the right of 'No' (main/ui/mnemonic.c:491-492).
    press(j, 'right', 'confirm passphrase: move to Yes')
    press_slow(j, 'click', 'confirm passphrase: Yes')


def fixture_xprv(vector):
    """The master xprv the vector publishes, from the same fixture the frames are built from."""
    with io.open(s39_recover.FIXTURE, encoding='utf-8') as handle:
        return json.load(handle)[vector][3]


def reference_xpub(secret):
    """The root xpub this master secret must produce, derived here rather than on the device.

    The vector's own published xprv is checked against the same derivation first, so the
    anchor is the vector rather than this function."""
    key = wally.bip32_key_from_seed(secret, wally.BIP32_VER_MAIN_PRIVATE, 0)
    published = wally.bip32_key_from_base58(fixture_xprv(PASSPHRASE_VECTOR))
    if wally.bip32_key_to_base58(key, wally.BIP32_FLAG_KEY_PRIVATE) \
            != wally.bip32_key_to_base58(published, wally.BIP32_FLAG_KEY_PRIVATE):
        raise SystemExit('fixture: the published xprv does not match its own master secret')
    return wally.bip32_key_to_base58(key, wally.BIP32_FLAG_KEY_PUBLIC)


def device_xpub(j, note):
    reply = j.rpc('get_xpub', {'network': NETWORK, 'path': []}, timeout=30)
    result = reply.get('result') if isinstance(reply, dict) else None
    if not isinstance(result, str):
        raise SystemExit('%s: get_xpub did not return a key: %s' % (note, str(reply)[:200]))
    return result


def locked_control(j):
    """A restarted device must refuse get_xpub until the PIN has been entered.

    Without this the equality further down could be satisfied by a keychain that never left
    memory, which is the opposite of what this run is measuring."""
    reply = j.rpc('get_xpub', {'network': NETWORK, 'path': []}, timeout=30)
    error = reply.get('error') if isinstance(reply, dict) else None
    if not isinstance(error, dict):
        raise SystemExit('control: a locked device answered get_xpub: %s' % str(reply)[:200])
    print('control: locked device refuses get_xpub (code %s)' % error.get('code'))


def wallet_menu_rows(j, tag, note):
    """Open the wallet's own menu from the home screen and count the rows it offers.

    Run 5's remaining question is whether a restored wallet still offers 'Backup'.  That row
    is laid out only for a wallet that still holds the entropy it was built from
    (main/process/dashboard.c:3468-3470), and this one came back from the blob as a
    tagged master secret, which keychain_load() derives the wallet from without caching any
    entropy (main/keychain.c:930-944) - the only writer is keychain_set_entropy(), reached from
    the paths that were given words (main/keychain.c:260).  The panel draws four rows at a time,
    so no single frame is the list: the walk goes down until the selection comes back to where
    it started, which is what counts the list, and every screen on the way is photographed.

    The ring stops on the title bar's back arrow as well as on the rows, which the last frame
    of the walk shows, so the number of rows is one less than the number of stops."""
    press(j, 'click', '%s: Session' % note)
    j.shot('%s_session' % tag)
    press(j, 'click', '%s: the wallet row' % note)
    top = j.settle(time.monotonic() + SLOW_DEADLINE)
    j.shot('%s_walletmenu_1' % tag)
    stops = 1
    while stops <= MAX_WALLET_STOPS:
        press(j, 'down', '%s: wallet menu, the stop after %d' % (note, stops))
        if j.settle(time.monotonic() + SLOW_DEADLINE) == top:
            return stops - 1
        stops += 1
        j.shot('%s_walletmenu_%d' % (tag, stops))
    raise SystemExit('%s: the wallet menu never came back to where the walk started' % note)


def control_wallet(j, secret):
    """Put the same master secret in again as words, so the missing row is measured.

    The two wallets differ in exactly what the 'Backup' row is gated on: this one is entered
    as a phrase, so debug_set_mnemonic keeps the entropy with it the way the user path does
    (main/process/debug_set_mnemonic.c:133), while the restored wallet only ever existed as a
    master secret.  Without this control 'no Backup row' would not be distinguishable from
    'the walk never reached the menu'.  Not temporary, so 'Forget' is not offered either and
    the one extra row this wallet has is the row under test."""
    phrase = wally.bip39_mnemonic_from_bytes(None, secret)
    baseline = j.settle(time.monotonic() + SLOW_DEADLINE)
    rid = j.send('debug_set_mnemonic', {'mnemonic': phrase, 'temporary_wallet': False})
    wait_for_change(j, baseline, 'control: the debug wallet warning')
    j.btn('click')
    reply = j.recv_id(rid)
    if not isinstance(reply, dict) or 'error' in reply:
        raise SystemExit('control: debug_set_mnemonic rejected: %s' % str(reply)[:160])
    j.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.5)


def settings_digest(settings):
    """The settings file's hash, never its contents: it holds the encrypted wallet blob."""
    lines = []
    for slot in ('a', 'b'):
        path = '%s.%s' % (settings, slot)
        if not os.path.exists(path):
            lines.append('%s missing' % slot)
            continue
        with io.open(path, 'rb') as handle:
            data = handle.read()
        lines.append('%s %d bytes sha256=%s' % (slot, len(data), hashlib.sha256(data).hexdigest()[:16]))
    return '; '.join(lines)


def main():
    if len(sys.argv) != 6:
        raise SystemExit('usage: s39_persist.py <daemon-binary> <socket> <settings-prefix>'
                         ' <frame-directory> <tag>')
    binary, sock, settings, frame_dir, tag = sys.argv[1:6]

    PINServerECDHv2.load_private_key()
    PINDb.storage = MemoryStorage
    with io.open(SERVER_PUBLIC_KEY, 'rb') as handle:
        pubkey = handle.read()
    expected = reference_xpub(s39_recover.fixture_secret(PASSPHRASE_VECTOR))

    for slot in ('a', 'b'):
        path = '%s.%s' % (settings, slot)
        if os.path.exists(path):
            os.remove(path)

    # First half: an uninitialised device restores the share and is given a PIN.
    proc, handle = start_daemon(binary, sock, settings, '%s.daemon1.log' % settings)
    try:
        j = jadectl.Jade(sock)
        if (j.w, j.h) != (W, H):
            raise SystemExit('the digit entry geometry here is measured for %dx%d px, '
                             'display is %dx%d px' % (W, H, j.w, j.h))
        set_test_oracle(j, pubkey)

        baseline = j.settle(time.monotonic() + SLOW_DEADLINE)
        rid = j.send('auth_user', {'network': NETWORK})
        wait_for_change(j, baseline, 'auth_user: setup welcome screen')
        restore_share(j, frame_dir, tag)

        wait_for_pin_entry(j, 'new PIN')
        j.shot('%s_newpin' % tag)
        glyph = pin_cell(kbd.pixels(j), 0)
        wheel_control(j)
        enter_pin(j, glyph, 'new PIN')
        wait_for_pin_entry(j, 'confirm PIN')
        j.shot('%s_confirmpin' % tag)
        enter_pin(j, glyph, 'confirm PIN')
        pin_roundtrip(j, rid, PINDb.set_pin, 'set_pin')

        first = device_xpub(j, 'first session')
        j.shot('%s_home1' % tag)
        print('setup: wallet matches the vector=%s' % (first == expected))
        print('settings after setup: %s' % settings_digest(settings))
    finally:
        stop_daemon(proc, handle)

    # Second half: the same settings file, a new daemon, the same PIN.
    proc, handle = start_daemon(binary, sock, settings, '%s.daemon2.log' % settings)
    try:
        j = jadectl.Jade(sock)
        locked_control(j)
        # The frame is taken AFTER that round trip on purpose.  The locked reject (-32002) is
        # emitted by dispatch_message (main/process/dashboard.c:562-567), whose only call site
        # is do_dashboard (:3801), and main() reaches that task only once the splash screen's
        # boot work is done (main/main.c:212-214,286-296).  So a reply in hand proves the
        # locked home is drawn.  Taken before the round trip, the shot catches the splash
        # instead: measured 2026-09-14, that run's frame showed 'Jade DIY' (main/gui.c:3268).
        baseline = j.settle(time.monotonic() + SLOW_DEADLINE)
        j.shot('%s_locked' % tag)

        rid = j.send('auth_user', {'network': NETWORK})
        wait_for_change(j, baseline, 'auth_user: unlock screen')
        wait_for_pin_entry(j, 'unlock PIN')
        j.shot('%s_unlockpin' % tag)
        enter_pin(j, glyph, 'unlock PIN')
        # The reply this waits for is sent after get_pin_load_keys() is done with the screen,
        # so it cannot arrive while get_passphrase() waits on one (main/process/auth_user.c:239-247).
        # Its arrival is therefore the measurement that no passphrase was asked for a second time.
        pin_roundtrip(j, rid, PINDb.get_aes_key, 'get_pin')
        print('restart: unlocked with the PIN alone, no passphrase screen in the way')

        second = device_xpub(j, 'second session')
        j.shot('%s_home2' % tag)
        restored_rows = wallet_menu_rows(j, tag, 'restored wallet')
    finally:
        stop_daemon(proc, handle)

    # Third daemon: the positive control for the row the restored wallet does not offer.  It
    # gets a settings file of its own, because a device that already has a PIN would ask for
    # it before drawing anything.
    control_settings = '%s.control' % settings
    for slot in ('a', 'b'):
        path = '%s.%s' % (control_settings, slot)
        if os.path.exists(path):
            os.remove(path)
    proc, handle = start_daemon(binary, sock, control_settings, '%s.daemon3.log' % settings)
    try:
        j = jadectl.Jade(sock)
        control_wallet(j, s39_recover.fixture_secret(PASSPHRASE_VECTOR))
        control_rows = wallet_menu_rows(j, '%s_control' % tag, 'control wallet')
    finally:
        stop_daemon(proc, handle)

    print('restart: same wallet=%s, matches the vector=%s'
          % (second == first, second == expected))
    print('oracle records held in memory: %d' % len(MemoryStorage._store))
    print('settings after restart: %s' % settings_digest(settings))
    print('wallet menu: restored wallet %d rows, words-built control %d rows'
          % (restored_rows, control_rows))
    return 0 if first == expected and second == first and control_rows == restored_rows + 1 else 1


if __name__ == '__main__':
    sys.exit(main())
