#!/usr/bin/env python3
"""Standard SeedQR camera frames (main/process/mnemonic.c import_seedqr).

The device accepts a bare string of zero-padded 4-digit BIP39 indices, exactly 48 or 96 bytes,
digits only and no separators (mnemonic.c:1411). The checksum is not verified there; it is
verified afterwards by import_and_validate_mnemonic, so the digits have to encode a mnemonic
that is actually valid.

Usage: mnemonic_qr.py <output_dir>
Dependencies: cbor2==6.1.2 and qrcode, in a virtualenv of your own.

Writes mne.gray/.png (12 words) and mne24.gray/.png (24 words), the frames desc_scenarios.sh
loads a wallet with. Public test vectors only: the all-zero entropy seeds, abandon x11 + about
and abandon x23 + art. The indices are derived from the entropy and asserted against those two
vectors, so a wrong index or a broken checksum fails here rather than on the device.
"""
import argparse
import hashlib
import os
import sys

import qrcode
from qrcode.constants import ERROR_CORRECT_L

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from epoch_qr import write_gray, write_png  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORDLIST = os.path.join(REPO, "components/libwally-core/upstream/src/data/wordlists/english.txt")

# What each vector has to expand to. Asserted, never assumed.
VECTORS = (
    (16, "mne", " ".join(["abandon"] * 11 + ["about"])),
    (32, "mne24", " ".join(["abandon"] * 23 + ["art"])),
)


def bip39_indices(entropy):
    """BIP39: the entropy bits plus len(entropy)*8/32 checksum bits, in 11-bit groups."""
    checksum_bits = len(entropy) * 8 // 32
    digest = hashlib.sha256(entropy).digest()
    bits = "".join(format(byte, "08b") for byte in entropy)
    bits += "".join(format(byte, "08b") for byte in digest)[:checksum_bits]
    return [int(bits[i : i + 11], 2) for i in range(0, len(bits), 11)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outdir")
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    with open(WORDLIST, encoding="ascii") as handle:
        words = [line.strip() for line in handle if line.strip()]
    assert len(words) == 2048, len(words)

    for entropy_len, name, expected in VECTORS:
        indices = bip39_indices(bytes(entropy_len))
        assert " ".join(words[index] for index in indices) == expected, name

        digits = "".join("%04d" % index for index in indices)
        assert len(digits) == (48 if entropy_len == 16 else 96) and digits.isdigit(), digits

        qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L, border=4)
        qr.add_data(digits)
        qr.make(fit=True)
        matrix = qr.get_matrix()

        prefix = os.path.join(args.outdir, name)
        write_png(prefix + ".png", matrix)
        write_gray(prefix + ".gray", matrix)
        print("%-6s %2d words, %2d digits, %d modules including the quiet zone"
              % (name, len(indices), len(digits), len(matrix)))


if __name__ == "__main__":
    main()
