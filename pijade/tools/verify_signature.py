# BBB-AIRGAP: checks a signature the device produced WITHOUT trusting the device.  The public key
# is recovered from the signature itself and compared against the key derived from the BIP39 words,
# so the constant in t4148_help.sh is not just "whatever the device printed that day".
# Usage:  python3 pijade/tools/verify_signature.py <base64 signature> [path] [message]
# The words below are the public BIP39 test vector; a real seed never reaches this machine.
# No third-party package: the secp256k1 arithmetic is right here, which is also why it is slow
# enough to notice (a few seconds) and fast enough not to matter for one signature.
import hashlib, hmac, base64, sys

P  = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N  = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
GX = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
GY = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

def inv(a, m): return pow(a, m - 2, m)

def add(p1, p2):
    if p1 is None: return p2
    if p2 is None: return p1
    x1, y1 = p1; x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0: return None
    if p1 == p2: lam = (3 * x1 * x1) * inv(2 * y1, P) % P
    else:        lam = (y2 - y1) * inv(x2 - x1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)

def mul(k, p):
    r = None
    while k:
        if k & 1: r = add(r, p)
        p = add(p, p); k >>= 1
    return r

G = (GX, GY)

def ser(p):
    x, y = p
    return bytes([2 + (y & 1)]) + x.to_bytes(32, 'big')

def bip39_seed(mnemonic, passphrase=''):
    return hashlib.pbkdf2_hmac('sha512', mnemonic.encode(), ('mnemonic' + passphrase).encode(), 2048)

def ckd(key, chain, index):
    if index >= 0x80000000: data = b'\x00' + key + index.to_bytes(4, 'big')
    else:                   data = ser(mul(int.from_bytes(key, 'big'), G)) + index.to_bytes(4, 'big')
    h = hmac.new(chain, data, hashlib.sha512).digest()
    k = (int.from_bytes(h[:32], 'big') + int.from_bytes(key, 'big')) % N
    return k.to_bytes(32, 'big'), h[32:]

def derive(seed, path):
    h = hmac.new(b'Bitcoin seed', seed, hashlib.sha512).digest()
    key, chain = h[:32], h[32:]
    for part in path.split('/')[1:]:
        hard = part.endswith("'")
        idx = int(part.rstrip("'")) + (0x80000000 if hard else 0)
        key, chain = ckd(key, chain, idx)
    return key

def varint(n):
    if n < 0xfd: return bytes([n])
    if n <= 0xffff: return b'\xfd' + n.to_bytes(2, 'little')
    return b'\xfe' + n.to_bytes(4, 'little')

def msg_hash(msg):
    pre = b'\x18Bitcoin Signed Message:\n'
    body = pre + varint(len(msg)) + msg
    return hashlib.sha256(hashlib.sha256(body).digest()).digest()

def recover(sig_b64, h):
    # Without validate=True the decoder drops anything outside the alphabet, so '!' pasted in front
    # of a signature still decodes to that signature.  It never let an invalid signature through, but
    # a reader comparing what they pasted with what was checked deserves the error.
    raw = base64.b64decode(sig_b64, validate=True)
    if len(raw) != 65:
        raise ValueError('signature is %d bytes, not 65' % len(raw))
    header = raw[0]
    if not 27 <= header <= 34:
        raise ValueError('header byte %d outside 27-34' % header)
    recid = (header - 27) & 3
    compressed = (header - 27) >= 4
    r = int.from_bytes(raw[1:33], 'big'); s = int.from_bytes(raw[33:65], 'big')
    # Without these four checks the recovery is not a signature check at all.  A signature with
    # s = 0 makes sR the point at infinity, Q collapses to -r^-1*e*G, and an attacker who knows the
    # public key can pick r so that this equals it: measured, such a pair was accepted as valid.
    # ECDSA requires both scalars in [1, N-1], and the recovered R must be a real curve point.
    if not 1 <= r < N:
        raise ValueError('r outside [1, N-1]')
    if not 1 <= s < N:
        raise ValueError('s outside [1, N-1]')
    x = r + N * (recid >> 1)
    if x >= P:
        raise ValueError('recovered x is not a field element')
    y2 = (pow(x, 3, P) + 7) % P
    y = pow(y2, (P + 1) // 4, P)
    if (y * y - y2) % P != 0:
        raise ValueError('recovered x is not on the curve')
    if (y & 1) != (recid & 1): y = P - y
    R = (x, y)
    e = int.from_bytes(h, 'big')
    rinv = inv(r, N)
    Q = mul(rinv, add(mul(s, R), mul(N - e % N, G)))
    # sR - eG can cancel exactly (measured: header 31, r = Gx, s = e), leaving the point at
    # infinity.  Serialising that raises TypeError, so without this the caller gets a traceback
    # instead of a stated reason; the exit code is 1 either way, but a traceback reads as a crash.
    if Q is None:
        raise ValueError('recovered point is the point at infinity')
    return header, recid, compressed, Q

SEED_WORDS = ('abandon abandon abandon abandon abandon abandon '
              'abandon abandon abandon abandon abandon about')
# The path and the text default to the page's self-test vector; both can be given instead, which
# is how a signature made under another purpose (m/49', m/84') is checked without a second copy of
# the arithmetic above (ROADMAP item 69d, 2026-09-11).
if not 2 <= len(sys.argv) <= 4:
    print("usage: verify_signature.py <base64 signature> [path] [message]")
    sys.exit(2)
SIG = sys.argv[1]
PATH = sys.argv[2] if len(sys.argv) > 2 else "m/44'/0'/0'/0/0"
MESSAGE = (sys.argv[3] if len(sys.argv) > 3 else 'hello').encode()

key = derive(bip39_seed(SEED_WORDS), PATH)
pub = ser(mul(int.from_bytes(key, 'big'), G))
h = msg_hash(MESSAGE)
try:
    header, recid, compressed, Q = recover(SIG, h)
except ValueError as e:
    # A malformed signature is a failed check, not a crash: print the reason and leave with 1 so a
    # caller reading the exit code cannot mistake a traceback for a pass.
    print('REJECTED        :', e)
    sys.exit(1)
rec = ser(Q)
print('path            :', PATH)
print('message         :', MESSAGE.decode())
print('derived pubkey  :', pub.hex())
print('recovered pubkey:', rec.hex())
print('header byte     : %d (recid %d, compressed %s)' % (header, recid, compressed))
ok = (pub == rec) and compressed and 31 <= header <= 34
print('MATCH           :', ok)
sys.exit(0 if ok else 1)
