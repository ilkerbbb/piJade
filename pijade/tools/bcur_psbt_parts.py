#!/usr/bin/env python3
"""Print the BC-UR frame texts of a PSBT, one per line.

Feed the output to bbqr_frames.py --parts-from to render them as emulator camera frames.  The
BBQr work needs these for two measurements that BBQr frames alone cannot make: that the BC-UR
path main/bcur.c already had still reads a PSBT (a regression check), and that a session holding
one format in progress ignores a frame of the other (main/bcur.c collect_any_bcur).

The encoder is the vendored one in pijade/tools/ur, the same library the other QR tools here use.
Fragment length is given in bytes of payload per part, before bytewords doubles it.

Usage (in the container):
    python3 /jade/pijade/tools/bcur_psbt_parts.py /jade/test_data/psbt_ss_p2wpkh.json > parts.txt
"""

import argparse
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cbor2  # noqa: E402

from ur.ur import UR  # noqa: E402
from ur.ur_encoder import UREncoder  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('psbt', help='test_data PSBT fixture; input.psbt is decoded from base64')
    parser.add_argument('--fragment', type=int, default=100, help='payload bytes per part')
    parser.add_argument('--parts', type=int, default=0,
                        help='stop after this many parts; 0 means one pass over the whole PSBT')
    args = parser.parse_args()

    with open(args.psbt, encoding='utf-8') as handle:
        raw = base64.b64decode(json.load(handle)['input']['psbt'])

    encoder = UREncoder(UR('crypto-psbt', bytearray(cbor2.dumps(raw))), args.fragment)
    # One pass is seq_len parts; past that the encoder keeps emitting fountain mixtures forever,
    # which a scanner can use but a fixed frame list does not need.
    wanted = args.parts if args.parts else len(encoder.fountain_encoder.fragments)
    for _ in range(wanted):
        print(encoder.next_part().upper())


if __name__ == '__main__':
    main()
