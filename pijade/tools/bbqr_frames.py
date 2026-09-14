#!/usr/bin/env python3
"""Render a BBQr transfer as emulator camera frames.

BBQr is the animated QR format Coldcard and Passport emit.  The collector that reads it is
main/bbqr.c and the branch that feeds it during a camera session is main/bcur.c; this writes the
frames those two are measured against.  One 640x480 grayscale frame per part, named
<prefix>-NN.gray so jadectl.py camfiles picks them up in order, plus <prefix>-parts.txt with the
frame text of each part for the record.

The split follows the reference encoder (vendor-audit/BBQr/python/bbqr/split.py and utils.py):
every part but the last carries the same number of characters, that number is a multiple of 8 for
base32 and 2 for hex, and the header is 'B$' + encoding + file type + two base36 digits of part
count + two of index.  The reference picks the part size from a QR capacity table (pyqrcode, which
is installed nowhere in this project); this takes it as an argument instead and checks the QR the
part actually produced against the scanner's limits.

The --corrupt families build frames the collector is expected to reject.  They are here rather
than in the shell driver so that the frame that carries a defect is generated the same way every
run, and so the defect is stated once, in code, next to the encoder it deviates from.

Usage (in the container, writing into /probe):
    python3 /jade/pijade/tools/bbqr_frames.py --psbt /jade/test_data/psbt_ss_p2wpkh.json \
        --encoding 2 --per-part 56 psbt2
    python3 /jade/pijade/tools/bbqr_frames.py --text-file setup.txt --type U --per-part 288 setup
    python3 /jade/pijade/tools/bbqr_frames.py --psbt ... --corrupt ceiling bad_ceiling
"""

import argparse
import base64
import json
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qrcode  # noqa: E402
from qrcode.constants import ERROR_CORRECT_L  # noqa: E402

from epoch_qr import frame_scale, write_gray  # noqa: E402

BASE36_DIGITS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
MAX_PARTS = 1295  # 'ZZ'; coinkite/BBQr python/bbqr/split.py
MAX_INPUT_MSG_SIZE = 1024 * 401  # main/process.h, the ceiling main/bbqr.c checks against

# quirc identifies nothing below 3 px per module (pijade/tools/screen_qr_to_camera.py), and the
# versions measured against this emulator's scanner stop at 10.  Larger codes are not refused
# outright - the ceiling family needs one - but they have to be asked for, so that a scenario
# never silently depends on a QR that has never been shown to decode here.
MIN_SCALE = 3
DEFAULT_MAX_VERSION = 10

CORRUPT_FAMILIES = ('b32len', 'blocklen', 'conflict', 'ceiling', 'bomb')


