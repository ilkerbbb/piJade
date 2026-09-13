#!/usr/bin/env python3
"""BBB-AIRGAP: emulator measurement of the Seed XOR combine path (2026-09-13).

Drives Options > Temporary Signer > Restore Wallet > SeedXOR through the word keyboard
and checks the wallet the parts make.  The screen between parts shows the final word of
the combined phrase, which is a seed word; no seed
word is written to a log, a file or a terminal here.  Words reach the device only as
keyboard positions computed from the bip39 wordlist, and the result leaves the device
only as a wallet fingerprint on its home screen.

Two arms, because neither alone separates "the parts were xored" from "one part was taken
and the rest ignored":

  cancel   - the same part twice.  Every bit cancels, so the wallet MUST be the one the
             all-zero entropy makes, and the screen MUST carry a third message line.
             Keeping either part alone would give that part's own wallet and two lines.
  identity - the all-zero part and a test part.  Zero is xor's identity, so the wallet
             MUST be the test part's own wallet, and there MUST be no third line.

The wallet is read from the home screen rather than over RPC: a wallet put in by hand is
bound to no message source, so every post-login request is refused as 'hardware locked'
(KEYCHAIN_UNLOCKED_BY_MESSAGE_SOURCE, main/process/process_utils.h:97).  That is the
device's own rule and not an emulator limit, so the measurement reads what the device
shows: the home screen, whose footer carries the wallet fingerprint and whose only
wallet-dependent pixels are that fingerprint (measured 2026-09-13: two wallets differ in
rows 209-219 and nowhere else).  A fingerprint is a wallet's public name, not its words.

The expected wallet is not hard-coded: a second daemon is given the expected phrase with
debug_set_mnemonic, and its home screen is the frame to match, so the comparison is
between two devices rather than between a device and a constant that can go stale.
"""
import ast
import hashlib
import io
import struct
import sys
import time

import jadectl
import kbd

WORDLIST_PATH = '/jade/components/libwally-core/upstream/src/data/wordlists/english.txt'
TEST_SUITE_PATH = '/jade/test_jade.py'

# main/process/mnemonic.c: the keyboard gives way to the 'Select word N' carousel once a
# typed stem has at most this many candidates.  A different value there means a different
# number of letters to type, so it is named rather than assumed.
NUM_WORDS_SELECT = 10

# Public bip39 test vector: zero entropy.  It is the one phrase that may be written down
# here, and it is what xor's identity element and its cancelled pair both have to produce.
ZERO_MNEMONIC = ' '.join(['abandon'] * 11 + ['about'])

# The band of rows the message lines occupy, measured on the 'Parts entered: 1' screen: the
# title ends at row 28 and the footer buttons start at row 180 (2026-09-13).  Lines are counted
# in this band and nowhere else, so the title and the buttons cannot be mistaken for a line.
BAND_TOP, BAND_BOTTOM = 40, 175

# Screen change budget for a step that runs a derivation rather than a redraw.
SLOW_DEADLINE = 20.0


def wordlist():
    with io.open(WORDLIST_PATH, encoding='utf-8') as handle:
        words = [line.strip() for line in handle if line.strip()]
    if len(words) != 2048:
        raise SystemExit('wordlist has %d entries, expected 2048' % len(words))
    return words


def test_mnemonic(name):
    """Read a phrase out of the test suite by name.  Copying it here would go stale silently.

    Parsed rather than matched with a regular expression: these constants are written with
    backslash line continuations, and a pattern that captures the raw text keeps the backslash
    and the newline, which then count as an extra word (measured 2026-09-13)."""
    with io.open(TEST_SUITE_PATH, encoding='utf-8') as handle:
        tree = ast.parse(handle.read())
    for node in tree.body:
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Constant):
            continue
        if any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return node.value.value
    raise SystemExit('%s not found in %s' % (name, TEST_SUITE_PATH))


def keystrokes(words, word):
    """(letters to type, carousel position) for one word.

    The keyboard closes at the first stem with NUM_WORDS_SELECT or fewer candidates, and the
    carousel lists those candidates in wordlist order, so both numbers follow from the list."""
    for length in range(1, len(word) + 1):
        stem = word[:length]
        candidates = [w for w in words if w.startswith(stem)]
        if len(candidates) <= NUM_WORDS_SELECT:
            return stem.upper(), candidates.index(word)
    raise SystemExit('no stem of this word narrows to %d candidates' % NUM_WORDS_SELECT)


def press(j, event, note):
    try:
        j.press(event)
    except jadectl.RpcError as exc:
        raise SystemExit('%s: %s' % (note, exc))


def press_slow(j, event, note):
    """A press whose screen may take a derivation's time to appear."""
    saved = jadectl.SETTLE_DEADLINE
    jadectl.SETTLE_DEADLINE = SLOW_DEADLINE
    try:
        press(j, event, note)
    finally:
        jadectl.SETTLE_DEADLINE = saved


def differing_rows(a, b):
    """Row numbers whose pixels differ between two devices' screens, reported as a range."""
    pa, pb = kbd.pixels(a), kbd.pixels(b)
    rows = [y for y in range(kbd.H) if pa[y * kbd.W:(y + 1) * kbd.W] != pb[y * kbd.W:(y + 1) * kbd.W]]
    return '%d-%d' % (rows[0], rows[-1]) if rows else 'none'


def bands(px, y_from, y_to):
    """Row ranges in [y_from, y_to) that carry drawn pixels, one per message line."""
    out = []
    inside = False
    for y in range(y_from, y_to):
        drawn = any(v != kbd.BLACK for v in px[y * kbd.W:(y + 1) * kbd.W])
        if drawn and not inside:
            out.append([y, y])
        elif drawn:
            out[-1][1] = y
        inside = drawn
    return out


