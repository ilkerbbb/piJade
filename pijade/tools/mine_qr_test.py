#!/usr/bin/env python3
"""mine_qr.py unit test. Run: ~/.venvs/pijade/bin/python pijade/tools/mine_qr_test.py"""
import glob
import os
import subprocess
import sys
import tempfile

import cbor2

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mine_qr.py")


def bits_to_target(bits):
    mantissa, exponent = bits & 0x007FFFFF, bits >> 24
    target = bytearray(32)
    offset = 32 - exponent
    target[offset : offset + 3] = mantissa.to_bytes(3, "big")
    return bytes(target)


def test_single_part_cbor():
    out = os.path.join(tempfile.mkdtemp(), "t")
    subprocess.run([sys.executable, TOOL, "--bits", "0x207fffff", "--height", "1", out], check=True)
    lines = open(out + ".out").read().splitlines()
    cbor_hex = [l for l in lines if l.startswith("cbor_hex=")][0].split("=", 1)[1]
    msg = cbor2.loads(bytes.fromhex(cbor_hex))
    assert msg["method"] == "mine" and msg["id"] == "1"
    p = msg["params"]
    assert p["bits"] == 0x207FFFFF and p["target"] == bits_to_target(0x207FFFFF)
    assert p["version"] == 0x20000000 and p["height"] == 1 and p["curtime"] > 0
    assert len(p["previousblockhash"]) == 32
    assert p["address"] == "tb1q0ht9tyks4vh7p5p904t340cr9nvahy7um9zdem"
    assert [l for l in lines if l.startswith("ur=ur:jade-mine/")]
    assert os.path.getsize(out + ".gray") == 320 * 240
    assert os.path.getsize(out + ".png") > 0


def test_multi_part_roundtrip():
    from ur.ur_decoder import URDecoder

    out = os.path.join(tempfile.mkdtemp(), "m")
    subprocess.run([sys.executable, TOOL, "--parts", out], check=True)
    lines = open(out + ".out").read().splitlines()
    cbor_hex = [l for l in lines if l.startswith("cbor_hex=")][0].split("=", 1)[1]
    parts = [l.split("=", 1)[1] for l in lines if l.startswith("part=")]
    assert len(parts) == 2 * -(-len(bytes.fromhex(cbor_hex)) // 60)
    for index in range(len(parts)):
        assert os.path.getsize("%s-%02d.gray" % (out, index)) == 320 * 240
    decoder = URDecoder()
    for part in parts:
        decoder.receive_part(part)
    assert decoder.is_success() and decoder.result.type == "jade-mine"
    assert decoder.result.cbor == bytes.fromhex(cbor_hex)
    assert cbor2.loads(decoder.result.cbor)["method"] == "mine"


def test_bad_args_rejected():
    out = os.path.join(tempfile.mkdtemp(), "b")
    for extra in (["--bits", "0x00800000"], ["--prevhash", "00" * 31], ["--parts", "--fragment", "9"]):
        r = subprocess.run([sys.executable, TOOL] + extra + [out], capture_output=True)
        assert r.returncode != 0 and not os.path.exists(out + ".out"), extra


def test_camfiles_pushes_in_order():
    import jadectl

    out = os.path.join(tempfile.mkdtemp(), "c")
    subprocess.run([sys.executable, TOOL, "--parts", out], check=True)
    frames = sorted(glob.glob(out + "-[0-9][0-9].gray"))
    assert len(frames) >= 6
    j = jadectl.Jade.__new__(jadectl.Jade)  # no socket; rpc is mocked
    calls = []
    j.rpc = lambda method, params=None, timeout=None: calls.append((method, params))
    jadectl.CAM_DELAY = 0
    assert j.camfiles(out + ":2") == 2 * len(frames)
    expected = [open(p, "rb").read() for p in frames] * 2
    assert [c[1]["bytes"] for c in calls] == expected
    assert all(c[0] == "libjade_request" and c[1]["request"] == "set_camera_bytes" for c in calls)
    for bad in (out + ":0", os.path.join(tempfile.mkdtemp(), "missing")):
        try:
            j.camfiles(bad)
            raise AssertionError("accepted: %s" % bad)
        except jadectl.RpcError:
            pass


if __name__ == "__main__":
    test_single_part_cbor()
    test_multi_part_roundtrip()
    test_bad_args_rejected()
    test_camfiles_pushes_in_order()
    print("mine_qr_test: PASS")
