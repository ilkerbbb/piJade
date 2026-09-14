#!/usr/bin/env python3
"""BBB-AIRGAP: emulator measurement of the SLIP-0039 recovery path (2026-09-14).

Drives Options > Temporary Signer > Restore Wallet > Split Backup > SLIP39 and feeds the share
frames written by pijade/tools/slip39_qr.py.  Shares are Trezor's published test vectors, so the
words are public; even so nothing here prints them, and the wallet leaves the device only as the
fingerprint its home screen shows.

Cases:
  single  one 20 word share - the whole backup, so the passphrase question comes straight away
  group   a 2-of-3 backup - the first share must NOT finish it, the second must
  long    one 33 word share - the 256 bit form, the largest share this fork accepts
  bad     a share with a broken RS1024 checksum - the scanner must refuse it and stay open
  foreign a valid share of a DIFFERENT backup - parses, and slip39_add_share must reject it
          (no special frame: vector 0 and vector 3 are different backups by construction)
  words   the same share as 'single', TYPED instead of scanned - the path a real SLIP-0039
          backup takes, since the standard prints words and puts them in no QR.  It must reach
          the same wallet the scanned share reaches, which is what says the word entry read the
          SLIP-0039 wordlist rather than the BIP39 one.
  words33 the same share as 'long', typed - 33 words is past the 24 the entry arrays used to
          hold, so this is the case that measures WORDLIST_ENTRY_MAXWORDS rather than assumes it
  wordsbad a typed share whose last word is a different wordlist word - every word is in the
          list, so the RS1024 check is what refuses it and the entry must reopen, not leave
  pass    the 33 word share again, this time with the passphrase the published vectors were
          made with.  In SLIP-0039 the passphrase decrypts the master secret rather than making
          a second wallet from one, so this is the case whose wallet can be checked against the
          vector's own xprv: a second daemon is given that master secret through
          debug_set_mnemonic and its home screen is the frame to match.  Skipping the
          passphrase, as every other case does, opens a different wallet by design.

Usage: s39_recover.py <socket> <case> <tag> <frame-directory> [reference-socket]
"""
import hashlib
import io
import json
import os
import re
import sys
import time

import jadectl
import kbd

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURE = os.path.join(REPO, 'pijade/tools/fixtures/slip39_vectors.json')
WORDLIST_SOURCE = os.path.join(REPO, 'main/slip39_english.c')

# main/process/mnemonic.c: the keyboard gives way to the 'Select word N' carousel once a typed
# stem has at most this many candidates.  Named rather than assumed, as sx_combine.py names it.
NUM_WORDS_SELECT = 10

# A derivation takes longer than a redraw; the same budget sx_combine.py uses.
SLOW_DEADLINE = 20.0

# The frames of each case, in the order they are fed.
CASES = {
    'single': ['s39v0-01.gray'],
    'group': ['s39v3-01.gray', 's39v3-02.gray'],
    'long': ['s39v19-01.gray'],
    'bad': ['s39v0_checksum-01.gray'],
    'foreign': ['s39v3-01.gray', 's39v0-01.gray'],
    'words': [],
    'words33': [],
    'wordsbad': [],
    'pass': ['s39v19-01.gray'],
}

# The passphrase the published vectors were encrypted with; the fixture does not carry it, so it
# is named here as libjade/selfcheck/slip39.c:21-36 names it, and it is test-vector data rather
# than a secret.  The vector it is used with here is the 256 bit one, because its master secret
# is 32 bytes and the reference daemon's debug_set_mnemonic takes a seed of 32 or 64 bytes
# (main/process/debug_set_mnemonic.c:51).
VECTOR_PASSPHRASE = 'TREZOR'
PASSPHRASE_VECTOR = 19

# Which fixture vector each typed case takes, and how long its share is.
TYPED = {
    'words': (0, 20),
    'words33': (19, 33),
    'wordsbad': (0, 20),
}


def press(j, event, note):
    try:
        j.press(event)
    except jadectl.RpcError as exc:
        raise SystemExit('%s: %s' % (note, exc))


def press_slow(j, event, note):
    saved = jadectl.SETTLE_DEADLINE
    jadectl.SETTLE_DEADLINE = SLOW_DEADLINE
    try:
        press(j, event, note)
    finally:
        jadectl.SETTLE_DEADLINE = saved