def base36_pair(value):
    if not 0 <= value <= MAX_PARTS:
        raise SystemExit('part count out of range: %d' % value)
    return BASE36_DIGITS[value // 36] + BASE36_DIGITS[value % 36]


def encode_payload(raw, encoding):
    """Encode as vendor-audit/BBQr/python/bbqr/utils.py encode_data() does.

    Returns the payload characters and the split modulus: the encoder may only cut a part on a
    boundary where the encoding completes whole bytes, which is 8 characters for base32 and 2 for
    hex.  Unlike the reference this never upgrades the encoding on its own; a scenario that asks
    for 'Z' wants the deflate path measured, so data that does not compress is an error here
    rather than a silent fall back to '2'.
    """
    if encoding == 'H':
        return raw.hex().upper(), 2

    if encoding == 'Z':
        compressor = zlib.compressobj(wbits=-10)
        compressed = compressor.compress(raw) + compressor.flush()
        if len(compressed) >= len(raw):
            raise SystemExit('deflate does not shrink this data: %d -> %d bytes'
                             % (len(raw), len(compressed)))
        raw = compressed

    return base64.b32encode(raw).decode('ascii').rstrip('='), 8


def split_payload(payload, split_mod, per_part):
    if per_part % split_mod:
        raise SystemExit('--per-part must be a multiple of %d for this encoding' % split_mod)
    chunks = [payload[offset:offset + per_part] for offset in range(0, len(payload), per_part)]
    if len(chunks) > MAX_PARTS:
        raise SystemExit('%d parts is over the BBQr limit of %d' % (len(chunks), MAX_PARTS))
    return chunks


def frame_text(encoding, file_type, num_parts, index, chunk):
    return 'B$%s%s%s%s%s' % (encoding, file_type, base36_pair(num_parts),
                             base36_pair(index), chunk)


def build_frames(encoding, file_type, chunks):
    return [frame_text(encoding, file_type, len(chunks), index, chunk)
            for index, chunk in enumerate(chunks)]


def corrupt_frames(family, encoding, file_type, payload, split_mod, per_part):
    """Frames for one rejection family, with the deviation from the encoder named in one line."""
    if family == 'b32len':
        # A part that is not the last one, one character short of a whole number of bytes.
        chunks = split_payload(payload, split_mod, per_part)
        if len(chunks) < 2:
            raise SystemExit('b32len needs a transfer of two parts or more')
        chunks[0] = chunks[0][:-1]
        return build_frames(encoding, file_type, chunks)

    if family == 'blocklen':
        # Two parts that both decode cleanly but claim different block lengths.
        chunks = split_payload(payload, split_mod, per_part)
        if len(chunks) < 3:
            raise SystemExit('blocklen needs a transfer of three parts or more')
        chunks[1] = chunks[1][:-split_mod]
        return build_frames(encoding, file_type, chunks)

    if family == 'conflict':
        # The same index twice with different content, which is the case a collector cannot
        # merge: the second frame carries the first part's header and the second part's payload.
        chunks = split_payload(payload, split_mod, per_part)
        if len(chunks) < 2:
            raise SystemExit('conflict needs a transfer of two parts or more')
        frames = build_frames(encoding, file_type, chunks)
        return [frames[0], frame_text(encoding, file_type, len(chunks), 0, chunks[1])] + frames[1:]

    if family == 'ceiling':
        # One frame that claims a transfer larger than the input ceiling.  The block length comes
        # from this part, so the claim is rejected on the first frame and the rest never exist.
        chunk_chars = per_part - (per_part % split_mod)
        decoded = chunk_chars * 5 // 8 if encoding != 'H' else chunk_chars // 2
        if decoded * MAX_PARTS <= MAX_INPUT_MSG_SIZE:
            raise SystemExit('a part of %d characters cannot claim more than the ceiling even at '
                             '%d parts; ask for a larger --per-part' % (chunk_chars, MAX_PARTS))
        if len(payload) < chunk_chars:
            raise SystemExit('payload is shorter than one part')
        return [frame_text(encoding, file_type, MAX_PARTS, 0, payload[:chunk_chars])]

    raise SystemExit('unknown corrupt family: %s' % family)


def bomb_payload():
    """Data whose deflate stream is small and whose inflated size is over the input ceiling."""
    raw = b'\0' * (MAX_INPUT_MSG_SIZE + 1024)
    compressor = zlib.compressobj(wbits=-10)
    compressed = compressor.compress(raw) + compressor.flush()
    return base64.b32encode(compressed).decode('ascii').rstrip('='), len(raw)


def render(frames, prefix, max_version):
    written = []
    for index, text in enumerate(frames):
        code = qrcode.QRCode(error_correction=ERROR_CORRECT_L)
        code.add_data(text)
        code.make(fit=True)
        matrix = code.get_matrix()
        scale = frame_scale(len(matrix))
        if code.version > max_version:
            raise SystemExit('part %d needs QR version %d, over the --max-version of %d: shorten '
                             '--per-part or raise the ceiling deliberately'
                             % (index, code.version, max_version))
        if scale < MIN_SCALE:
            raise SystemExit('part %d renders at %d px per module, under the %d px quirc needs'
                             % (index, scale, MIN_SCALE))
        path = '%s-%02d.gray' % (prefix, index)
        write_gray(path, matrix)
        written.append((path, code.version, scale, len(text)))
    return written


def read_source(args):
    if args.psbt:
        with open(args.psbt, encoding='utf-8') as handle:
            return base64.b64decode(json.load(handle)['input']['psbt'])
    if args.text_file:
        with open(args.text_file, 'rb') as handle:
            return handle.read()
    if args.file:
        with open(args.file, 'rb') as handle:
            return handle.read()
    raise SystemExit('give one of --psbt, --text-file or --file')


def main():
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--psbt', help='test_data PSBT fixture; input.psbt is decoded from base64')
    source.add_argument('--text-file', help='file whose bytes are the payload, as read')
    source.add_argument('--file', help='same as --text-file, named for binary payloads')
    source.add_argument('--parts-from', help='file of ready frame texts, one per line, rendered '
                                             'as they are: the way a run mixes BBQr with frames '
                                             'of another format (see bcur_psbt_parts.py)')
    parser.add_argument('--type', default='P', help='BBQr file type character (P, T, U, ...)')
    parser.add_argument('--encoding', default='2', choices=('2', 'Z', 'H'))
    parser.add_argument('--per-part', type=int, default=56,
                        help='payload characters per part, the last one excepted')
    parser.add_argument('--max-version', type=int, default=DEFAULT_MAX_VERSION)
    parser.add_argument('--corrupt', choices=CORRUPT_FAMILIES)
    parser.add_argument('prefix')
    args = parser.parse_args()

    if args.parts_from:
        with open(args.parts_from, encoding='utf-8') as handle:
            frames = [line.rstrip('\n') for line in handle if line.strip()]
        written = render(frames, args.prefix, args.max_version)
        print('frames=%d, read as they are from %s' % (len(frames), args.parts_from))
        for path, version, scale, length in written:
            print('  %s version=%d scale=%dpx text=%d'
                  % (os.path.basename(path), version, scale, length))
        return

    if args.corrupt == 'bomb':
        payload, inflated_len = bomb_payload()
        split_mod = 8
        encoding = 'Z'
        note = 'inflates to %d bytes, over the %d ceiling' % (inflated_len, MAX_INPUT_MSG_SIZE)
    else:
        payload, split_mod = encode_payload(read_source(args), args.encoding)
        encoding = args.encoding
        note = ''

    if args.corrupt and args.corrupt != 'bomb':
        frames = corrupt_frames(args.corrupt, encoding, args.type, payload, split_mod,
                                args.per_part)
    else:
        frames = build_frames(encoding, args.type, split_payload(payload, split_mod,
                                                                 args.per_part))

    written = render(frames, args.prefix, args.max_version)
    with open('%s-parts.txt' % args.prefix, 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(frames) + '\n')

    print('encoding=%s type=%s frames=%d payload_chars=%d%s'
          % (encoding, args.type, len(frames), len(payload), ' (%s)' % note if note else ''))
    for path, version, scale, length in written:
        print('  %s version=%d scale=%dpx text=%d' % (os.path.basename(path), version, scale,
                                                      length))


if __name__ == '__main__':
    main()
