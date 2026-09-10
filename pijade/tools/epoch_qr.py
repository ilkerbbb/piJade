#!/usr/bin/env python3
"""Generate a single-part epoch UR QR for Jade (ur:jade-epoch; main/qrmode.c:1401).

Usage: epoch_qr.py [--epoch N] <output.png> [<output.gray>]
(gray: 640x480 emulator camera frame). Recipe and evidence: pijade/UPSTREAM.md section 25.

Mac dependencies:
    python3 -m venv ~/.venvs/pijade
    ~/.venvs/pijade/bin/pip install cbor2==6.1.2 qrcode
"""

import argparse
import struct
import time
import zlib

import cbor2
import qrcode
from qrcode.constants import ERROR_CORRECT_L


BYTEWORDS = (
    "ableacidalsoapexaquaarchatomauntawayaxisbackbaldbarnbeltbetabiasbluebody"
    "bragbrewbulbbuzzcalmcashcatschefcityclawcodecolacookcostcruxcurlcuspcyan"
    "darkdatadaysdelidicedietdoordowndrawdropdrumdulldutyeacheasyechoedgeepic"
    "evenexamexiteyesfactfairfernfigsfilmfishfizzflapflewfluxfoxyfreefrogfuel"
    "fundgalagamegeargemsgiftgirlglowgoodgraygrimgurugushgyrohalfhanghardhawk"
    "heathelphighhillholyhopehornhutsicedideaidleinchinkyintoirisironitemjade"
    "jazzjoinjoltjowljudojugsjumpjunkjurykeepkenokeptkeyskickkilnkingkitekiwi"
    "knoblamblavalazyleaflegsliarlimplionlistlogoloudloveluaulucklungmainmany"
    "mathmazememomenumeowmildmintmissmonknailnavyneednewsnextnoonnotenumbobey"
    "oboeomitonyxopenovalowlspaidpartpeckplaypluspoempoolposepuffpumapurrquad"
    "quizraceramprealredorichroadrockroofrubyruinrunsrustsafesagascarsetssilk"
    "skewslotsoapsolosongstubsurfswantacotasktaxitenttiedtimetinytoiltombtoys"
    "triptunatwinuglyundouniturgeuservastveryvetovialvibeviewvisavoidvowswall"
    "wandwarmwaspwavewaxywebswhatwhenwhizwolfworkyankyawnyellyogayurtzapszero"
    "zestzinczonezoom"
)

PNG_SCALE = 10
QUIET_MODULES = 4
CAMERA_WIDTH = 640
CAMERA_HEIGHT = 480


def chunk(tag, data):
    checked = tag + data
    return (
        struct.pack(">I", len(data))
        + checked
        + struct.pack(">I", zlib.crc32(checked) & 0xFFFFFFFF)
    )


def bytewords_minimal(data):
    checksum = struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)
    encoded = []
    for value in data + checksum:
        word = BYTEWORDS[value * 4 : value * 4 + 4]
        encoded.append(word[0] + word[3])
    return "".join(encoded)


def write_png(path, matrix):
    rows = []
    for row in matrix:
        pixels = b"".join(
            (b"\x00" if is_dark else b"\xff") * PNG_SCALE for is_dark in row
        )
        rows.extend([b"\x00" + pixels] * PNG_SCALE)

    side = len(matrix) * PNG_SCALE
    header = struct.pack(">IIBBBBB", side, side, 8, 0, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as output:
        output.write(png)


def write_gray(path, matrix):
    # See the scale note in pijade/tools/screen_qr_to_camera.py: quirc reads only the central
    # scan window (SCAN_MARGIN, main/qrscan.c:13) and identifies nothing above 7 px per module
    # (measured 2026-09-10), so cap the scale rather than filling the frame height.
    scan_window = min(CAMERA_WIDTH, CAMERA_HEIGHT) - 20
    scale = min(6, scan_window // len(matrix))
    side = len(matrix) * scale
    offset_x = (CAMERA_WIDTH - side) // 2
    offset_y = (CAMERA_HEIGHT - side) // 2
    frame = bytearray(b"\xff" * (CAMERA_WIDTH * CAMERA_HEIGHT))

    for row_index, row in enumerate(matrix):
        for column_index, is_dark in enumerate(row):
            if not is_dark:
                continue
            for delta_y in range(scale):
                start = (
                    (offset_y + row_index * scale + delta_y) * CAMERA_WIDTH
                    + offset_x
                    + column_index * scale
                )
                frame[start : start + scale] = b"\x00" * scale

    with open(path, "wb") as output:
        output.write(frame)


def epoch_arg(text):
    # Jade reads the epoch with rpc_get_uint64 (main/process/process_utils.c:87); negative
    # values or values exceeding 64 bits are rejected with "Failed to extract valid epoch value".
    value = int(text)
    if not 0 <= value < 2**64:
        raise argparse.ArgumentTypeError("epoch must be between 0 and 2^64-1")
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epoch", type=epoch_arg)
    parser.add_argument("png_output")
    parser.add_argument("gray_output", nargs="?")
    args = parser.parse_args()

    epoch = int(time.time()) if args.epoch is None else args.epoch
    payload = {
        "id": "1",
        "method": "set_epoch",
        "params": {"epoch": epoch},
    }
    cbor = cbor2.dumps(payload)
    ur = "ur:jade-epoch/" + bytewords_minimal(cbor)

    qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L)
    qr.add_data(ur.upper())
    qr.make(fit=True)
    matrix = qr.get_matrix()
    if qr.border != QUIET_MODULES:
        raise RuntimeError("qrcode default quiet zone is not 4 modules")
    if len(matrix) != qr.modules_count + 2 * qr.border:
        raise RuntimeError("qrcode matrix does not include the expected quiet zone")

    write_png(args.png_output, matrix)
    if args.gray_output is not None:
        write_gray(args.gray_output, matrix)

    print(f"epoch={epoch}")
    print(f"ur={ur.upper()}")
    print(f"cbor_hex={cbor.hex()}")


if __name__ == "__main__":
    main()