def slip39_wordlist():
    """The 1024 SLIP-0039 words, read from the vendored C table rather than copied."""
    with io.open(WORDLIST_SOURCE, encoding='ascii') as handle:
        body = handle.read()
    words = re.findall(r'"([a-z]+)"', body[body.index('SLIP39_WORDLIST[SLIP39_WORD_COUNT]'):])
    if len(words) != 1024:
        raise SystemExit('SLIP-0039 wordlist has %d entries, expected 1024' % len(words))
    return words


def fixture_share(vector, index=0):
    """One share's text from the same fixture the selfcheck and the QR frames are built from."""
    with io.open(FIXTURE, encoding='utf-8') as handle:
        vectors = json.load(handle)
    return vectors[vector][1][index]


def keystrokes(words, word):
    """(letters to type, carousel position) for one word, from the wordlist itself.

    The keyboard closes at the first stem with NUM_WORDS_SELECT or fewer candidates and the
    carousel lists those candidates in wordlist order, so both numbers follow from the list.
    Measured on this list: three letters are always enough (2026-09-14)."""
    for length in range(1, len(word) + 1):
        stem = word[:length]
        candidates = [w for w in words if w.startswith(stem)]
        if len(candidates) <= NUM_WORDS_SELECT:
            return stem.upper(), candidates.index(word)
    raise SystemExit('no stem of this word narrows to %d candidates' % NUM_WORDS_SELECT)


def to_slip39_menu(j, scan=True, nwords=20):
    """Walk to the share intake the case needs.  Each step is named so a changed menu fails by name."""
    for event, note in (('right', 'dashboard: Scan QR'), ('right', 'dashboard: Options'),
                        ('click', 'Options menu'), ('click', 'Temporary Signer'),
                        ('click', 'temporary login question'),
                        ('down', 'Restore Wallet: Split Backup row'), ('click', 'Split Backup'),
                        ('down', 'Split Backup: SLIP39 row'), ('click', 'SLIP39')):
        press(j, event, note)

    # Entry Method: 'Word' is the initial selection, 'Scan QR' is one row below it.
    if scan:
        press(j, 'down', 'Entry Method: Scan QR row')
        press(j, 'click', 'Entry Method: Scan QR')
        return

    press(j, 'click', 'Entry Method: Word')
    # SLIP39 Share: '20 Words' is the initial selection, '33 Words' is one row below it.
    if nwords == 33:
        press(j, 'down', 'SLIP39 Share: 33 Words row')
    press(j, 'click', 'SLIP39 Share: %d Words' % nwords)


def type_share(j, share):
    """Type one share.  Words reach the device only as keyboard positions, never as text."""
    words = slip39_wordlist()
    share_words = share.split()
    for index, word in enumerate(share_words):
        letters, steps = keystrokes(words, word)
        try:
            kbd.word(j, letters, steps)
        except (RuntimeError, jadectl.RpcError) as exc:
            raise SystemExit('word %d of %d could not be entered: %s' % (index + 1, len(share_words), exc))


def fixture_secret(vector):
    """One vector's master secret, the value its shares decrypt to with the right passphrase."""
    with io.open(FIXTURE, encoding='utf-8') as handle:
        vectors = json.load(handle)
    return bytes.fromhex(vectors[vector][2])


def reference_home(ref, secret):
    """The home screen a second daemon shows for this master secret; the frame to match.

    debug_set_mnemonic takes a seed as well as words, and a seed is what a SLIP-0039 backup
    yields: the reference wallet is made with the same keychain_derive_from_seed() call the
    measured path makes (main/process/debug_set_mnemonic.c:56 and main/process/mnemonic.c:2352),
    from the secret the vector publishes.  A seed wallet is implicitly temporary there, which is
    what Temporary Signer produces on the driven device, so the two screens are comparable in
    full rather than only in the fingerprint."""
    baseline = hashlib.sha256(ref.display_bytes()).digest()
    rid = ref.send('debug_set_mnemonic', {'seed': secret})
    deadline = time.monotonic() + SLOW_DEADLINE
    while time.monotonic() < deadline:
        time.sleep(0.05)
        if hashlib.sha256(ref.display_bytes()).digest() != baseline:
            break
    else:
        raise SystemExit('reference: debug_set_mnemonic showed no confirmation screen')
    ref.btn('click')
    response = ref.recv_id(rid)
    if not isinstance(response, dict) or 'error' in response:
        raise SystemExit('reference: debug_set_mnemonic rejected: %s' % str(response)[:160])
    return ref.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.5)


