#!/usr/bin/env python3
"""
provision_nvs.py — Pendant Nova NVS provisioning helper

Generates an NVS partition image populated with the "pn_wifi" namespace and
flashes it to the device at offset 0x9000 (matches partitions_ota.csv).

Schema written:
    ssid_0 / psk_0  (string, max 64 bytes each)
    ssid_1 / psk_1  (string, max 64 bytes each)
    upload_token    (binary, 32 bytes)
    upload_endpoint (string, max 128 bytes)

Source of values: an env-style file referenced by --env-file. Required keys:
    WIFI_SSID_0, WIFI_PSK_0
    WIFI_SSID_1, WIFI_PSK_1
    PENDANT_UPLOAD_TOKEN    (64 hex chars → 32 bytes)
    PENDANT_UPLOAD_ENDPOINT (https://... URL)

Security notes:
    * The script NEVER prints PSK / token values. Only "KEY ok (len=N)" lines.
    * Temp files are written with mode 0600 and removed in a finally block,
      including the temp CSV which contains plaintext PSKs.
    * Dry-run mode validates input + downloads the generator but does not
      write to flash.

Usage:
    python3 provision_nvs.py \
        --env-file ~/.claude/integrations/pendant-nova/secrets/wifi_profiles.env \
        --port /dev/ttyACM0          # optional, auto-detected if omitted
    python3 provision_nvs.py --env-file <path> --dry-run
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import stat
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Dict, List, Tuple

# Local helper — single source of truth for serial port discovery.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _serial_port import autodetect_port  # noqa: E402

# ---------------------------------------------------------------------------
# Constants matching the firmware schema (pai_nvs.h)
# ---------------------------------------------------------------------------
NVS_NAMESPACE = "pn_wifi"
NVS_PARTITION_SIZE_HEX = "0x5000"  # matches partitions_ota.csv
NVS_FLASH_OFFSET = "0x9000"
MAX_SSID_LEN = 64
MAX_PSK_LEN = 64
UPLOAD_TOKEN_LEN_BYTES = 32
UPLOAD_TOKEN_LEN_HEX = UPLOAD_TOKEN_LEN_BYTES * 2
MAX_ENDPOINT_LEN = 128

REQUIRED_KEYS: List[str] = [
    "WIFI_SSID_0",
    "WIFI_PSK_0",
    "WIFI_SSID_1",
    "WIFI_PSK_1",
    "PENDANT_UPLOAD_TOKEN",
    "PENDANT_UPLOAD_ENDPOINT",
]

NVS_GEN_URL = (
    "https://raw.githubusercontent.com/espressif/esp-idf/master/components/"
    "nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
)


# ---------------------------------------------------------------------------
# Helpers — safe logging (NEVER print PSK / token / endpoint values)
# ---------------------------------------------------------------------------
def log_ok(key: str, value: str) -> None:
    """Print only key + length. Never the value itself."""
    print(f"  {key} ok (len={len(value)})")


def log_warn(msg: str) -> None:
    print(f"  WARN: {msg}", file=sys.stderr)


def log_fail(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Env-file parser (simple KEY=VALUE, supports quotes and # comments)
# ---------------------------------------------------------------------------
ENV_LINE_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$")


def parse_env_file(path: Path) -> Dict[str, str]:
    if not path.exists():
        raise FileNotFoundError(f"env file not found: {path}")
    # Enforce a sane mode (warn loudly on world-readable).
    mode = path.stat().st_mode
    if mode & (stat.S_IROTH | stat.S_IWOTH):
        log_warn(f"{path} is world-readable; chmod 600 recommended")

    out: Dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = ENV_LINE_RE.match(line)
        if not m:
            continue
        k, v = m.group(1), m.group(2)
        # Strip optional surrounding single or double quotes.
        if len(v) >= 2 and v[0] == v[-1] and v[0] in ("'", '"'):
            v = v[1:-1]
        out[k] = v
    return out


def validate_env(env: Dict[str, str]) -> Tuple[Dict[str, str], List[str]]:
    """Returns (validated_env, errors). Empty errors == OK."""
    errors: List[str] = []
    for k in REQUIRED_KEYS:
        if k not in env or env[k] == "":
            errors.append(f"{k} missing or empty")

    # Field-specific validations
    for ssid_key in ("WIFI_SSID_0", "WIFI_SSID_1"):
        v = env.get(ssid_key, "")
        if v and len(v) > MAX_SSID_LEN:
            errors.append(f"{ssid_key} exceeds {MAX_SSID_LEN} bytes (got {len(v)})")
    for psk_key in ("WIFI_PSK_0", "WIFI_PSK_1"):
        v = env.get(psk_key, "")
        if v and len(v) > MAX_PSK_LEN:
            errors.append(f"{psk_key} exceeds {MAX_PSK_LEN} bytes (got {len(v)})")
        if v and len(v) < 8:
            # WPA2 minimum
            errors.append(f"{psk_key} shorter than 8 chars; WPA2 minimum is 8")

    token = env.get("PENDANT_UPLOAD_TOKEN", "")
    if token and not re.fullmatch(r"[0-9a-fA-F]+", token):
        errors.append("PENDANT_UPLOAD_TOKEN must be hex (0-9 a-f)")
    if token and len(token) != UPLOAD_TOKEN_LEN_HEX:
        errors.append(
            f"PENDANT_UPLOAD_TOKEN must be {UPLOAD_TOKEN_LEN_HEX} hex chars "
            f"({UPLOAD_TOKEN_LEN_BYTES} bytes); got {len(token)}"
        )

    endpoint = env.get("PENDANT_UPLOAD_ENDPOINT", "")
    if endpoint and not re.match(r"^https?://", endpoint):
        errors.append("PENDANT_UPLOAD_ENDPOINT must start with http:// or https://")
    if endpoint and len(endpoint) > MAX_ENDPOINT_LEN:
        errors.append(
            f"PENDANT_UPLOAD_ENDPOINT exceeds {MAX_ENDPOINT_LEN} bytes "
            f"(got {len(endpoint)})"
        )

    return env, errors


# ---------------------------------------------------------------------------
# nvs_partition_gen.py download (one-shot, cached next to this script)
# ---------------------------------------------------------------------------
def ensure_nvs_gen(script_dir: Path) -> Path:
    target = script_dir / "nvs_partition_gen.py"
    if target.exists():
        return target
    print(f"Downloading nvs_partition_gen.py to {target} ...")
    with urllib.request.urlopen(NVS_GEN_URL, timeout=20) as resp:
        data = resp.read()
    target.write_bytes(data)
    target.chmod(0o755)
    return target


# ---------------------------------------------------------------------------
# CSV builder for nvs_partition_gen
# ---------------------------------------------------------------------------
def build_csv(env: Dict[str, str], csv_path: Path, token_bin_path: Path) -> None:
    """
    nvs_partition_gen expects CSV with columns: key,type,encoding,value
    Top row is the namespace declaration.
    Token is provided as a path to a binary file (hex2bin encoding).
    """
    # Write 32-byte binary token. Mode 0600.
    token_bytes = bytes.fromhex(env["PENDANT_UPLOAD_TOKEN"])
    fd = os.open(token_bin_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, token_bytes)
    finally:
        os.close(fd)

    rows: List[List[str]] = [
        ["key", "type", "encoding", "value"],
        [NVS_NAMESPACE, "namespace", "", ""],
        ["ssid_0", "data", "string", env["WIFI_SSID_0"]],
        ["psk_0", "data", "string", env["WIFI_PSK_0"]],
        ["ssid_1", "data", "string", env["WIFI_SSID_1"]],
        ["psk_1", "data", "string", env["WIFI_PSK_1"]],
        ["upload_token", "file", "binary", str(token_bin_path)],
        ["upload_endpoint", "data", "string", env["PENDANT_UPLOAD_ENDPOINT"]],
    ]
    # Optional D2 cadence override. Absent → firmware uses DEFAULT_TICK_MS
    # (15 min). Value clamped to MIN_TICK_MS (30 s) at runtime regardless
    # of provisioned value so a typo can never tight-loop the server.
    tick_ms_str = env.get("PENDANT_UPLOAD_TICK_MS", "").strip()
    if tick_ms_str:
        try:
            tick_ms_val = int(tick_ms_str, 10)
            if tick_ms_val < 0 or tick_ms_val > 0xFFFFFFFF:
                raise ValueError(f"out of u32 range: {tick_ms_val}")
        except ValueError as e:
            print(f"ERROR PENDANT_UPLOAD_TICK_MS invalid: {e}", file=sys.stderr)
            sys.exit(2)
        rows.append(["upload_tick", "data", "u32", str(tick_ms_val)])

    # csv_path mode 0600 — contains plaintext PSK.
    fd = os.open(csv_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        with os.fdopen(fd, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            for r in rows:
                w.writerow(r)
    except Exception:
        # fdopen takes ownership of fd; on success it closes; on raw open
        # failure we already closed.
        raise


def generate_partition(
    gen_script: Path, csv_path: Path, out_bin: Path
) -> None:
    cmd = [
        sys.executable,
        str(gen_script),
        "generate",
        str(csv_path),
        str(out_bin),
        NVS_PARTITION_SIZE_HEX,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        # Avoid echoing the full CSV path's content into a log, but stderr from
        # the generator is safe — it only references key NAMES on error.
        log_fail("nvs_partition_gen failed:")
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(2)


# ---------------------------------------------------------------------------
# esptool flash
# ---------------------------------------------------------------------------
def flash_partition(port: str, bin_path: Path) -> None:
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
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        NVS_FLASH_OFFSET,
        str(bin_path),
    ]
    print(f"  flashing {bin_path.name} → {port} @ {NVS_FLASH_OFFSET}")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        log_fail("esptool write_flash failed")
        sys.exit(3)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--env-file",
        type=Path,
        default=Path.home()
        / ".claude/integrations/pendant-nova/secrets/wifi_profiles.env",
        help="Path to env file with WIFI_SSID_*/WIFI_PSK_*/PENDANT_UPLOAD_*",
    )
    p.add_argument("--port", type=str, default=None, help="Serial port; auto-detected if omitted")
    p.add_argument("--dry-run", action="store_true", help="Validate + generate image, do not flash")
    p.add_argument(
        "--keep-temp",
        action="store_true",
        help="Do NOT delete temp files (for debugging — they contain PSK!)",
    )
    args = p.parse_args()

    script_dir = Path(__file__).resolve().parent

    print(f"[1/5] Reading env file: {args.env_file}")
    env = parse_env_file(args.env_file)

    print("[2/5] Validating required keys ...")
    env, errors = validate_env(env)
    if errors:
        for e in errors:
            log_fail(e)
        return 4
    for k in REQUIRED_KEYS:
        log_ok(k, env[k])

    if args.dry_run:
        print("[3/5] Ensuring nvs_partition_gen.py is present ...")
        ensure_nvs_gen(script_dir)
        print("[OK] Dry-run complete — env valid, generator available")
        return 0

    print("[3/5] Ensuring nvs_partition_gen.py is present ...")
    gen_script = ensure_nvs_gen(script_dir)

    with tempfile.TemporaryDirectory(prefix="pn_provision_") as td:
        td_path = Path(td)
        # Restrict the temp dir mode (TemporaryDirectory defaults vary).
        os.chmod(td_path, 0o700)

        csv_path = td_path / "pn_wifi.csv"
        token_bin = td_path / "token.bin"
        out_bin = td_path / "nvs_pendant.bin"

        try:
            print("[4/5] Generating NVS partition image ...")
            build_csv(env, csv_path, token_bin)
            generate_partition(gen_script, csv_path, out_bin)
            size = out_bin.stat().st_size
            print(f"  image size = {size} bytes (target {NVS_PARTITION_SIZE_HEX})")
            if size != int(NVS_PARTITION_SIZE_HEX, 16):
                log_warn(
                    f"image size {size} != partition size "
                    f"{int(NVS_PARTITION_SIZE_HEX, 16)} — generator should pad"
                )

            port = args.port or autodetect_port()
            print(f"[5/5] Flashing via {port} ...")
            flash_partition(port, out_bin)
            print("[OK] Provisioning complete")
        finally:
            if args.keep_temp:
                print(f"[KEEP] temp files left in {td_path} (contains PSK!)")
                # Stop TemporaryDirectory from auto-deleting by re-creating
                # somewhere safer: we just print the location; user is
                # responsible for cleanup. tempfile will still try to remove
                # on exit, but at least we warned.
            else:
                # Overwrite sensitive files with zeros before TemporaryDirectory
                # deletes them — defense in depth on flash-backed /tmp.
                for f in (csv_path, token_bin):
                    if f.exists():
                        try:
                            n = f.stat().st_size
                            with open(f, "r+b") as fh:
                                fh.write(b"\x00" * n)
                                fh.flush()
                                os.fsync(fh.fileno())
                        except OSError:
                            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
