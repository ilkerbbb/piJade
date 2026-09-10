#!/usr/bin/env python3
"""33e measurement: verify that wallet records in card settings files remain sealed.

BBB-AIRGAP: NEVER print the CONTENTS of a settings file. Print only namespace indices,
key names (user-supplied wallet names), value LENGTHS, seal version bytes and derived metrics.
Never expose value bytes, digests or key material; treat the file like a private key.

This measures data AT REST: whether record bodies are exposed in bytes on the card.
Binding decisions:
  1. Scan BOTH SLOTS (.a and .b). The store alternates writes, leaving the old slot readable;
     inspecting only the active slot would miss an exposed record (Codex gate r2, P2).
  2. Scan even payloads the device REJECTS: the bytes remain on the card. Report device
     acceptance on a SEPARATE line.
  3. With valid slot and payload headers (magic + digest), scan recovered entries even if
     the entry stream is damaged. This PARTIAL payload is incomplete inspection and blocks
     PASS. Device acceptance is NO; a WARNING reports partial scanning, even with zero
     recovered entries.
  4. Unreadable files, slots or payload headers do not count as incomplete inspection.
     Search all readable file bytes only for PLAINTEXT markers; this COARSE scan does not
     apply ASCII_RUN_THRESHOLD. Print marker names and counts and FAIL if found; otherwise
     report that inspection was impossible and no traces were found.
  5. First matching result wins: usage error, definite finding, no valid slot, no valid
     payload header, partial payload, no records, PASS. Definite findings take precedence
     over incomplete inspection, which is also reported. An unreadable clean file does not
     block PASS.

WHY STRUCTURE: records store namespaces by INDEX byte, so searching for "descriptor" is
not a valid detector. Plain ASCII is insufficient too: an unsealed multisig record carries
decoded 78-byte BIP32 keys and binary derivation paths, with NO "xpub" string (Codex gate
r1, P1). The primary criterion is therefore the seal envelope STRUCTURE:
    version(1) | IV(16) + AES-256-CBC(body) | HMAC-SHA256(32)     (main/registration_seal.h)
A sealed record is 49 + a positive multiple of 16 bytes, at least 65 bytes; its first byte is
the record version (multisig 4, main/multisig.c:17; descriptor 1, main/descriptor.c:21).
An old unsealed record fails at least one of these two criteria.

MEASUREMENT LIMIT: without the wallet key, HMAC verification and decryption are impossible.
This tool proves the record has a SEAL ENVELOPE, not that encryption is correct. The device
provides the positive encryption control: the active wallet opens the record; another wallet
gets `Record Unreadable`.

Usage:
  python3 pijade/tools/t33e_settings_entries.py /Volumes/PIJADE

Exit codes (each result has its own code; nonzero alone is not meaningful):
  0 PASS          records exist, no incomplete inspection, all sealed, no plaintext traces
  1 FAIL          unsealed record, plaintext traces or raw scan marker
  2 usage error
  3 UNMEASURABLE  payload read but no records to measure (ns 1 or 2)
  4 no readable slot (missing file, invalid magic, length or crc)
  5 no valid payload header (invalid magic/version or digest)
  6 INCONCLUSIVE  damaged entry stream; incomplete inspection, no definite finding
"""
import binascii
import hashlib
import struct
import sys

# --------------------------------------------------------------------------------------------
# SECOND COPY of constants from C source. Each cites its source file and line; t33e_selftest.py
# reads the C source and compares it to catch drift in this copy.
# --------------------------------------------------------------------------------------------
SLOT_MAGIC = b"PJSLOT01"          # pijade/host/settings_store.c:23
SLOT_HEADER_LEN = 16              # pijade/host/settings_store.c:19
SLOT_CRC_LEN = 4                  # pijade/host/settings_store.c:20
SLOT_OVERHEAD = SLOT_HEADER_LEN + SLOT_CRC_LEN
SETTINGS_MAX_LEN = 131072         # pijade/host/settings_store.c:17

PAYLOAD_MAGIC = b"PIJADES4"       # libjade/pijade_settings.c:83
PAYLOAD_DIGEST_LEN = 4            # libjade/pijade_settings.c:84
PAYLOAD_HEADER_LEN = len(PAYLOAD_MAGIC) + PAYLOAD_DIGEST_LEN
NVS_KEY_NAME_MAX_SIZE = 16        # libjade/include/nvs.h:4

