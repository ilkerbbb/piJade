#!/usr/bin/env python3
"""Item 43 boundary measurement: a multisig file frame with a 15-character name.

Prove that the notification screen shows the longest valid name without clipping (Codex Phase 1 P2).
Usage: t43_longname.py <output_prefix>"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))
import descriptor_qr as d

src = open(d.MSFILE, encoding="utf-8").read()
long_name = "W" * 15
out = src.replace("Name: Vault", "Name: " + long_name, 1)
assert long_name in out
d.write_frame(sys.argv[1], out)
print("name:", long_name, "length:", len(long_name))