def feed(j, path, rounds=3):
    """Push one frame at the camera until the scanner takes it."""
    return j.camfile('%s:%d' % (path, rounds))


def main():
    if len(sys.argv) not in (5, 6):
        raise SystemExit('usage: s39_recover.py <socket> <case> <tag> <frame-directory> [reference-socket]')
    socket, case, tag, frame_dir = sys.argv[1:5]
    ref_socket = sys.argv[5] if len(sys.argv) == 6 else None
    if case not in CASES:
        raise SystemExit('case must be one of: %s' % ', '.join(sorted(CASES)))

    # The expected frame is taken before the measured device is touched, so a reference that
    # cannot be set up fails the case rather than being discovered after the share is fed in.
    expected = None
    if case == 'pass':
        if not ref_socket:
            raise SystemExit("case 'pass' needs the reference socket as a fifth argument")
        expected = reference_home(jadectl.Jade(ref_socket), fixture_secret(PASSPHRASE_VECTOR))

    j = jadectl.Jade(socket)

    if case in TYPED:
        vector, nwords = TYPED[case]
        share = fixture_share(vector)
        if len(share.split()) != nwords:
            raise SystemExit('vector %d share is %d words, expected %d'
                             % (vector, len(share.split()), nwords))
        if case == 'wordsbad':
            # A different valid word in the last position: every word is still in the list, so
            # this reaches the RS1024 check rather than the wordlist lookup.  Same defect
            # slip39_qr.py --corrupt checksum builds, stated the same way.
            words = slip39_wordlist()
            parts = share.split()
            parts[-1] = words[(words.index(parts[-1]) + 1) % len(words)]
            share = ' '.join(parts)
        to_slip39_menu(j, scan=False, nwords=nwords)
        j.shot('%s_keyboard' % tag)
        type_share(j, share)
        j.shot('%s_after1' % tag)
        if case == 'wordsbad':
            # The error screen is the measurement; acknowledge it and check the entry reopened
            # rather than the flow leaving.
            press(j, 'click', 'checksum error: acknowledge')
            reopened = kbd.is_keyboard(kbd.pixels(j))
            j.shot('%s_reopened' % tag)
            print('%s: %s after the refusal' % (case, 'keyboard reopened' if reopened else 'FAIL - not the keyboard'))
            return 0 if reopened else 1
    else:
        to_slip39_menu(j, scan=True)
        j.shot('%s_scanner' % tag)

    for index, frame in enumerate(CASES[case], start=1):
        if index > 1:
            # A share that does not finish the set leaves an 'Add Share' screen in front of the
            # scanner; its initial selection reopens the camera.  Without this press the next
            # frame is pushed at a screen that is not scanning and is silently dropped.
            press(j, 'click', 'shares entered: Add Share')
        feed(j, '%s/%s' % (frame_dir, frame))
        time.sleep(1.0)
        j.shot('%s_after%d' % (tag, index))

    if case in ('bad', 'foreign'):
        # The rejection screen is the measurement; acknowledge it and leave.
        print('%s: rejection frames written as %s_after*.rgb565' % (case, tag))
        return 0

    if case == 'pass':
        # 'Enter' sits to the right of 'Skip', which is the initial selection
        # (main/ui/dialogs.c:1068-1073).
        press(j, 'right', 'passphrase question: move to Enter')
        press(j, 'click', 'passphrase question: Enter')
        if kbd.ascii_selected(kbd.pixels(j), 0) is None:
            raise SystemExit('passphrase: the keyboard did not open')
        j.shot('%s_keyboard' % tag)
        kbd.ascii_text(j, VECTOR_PASSPHRASE)
        j.shot('%s_confirm' % tag)
        # Confirm Passphrase: 'Yes' sits to the right of 'No' (main/ui/mnemonic.c:491-493).
        press(j, 'right', 'confirm passphrase: move to Yes')
        press_slow(j, 'click', 'confirm passphrase: Yes')
        home = j.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.5)
        j.shot('%s_home' % tag)
        match = home == expected
        print('pass: wallet match=%s (the vector\'s own master secret)' % match)
        return 0 if match else 1

    # Complete: 'Enter a passphrase?' - Skip is the initial selection, so click takes it.
    press(j, 'click', 'passphrase question: Skip')
    home = j.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.5)
    j.shot('%s_home' % tag)
    print('%s: home screen reached, %d bytes' % (case, len(home) if home else 0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
