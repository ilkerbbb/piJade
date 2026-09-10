#!/usr/bin/env python3
"""Build and run every custom libjade selfcheck in isolated directories."""

# BBB-AIRGAP: normal builds were measured to link neither migrated custom selfcheck. Build each
# selection under /tmp and require its unique runtime marker without touching normal build caches.

import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from jadectl import Jade, RpcError


REPO_ROOT = Path(__file__).resolve().parents[2]
SELFCHECKS = (
    ("descriptor", "Testing miniscript descriptors"),
    ("urldecode", "Testing validator and decoder for URL-encoded strings"),
    ("mining", "Testing bitcoin mining hash loop"),
)


def run_command(command):
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    ).returncode


def wait_for_socket(process, socket_path):
    # BBB-AIRGAP: bind() creates the path (libjade/daemon.c:330) but connect() only succeeds after
    # listen() (:336), so treating the path as readiness can hand Jade() an ECONNREFUSED and report
    # a live selfcheck as failed. Probe an actual connection instead. Closing the probe is safe:
    # the daemon accepts inside a loop (:344) and takes the next client.
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.settimeout(0.5)
                probe.connect(str(socket_path))
            return True
        except OSError:
            time.sleep(0.05)
    return False


def stop_daemon(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run_selfcheck(temp_root, name, marker):
    build_dir = temp_root / name
    configure = [
        "cmake",
        "-S",
        str(REPO_ROOT),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DLOG=ON",
        f"-DSELFCHECK={name}",
    ]
    if run_command(configure) != 0:
        return "configure"

    if run_command(["cmake", "--build", str(build_dir), "--target", "libjade_daemon", "-j8"]) != 0:
        return "build"

    socket_path = temp_root / f"{name}.sock"
    daemon_log_path = temp_root / f"{name}.log"
    daemon_path = build_dir / "libjade" / "libjade_daemon"
    with daemon_log_path.open("w+") as daemon_log:
        daemon = subprocess.Popen(
            [str(daemon_path), "--socketfile", str(socket_path), "--log-level", "info"],
            cwd=REPO_ROOT,
            stdout=daemon_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            if not wait_for_socket(daemon, socket_path):
                return "daemon startup"

            try:
                jade = Jade(str(socket_path))
                try:
                    response = jade.rpc("debug_selfcheck", timeout=120)
                finally:
                    jade.s.close()
            except (OSError, RpcError, socket.timeout):
                return "selfcheck RPC"

            if not isinstance(response, dict) or "error" in response or not isinstance(response.get("result"), int):
                return "selfcheck RPC"

            daemon_log.flush()
            daemon_log.seek(0)
            if marker not in daemon_log.read():
                return "missing marker"
        finally:
            stop_daemon(daemon)

    return None


def main():
    if "IDF_PATH" not in os.environ:
        print("libjade selfchecks: FAIL (IDF_PATH is not set)")
        return 1

    all_passed = True
    with tempfile.TemporaryDirectory(prefix="libjade-selfchecks-", dir="/tmp") as temp_dir:
        temp_root = Path(temp_dir)
        for name, marker in SELFCHECKS:
            failure_stage = run_selfcheck(temp_root, name, marker)
            if failure_stage is None:
                print(f"{name}: PASS (marker: {marker})")
            else:
                all_passed = False
                print(f"{name}: FAIL ({failure_stage})")

    print(f"libjade selfchecks: {'PASS' if all_passed else 'FAIL'}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
