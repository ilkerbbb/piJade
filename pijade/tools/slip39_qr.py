#!/usr/bin/env python3
"""SLIP-0039 share camera frames (main/process/mnemonic.c validate_slip39_share).

The device takes a share as the plain text of its words, which is what slip39_parse_share reads:
any control character or space separates words, so one share per QR and nothing around it.  There
is no SeedQR-style digit encoding for SLIP-0039 - SeedQR is defined over the BIP39 list.

Shares come from pijade/tools/fixtures/slip39_vectors.json, the same fixture the selfcheck is
built from (pijade/tools/gen_slip39_vectors.py), so the frames and the C vectors cannot drift
apart.  Those are Trezor's published test vectors: public material, safe to write to disk here.

The --corrupt families build frames the device is expected to reject, each defect stated once:
  checksum  last word replaced with another wordlist word - RS1024 fails
  word      one word replaced with a string that is not in the wordlist at all
A share that parses but belongs to another backup needs no corruption: any two vectors are
different backups, so feeding one vector's share after another's is the measurement.

Usage (in the container, writing into /probe):
    python3 /jade/pijade/tools/slip39_qr.py --vector 0 /probe/slip39
    python3 /jade/pijade/tools/slip39_qr.py --vector 3 --corrupt checksum /probe/slip39
Dependencies: qrcode, in a virtualenv of your own.
"""
import argparse
import json
import os
import sys

import qrcode
from qrcode.constants import ERROR_CORRECT_L

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from epoch_qr import frame_scale, write_gray, write_png  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURE = os.path.join(REPO, "pijade/tools/fixtures/slip39_vectors.json")
WORDLIST_SOURCE = os.path.join(REPO, "main/slip39_english.c")

# quirc identifies nothing below 3 px per module (see frame_scale in epoch_qr.py).  A 33 word
# share is the largest thing this renders, and it is checked rather than assumed: a share that
# came out under the floor would fail to scan and look like a device bug.
MIN_SCALE = 3


def wordlist():
    """The 1024 SLIP-0039 words, read from the vendored C table rather than copied."""
    import re

    with open(WORDLIST_SOURCE, encoding="ascii") as handle:
        words = re.findall(r'"([a-z]+)"', handle.read())
    assert len(words) == 1024, len(words)
    return words


def corrupt(share, kind, words):
    """Return the share text with one stated defect in it."""
    parts = share.split()
    if kind == "checksum":
        # A different valid word in the last position: every word is still in the list, so this
        # reaches the RS1024 check rather than the wordlist lookup.
        parts[-1] = words[(words.index(parts[-1]) + 1) % len(words)]
    elif kind == "word":
        parts[3] = "notaslip39word"
    else:
        raise SystemExit("unknown corruption: %s" % kind)
    return " ".join(parts)


def render(text, path_stem):
    code = qrcode.QRCode(error_correction=ERROR_CORRECT_L, border=4)
    code.add_data(text)
    code.make(fit=True)
    matrix = code.get_matrix()
    scale = frame_scale(len(matrix))
    if scale < MIN_SCALE:
        raise SystemExit("%s: %d modules gives %d px per module, below the %d px floor"
                         % (path_stem, len(matrix), scale, MIN_SCALE))
    write_gray(path_stem + ".gray", matrix)
    write_png(path_stem + ".png", matrix)
    return len(matrix), scale


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vector", type=int, required=True, help="index into the fixture")
    parser.add_argument("--corrupt", choices=("checksum", "word"),
                        help="render a share the device must reject")
    parser.add_argument("--share", type=int, help="render only this share of the vector (1 based)")
    parser.add_argument("outdir")
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    with open(FIXTURE, encoding="utf-8") as handle:
        vectors = json.load(handle)
    description, mnemonics, secret, _xprv = vectors[args.vector]
    if not secret:
        raise SystemExit("vector %d is an invalid-mnemonic case, not a recoverable backup"
                         % args.vector)

    words = wordlist()
    if args.corrupt:
        mnemonics = [corrupt(mnemonics[0], args.corrupt, words)]
        description = "%s | %s defect" % (description, args.corrupt)
    elif args.share:
        mnemonics = [mnemonics[args.share - 1]]

    stem = "s39v%d%s" % (args.vector, "_" + args.corrupt if args.corrupt else "")
    print(description)
    for index, mnemonic in enumerate(mnemonics, start=1):
        path_stem = os.path.join(args.outdir, "%s-%02d" % (stem, index))
        modules, scale = render(mnemonic, path_stem)
        print("  %s.gray  %d words, %d modules, %d px per module"
              % (os.path.basename(path_stem), len(mnemonic.split()), modules, scale))


if __name__ == "__main__":
    main()
