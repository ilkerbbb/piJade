"""Mac (~/.venvs/pijade) or container: verify jade-mine-reply using <tag>_parts.txt
(UR parts decoded from the display) and <template>.out.
Usage: python3 m3_verify_reply.py <tag> <template> [directory]; directory holds parts.txt and .out
files (default .)."""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbor2
from ur.ur_decoder import URDecoder
S = sys.argv[3] if len(sys.argv) > 3 else "."
tag, tpl = sys.argv[1], sys.argv[2]
parts = [l.strip() for l in open(f"{S}/{tag}_parts.txt") if l.strip()]
dec = URDecoder()
for p in parts:
    dec.receive_part(p.lower())
    if dec.is_success():
        break
print("part count:", len(parts), "| decode succeeded:", bool(dec.is_success()),
      "| type:", dec.result.type if dec.is_success() else "-")
if not dec.is_success():
    sys.exit(1)
reply = cbor2.loads(bytes(dec.result.cbor))
req = cbor2.loads(bytes.fromhex(open(f"{S}/{tpl}.out").read().split("cbor_hex=")[1].split("\n")[0]))
print("reply keys:", sorted(reply.keys()), "| id equal:", reply.get("id") == req["id"], repr(reply.get("id")))
res = reply["result"]; header = res[:80]
print("result bytes:", len(res), "| header 80 + txcount", res[80], "+ rawtx", len(res) - 81)
version, = struct.unpack("<I", header[0:4]); prev = header[4:36]; ts, bits, nonce = struct.unpack("<III", header[68:80])
P = req["params"]
print("version equal:", version == P["version"], hex(version), "| bits equal:", bits == P["bits"], hex(bits),
      "| curtime equal:", ts == P["curtime"])
print("prevhash header==tpl:", prev == P["previousblockhash"], "| reversed:", prev == P["previousblockhash"][::-1])
h = hashlib.sha256(hashlib.sha256(header).digest()).digest()
T = int.from_bytes(P["target"], "big")
print("sha256d reversed big-endian <= target:", int.from_bytes(h[::-1], "big") <= T,
      "| forward big-endian <= target:", int.from_bytes(h, "big") <= T)
print("nonce:", nonce, "| block hash:", h[::-1].hex())
