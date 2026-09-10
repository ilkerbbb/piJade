"""T3 boundary matrix: a single-part ur:jade-mine QR frame (.gray) and expected log line for each case.
Usage (Mac, ~/.venvs/pijade; requires qrcode + cbor2): python3 m3_matrix.py <output directory>;
mount the directory at /probe in the container, then run sh /jade/pijade/tools/m3_matrix.sh there."""
import os, sys, time, cbor2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mine_qr
S = sys.argv[1] if len(sys.argv) > 1 else "."
REJ = "Mining template rejected at parse"
ACC = "Mining template accepted"
PARSE = "Failed to parse mining template"
base = dict(version=0x20000000, previousblockhash=b"\0" * 32, target=mine_qr.bits_to_target(0x207FFFFF),
            curtime=int(time.time()), bits=0x207FFFFF, height=1, address=mine_qr.DEFAULT_ADDRESS)
def p(**kw):
    d = dict(base); d.update(kw); return d
cases = [
    ("a25", p(address="1" + "a" * 24), REJ),
    ("a26", p(address="1" + "a" * 25), ACC),
    ("a90", p(address="tb1" + "q" * 87), ACC),
    ("a91", p(address="tb1" + "q" * 88), REJ),
    ("h31", p(previousblockhash=b"\0" * 31), REJ),
    ("h33", p(previousblockhash=b"\0" * 33), REJ),
    ("t31", p(target=b"\xff" * 31), REJ),
    ("t33", p(target=b"\xff" * 33), REJ),
    ("height0", p(height=0), REJ),
    ("curtime0", p(curtime=0), REJ),
    ("u32max", p(height=0xFFFFFFFF), ACC),
    ("u32over", p(height=0x100000000), REJ),
    ("negver", p(version=-1), REJ),
    ("missing", {k: v for k, v in base.items() if k != "bits"}, REJ),
    ("badtype", p(bits="abc"), REJ),
]
msgs = [(n, cbor2.dumps({"id": "1", "method": "mine", "params": pr}), exp) for n, pr, exp in cases]
msgs.append(("method", cbor2.dumps({"id": "1", "method": "mine2", "params": base}), PARSE))
msgs.append(("notmap", cbor2.dumps({"id": "1", "method": "mine", "params": 5}), PARSE))
with open(S + "/m3_matrix.txt", "w") as f:
    for name, cbor, exp in msgs:
        mine_qr.single_part(cbor, S + "/mx_" + name)
        ver = open(S + "/mx_" + name + ".out").read().split("qr_version=")[1].strip()
        f.write("%s|%s\n" % (name, exp))
        print("%-9s cbor=%3d qr_v=%s expected=%s" % (name, len(cbor), ver, exp))