# libjade/pijade_settings.c PERSISTED_FIELDS: key -> (min, max, nul_terminated)
PERSISTED_FIELDS = {
    "guiflags": (1, 1, False), "idletimeout": (2, 2, False), "screentimeout": (2, 2, False),
    "brightness": (1, 1, False), "qrflags": (4, 4, False), "privatekey": (32, 32, False),
    "blob": (80, 256, False), "counter": (1, 1, False), "antireplay": (4, 4, False),
    "keyflags": (1, 1, False), "featflags": (1, 1, False), "walleterasepin": (48, 48, False),
    "networktype": (4, 4, False), "pinsvrurlA": (1, 120, True), "pinsvrurlB": (1, 120, True),
    "pinsvrpubkey": (33, 33, False), "pinsvrcert": (1, 2048, True),
}
# libjade/pijade_settings.c PERSISTED_NAMESPACES: ns -> (min, max, modulo, max_entries)
PERSISTED_NAMESPACES = {1: (114, 3281, 0, 16), 2: (41, 3281, 0, 16),
                        3: (32, 288, 16, 16), 4: (8, 8, 0, 16)}
SETTINGS_NAMESPACE_COUNT = 1 + len(PERSISTED_NAMESPACES)

# Seal envelope: version(1) + IV(16) + encrypted body(multiple of 16) + HMAC(32)
SEAL_FIXED_LEN = 1 + 16 + 32
AES_BLOCK_LEN = 16
SEAL_MIN_LEN = SEAL_FIXED_LEN + AES_BLOCK_LEN
EXPECTED_VERSION = {1: 4, 2: 1}

# Only these two namespaces are in scope for 33e. ns 3 holds OTP records; ns 4 holds HOTP
# counters deliberately stored in plaintext (main/storage.c storage_set_hotp_counter).
# Neither is measured.
MEASURED_NS = {1: "multisig", 2: "descriptor"}
OTHER_NS = {3: "otp record", 4: "otp counter (deliberately plaintext)"}

# Secondary check. Detects an unsealed ASCII body but cannot detect a binary body.
PLAINTEXT = [b"xpub", b"tpub", b"ypub", b"zpub", b"vpub", b"upub",
             b"wsh(", b"sh(", b"multi(", b"sortedmulti(", b"desc-", b"/84h", b"/48h"]
ASCII_RUN_THRESHOLD = 16


def parse_slot(raw):
    """Same acceptance rules as pijade/host/settings_store.c read_slot()."""
    if len(raw) < SLOT_OVERHEAD:
        return None, "file shorter than slot header"
    if raw[:8] != SLOT_MAGIC:
        return None, "slot magic missing"
    seq, plen = struct.unpack("<II", raw[8:16])
    if plen == 0 or plen > SETTINGS_MAX_LEN:
        return None, "invalid payload length (%d)" % plen
    if len(raw) != SLOT_OVERHEAD + plen:
        return None, "file size differs from payload length (%d, expected %d)" % (
            len(raw), SLOT_OVERHEAD + plen)
    end = SLOT_HEADER_LEN + plen
    expected_crc = struct.unpack("<I", raw[end:end + SLOT_CRC_LEN])[0]
    if (binascii.crc32(raw[:end]) & 0xFFFFFFFF) != expected_crc:
        return None, "crc mismatch"
    return {"seq": seq, "payload": raw[SLOT_HEADER_LEN:end]}, None


def parse_payload(payload):
    """Decode entries. Magic and digest checks match the device; measure entry acceptance
    SEPARATELY because even device-rejected payloads must be scanned.
    Header errors raise ValueError; entry stream errors return (recovered_entries, error)."""
    if len(payload) < PAYLOAD_HEADER_LEN:
        raise ValueError("payload shorter than header")
    if payload[:8] != PAYLOAD_MAGIC:
        raise ValueError("magic/version differs from expected %s" % PAYLOAD_MAGIC.decode())
    body = payload[PAYLOAD_HEADER_LEN:]
    if hashlib.sha256(body).digest()[:PAYLOAD_DIGEST_LEN] != payload[8:PAYLOAD_HEADER_LEN]:
        raise ValueError("digest mismatch")
    p, entries = 0, []
    while p < len(body):
        if p + 2 > len(body):
            return entries, "truncated entry header"
        ns, klen = body[p], body[p + 1]
        p += 2
        if klen == 0 or p + klen + 2 > len(body):
            return entries, "truncated or empty key"
        key = body[p:p + klen]
        p += klen
        vlen = struct.unpack("<H", body[p:p + 2])[0]
        p += 2
        if p + vlen > len(body):
            return entries, "truncated value"
        entries.append((ns, key, body[p:p + vlen]))
        p += vlen
    return entries, None


