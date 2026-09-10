#!/usr/bin/env python3
"""Positive and negative controls for t33e_settings_entries.py.

Two tasks:
  1. CONSTANT MIRROR: reread the original C constants and compare them with the tool's copies.
     A manually copied constant drifting from its source would silently corrupt measurement.
  2. FIXTURE RUN: generate synthetic settings files and verify the CORRECT exit code for each
     case. Never touch real card files; use a temporary directory and a fixed random seed.

Some fixtures came from Codex gate findings and remain as regression controls:
  r1: H (unsealed BINARY record, no ASCII), G (OTP only), F (bad inner digest)
  r2: L (unsealed record in old slot), M (bytes appended to slot), N (device-rejected ns)
  r3: P/R/T/U (partial stream, recovered entries and result precedence), V/Y (coarse raw scan)

Run:   python3 pijade/tools/t33e_selftest.py
Exit:  0 all checks pass, 1 at least one failed
"""
import binascii
import hashlib
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "t33e_settings_entries.py")
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RND = random.Random(20260909)


def read(relative):
    with open(os.path.join(ROOT, relative), encoding="utf-8") as f:
        return f.read()


# ------------------------------------------------------------------ 1. constant mirror
def tool_constants():
    """Read the tool's module-level constants FROM SOURCE.

    Avoid importlib: Python caches bytecode in __pycache__ by mtime and size. A same-size edit
    within one second can leave a STALE CACHE, making the mirror read the old value instead
    of the correct value on disk (happened on 2026-09-09: drift was still reported after the
    file was fixed). Parsing with ast avoids execution and eliminates this failure class."""
    import ast
    with open(TOOL, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    constants = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name):
            try:
                constants[node.targets[0].id] = ast.literal_eval(node.value)
            except ValueError:
                pass
    return constants


