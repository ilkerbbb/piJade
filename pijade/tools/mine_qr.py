#!/usr/bin/env python3
"""Generate a ur:jade-mine template QR for Jade (main/qrmode.c handle_mining_qr, mining M3).

The body is the CBOR map of the upstream `mine` RPC (e1bee5a1 jadepy/jade.py); the companion
port uses the same dictionary. Defaults are selfcheck vectors (libjade/selfcheck/mining.c):
bits 0x207fffff (target 0x7fffff00..., about half the hashes pass; libjade/selfcheck/mining.c:26),
address tb1q0ht9tyks4vh7p5p904t340cr9nvahy7um9zdem. Numeric fields are not validated (device negative tests).

Usage: mine_qr.py [--bits HEX] [--height N] [--address A] [--prevhash HEX64] [--curtime N]
                     [--parts] [--fragment 60] <prefix>
Single part: <prefix>.png/.gray/.out (ur=, cbor_hex=, qr_version=).
--parts: <prefix>-NN.gray + <prefix>.out (cbor_hex= and part= / qr_version= for each part).
Multipart encoder: pijade/tools/ur (foundation-ur-py a371f6355fb3, see ur/VENDORED.md).
Addresses are not validated: negative device tests deliberately generate invalid templates.
"""
import argparse
import glob
import os
import time

import cbor2
import qrcode
from qrcode.constants import ERROR_CORRECT_L

from epoch_qr import bytewords_minimal, write_gray, write_png

DEFAULT_ADDRESS = "tb1q0ht9tyks4vh7p5p904t340cr9nvahy7um9zdem"


def bits_to_target(bits):
    # Same rules as components/miner/miner.c:855-868
    mantissa, exponent = bits & 0x007FFFFF, bits >> 24
    if bits & 0x00800000 or not mantissa or exponent < 3 or exponent > 32:
        raise SystemExit("invalid bits (same rules as miner.c bits_to_target)")
    target = bytearray(32)
    offset = 32 - exponent
    target[offset : offset + 3] = mantissa.to_bytes(3, "big")
    return bytes(target)


def build_cbor(args):
    prevhash = bytes.fromhex(args.prevhash)
    if len(prevhash) != 32:
        raise SystemExit("--prevhash must be 32 bytes (64 hex)")
    params = {
        "version": 0x20000000,
        "previousblockhash": prevhash,
        "target": bits_to_target(args.bits),
        "curtime": args.curtime,
        "bits": args.bits,
        "height": args.height,
        "address": args.address,
    }
    return cbor2.dumps({"id": "1", "method": "mine", "params": params})


def qr_matrix(text):
    qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L)
    qr.add_data(text.upper())
    qr.make(fit=True)
    matrix = qr.get_matrix()
    # Same two guards as epoch_qr.py:136-139; the Jade scanner expects a quiet zone
    if qr.border != 4:
        raise RuntimeError("qrcode default quiet zone is not 4 modules")
    if len(matrix) != qr.modules_count + 2 * qr.border:
        raise RuntimeError("qrcode matrix does not include the expected quiet zone")
    return matrix, qr.version


def single_part(cbor, prefix):
    # bytewords_minimal appends CRC32 itself (epoch_qr.py:55-56); do not add a CRC here
    ur = "ur:jade-mine/" + bytewords_minimal(cbor)
    matrix, version = qr_matrix(ur)
    write_png(prefix + ".png", matrix)
    write_gray(prefix + ".gray", matrix)
    with open(prefix + ".out", "w") as out:
        out.write("ur=%s\ncbor_hex=%s\nqr_version=%d\n" % (ur, cbor.hex(), version))


def multi_part(cbor, prefix, fragment):
    from ur.ur import UR
    from ur.ur_encoder import UREncoder

    # The library requires bytearray: partition_message pads the last part with .append
    # (ur/fountain_encoder.py:112-120); passing bytes raises AttributeError. The lower bound 10
    # is its min_fragment_len; parts larger than the body produce single-part UR, making --parts moot.
    if fragment < 10 or fragment >= len(cbor):
        raise SystemExit("--fragment must be between 10 and the body length (%d)" % len(cbor))
    pure = -(-len(cbor) // fragment)  # pure part count; the same number of fountain parts
    if 2 * pure > 99:
        raise SystemExit("part count exceeds 99; camfiles reads only -NN.gray names")
    for stale in glob.glob(prefix + "-[0-9][0-9].gray"):
        os.remove(stale)  # keep frames from previous runs out of camfiles
    encoder = UREncoder(UR("jade-mine", bytearray(cbor)), fragment)
    with open(prefix + ".out", "w") as out:
        out.write("cbor_hex=%s\n" % cbor.hex())
        for index in range(2 * pure):
            part = encoder.next_part()
            matrix, version = qr_matrix(part)
            write_gray("%s-%02d.gray" % (prefix, index), matrix)
            out.write("part=%s\nqr_version=%d\n" % (part, version))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bits", type=lambda s: int(s, 0), default=0x207FFFFF)
    parser.add_argument("--height", type=int, default=1)
    parser.add_argument("--address", default=DEFAULT_ADDRESS)
    parser.add_argument("--prevhash", default="00" * 32)
    parser.add_argument("--curtime", type=int, default=int(time.time()))
    parser.add_argument("--parts", action="store_true")
    parser.add_argument("--fragment", type=int, default=60)
    parser.add_argument("prefix")
    args = parser.parse_args()
    cbor = build_cbor(args)
    if args.parts:
        multi_part(cbor, args.prefix, args.fragment)
    else:
        single_part(cbor, args.prefix)


if __name__ == "__main__":
    main()