def scan_raw_plaintext(raw):
    """Unreadable files only: coarse marker search across all bytes; never print content."""
    markers = [(m, raw.count(m)) for m in PLAINTEXT if m in raw]
    print("    COARSE raw scan: PLAINTEXT markers only")
    for m, n in markers:
        print("    PLAINTEXT: %s -> %d occurrences" % (m.decode(), n))
    if not markers:
        print("    could not inspect; no traces in raw plaintext scan")
    return bool(markers)


def device_accepts(entries):
    """Mirror libjade/pijade_settings.c walk_entries() + user_entry_valid().
    Return (accepted, reason). Only reported; does NOT determine the verdict."""
    urlA = urlB = False
    counts = {ns: 0 for ns in PERSISTED_NAMESPACES}
    for ns, key, value in entries:
        if ns >= SETTINGS_NAMESPACE_COUNT:
            return False, "ns=%d is undefined" % ns
        if ns == 0:
            name = key.decode("ascii", "replace")
            if name not in PERSISTED_FIELDS:
                return False, "default field is not listed: %s" % name
            mn, mx, nul = PERSISTED_FIELDS[name]
            if not mn <= len(value) <= mx:
                return False, "%s length %d, allowed %d..%d" % (name, len(value), mn, mx)
            if nul and value[-1:] != b"\x00":
                return False, "%s does not end in NUL" % name
            if name == "antireplay" and struct.unpack("<I", value)[0] == 0xFFFFFFFF:
                return False, "antireplay UINT32_MAX"
            if name == "networktype" and struct.unpack("<I", value)[0] > 2:
                return False, "networktype greater than 2"
            urlA = urlA or name == "pinsvrurlA"
            urlB = urlB or name == "pinsvrurlB"
            continue
        if not 1 <= len(key) < NVS_KEY_NAME_MAX_SIZE:
            return False, "ns=%d key name length %d" % (ns, len(key))
        if any(b < 33 or b > 126 for b in key):
            return False, "ns=%d nonprintable byte in key name" % ns
        mn, mx, mod, limit = PERSISTED_NAMESPACES[ns]
        if not mn <= len(value) <= mx:
            return False, "ns=%d value length %d, allowed %d..%d" % (ns, len(value), mn, mx)
        if mod and (len(value) - AES_BLOCK_LEN) % mod != 0:
            return False, "ns=%d value length is not a multiple of %d" % (ns, mod)
        counts[ns] += 1
        if counts[ns] > limit:
            return False, "ns=%d record count limit (%d) exceeded" % (ns, limit)
    if urlA != urlB:
        return False, "pinsvrurlA and pinsvrurlB are not paired"
    return True, "accepted"


def longest_ascii(data):
    longest = current = 0
    for b in data:
        current = current + 1 if 0x20 <= b <= 0x7E else 0
        longest = max(longest, current)
    return longest


def measure_record(ns, value):
    length_ok = len(value) >= SEAL_MIN_LEN and (len(value) - SEAL_FIXED_LEN) % AES_BLOCK_LEN == 0
    version = value[0] if value else None
    markers = [(m, value.count(m)) for m in PLAINTEXT if m in value]
    run = longest_ascii(value)
    return {"length_ok": length_ok, "version": version,
            "version_ok": version == EXPECTED_VERSION[ns], "markers": markers, "ascii_run": run,
            "ok": length_ok and version == EXPECTED_VERSION[ns] and not markers
                     and run < ASCII_RUN_THRESHOLD}


