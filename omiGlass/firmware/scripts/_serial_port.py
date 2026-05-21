"""
_serial_port.py — shared serial-port discovery for firmware tooling.

Two callable surfaces, used by provision_nvs.py (unattended best-guess)
and flash_esp32.py (interactive list + pick).

  autodetect_port()        -> str
        Single best-guess. Raises RuntimeError if nothing plausible found.
        Order of preference (macOS > Linux > Windows), filtered for likely
        ESP32-class adapters (CDC-ACM, CP210x, CH34x, WCH, Silabs).

  list_serial_ports()      -> list[str]
        Full filtered list, suitable for interactive selection.

Filtering rationale:
    macOS exposes both /dev/tty.* (raw) and /dev/cu.* (callout). For programmatic
    flashing the callout variant is correct (does NOT block on DCD). We prefer
    cu.usbmodem* (Espressif/Apple-class CDC) over generic cu.* matches.

    Linux exposes /dev/ttyACM* (CDC-ACM, common on XIAO ESP32-S3) and
    /dev/ttyUSB* (USB-Serial bridges like CP2102/CH340). ACM is preferred when
    both are present because XIAO's onboard USB shows up as ACM.

This module intentionally has zero side effects at import time — no port
probing, no logging, no env-var reads.
"""

from __future__ import annotations

import glob
import platform
from typing import List


# Substrings that strongly suggest an ESP32-class USB-serial device on macOS,
# where /dev/cu.* enumerates everything including Bluetooth and IR.
_MAC_DEVICE_HINTS = ("usbmodem", "usbserial", "wchusb", "slab", "cp210", "ch34", "acm")


def list_serial_ports() -> List[str]:
    """Return a filtered list of candidate serial ports for the current OS.

    Order matters: the most likely match for the XIAO ESP32-S3 comes first.
    """
    system = platform.system()

    if system == "Darwin":
        # cu.usbmodem* is what XIAO ESP32-S3 native USB enumerates as. Sort it
        # ahead of looser cu.* matches by querying it first.
        primary = sorted(glob.glob("/dev/cu.usbmodem*"))
        secondary = sorted(
            p
            for p in glob.glob("/dev/cu.*")
            if any(hint in p.lower() for hint in _MAC_DEVICE_HINTS) and p not in primary
        )
        return primary + secondary

    if system == "Linux":
        # ACM first (XIAO native USB), then USB-Serial bridges.
        acm = sorted(glob.glob("/dev/ttyACM*"))
        usb = sorted(glob.glob("/dev/ttyUSB*"))
        return acm + usb

    if system == "Windows":
        # Lazy import so pyserial is only a hard dep on Windows. Returns
        # an empty list (caller prompts user) if pyserial is unavailable.
        try:
            from serial.tools import list_ports as _lp

            return [p.device for p in _lp.comports()]
        except ImportError:
            return []

    return []


def autodetect_port() -> str:
    """Return the single best-guess serial port, or raise RuntimeError.

    Raises:
        RuntimeError: when no plausible ESP32 port is present. The caller is
            expected to surface a helpful message asking the user to pass an
            explicit --port argument.
    """
    candidates = list_serial_ports()
    if not candidates:
        raise RuntimeError(
            "no serial port auto-detected; pass --port /dev/<your-port>"
        )
    return candidates[0]
