#!/usr/bin/env python3
"""Flash the Laserfabriken UGN board's ESP32-S3 display through the STM32.

The display has no USB port of its own once mounted in the final product --
it's reachable only via a UART the STM32 also owns. This script asks the
STM32 to relay raw bytes between this PC and the ESP32's UART while it's
held in its ROM bootloader, runs the real `esptool` against that relay as
an ordinary subprocess, then tells the STM32 to stop relaying and boot the
new firmware. `esptool` itself is unmodified -- see
Docs/esp32_display_firmware_update.md for the full protocol and usage.

Requires: pip install pyserial esptool
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

# Drop Arduino's exported .bin files here (Tools/display-updater/build/) and
# just type their bare names on the command line -- see resolve_file_args().
BUILD_DIR = Path(__file__).resolve().parent / "build"


def resolve_file_args(esptool_args):
    """Any esptool arg that isn't already a valid path, as given, is looked
    up in BUILD_DIR instead -- lets you type `firmware.bin` instead of a
    full path every time, by dropping Arduino's exported files into this
    script's own build/ folder. Addresses (0x...) and flags never match an
    existing file either way, so they pass through untouched; a genuinely
    missing filename is left as-is too, so esptool still reports the real
    "file not found" error rather than this script silently swallowing it."""
    resolved = []
    for arg in esptool_args:
        if not arg.startswith("0x") and not Path(arg).exists():
            candidate = BUILD_DIR / arg
            if candidate.exists():
                arg = str(candidate)
        resolved.append(arg)
    return resolved


def send_command(ser, cmd, ack_marker, timeout_s=5.0):
    """Write `cmd` as a CLI line and read until `ack_marker` appears in the
    incoming bytes, or timeout. The STM32 CLI is line-oriented text with
    echo/prompts mixed in, not a clean request/response protocol, so this
    just scans the accumulated bytes rather than parsing a fixed reply."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode("ascii"))

    deadline = time.monotonic() + timeout_s
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            if ack_marker.encode("ascii") in buf:
                return buf
    raise TimeoutError(
        f"timed out waiting for {ack_marker!r} after sending {cmd!r} "
        f"(got: {buf!r})"
    )


def wait_for(ser, marker, timeout_s):
    """Like send_command's read loop, but without writing anything first --
    used to wait for an asynchronous message (e.g. bridge entry/exit)."""
    deadline = time.monotonic() + timeout_s
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            if marker.encode("ascii") in buf:
                return buf
    return None  # not required to succeed -- caller decides if that matters


def exit_bridge_mode(ser, attempts=3):
    """Send the BREAK exit signal and confirm the STM32 actually left bridge
    mode, retrying if not -- confirmed necessary on real hardware, not just
    theoretical: a single fire-and-forget break was observed to go
    unnoticed at least once, leaving `display_update_finished` typed while
    the STM32 was still silently relaying bytes to the ESP32 instead of
    processing it as a command (empty response, GPIO0 left low, display
    stuck black until run again by hand).

    Uses Serial.send_break(), not the break_condition property -- tried
    switching to break_condition (TIOCSBRK/TIOCCBRK, unambiguous explicit
    start/stop, in theory more reliable than send_break()'s POSIX-
    implementation-defined duration handling) but that raised
    "OSError: [Errno 95] Operation not supported" outright on real
    hardware -- this exact USB-CDC device/driver combo doesn't support
    those ioctls at all. send_break() at least doesn't error; the retry
    loop below is what actually makes it reliable, not which underlying
    primitive sends it."""
    for attempt in range(1, attempts + 1):
        print(f"Exiting bridge mode (attempt {attempt}/{attempts})...")
        ser.reset_input_buffer()
        ser.send_break(duration=0.5)
        if wait_for(ser, "resuming normal CLI", timeout_s=3.0) is not None:
            return True
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Flash the ESP32-S3 display through the STM32's USB-CDC bridge."
    )
    parser.add_argument("--port", required=True, help="STM32's USB-CDC serial port")
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="Must match USART1's fixed rate (115200) -- the STM32 bridge relays "
        "raw bytes and never reconfigures UART1's baud, so passing a higher "
        "value here would just make esptool try (and fail) to negotiate a "
        "post-handshake speed-up the bridge can't follow. Default is correct "
        "for this board; don't raise it.",
    )
    parser.add_argument(
        "esptool_args",
        nargs=argparse.REMAINDER,
        help="Everything after this script's own flags is passed straight "
        "through to esptool, e.g. write_flash 0x0 firmware.bin",
    )
    args = parser.parse_args()

    if not args.esptool_args:
        parser.error("no esptool command given, e.g. 'write_flash 0x0 firmware.bin'")

    resolved_esptool_args = resolve_file_args(args.esptool_args)
    if resolved_esptool_args != args.esptool_args:
        print(f"(resolved some filenames against {BUILD_DIR})")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    esptool_returncode = None
    try:
        print("Requesting bridge mode...")
        send_command(ser, "display_update", ack_marker="OK")
        wait_for(ser, "relay active", timeout_s=3.0)  # cosmetic only, not required

        ser.close()  # esptool needs to open the port itself, as its own process

        cmd = [
            sys.executable,
            "-m",
            "esptool",
            "--port",
            args.port,
            "--baud",
            str(args.baud),
            "--before",
            "no-reset",
            "--after",
            "no-reset",
            "--chip",
            "esp32s3",
        ] + resolved_esptool_args
        print("Running:", " ".join(cmd))
        esptool_returncode = subprocess.run(cmd).returncode

    finally:
        # Runs regardless of whether esptool succeeded -- leaving the ESP32
        # stuck in its ROM bootloader after a failed flash (looking
        # completely dead) is worse than clearly reporting the failure and
        # still returning the board to a bootable state.
        if not ser.is_open:
            # NOT `ser.closed` -- pyserial's Serial inherits a `.closed`
            # property from io.RawIOBase that's never wired to pyserial's
            # own open/close tracking, so it's always False regardless of
            # real port state. `is_open` is pyserial's actual flag.
            ser = serial.Serial(args.port, args.baud, timeout=0.2)
        if not exit_bridge_mode(ser):
            print(
                "WARN: STM32 never confirmed leaving bridge mode after retries. "
                "It should still recover on its own via the 60s idle timeout, "
                "but the display will stay stuck black (GPIO0 low, still in its "
                "bootloader) until you run 'display_update_finished' by hand "
                "once it does.",
                file=sys.stderr,
            )
        try:
            send_command(ser, "display_update_finished", ack_marker="OK", timeout_s=5.0)
        except TimeoutError as e:
            print(f"WARN: display didn't confirm reboot cleanly: {e}", file=sys.stderr)
        ser.close()

    if esptool_returncode != 0:
        print(
            f"WARN: esptool exited with code {esptool_returncode} -- flash may not "
            "have completed. The display has still been returned to normal boot.",
            file=sys.stderr,
        )
        sys.exit(esptool_returncode)

    print("Done.")


if __name__ == "__main__":
    main()
