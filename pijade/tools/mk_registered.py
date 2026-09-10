#!/usr/bin/env python3
"""Create a registered multisig wallet in the emulator to measure the registered-wallet
branch of the address explorer.

register_multisig requires THIS device to be a signer (main/signer.c:83 `return bFound`),
so existing repository test data carrying another seed's fingerprint is rejected. Build
registration with the device's own xpub instead.

Use only public test vectors: device seed is BIP39 `abandon` x11 + `about`, and the second
signer is the master xpub from BIP32 Test Vector 1.

Usage (in the container): python3 mk_registered.py <socket> [name]
Set MKREG_SHOTS=1 to capture a frame before every click (diagnostics).
"""
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jadectl

PUBLIC_TEST_MNEMONIC = ' '.join(['abandon'] * 11 + ['about'])
# BIP32 Test Vector 1, chain m. Public; stands in for the second signer.
FOREIGN_XPUB = ('xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1'
                'Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8')
# Fixed public 32 bytes, not a real wallet blinding key; registration checks only its length,
# without comparing it to its own key (main/multisig.h IS_VALID_BLINDING_KEY).
LIQUID_BLINDING_KEY = bytes(range(32))

H = 0x80000000
DERIVATION = [48 + H, 0 + H, 0 + H, 2 + H]  # conventional root for wsh(multi)
B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'


def b58decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    body = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return b'\x00' * (len(s) - len(s.lstrip('1'))) + body


def fingerprint_of(xpub):
    """First 4 bytes of hash160 of the serialized key (BIP32 definition)."""
    raw = b58decode(xpub)[:-4]  # discard checksum
    assert len(raw) == 78, len(raw)
    pubkey = raw[45:78]
    return hashlib.new('ripemd160', hashlib.sha256(pubkey).digest()).digest()[:4]


def wait_for_reply(j, rid, label, max_clicks=24):
    """Click through confirmation screens until a response arrives.

    Click count depends on screen count (summary -> one screen per signer -> final summary).
    Some screens advance automatically in CI mode; this loop handles both. A missing response
    is a measurement failure and must never silently count as a pass.
    """
    for i in range(max_clicks):
        if os.environ.get('MKREG_SHOTS'):
            # Confirmation screen order comes from upstream and may change, silently returning
            # "User declined" here. Frames show what happened on each screen.
            j.shot('mkreg_%s_%02d' % (label, i))
        r = j.try_recv_id(rid, timeout=0.6)
        if r is not None:
            return r
        try:
            # Selection is put back to a known place on every screen before clicking.  These
            # screens carry [reject/back] then [forward/confirm] then the detail rows, and the
            # selection does not wrap at either end (measured), so 'first' followed by one step
            # right always lands on the forward button.  Clicking wherever the selection happened
            # to be walks into the detail rows and back out again, and the run never finishes.
            j.btn('first', timeout=3.0)
            j.btn('right', timeout=3.0)
            j.btn('click', timeout=3.0)
        except (jadectl.RpcError, OSError) as exc:
            print('%s: click %d rejected: %s' % (label, i + 1, exc))
            break
        time.sleep(0.15)
    return j.try_recv_id(rid, timeout=3.0)


