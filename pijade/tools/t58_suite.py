"""Run ONLY the two upstream message-signing tests against the fork's new length rule.

Discriminating: msg_large/msgfile_large (2448 bytes) must now be REJECTED with
"Message too long to display", while every other fixture (12-68 bytes) must still produce
its expected signature.  A run that rejected everything would fail on the short fixtures,
and a run that accepted everything would fail on the large ones.

The six msg_bbb_*/msgfile_bbb_* fixtures carry the second rule the fork added: a message is
refused unless every byte is printable ASCII.  They cover a newline, a tab, a byte with no
glyph and a NUL, on both entry points.  What they cannot see is the screen, so they cannot
tell a refusal apart from a rejection drawn after the summary; pijade/tools/t64_ascii_suite.py
measures that half against the running daemon.

Run it inside the jade-dev container, from the repo root mounted at /jade:

    docker exec jade-dev sh -lc \
      'cd /jade && LD_LIBRARY_PATH=/jade/build_linux/libjade python3 pijade/tools/t58_suite.py'

Two traps, both hit on 2026-09-10:

  * The library this loads is libjade.so, whose make target is `jade` (libjade/CMakeLists.txt).
    `make libjade_daemon` links jade_static and leaves libjade.so untouched, so a suite run
    after building only the daemon measures yesterday's code and can report a signature for a
    message the current source rejects.  Build with `make jade` before running.
  * test_jade.py reads a module-level `args`; it is normally set by its own argument parser,
    so a driver that imports it has to provide one.
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
    jade.get_version_info()
    assert jade.add_entropy(os.urandom(64)) is True
    assert jade.set_epoch(int(time.time())) is True
    assert jade.set_mnemonic(T.TEST_MNEMONIC) is True

    n_msg = len(list(T._get_test_cases(T.SIGN_MSG_TESTS)))
    n_file = len(list(T._get_test_cases(T.SIGN_MSG_FILE_TESTS)))
    print('fixtures: msg_*=%d  msgfile_*=%d' % (n_msg, n_file))
    assert n_msg >= 7 and n_file >= 17, 'fixture count too low; glob broken'

    T.test_sign_message(jade)
    print('PASS test_sign_message')
    T.test_sign_message_file(jade)
    print('PASS test_sign_message_file')

print('T58 SUITE OK')