def measure_constants():
    m = tool_constants()

    error = []
    settings = read("libjade/pijade_settings.c")
    store = read("pijade/host/settings_store.c")

    def equal(name, expected, actual):
        if expected != actual:
            error.append("%s: source %r, tool %r" % (name, expected, actual))

    # PERSISTED_FIELDS
    body = settings.split("PERSISTED_FIELDS[] = {", 1)[1].split("\n};", 1)[0]
    fields = {}
    for k, mn, mx, nul in re.findall(
            r'\{\s*"([A-Za-z]+)",\s*(\d+),\s*(\d+),\s*(true|false)\s*\}', body):
        fields[k] = (int(mn), int(mx), nul == "true")
    equal("PERSISTED_FIELDS", fields, m["PERSISTED_FIELDS"])

    # PERSISTED_NAMESPACES (ordered ns 1..4)
    body = settings.split("PERSISTED_NAMESPACES[] = {", 1)[1].split("\n};", 1)[0]
    ns = {}
    for i, quad in enumerate(re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", body), 1):
        ns[i] = tuple(int(x) for x in quad)
    equal("PERSISTED_NAMESPACES", ns, m["PERSISTED_NAMESPACES"])

    # Individual constants
    magic = "".join(re.findall(r"'(.)'", settings.split("SETTINGS_MAGIC[8] = {", 1)[1].split("}", 1)[0]))
    equal("PAYLOAD_MAGIC", magic.encode(), m["PAYLOAD_MAGIC"])
    equal("PAYLOAD_DIGEST_LEN",
         int(re.search(r"#define SETTINGS_DIGEST_LEN (\d+)", settings).group(1)), m["PAYLOAD_DIGEST_LEN"])
    smagic = "".join(re.findall(r"'(.)'", store.split("SLOT_MAGIC[8] = {", 1)[1].split("}", 1)[0]))
    equal("SLOT_MAGIC", smagic.encode(), m["SLOT_MAGIC"])
    equal("SLOT_HEADER_LEN",
         int(re.search(r"#define SLOT_HEADER_LEN (\d+)", store).group(1)), m["SLOT_HEADER_LEN"])
    equal("SLOT_CRC_LEN",
         int(re.search(r"#define SLOT_CRC_LEN (\d+)", store).group(1)), m["SLOT_CRC_LEN"])
    equal("SETTINGS_MAX_LEN",
         int(re.search(r"#define SETTINGS_MAX_LEN (\d+)", store).group(1)), m["SETTINGS_MAX_LEN"])
    equal("NVS_KEY_NAME_MAX_SIZE",
         int(re.search(r"#define NVS_KEY_NAME_MAX_SIZE (\d+)", read("libjade/include/nvs.h")).group(1)),
         m["NVS_KEY_NAME_MAX_SIZE"])
    equal("EXPECTED_VERSION[1] (multisig)",
         int(re.search(r"CURRENT_MULTISIG_RECORD_VERSION = (\d+)", read("main/multisig.c")).group(1)),
         m["EXPECTED_VERSION"][1])
    equal("EXPECTED_VERSION[2] (descriptor)",
         int(re.search(r"CURRENT_DESCRIPTOR_RECORD_VERSION = (\d+)", read("main/descriptor.c")).group(1)),
         m["EXPECTED_VERSION"][2])

    for h in error:
        print("  CONSTANT DRIFT: %s" % h)
    print("constant mirror: %s" % ("OK" if not error else "%d DRIFT" % len(error)))
    return len(error)


# ------------------------------------------------------------------ 2. fixture run
def rb(n):
    return bytes(RND.randrange(256) for _ in range(n))


def entry(ns, name, value):
    return bytes([ns, len(name)]) + name + struct.pack("<H", len(value)) + value


def payload(entries, bad_digest=False, magic=b"PIJADES4"):
    body = b"".join(entries)
    return magic + (bytes(4) if bad_digest else hashlib.sha256(body).digest()[:4]) + body


def slot(seq, y, bad_crc=False, extra_bytes=0):
    frame = b"PJSLOT01" + struct.pack("<II", seq, len(y)) + y
    crc = binascii.crc32(frame) & 0xFFFFFFFF
    return frame + struct.pack("<I", (crc ^ 1) if bad_crc else crc) + bytes(extra_bytes)


def sealed(version, blocks=40):
    """version(1) + IV(16) + encrypted(blocks*16) + hmac(32) = 49 + blocks*16"""
    return bytes([version]) + rb(16) + rb(blocks * 16) + rb(32)


DEFAULTS = [entry(0, b"guiflags", b"\x01"), entry(0, b"blob", rb(96))]
# Codex r1 P1: decoded 78-byte BIP32 key + binary derivation path; NO ASCII marker
OLD_RECORD = bytes([3]) + bytes([2, 1]) + bytes.fromhex("3442193e") \
    + bytes.fromhex("0488b21e") + rb(74) + rb(32)
PLAIN_ASCII = bytes([4]) + rb(16) \
    + b"wsh(sortedmulti(2,[3442193e/48h/0h/0h/2h]xpub661MyMwAqRbcFtXgS5sYJABq" + rb(16) + rb(32)
SEALED_MS = entry(1, b"Vault", sealed(4))
# Raw scans must never print the value itself or the bytes surrounding a marker.
RAW_SECRET = b"RAW_SECRET_PRE_xpub_xpub_RAW_SECRET_POST"
RAW_CLEAN = b"RAW_SECRET_UNMARKED_LONG_ASCII_VALUE"

# (name, .a payload, .b payload, .a extra bytes, .b extra bytes, .a bad crc, expected exit)
CASES = [
    ("A_no_records",        payload(DEFAULTS), payload(DEFAULTS), 0, 0, False, 3),
    ("B_sealed_ms",      payload(DEFAULTS + [SEALED_MS]), payload(DEFAULTS), 0, 0, False, 0),
    ("C_plain_ascii",       payload(DEFAULTS + [entry(1, b"Vault", PLAIN_ASCII)]), payload(DEFAULTS), 0, 0, False, 1),
    ("D_new_slot_damaged", payload(DEFAULTS), payload(DEFAULTS + [SEALED_MS]), 0, 0, True, 0),
    ("F_bad_digest",      payload(DEFAULTS + [SEALED_MS], bad_digest=True), payload(DEFAULTS, bad_digest=True), 0, 0, False, 5),
    ("G_otp_only",      payload(DEFAULTS + [entry(3, b"jade", rb(64)),
                                            entry(4, b"jade", struct.pack("<Q", 7))]), payload(DEFAULTS), 0, 0, False, 3),
    ("H_unsealed_binary",  payload(DEFAULTS + [entry(1, b"Vault", OLD_RECORD)]), payload(DEFAULTS), 0, 0, False, 1),
    ("I_wrong_version",    payload(DEFAULTS + [entry(1, b"Vault", sealed(1))]), payload(DEFAULTS), 0, 0, False, 1),
    ("J_descriptor",      payload(DEFAULTS + [entry(2, b"Desc", sealed(1, 30))]), payload(DEFAULTS), 0, 0, False, 0),
    ("K_old_magic",      payload(DEFAULTS + [SEALED_MS], magic=b"PIJADES3"),
                          payload(DEFAULTS, magic=b"PIJADES3"), 0, 0, False, 5),
    # Codex r2 P2: the old slot retains an unsealed record, exposed even when the new one is clean.
    ("L_old_slot_exposed",  payload(DEFAULTS + [SEALED_MS]),
                          payload(DEFAULTS + [entry(1, b"Vault", OLD_RECORD)]), 0, 0, False, 1),
    # Codex r2 P2: bytes appended to a slot; the device ignores the file, so the tool rejects it too.
    ("M_overlong_slot",       payload(DEFAULTS), payload(DEFAULTS + [SEALED_MS]), 8, 0, False, 0),
    ("M2_both_slots_overlong",  payload(DEFAULTS + [SEALED_MS]), payload(DEFAULTS), 8, 8, False, 4),
    # Codex r2 P2: device-rejected ns; the record is still sealed and the verdict checks its body.
    # Device acceptance is reported as NO on a SEPARATE line.
    ("N_invalid_ns5",    payload(DEFAULTS + [SEALED_MS, entry(5, b"x", rb(32))]), payload(DEFAULTS), 0, 0, False, 0),
    # Empty key name: valid header, earlier ns 0 entries recovered; incomplete inspection.
    ("O_empty_name",          payload(DEFAULTS + [entry(1, b"", sealed(4))]),
                          payload(DEFAULTS + [entry(1, b"", sealed(4))]), 0, 0, False, 6),
    # Definite findings precede incomplete inspection; the unsealed record in .b must not be lost.
    ("P_partial_unsealed",  payload(DEFAULTS + [SEALED_MS]),
                          payload(DEFAULTS + [entry(1, b"Open", OLD_RECORD), b"\x01"]), 0, 0, False, 1),
    # Three stream errors: truncated key, truncated header and truncated value.
    ("R_partial_clean",     payload(DEFAULTS + [SEALED_MS]),
                          payload(DEFAULTS + [b"\x01\x05ab"]), 0, 0, False, 6),
    ("T_first_entry_error", payload(DEFAULTS + [SEALED_MS]), payload([b"\x01"]), 0, 0, False, 6),
    ("U_both_payloads_partial", payload(DEFAULTS + [entry(1, b"Split", b"\x00\x01")[:-1]]),
                          payload([b"\x01"]), 0, 0, False, 6),
    ("V_raw_plaintext",   payload(DEFAULTS + [entry(1, b"Open", RAW_SECRET)], bad_digest=True),
                          payload(DEFAULTS + [SEALED_MS]), 0, 0, False, 1),
    # A long ASCII string without markers is not a definite finding in class B.
    ("Y_unreadable_clean", payload([entry(0, b"blob", RAW_CLEAN)]),
                          payload(DEFAULTS + [SEALED_MS]), 0, 0, True, 0),
]


def run(directory):
    p = subprocess.run([sys.executable, TOOL, directory], capture_output=True, text=True)
    result = [s for s in p.stdout.splitlines() if s.startswith("RESULT")]
    return p.returncode, (result[-1] if result else "(no RESULT line)"), p.stdout


def run_fixtures():
    error = 0
    with tempfile.TemporaryDirectory() as root:
        for name, ya, yb, extra_a, extra_b, damaged, expected in CASES:
            d = os.path.join(root, name)
            os.makedirs(d)
            open(os.path.join(d, "pijade-settings.bin.a"), "wb").write(slot(51, ya, damaged, extra_a))
            open(os.path.join(d, "pijade-settings.bin.b"), "wb").write(slot(50, yb, False, extra_b))
            rc, result, output = run(d)
            ok = rc == expected
            # Case N also verifies that device acceptance is reported as NO.
            if ok and name == "N_invalid_ns5" and "device acceptance: NO" not in output:
                ok, result = False, "device acceptance NO was not reported"
            if ok and name == "M_overlong_slot" and "slot rejected" not in output:
                ok, result = False, "overlong slot was not reported as rejected"
            # Look for checks in the correct slot block; a line in .a proves nothing about .b.
            a_output, b_output = output.split("slot .b:", 1)
            if name in {"O_empty_name", "P_partial_unsealed", "R_partial_clean",
                      "T_first_entry_error", "U_both_payloads_partial"}:
                if not all(s in b_output for s in ("device acceptance: NO (damaged entry stream)",
                                                 "WARNING: .b payload", "partially scanned", "incomplete inspection")) \
                        or "COARSE raw scan" in output:
                    ok, result = False, "incorrect partial payload report or scan class"
            if name == "P_partial_unsealed" and not all(s in b_output for s in ("name=Open", "UNSEALED:")):
                ok, result = False, "unsealed record in .b was not scanned"
            if name == "T_first_entry_error" and "entry count 0," not in b_output:
                ok, result = False, "zero recovered entries was not reported"
            if name == "U_both_payloads_partial" and "device acceptance: NO (damaged entry stream)" not in a_output:
                ok, result = False, "incorrect .a partial payload acceptance"
            if name == "V_raw_plaintext" and not all(s in a_output for s in ("COARSE raw scan",
                                                                 "PLAINTEXT: xpub -> 2 occurrences")):
                ok, result = False, "raw marker name and count were not reported"
            if name == "Y_unreadable_clean" and not all(s in a_output for s in ("COARSE raw scan",
                                              "could not inspect; no traces in raw plaintext scan")):
                ok, result = False, "raw scan of unreadable clean file was not reported"
            if "Slot the device will load:" in output or "RAW_SECRET" in output \
                    or "xpub661MyMwAqRbcFtXgS5sYJABq" in output:
                ok, result = False, "removed slot selection or value content was printed"
            error += 0 if ok else 1
            print("%-20s rc=%d expected=%d %-5s | %s" % (name, rc, expected, "OK" if ok else "ERROR", result))
        d = os.path.join(root, "E_no_file")
        os.makedirs(d)
        rc, result, _ = run(d)
        ok = rc == 4
        error += 0 if ok else 1
        print("%-20s rc=%d expected=4 %-5s | %s" % ("E_no_file", rc, "OK" if ok else "ERROR", result))
    return error


def main():
    error = measure_constants()
    print()
    error += run_fixtures()
    print("\nTOTAL ERRORS: %d" % error)
    return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
