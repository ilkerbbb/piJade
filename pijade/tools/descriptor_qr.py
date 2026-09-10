#!/usr/bin/env python3
"""Descriptor QR fixtures (plain text, Specter JSON, UR:CRYPTO-OUTPUT, UR:BYTES multisig).

Usage: descriptor_qr.py [--fragment 60] [--hex] [--text] <output_dir>
Dependencies: cbor2==6.1.2 and qrcode, in a virtualenv of your own.
Public test keys only: device A (abandon x11 + about) m/48h/0h/0h/2h and BIP32 TV1 m/0H/1/2H/2.
"""
import argparse
import hashlib
import json
import os
import sys

import cbor2
import qrcode
from qrcode.constants import ERROR_CORRECT_L

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from epoch_qr import bytewords_minimal, write_gray, write_png  # noqa: E402

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
H = 0x80000000

XPUB_A = ("xpub6DkFAXWQ2dHxq2vatrt9qyA3bXYU4ToWQwCHbf5XB2mSTexcHZCeKS1VZYcPoBd5X8yVcbXFHJR9R8UCVpt82VX"
          "1VhR28mCyxUFL4r6KFrf")
ORIGIN_A = ("73c5da0a", [48 + H, 0 + H, 0 + H, 2 + H])
XPUB_F = ("xpub6FHa3pjLCk84BayeJxFW2SP4XRrFd1JYnxeLeU8EqN3vDfZmbqBqaGJAyiLjTAwm6ZLRQUMv1ZACTj37sR62cfN7"
          "fe5JnJ7dh8zL4fiyLHV")
ORIGIN_F = ("3442193e", [0 + H, 1, 2 + H, 2])
MSFILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "msfile_vault.txt")


def b58decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    pad = len(s) - len(s.lstrip("1"))
    return b"\x00" * pad + raw


def xpub_fields(xpub):
    raw = b58decode(xpub)[:-4]
    assert len(raw) == 78, len(raw)
    return {"version": int.from_bytes(raw[0:4], "big"), "depth": raw[4], "parent_fp": int.from_bytes(raw[5:9], "big"),
            "child": int.from_bytes(raw[9:13], "big"), "chain_code": raw[13:45], "key": raw[45:78]}


def path_str(path, hard="h"):
    return "/".join(("%d%s" % (p - H, hard)) if p >= H else str(p) for p in path)


def key_text(origin, xpub, child="/<0;1>/*", hard="h"):
    return "[%s/%s]%s%s" % (origin[0], path_str(origin[1], hard), xpub, child)


# BIP380 descriptor checksum (added by Sparrow and Specter; stripped by the device)
INPUT_CHARSET = ("0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
                 "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ")
CHECKSUM_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def descsum_polymod(symbols):
    chk = 1
    for value in symbols:
        top = chk >> 35
        chk = (chk & 0x7ffffffff) << 5 ^ value
        for i in range(5):
            chk ^= [0xf5dee51989, 0xa9fdca3312, 0x1bab10e32d, 0x3706b1677a, 0x644d626ffd][i] if ((top >> i) & 1) else 0
    return chk


def descsum_create(s):
    symbols = []
    groups = []
    for c in s:
        v = INPUT_CHARSET.index(c)
        symbols.append(v & 31)
        groups.append(v >> 5)
        if len(groups) == 3:
            symbols.append(groups[0] * 9 + groups[1] * 3 + groups[2])
            groups = []
    if len(groups) == 1:
        symbols.append(groups[0])
    elif len(groups) == 2:
        symbols.append(groups[0] * 3 + groups[1])
    checksum = descsum_polymod(symbols + [0, 0, 0, 0, 0, 0, 0, 0]) ^ 1
    return s + "#" + "".join(CHECKSUM_CHARSET[(checksum >> (5 * (7 - i))) & 31] for i in range(8))


