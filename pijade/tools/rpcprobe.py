#!/usr/bin/env python3
"""Verify that the debug surface is disabled in the production build.

libjade used to force CONFIG_DEBUG_MODE to 1 in sdkconfig.h, exposing both the libjade RPC
surface and Jade debug messages even in Release builds. A rebase restoring the upstream
line would compile and pass most tests while silently unlocking it. Run this tool before
using an updated fork (pijade/UPSTREAM.md, 5.6).

A caveat: an INVENTED method gets the same rejection in production (measured:
zzz_not_a_real_method -> -32002 hardware locked). A misspelled probe could silently pass.
Verify probe names against main/process/dashboard.c; --control proves they are dispatched.

A visible library symbol is not evidence: process_libjade_request is defined without a guard,
but its main/wire.c call is guarded. The proof is a live call.

Usage (with the daemon running):
    python3 pijade/tools/rpcprobe.py [--control] [--socket PATH]

    default      expect --no-debug build; every method must be rejected
    --control    expect default build; none should be rejected. Probes interfere with each
                 other (one waits on confirmation, another triggers JADE_ASSERT), so use
                 --only to run them individually with a fresh daemon each time.

Exit 0 only when all probes return the expected result. Every deviation is an error so a
missing method name or stopped daemon cannot silently count as a pass.
"""
import argparse
import socket
import sys

import cbor2

# Text of the rejection: main/process/dashboard.c, message on a locked/uninitialized device.
LOCKED_MARKER = "hardware locked or uninitialized"

# Public BIP39 test vector. Never write a real mnemonic to this repository or this machine.
PUBLIC_TEST_MNEMONIC = " ".join(["abandon"] * 11 + ["about"])

# Compressed secp256k1 generator point. The handler requires 33 bytes
# (get_bip85_entropy.c: pubkey_len != EC_PUBLIC_KEY_LEN -> BAD_PARAMETERS);
# its content is irrelevant; a valid length suffices.
VALID_PUBKEY = bytes.fromhex(
    "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798")

# Methods handled BEFORE "Methods only available after user authorised" in
# main/process/dashboard.c; the device lock does not stop these.
#
# debug_capture_image_data is excluded: it also has a CONFIG_RETURN_CAMERA_IMAGES guard
# (dashboard.c:547), which our configuration does not define. Rejection in the default build
# too would prove nothing about the lock; including it would give false confidence.
PROBES = [
    ("ping", {}),
    ("libjade_request", {"request": "get_display_size"}),
    ("libjade_request", {"request": "get_nvs"}),
    ("debug_selfcheck", {}),
    ("debug_handshake", {"id": "probe"}),
    ("debug_scan_qr", {}),
    ("get_bip85_bip39_entropy", {"num_words": 12, "index": 0, "pubkey": VALID_PUBKEY}),
    ("get_bip85_rsa_entropy", {"key_bits": 2048, "index": 0, "pubkey": VALID_PUBKEY}),
    ("debug_set_mnemonic", {"mnemonic": PUBLIC_TEST_MNEMONIC, "temporary_wallet": True}),
    # Last because it clears the keychain.
    ("debug_clean_reset", {}),
]


def call(socket_path, method, params, timeout):
    """Open a fresh connection for one method and collect the full response."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(socket_path)
        sock.sendall(cbor2.dumps({"id": "probe", "method": method, "params": params}))
        buf = b""
        while True:
            chunk = sock.recv(262144)
            if not chunk:
                return "CONNECTION CLOSED", None
            buf += chunk
            try:
                return None, cbor2.loads(buf)
            except Exception:
                continue  # CBOR is incomplete; keep reading
    except socket.timeout:
        return "TIMEOUT", None
    except OSError as err:
        return "SOCKET ERROR: %s" % err, None
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default="/tmp/j.sock")
    parser.add_argument("--control", action="store_true",
                        help="verify default build: no method should be rejected")
    parser.add_argument("--only", help="try only this method (for the control group: "
                                       "each probe requires a fresh daemon)")
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    expect_locked = not args.control
    probes = [p for p in PROBES if not args.only or p[0] == args.only or
              (p[0] == "libjade_request" and p[1].get("request") == args.only)]
    if not probes:
        print("Unknown probe: %s" % args.only)
        return 2

    failures = []
    for method, params in probes:
        label = method if method != "libjade_request" else "libjade/%s" % params["request"]
        err, reply = call(args.socket, method, params, args.timeout)

        if err:
            # No response is a measurement failure, even in the control group. Usually an earlier probe
            # left the device on a confirmation screen or triggered JADE_ASSERT. Restart the daemon and
            # run probes individually with --only.
            print("%-28s %s  <-- COULD NOT MEASURE" % (label, err))
            failures.append(label)
            continue

        message = ""
        if isinstance(reply, dict) and "error" in reply:
            message = str(reply["error"].get("message", ""))
        locked = LOCKED_MARKER in message

        if method == "ping":
            # Must respond in both builds; otherwise the daemon is dead and all rejections are spurious.
            print("%-28s %s (daemon alive)" % (label, "REJECTED" if locked else "ACCEPTED"))
            if locked:
                failures.append(label)
            continue

        # The only criterion is lock rejection. In the control group, the handler result (success,
        # invalid parameters, keychain error) does not matter; the message must pass the lock check
        # and reach the handler.
        ok = locked if expect_locked else not locked
        detail = message if message else str(reply.get("result"))[:60]
        print("%-28s %-5s %s%s" % (label, "REJECTED" if locked else "REACHED", detail[:70],
                                   "" if ok else "  <-- UNEXPECTED"))
        if not ok:
            failures.append(label)

    mode = "control group (default build)" if args.control else "production build (--no-debug)"
    if failures:
        print("\nFAILED [%s]: %s" % (mode, ", ".join(failures)))
        return 1
    print("\nPASS [%s]: all %d probes behaved as expected." % (mode, len(probes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
