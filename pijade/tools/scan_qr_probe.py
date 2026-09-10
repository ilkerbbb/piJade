#!/usr/bin/env python3
"""Run the core of test_jade.py::test_scan_qr on its own.

Prove the debug_scan_qr RPC path still works in a camera-free libjade build. Narrowing camera
macros can silently disable this path (it happened once). Run only this path because the full
test suite requires hardware and network dependencies in the container.

Usage (in the container):
  LD_LIBRARY_PATH=/jade/build/nocam/libjade \
      python3 pijade/tools/scan_qr_probe.py [test_data/qr_qvga_*.json names]

Defaults to qr_qvga_compactseedqr_vec1.json. In-process execution with the camera-enabled
library is unsupported: that branch expects host frames (libjade_camera_active), crashes
without a feeder and returns 139. Drive the camera-enabled build through libjade_daemon.
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST_DATA = os.path.join(REPO, 'test_data')

sys.path.insert(0, REPO)

# pyserial is not installed in the container and this proof exercises only the libjade
# backend; a minimal placeholder lets jadepy import its serial backend without adding
# another dependency.
import types  # noqa: E402

if 'serial' not in sys.modules:
    _serial = types.ModuleType('serial')
    _tools = types.ModuleType('serial.tools')
    _ports = types.ModuleType('serial.tools.list_ports')
    _ports.comports = lambda: []
    _tools.list_ports = _ports
    _serial.tools = _tools
    _serial.Serial = object
    _serial.SerialException = Exception
    sys.modules['serial'] = _serial
    sys.modules['serial.tools'] = _tools
    sys.modules['serial.tools.list_ports'] = _ports

from jadepy import JadeAPI  # noqa: E402


def h2b(hexstr):
    return bytes.fromhex(hexstr)


def main():
    cases = sys.argv[1:] or ['qr_qvga_compactseedqr_vec1.json']
    ok = 0
    with JadeAPI.create_libjade(timeout=0) as jade:
        for name in cases:
            with open(os.path.join(TEST_DATA, name)) as f:
                spec = json.load(f)
            expected = spec['expected_output']
            with open(os.path.join(TEST_DATA, spec['input']['image']), 'rb') as f:
                image = f.read()

            rslt = jade.scan_qr(image)
            assert rslt, f'{name}: empty result'
            if expected.get('text') is not None:
                assert rslt.decode() == expected['text'], f'{name}: text mismatch'
            else:
                assert rslt == h2b(expected['hex']), f'{name}: hex mismatch'
            print(f'PASS {name} ({len(rslt)} bytes)')
            ok += 1
    print(f'TOTAL {ok}/{len(cases)} passed')


if __name__ == '__main__':
    main()