def hdkey(origin, xpub, children=None):
    f = xpub_fields(xpub)
    comps = []
    for p in origin[1]:
        comps += [p - H if p >= H else p, p >= H]
    key = {3: f["key"], 4: f["chain_code"],
           5: cbor2.CBORTag(305, {1: 0, 2: 0}),  # use-info: bitcoin mainnet (Sparrow always writes it)
           6: cbor2.CBORTag(304, {1: comps, 2: int(origin[0], 16), 3: len(origin[1])}),
           8: f["parent_fp"]}
    if children is not None:
        key[7] = cbor2.CBORTag(304, {1: children})
    return cbor2.CBORTag(303, key)


def crypto_output(threshold, keys, children=None):
    hdkeys = [hdkey(o, x, children) for (o, x) in keys]
    return cbor2.dumps(cbor2.CBORTag(401, cbor2.CBORTag(407, {1: threshold, 2: hdkeys})))


def qr_matrix(text):
    qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L, border=4)
    qr.add_data(text)
    qr.make(fit=True)
    return qr.get_matrix()


def write_frame(prefix, text):
    matrix = qr_matrix(text)
    write_png(prefix + ".png", matrix)
    write_gray(prefix + ".gray", matrix)
    print("%s: QR modules %d, %d characters" % (os.path.basename(prefix), len(matrix), len(text)))


def multi_part(cbor, ur_type, prefix, fragment):
    from ur.ur import UR
    from ur.ur_encoder import UREncoder
    encoder = UREncoder(UR(ur_type, bytearray(cbor)), fragment)
    pure = -(-len(cbor) // fragment)
    for index in range(2 * pure):
        part = encoder.next_part()
        matrix = qr_matrix(part.upper())
        write_gray("%s-%02d.gray" % (prefix, index), matrix)
    print("%s: %d parts (pure %d)" % (os.path.basename(prefix), 2 * pure, pure))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fragment", type=int, default=60)
    parser.add_argument("--hex", action="store_true", help="print CBOR hex (C selfcheck vector)")
    parser.add_argument("--text", action="store_true", help="print expected canonical text")
    parser.add_argument("outdir")
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    out = lambda name: os.path.join(args.outdir, name)  # noqa: E731

    keys = [(ORIGIN_A, XPUB_A), (ORIGIN_F, XPUB_F)]
    canonical = "wsh(sortedmulti(1,%s,%s))" % (key_text(*keys[0]), key_text(*keys[1]))
    sparrow_text = descsum_create(canonical)
    specter_desc = "wsh(sortedmulti(1,%s,%s))" % (
        key_text(*keys[0], child="/{0,1}/*", hard="'"), key_text(*keys[1], child="/{0,1}/*", hard="'"))
    specter_json = json.dumps({"label": "Strongbox", "blockheight": 0, "descriptor": descsum_create(specter_desc)})

    single = crypto_output(1, keys)
    children = crypto_output(1, keys, children=[[0, False, 1, False], [], False])
    bad = crypto_output(1, keys, children=[[0, 5], False, [], False])  # range component: reject

    if args.text:
        # canonical text + device-generated hash name (script || @i || value, in order)
        script = "wsh(sortedmulti(1,@0/<0;1>/*,@1/<0;1>/*))"
        values = [("@0", key_text(*keys[0], child="")), ("@1", key_text(*keys[1], child=""))]
        digest = hashlib.sha256((script + "".join(k + v for k, v in values)).encode()).hexdigest()
        print(canonical)
        print("name: desc-" + digest[:8])
    if args.hex:
        print("single:", single.hex())
        print("children:", children.hex())
        print("bad:", bad.hex())

    write_frame(out("desc_text"), sparrow_text)
    write_frame(out("desc_specter"), specter_json)
    write_frame(out("desc_ur_single"), "ur:crypto-output/" + bytewords_minimal(single).upper())
    multi_part(single, "crypto-output", out("desc_ur"), args.fragment)
    write_frame(out("desc_ur_children"), "ur:crypto-output/" + bytewords_minimal(children).upper())
    write_frame(out("desc_ur_bad"), "ur:crypto-output/" + bytewords_minimal(bad).upper())
    with open(MSFILE, "rb") as f:
        multi_part(cbor2.dumps(f.read()), "bytes", out("msfile_ur"), args.fragment)


if __name__ == "__main__":
    main()
