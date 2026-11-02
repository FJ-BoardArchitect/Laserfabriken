#!/usr/bin/env python3
"""Identify a Laserfabriken board over ST-Link and flash the matching firmware.

Meant for programming new, blank boards: nothing on the board needs to run.
Everything goes through OpenOCD (SWD), so the script can read the hardware
before writing anything.

    flash_board.py                      # identify, flash Release build, verify
    flash_board.py --build Debug        # the Debug build (RTT console) instead
    flash_board.py --probe-only         # just say which board this is
    flash_board.py --board SPUD         # skip identification (blank board of known type)
    flash_board.py --bin path/to.bin    # explicit image (board still checked unless --board)
    flash_board.py --yes                # no confirmation prompt

How the board is identified (no firmware involved):
    PB8 is BOOT0. On UGN it has an external 10k pull-down (R19); on SPUD it is
    unconnected. With the MCU's internal ~40k pull-up enabled, UGN still reads
    0 and SPUD reads 1. Both must read 0 with the internal pull-down, otherwise
    something else is driving the pin and the script refuses to guess.
    When the next board revision gets a dedicated BOARD_ID strap, add it to
    SIGNATURES below -- the rest of the script does not care how a board is
    recognised.

After flashing SPUD the option bytes are set so the chip always boots from
main flash (nSWBOOT0=0, nBOOT0=1): with BOOT0 floating it would otherwise
land in the ROM bootloader on every reset.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

OPENOCD = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32g4x.cfg"]

# GPIOB registers and RCC AHB2ENR (STM32G431)
RCC_AHB2ENR = 0x4002104C
GPIOB_MODER, GPIOB_PUPDR, GPIOB_IDR = 0x48000400, 0x4800040C, 0x48000410
FLASH_OPTR  = 0x1FFF7800
OPTR_nSWBOOT0 = 1 << 26
OPTR_nBOOT0   = 1 << 27

# (pb8_with_pullup, pb8_with_pulldown) -> board
SIGNATURES = {
    (0, 0): "UGN",    # external pull-down on BOOT0 wins over the internal pull-up
    (1, 0): "SPUD",   # BOOT0 floating: follows whatever pull the MCU applies
}

def openocd(cmds, timeout=60):
    """Run OpenOCD with the given -c commands, return (ok, output)."""
    argv = list(OPENOCD)
    for c in ["init"] + cmds + ["shutdown"]:
        argv += ["-c", c]
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        sys.exit("openocd not found in PATH")
    except subprocess.TimeoutExpired:
        return False, "openocd timed out"
    out = p.stdout + p.stderr
    ok = "Error" not in out.replace("Error: stm32l4x option load failed", "") or "shutdown command invoked" in out
    return ok, out

def need(out, pattern):
    m = re.search(pattern, out)
    if not m:
        print(out, file=sys.stderr)
        sys.exit(f"could not find '{pattern}' in OpenOCD output -- is the board powered and the ST-Link connected?")
    return m

def probe():
    """Return dict with chip, voltage, optr, blank, pb8 readings, board guess."""
    set_pull = lambda v: f"mww {GPIOB_PUPDR:#x} [expr {{([mrw {GPIOB_PUPDR:#x}] & ~0x00030000) | {v:#x}}}]"
    cmds = [
        "halt",
        f"echo \"OPTR=[format 0x%08x [mrw {FLASH_OPTR:#x}]]\"",
        "echo \"FLASH0=[format 0x%08x [mrw 0x08000000]]\"",
        "echo \"FLASH1=[format 0x%08x [mrw 0x08000004]]\"",
        f"mww {RCC_AHB2ENR:#x} [expr {{[mrw {RCC_AHB2ENR:#x}] | 0x2}}]",          # GPIOB clock
        f"mww {GPIOB_MODER:#x} [expr {{[mrw {GPIOB_MODER:#x}] & ~0x00030000}}]",  # PB8 input
        set_pull(0x00010000), "sleep 5",
        f"echo \"PB8_UP=[expr {{([mrw {GPIOB_IDR:#x}] >> 8) & 1}}]\"",
        set_pull(0x00020000), "sleep 5",
        f"echo \"PB8_DOWN=[expr {{([mrw {GPIOB_IDR:#x}] >> 8) & 1}}]\"",
        set_pull(0x00000000),
        f"mww {GPIOB_MODER:#x} [expr {{[mrw {GPIOB_MODER:#x}] | 0x00030000}}]",   # back to analog
        "resume",
    ]
    ok, out = openocd(cmds)
    r = {}
    r["voltage"] = float(need(out, r"Target voltage: ([0-9.]+)").group(1))
    r["chip"]    = need(out, r"\] (Cortex-M4 r\dp\d) processor").group(1)
    r["optr"]    = int(need(out, r"OPTR=(0x[0-9a-f]+)").group(1), 16)
    f0 = int(need(out, r"FLASH0=(0x[0-9a-f]+)").group(1), 16)
    f1 = int(need(out, r"FLASH1=(0x[0-9a-f]+)").group(1), 16)
    r["blank"]   = (f0 == 0xFFFFFFFF and f1 == 0xFFFFFFFF)
    r["valid_vectors"] = (0x20000000 <= f0 <= 0x20008000) and (0x08000000 <= f1 < 0x08020000)
    r["pb8_up"]   = int(need(out, r"PB8_UP=(\d)").group(1))
    r["pb8_down"] = int(need(out, r"PB8_DOWN=(\d)").group(1))
    r["board"]    = SIGNATURES.get((r["pb8_up"], r["pb8_down"]))
    return r

def flash(path):
    ok, out = openocd([f"program {path} 0x08000000 verify", "reset", "resume"], timeout=180)
    if "** Verified OK **" not in out or "** Programming Finished **" not in out:
        print(out, file=sys.stderr)
        sys.exit("flashing failed")

def ensure_boot_from_flash(optr):
    if (optr & OPTR_nSWBOOT0) == 0 and (optr & OPTR_nBOOT0):
        return False
    # nSWBOOT0 -> 0, nBOOT0 -> 1 (mask covers both bits)
    openocd(["halt", "stm32l4x option_write 0 0x20 0x08000000 0x0C000000", "stm32l4x option_load 0"])
    return True

def read_pc_and_optr():
    ok, out = openocd(["halt", f"echo \"OPTR=[format 0x%08x [mrw {FLASH_OPTR:#x}]]\"", "reg pc", "resume"])
    pc   = int(need(out, r"pc \(/32\): (0x[0-9a-f]+)").group(1), 16)
    optr = int(need(out, r"OPTR=(0x[0-9a-f]+)").group(1), 16)
    return pc, optr

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", choices=["UGN", "SPUD"], help="skip identification and trust this")
    ap.add_argument("--build", default="Release", choices=["Debug", "Release"])
    ap.add_argument("--bin", help="image to flash (default: build/<BOARD>-<build>/laserfabriken_<board>.bin)")
    ap.add_argument("--probe-only", action="store_true")
    ap.add_argument("--yes", "-y", action="store_true", help="do not ask before flashing")
    a = ap.parse_args()

    print("Probing board over ST-Link ...")
    r = probe()
    print(f"  target voltage : {r['voltage']:.2f} V")
    print(f"  core           : {r['chip']} (STM32G431)")
    print(f"  option bytes   : {r['optr']:#010x}  "
          f"({'boot from flash' if (r['optr'] & OPTR_nSWBOOT0) == 0 else 'boot source = BOOT0 pin'})")
    print(f"  flash          : {'blank' if r['blank'] else ('firmware present' if r['valid_vectors'] else 'contains data, no valid vector table')}")
    print(f"  BOOT0/PB8      : pull-up -> {r['pb8_up']}, pull-down -> {r['pb8_down']}")
    detected = r["board"]
    print(f"  board          : {detected or 'UNKNOWN (no matching signature)'}")

    if a.probe_only:
        return

    if a.board:
        if detected and detected != a.board:
            sys.exit(f"refusing: you said --board {a.board} but the hardware looks like {detected}")
        board = a.board
    else:
        if not detected:
            sys.exit("cannot identify the board -- use --board UGN|SPUD if you are sure")
        board = detected

    path = a.bin or os.path.join(ROOT, "build", f"{board}-{a.build}", f"laserfabriken_{board.lower()}.bin")
    if not os.path.isfile(path):
        sys.exit(f"image not found: {path}\n  build it with: cmake --preset {board}-{a.build} && cmake --build --preset {board}-{a.build}")
    if a.bin and board.lower() not in os.path.basename(path).lower():
        sys.exit(f"refusing: image name '{os.path.basename(path)}' does not mention {board}")

    print(f"\nWill flash {board} with {os.path.relpath(path, ROOT)} ({os.path.getsize(path)} bytes)")
    if board == "SPUD" and (r["optr"] & OPTR_nSWBOOT0):
        print("Will also set option bytes nSWBOOT0=0/nBOOT0=1 (boot from flash; BOOT0 floats on SPUD)")
    if not a.yes and input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
        sys.exit("aborted")

    flash(path)
    print("  programmed and verified")
    if board == "SPUD" and ensure_boot_from_flash(r["optr"]):
        print("  option bytes written")

    pc, optr = read_pc_and_optr()
    running = 0x08000000 <= pc < 0x08020000
    print(f"  after reset    : pc={pc:#010x} ({'running from flash' if running else 'NOT in flash -- check BOOT0/option bytes'}), "
          f"option bytes {optr:#010x}")
    if not running:
        sys.exit(1)
    print(f"\nDone: {board} is running {os.path.basename(path)}.")

if __name__ == "__main__":
    main()
