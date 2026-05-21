#!/usr/bin/env python3
"""
verify_nvs.py — Pendant Nova NVS verifier (S1 v1 — size-only check)

For Slice 1 we cannot interrogate the device over BLE for NVS contents yet
(that command parser ships in Slice 2). Instead, this verifier:

    1. Re-parses the same env file used for provisioning.
    2. Re-generates the NVS image into a temp file.
    3. Re-reads it back from flash via esptool read_flash.
    4. Compares the two binaries byte-for-byte.

This proves "what's on the chip == what we intended to flash" WITHOUT ever
reading back SSID / PSK / token values to the operator. Only PASS / FAIL and
mismatch byte counts are printed.

Usage:
    python3 verify_nvs.py --env-file <path> --port /dev/ttyACM0
"""

from __future__ import annotations

import argparse
import filecmp
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Reuse constants + helpers from provision_nvs to keep the source of truth single.
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import provision_nvs as pn  # noqa: E402


def read_flash(port: str, out_bin: Path) -> None:
    cmd = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        "460800",
        "read_flash",
        pn.NVS_FLASH_OFFSET,
        pn.NVS_PARTITION_SIZE_HEX,
        str(out_bin),
    ]
    print(f"  reading flash @ {pn.NVS_FLASH_OFFSET} size {pn.NVS_PARTITION_SIZE_HEX} → {out_bin.name}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("ERROR: esptool read_flash failed", file=sys.stderr)
        sys.exit(2)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--env-file",
        type=Path,
        default=Path.home() / ".claude/integrations/pendant-nova/secrets/wifi_profiles.env",
    )
    p.add_argument("--port", type=str, default=None)
    args = p.parse_args()

    env = pn.parse_env_file(args.env_file)
    env, errors = pn.validate_env(env)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 3

    gen_script = pn.ensure_nvs_gen(SCRIPT_DIR)
    port = args.port or pn.autodetect_port()

    with tempfile.TemporaryDirectory(prefix="pn_verify_") as td:
        td_path = Path(td)
        os.chmod(td_path, 0o700)

        csv_path = td_path / "pn_wifi.csv"
        token_bin = td_path / "token.bin"
        expected = td_path / "expected.bin"
        actual = td_path / "actual.bin"

        try:
            pn.build_csv(env, csv_path, token_bin)
            pn.generate_partition(gen_script, csv_path, expected)
            read_flash(port, actual)

            if filecmp.cmp(expected, actual, shallow=False):
                # NOTE: intentionally no value echoes; we only confirm parity.
                print(
                    f"[PASS] flash matches expected image "
                    f"(size={expected.stat().st_size} bytes)"
                )
                return 0

            # Mismatch — count differing bytes, never print contents.
            mismatched = 0
            total = 0
            with open(expected, "rb") as a, open(actual, "rb") as b:
                while True:
                    ca = a.read(4096)
                    cb = b.read(4096)
                    if not ca and not cb:
                        break
                    total += max(len(ca), len(cb))
                    for x, y in zip(ca, cb):
                        if x != y:
                            mismatched += 1
                    if len(ca) != len(cb):
                        mismatched += abs(len(ca) - len(cb))
            print(
                f"[FAIL] flash differs from expected: "
                f"{mismatched}/{total} bytes mismatched",
                file=sys.stderr,
            )
            return 1
        finally:
            # Best-effort zero of files with secrets before TemporaryDirectory removes them.
            for f in (csv_path, token_bin, expected, actual):
                if f.exists():
                    try:
                        n = f.stat().st_size
                        with open(f, "r+b") as fh:
                            fh.write(b"\x00" * n)
                            fh.flush()
                            os.fsync(fh.fileno())
                    except OSError:
                        pass


if __name__ == "__main__":
    sys.exit(main())