def save_band(px, band, path):
    """Write one message line's rows out as a frame of its own, for a person to look at.

    Only the third line is ever saved, and only the cancelled-pair arm has one: by construction
    it is 'Parts cancel out!' (main/process/mnemonic.c), never a word.  Saving it is how the
    line is known to fit on the panel - two lines of this feature did not, and counting
    characters did not find them, because the font is proportional."""
    y_from, y_to = band[0], band[1] + 1
    with open(path, 'wb') as handle:
        handle.write(struct.pack('>%dH' % ((y_to - y_from) * kbd.W),
                                 *px[y_from * kbd.W:y_to * kbd.W]))
    return y_to - y_from


def enter_part(j, words, phrase, label):
    """Type one part; the last word is preceded by the advanced-mode 'Final Word' question."""
    part_words = phrase.split()
    for index, word in enumerate(part_words):
        if index == len(part_words) - 1:
            # 'Existing' is the initial selection; 'Calculate' would randomise the carousel's
            # starting position (random_first_selection_word, main/process/mnemonic.c).
            press(j, 'click', '%s: final-word question' % label)
        letters, steps = keystrokes(words, word)
        try:
            kbd.word(j, letters, steps)
        except (RuntimeError, jadectl.RpcError) as exc:
            raise SystemExit('%s: word %d could not be entered: %s' % (label, index + 1, exc))


def reference_home(ref, phrase):
    """The home screen a second daemon shows for a phrase, used as the expected frame.

    The wallet is put in as temporary, which is what the measured path produces, so the two
    home screens are comparable in full rather than only in the fingerprint."""
    baseline = hashlib.sha256(ref.display_bytes()).digest()
    rid = ref.send('debug_set_mnemonic', {'mnemonic': phrase, 'temporary_wallet': True})
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


def main():
    if len(sys.argv) != 5:
        raise SystemExit('usage: sx_combine.py <ui-socket> <reference-socket> <case> <tag>')
    ui_socket, ref_socket, case, tag = sys.argv[1:5]
    words = wordlist()
    other = test_mnemonic('TEST_MNEMONIC_12')
    if len(other.split()) != 12:
        raise SystemExit('TEST_MNEMONIC_12 is not a 12-word phrase')

    if case == 'cancel':
        parts = (other, other)
        expected_phrase = ZERO_MNEMONIC
        expected_bands = 3
    elif case == 'identity':
        parts = (ZERO_MNEMONIC, other)
        expected_phrase = other
        expected_bands = 2
    else:
        raise SystemExit("case must be 'cancel' or 'identity'")

    ref = jadectl.Jade(ref_socket)
    expected = reference_home(ref, expected_phrase)

    j = jadectl.Jade(ui_socket)
    # Options > Temporary Signer > 'log in with a recovery phrase?' > Restore Wallet.
    for event, note in (('right', 'dashboard: Scan QR'), ('right', 'dashboard: Options'),
                        ('click', 'Options menu'), ('click', 'Temporary Signer'),
                        ('click', 'temporary login question'), ('down', 'Restore Wallet: SeedXOR row'),
                        ('click', 'SeedXOR'), ('click', 'Recovery Phrase: 12 words')):
        press(j, event, note)

    enter_part(j, words, parts[0], 'part A')
    # 'Parts entered: 1' with two lines and no seed word in them: the positive control for the
    # band count, and the only screen of this flow that is safe to keep as a frame.
    first_bands = len(bands(kbd.pixels(j), BAND_TOP, BAND_BOTTOM))
    j.shot('%s_part1' % tag)
    press(j, 'click', 'part A: Add Part')

    enter_part(j, words, parts[1], 'part B')
    last_screen = kbd.pixels(j)
    last_bands = bands(last_screen, BAND_TOP, BAND_BOTTOM)
    second_bands = len(last_bands)
    if len(last_bands) > 2:
        rows = save_band(last_screen, last_bands[2], '/probe/%s_lastline.rgb565' % tag)
        print('%s: third line saved as %s_lastline.rgb565 (%d rows)' % (case, tag, rows))
    # 'Add Part' is the right-hand footer button and the initial selection, so 'Done' - which
    # takes 'Cancel''s place once two parts are in - is one step to the left.
    press(j, 'left', 'part B: move to Done')
    press(j, 'click', 'part B: Done')
    if kbd.is_keyboard(kbd.pixels(j)):
        raise SystemExit('part B: Done opened another word entry, so Add Part was pressed instead')

    # 'Export recovery phrase as a SeedQR?' - advanced mode is on for a temporary restore
    # (main/process/mnemonic.c: advanced_mode = temporary_restore).  Decline it.
    press(j, 'left', 'SeedQR offer: move to No')
    press_slow(j, 'click', 'SeedQR offer: No')
    combined = j.settle(time.monotonic() + SLOW_DEADLINE, first_wait=0.5)

    ok = True
    if first_bands != 2:
        print('FAIL %s: control screen has %d message lines, expected 2' % (case, first_bands))
        ok = False
    if second_bands != expected_bands:
        print('FAIL %s: screen after the last part has %d message lines, expected %d'
              % (case, second_bands, expected_bands))
        ok = False
    if combined != expected:
        # Name the rows that differ, never their content: on this screen the difference is
        # meant to be the fingerprint, and saying which rows moved is what tells a wrong
        # wallet apart from a screen that is not the home screen at all.
        rows = differing_rows(j, ref)
        print('FAIL %s: home screen does not match the expected wallet; rows %s differ'
              % (case, rows))
        ok = False
    print('%s %s: wallet match=%s, message lines %d -> %d'
          % ('PASS' if ok else 'FAIL', case, combined == expected, first_bands, second_bands))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
