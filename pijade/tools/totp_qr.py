#!/usr/bin/env python3
"""TOTP registration QR and INDEPENDENT code generator (for T8 acceptance).

A reference is needed to check the code on the Jade TOTP screen. This tool independently
implements RFC 6238 with the stdlib; it shares no code with main/otpauth.c.

The generated QR is a plain otpauth:// URI (not UR); scan it through
Settings > OTP > New OTP Record > Scan QR on the device.

SECURITY: the secret here is a FIXED TEST value and must stay that way. Do not supply a
real TOTP secret to this tool; the experimental device is not used with real secrets.

Usage:
  totp_qr.py qr <output.png>        write the otpauth QR
  totp_qr.py code [--at EPOCH]      print the 6-digit code for now (or the given epoch)
"""
import argparse
import base64
import hashlib
import hmac
import struct
import sys
import time

# Fixed test secret: "JBSWY3DPEHPK3PXP" is a common example, unrelated to any real account.
TEST_SECRET_B32 = "JBSWY3DPEHPK3PXP"
LABEL = "piJade:T8"
ISSUER = "piJade"
PERIOD = 30
DIGITS = 6


def uri(secret_b32):
    return (f"otpauth://totp/{LABEL}?secret={secret_b32}&issuer={ISSUER}"
            f"&algorithm=SHA1&digits={DIGITS}&period={PERIOD}")


def totp(secret_b32, at):
    """RFC 6238, SHA1, 6 digits, 30 s. Independent of the Jade implementation."""
    key = base64.b32decode(secret_b32, casefold=True)
    counter = int(at) // PERIOD
    mac = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
    offset = mac[-1] & 0x0F
    code = struct.unpack(">I", mac[offset:offset + 4])[0] & 0x7FFFFFFF
    return str(code % (10 ** DIGITS)).zfill(DIGITS)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_qr = sub.add_parser("qr", help="write otpauth QR as PNG")
    p_qr.add_argument("png_output")

    p_code = sub.add_parser("code", help="print reference code")
    p_code.add_argument("--at", type=int, default=None,
                        help="epoch seconds; defaults to current real time")

    args = ap.parse_args()
    secret = TEST_SECRET_B32

    if args.cmd == "qr":
        # Reuse the PNG writer from epoch_qr.py so both tools produce the same format without
        # duplicating code (this also avoids a PIL dependency).
        import qrcode
        from qrcode.constants import ERROR_CORRECT_L
        from epoch_qr import write_png

        u = uri(secret)
        qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L)
        qr.add_data(u)
        qr.make(fit=True)
        write_png(args.png_output, qr.get_matrix())
        print(f"written: {args.png_output}")
        print(f"uri: {u}")
        return 0

    at = args.at if args.at is not None else int(time.time())
    remaining = PERIOD - (at % PERIOD)
    print(f"epoch: {at}")
    print(f"utc:   {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(at))}")
    print(f"code:   {totp(secret, at)}")
    print(f"this code is valid for {remaining} more seconds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