def scan_payload(label, entries):
    """Measure records in one payload. Return (measured_record_count, failed_count)."""
    records = [g for g in entries if g[0] in MEASURED_NS]
    for ns, key, value in entries:
        if ns in OTHER_NS:
            print("    excluded from measurement: ns=%d (%s) name=%s value=%d bytes"
                  % (ns, OTHER_NS[ns], key.decode("ascii", "replace"), len(value)))
        elif ns != 0 and ns not in MEASURED_NS:
            print("    UNKNOWN namespace: ns=%d name=%s value=%d bytes"
                  % (ns, key.decode("ascii", "replace"), len(value)))
    print("    measured records (ns 1/2): %d" % len(records))
    failed = 0
    for ns, key, value in records:
        o = measure_record(ns, value)
        print("      ns=%d (%s) name=%s value=%d bytes version=%s"
              % (ns, MEASURED_NS[ns], key.decode("ascii", "replace"), len(value), o["version"]))
        if not o["length_ok"]:
            print("          UNSEALED: length does not fit envelope (49 + multiple of 16, at least %d)" % SEAL_MIN_LEN)
        if not o["version_ok"]:
            print("          UNSEALED: version byte %s, expected %d" % (o["version"], EXPECTED_VERSION[ns]))
        for m, n in o["markers"]:
            print("          PLAINTEXT: %s -> %d occurrences" % (m.decode(), n))
        if o["ascii_run"] >= ASCII_RUN_THRESHOLD:
            print("          SUSPECTED PLAINTEXT: longest printable ASCII run is %d bytes" % o["ascii_run"])
        if not o["ok"]:
            failed += 1
    return len(records), failed


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1].rstrip("/")
    valid_slots = 0
    valid_headers = 0
    partial = 0
    total_records = 0
    total_failed = 0
    raw_findings = 0     # unreadable files with markers found by raw scan
    rejected = []   # scanned payloads rejected by the device

    for suffix in ("a", "b"):
        path = "%s/pijade-settings.bin.%s" % (root, suffix)
        print("slot .%s:" % suffix)
        raw = b""
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError as e:
            # The file could not be opened: there are NO bytes to scan. Passing an empty buffer to the
            # raw scan would print "could not inspect; no traces in raw plaintext scan", misleading
            # the reader into thinking something had been scanned.
            print("    could not read: %s" % e.strerror)
            continue
        slot, error = parse_slot(raw)
        if error:
            print("    slot rejected: %s" % error)
            raw_findings += scan_raw_plaintext(raw)
            continue
        valid_slots += 1
        print("    seq=%d payload=%d bytes, valid slot format" % (slot["seq"], len(slot["payload"])))
        try:
            entries, stream_error = parse_payload(slot["payload"])
        except ValueError as e:
            print("    could not parse payload: %s (device also rejects it)" % e)
            raw_findings += scan_raw_plaintext(raw)
            continue
        valid_headers += 1
        if stream_error:
            partial += 1
            print("    entry stream error: %s" % stream_error)
            accepted, reason = False, "damaged entry stream"
        else:
            accepted, reason = device_accepts(entries)
        print("    entry count %d, device acceptance: %s" % (len(entries), "YES" if accepted else "NO (%s)" % reason))
        if not accepted:
            rejected.append((suffix, reason, stream_error))
        n, k = scan_payload(suffix, entries)
        total_records += n
        total_failed += k

    print()
    for suffix, reason, stream_error in rejected:
        print("WARNING: .%s payload REJECTED BY DEVICE (%s); its bytes remain on the card" % (suffix, reason))
        print("       %s, but the device WILL NOT LOAD those settings."
              % ("partially scanned" if stream_error else "scanned anyway"))
    if partial:
        print("WARNING: incomplete inspection; %d payloads have damaged entry streams" % partial)
    print("Limit: HMAC cannot be verified without the wallet key; this measurement shows a SEAL ENVELOPE")
    print("around the record, not encryption correctness. The device provides the positive encryption control.")
    # DECISION 4: first match wins; definite findings take precedence over incomplete inspection.
    definite_findings = total_failed + raw_findings
    if definite_findings:
        print("RESULT: FAIL (%d records failed criteria out of %d; raw markers in %d files)"
              % (total_failed, total_records, raw_findings))
        return 1
    if not valid_slots:
        print("RESULT: no readable slot")
        return 4
    if not valid_headers:
        print("RESULT: no valid payload header")
        return 5
    if partial:
        print("RESULT: INCONCLUSIVE (%d payloads partially scanned; incomplete inspection)" % partial)
        return 6
    if not total_records:
        print("RESULT: UNMEASURABLE (no records in either slot)")
        return 3
    print("RESULT: PASS (%d records in seal envelopes, no plaintext traces)" % total_records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
