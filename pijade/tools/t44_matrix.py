#!/usr/bin/env python3
"""Generate module matrices for t44 measurement fixtures (pijade/tools/t44_matrix.h).

Dependency: segno. Commit the output so building the measurement tool does not
require segno; run this script only when the matrices need to be regenerated.

The contents are SYNTHETIC and deliberately meaningless: measurement depends only
on QR VERSION (module count) and error-correction level, not the encoded string.
Do NOT use a real wallet descriptor as a fixture or commit a binary frame.

Lengths mimic two real cases measured in round 6:
  medium -> version 10 (57 modules); the size of a Sparrow singlesig descriptor
  dense  -> version 14 (73 modules); the unreadable multisig descriptor case"""
import sys

try:
    import segno
except ImportError:
    sys.exit("segno is not installed; install it in a python3 -m venv environment and retry")

ALPHABET = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ123456789"


def padding(n):
    """Deterministic n-character string with lowercase letters, forcing byte mode."""
    return "".join(ALPHABET[i % len(ALPHABET)] for i in range(n))


FIXTURES = (
    ("medium", 231),
    ("dense", 445),
)

lines = [
    "/* GENERATED FILE; do not edit by hand. Source: pijade/tools/t44_matrix.py */",
    "#ifndef T44_MATRIX_H",
    "#define T44_MATRIX_H",
    "",
    "struct t44_matrix {",
    "    const char* name;",
    "    int version;      /* QR version */",
    "    int n;          /* modules per side */",
    "    const unsigned char* bit;  /* packed row by row; bit 7 is the leftmost module */",
    "};",
    "",
]

definitions = []
for name, length in FIXTURES:
    qr = segno.make(padding(length), error="l", boost_error=False)
    mat = [list(r) for r in qr.matrix]
    n = len(mat)
    row_bytes = (n + 7) // 8
    raw = bytearray()
    for row in mat:
        b = bytearray(row_bytes)
        for x, v in enumerate(row):
            if v:
                b[x // 8] |= 0x80 >> (x % 8)
        raw += b
    print(f"{name}: length {length}, version {qr.version}, modules {n}x{n}, {len(raw)} bytes")

    lines.append(f"static const unsigned char t44_bit_{name}[{len(raw)}] = {{")
    for i in range(0, len(raw), 12):
        lines.append("    " + " ".join(f"0x{v:02x}," for v in raw[i:i + 12]))
    lines.append("};")
    lines.append("")
    definitions.append(f'    {{ "{name}", {qr.version}, {n}, t44_bit_{name} }},')

lines.append("static const struct t44_matrix t44_matrices[] = {")
lines += definitions
lines.append("};")
lines.append("")
lines.append("#endif /* T44_MATRIX_H */")

with open("pijade/tools/t44_matrix.h", "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("pijade/tools/t44_matrix.h written")
