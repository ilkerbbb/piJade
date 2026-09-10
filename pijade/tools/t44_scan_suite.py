"""Run ONLY the upstream QR-scan test against the fork's VGA camera contract.

Discriminating: test_scan_qr() pushes 13 compressed fixtures through debug_scan_qr, which
rejects anything that does not decompress to exactly CAMERA_IMAGE_WIDTH * CAMERA_IMAGE_HEIGHT
bytes (main/process/debug_scan_qr.c). A fixture still holding a 320x240 image fails with
"Failed to decompress image data" instead of decoding, so this run fails loudly if the
fixtures and the camera contract ever drift apart again. Each fixture also asserts its own
expected text or hex, so an image that decompresses but decodes to something else fails too.

Run it inside the jade-dev container, from the repo root mounted at /jade:

    docker exec jade-dev sh -lc \
      'cd /jade && LD_LIBRARY_PATH=/jade/build_linux/libjade python3 pijade/tools/t44_scan_suite.py'

Same two traps as pijade/tools/t58_suite.py: build with `make jade` (the daemon target links
jade_static and leaves libjade.so behind), and test_jade.py needs a module-level `args`.
"""
import os
import sys
import time
import types

os.chdir('/jade')
sys.path.insert(0, '/jade')

import test_jade as T
from jadepy import JadeAPI

T.args = types.SimpleNamespace(json_filter=None, sample_percent=100)

with JadeAPI.create_libjade() as jade:
    assert jade.clean_reset() is True
    info = jade.get_version_info()
    assert jade.add_entropy(os.urandom(64)) is True
    assert jade.set_epoch(int(time.time())) is True
    assert jade.set_mnemonic(T.TEST_MNEMONIC) is True

    n = len(list(T._get_test_cases(T.QR_VGA_SCAN_TESTS)))
    print('fixtures: qr_vga_*=%d' % n)
    assert n >= 13, 'fixture count too low; glob broken'

    T.test_scan_qr(jade, info['BOARD_TYPE'])
    print('PASS test_scan_qr')

print('T44 SCAN SUITE OK')
