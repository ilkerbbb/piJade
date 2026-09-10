# Vendored: foundation-ur-py (BC-UR, Python)

- Source: https://github.com/Foundation-Devices/foundation-ur-py
- Commit: a371f6355fb3433cc989ad5bf28f87a347b222fe (pushed 2025-09-17)
- License: BSD-2-Clause Plus Patent (see LICENSE in this directory)
- Files: the upstream `ur/` package, unmodified. The upstream repo has no setup.py or
  pyproject.toml, so pip cannot install it; copying the package is the reproducible path.
- Used by: pijade/tools/mine_qr.py (encoder) and the jade-mine-reply decode proof (decoder).
- Verified on vendoring: upstream test.py green with PYTHONPATH=pijade/tools; 200-byte,
  60-byte-fragment round trip.
- Contract (measured 2026-09-02): the UR cbor passed to `UREncoder` must be a `bytearray`;
  `FountainEncoder.partition_message` pads the last fragment with `.append(0)`
  (fountain_encoder.py:112-120), so a `bytes` message of a length that is not an exact multiple
  of the fragment length raises AttributeError. mine_qr.py wraps its cbor in `bytearray()`.