def main():
    sock = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else 'bbbms'
    j = jadectl.Jade(sock)

    rid = j.send('debug_set_mnemonic', {'mnemonic': PUBLIC_TEST_MNEMONIC})
    r = wait_for_reply(j, rid, 'seed')
    print('seed:', str(r)[:120])
    if not isinstance(r, dict) or not r.get('result'):
        return 1

    r = j.rpc('get_xpub', {'network': 'mainnet', 'path': []})
    master_xpub = r.get('result')
    print('master xpub:', str(master_xpub)[:24], '...')
    fp = fingerprint_of(master_xpub)
    print('fingerprint:', fp.hex().upper())

    r = j.rpc('get_xpub', {'network': 'mainnet', 'path': DERIVATION})
    our_xpub = r.get('result')
    print('our xpub:', str(our_xpub)[:24], '...')

    signers = [
        {'fingerprint': fp, 'derivation': DERIVATION, 'xpub': our_xpub, 'path': []},
        {'fingerprint': fingerprint_of(FOREIGN_XPUB), 'derivation': DERIVATION,
         'xpub': FOREIGN_XPUB, 'path': []},
    ]
    params = {'network': 'mainnet', 'multisig_name': name,
              'descriptor': {'variant': 'wsh(multi(k))', 'sorted': False,
                             'threshold': 1, 'signers': signers,
                             'master_blinding_key': None}}
    rid = j.send('register_multisig', params)
    r = wait_for_reply(j, rid, 'register')
    print('register_multisig:', str(r)[:200])
    ok = isinstance(r, dict) and r.get('result') is True

    # The same quorum again, as a descriptor.  Registering both lets the two branches of the
    # explorer be compared against each other: an identical quorum has to list identical addresses,
    # so a mistake in either branch shows up as a difference.
    def origin(fp, xpub):
        return "[%s/48'/0'/0'/2']%s" % (fp.hex(), xpub)

    params = {'network': 'mainnet', 'descriptor_name': name + 'd',
              'descriptor': 'wsh(multi(1,@0/**,@1/**))',
              'datavalues': {'@0': origin(fp, our_xpub),
                             '@1': origin(fingerprint_of(FOREIGN_XPUB), FOREIGN_XPUB)}}
    rid = j.send('register_descriptor', params)
    r = wait_for_reply(j, rid, 'descriptor')
    print('register_descriptor:', str(r)[:200])
    ok_desc = isinstance(r, dict) and r.get('result') is True

    # Five registrations register_multisig() has to refuse.  A stored record does not carry its
    # network, while the address explorer rebuilds one exact network from the device setting
    # (handle_address_explorer(), main/qrmode.c): unrestricted means mainnet, and only a
    # test-restricted device means testnet.  So the refusal is not about liquid alone - any
    # network other than the one that screen will assume would sit on disk indistinguishable and
    # be listed under the wrong one.  Testnet and localtest are tried here because this emulator
    # is unrestricted, which makes mainnet the only network it may store.  Both liquid shapes are
    # tried because the blinding key is optional for liquid as well - an unconfidential liquid
    # wallet carries none, so its presence cannot tell the two networks apart.  The last shape is
    # the mirror image: a bitcoin registration carrying a blinding key, which the file path would
    # otherwise hand straight through and leave on disk looking like liquid.
    ok_ref = True
    for net, suffix, blinding in (('liquid', 'l', LIQUID_BLINDING_KEY),
                                  ('liquid', 'lu', None),
                                  ('testnet', 't', None),
                                  ('localtest', 'lt', None),
                                  ('mainnet', 'bk', LIQUID_BLINDING_KEY)):
        params = {'network': net, 'multisig_name': name + suffix,
                  'descriptor': {'variant': 'wsh(multi(k))', 'sorted': False,
                                 'threshold': 1, 'signers': signers,
                                 'master_blinding_key': blinding}}
        rid = j.send('register_multisig', params)
        r = wait_for_reply(j, rid, 'ref' + suffix)
        refused = isinstance(r, dict) and r.get('result') is not True
        print('register_multisig (%s, blinding key %s): %s'
              % (net, 'present' if blinding else 'absent', str(r)[:160]))
        ok_ref = ok_ref and refused

    # And the same for a descriptor.  This one is the reason the refusal in register_descriptor()
    # is unconditional: upstream lets a CONFIG_DEBUG_MODE build through (descriptor_allow_liquid())
    # and this emulator is such a build (DEBUG_MODE=ON), so without that change the emulator would
    # accept what the device refuses - and the run below would prove nothing about what ships.
    params = {'network': 'liquid', 'descriptor_name': name + 'dl',
              'descriptor': 'wsh(multi(1,@0/**,@1/**))',
              'datavalues': {'@0': origin(fp, our_xpub),
                             '@1': origin(fingerprint_of(FOREIGN_XPUB), FOREIGN_XPUB)}}
    rid = j.send('register_descriptor', params)
    r = wait_for_reply(j, rid, 'refdl')
    print('register_descriptor (liquid):', str(r)[:160])
    ok_ref = ok_ref and isinstance(r, dict) and r.get('result') is not True

    print('RESULT: multisig %s, descriptor %s, expected refusals %s'
          % ('REGISTERED' if ok else 'NOT REGISTERED', 'REGISTERED' if ok_desc else 'NOT REGISTERED',
             'REJECTED' if ok_ref else 'NOT REJECTED'))
    return 0 if ok and ok_desc and ok_ref else 1


if __name__ == '__main__':
    sys.exit(main())
