# ESP32 Display Firmware Update Guide

This guide explains how to update the firmware on the round touch display, using the UGN board's own USB port — you don't need to open up the display module or find its separate USB-C port.

> ✅ **Confirmed working end to end on real hardware** — entering the bootloader, writing and verifying all 4 files, and cleanly exiting back to the display's normal firmware. See "History" at the bottom for what it took to get there and what's still genuinely untested (mainly: Windows).

## What you need

- **Python 3**, installed on your computer.
- Two free tools, installed once — open a terminal/command prompt and run:
  ```
  pip install pyserial esptool
  ```
- **`flash_display.py`** — in this repo at `Tools/display-updater/flash_display.py`.
- **The new firmware** — you'll need 4 files together (not just one), listed in Step 2. Ask whoever built it for all 4, or see "Building the firmware yourself" below if that's you.
- **A USB cable**, connected to the UGN board's normal CLI port — the same one used for the STM32's own firmware updates, *not* the display's own USB-C port.

## Step 1 — turn the crystal TEC off

Connect to the UGN board like you normally would to type any command, and run:
```
crystal.tec off
```
Do this every time, right before flashing the display. It's a manual step for now — nothing stops you if you forget, so just make it a habit.

## Step 2 — flash the new firmware

Put these 4 files into `Tools/display-updater/build/` (create that folder if it doesn't exist yet):
- `KnobRGBControl.ino.bootloader.bin`
- `KnobRGBControl.ino.partitions.bin`
- `boot_app0.bin`
- `KnobRGBControl.ino.bin`

Then run this exact command from anywhere, replacing `<PORT>` with your board's port (`COM3` on Windows, something like `/dev/ttyACM0` on Linux/Mac) — you don't need to type full paths, the script looks for these filenames in that `build/` folder automatically:

```
python flash_display.py --port <PORT> write-flash -z 0x0 KnobRGBControl.ino.bootloader.bin 0x8000 KnobRGBControl.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 KnobRGBControl.ino.bin
```

You'll see progress output while it flashes, then **`Done.`** The display reboots into the new firmware automatically — no other steps needed.

## Troubleshooting

**Nothing happens / it times out right away:**
- Make sure no other program (a terminal app, PuTTY, Arduino's serial monitor, etc.) is also connected to the board — only one program can use the port at a time. Close it first, then try again.
- Type `status` on its own — if the `Display bridge:` line doesn't say `idle`, a previous attempt didn't clean up properly. Unplug and replug the board, then try again.

**It gets partway through and fails:**
- Just try again — a real flash takes a little while, and a one-off USB hiccup isn't unusual.
- Try a different USB cable — some cheap cables only carry power, not data.

**The screen looks dead/blank right after flashing:**
- Give it 15-20 seconds first (the display's own boot takes a moment).
- Still black? Connect with a plain terminal and run `status`. If `Display bridge:` says `idle` but the screen is still black, the exit signal didn't register and GPIO0 is stuck low (this shouldn't happen anymore — see "History" below for the bug that used to cause it — but if it ever does). Run `display_update_finished` by hand — that alone should bring the screen back within a couple seconds, without needing to reflash anything.
- If `Display bridge:` says anything other than `idle`, wait up to 60 seconds (the idle-timeout fallback) and check again before trying the manual command above.

**Still stuck:** run `status` and note what the `Display bridge:` line says, plus anything else the board printed — that narrows down which step it got stuck on. Pass that along when asking for help.

---

## Building the firmware yourself

1. Open `Laserfabriken_Display/KnobRGBControl/KnobRGBControl.ino` in Arduino IDE.
2. Install the libraries listed in `Laserfabriken_Display/README.md` §2.1 (`lvgl` 8.4.0, `ESP32_Display_Panel` 1.0.4, `esp-lib-utils` 0.2.3, `ESP32_IO_Expander` 1.1.1) if you haven't already — the build won't compile without them. Also make sure `lv_conf.h` sits *next to* (not inside) the `lvgl` library folder.
3. Set the board options exactly per `Laserfabriken_Display/README.md` §2.2 ("Board-inställningar") — that table is confirmed working on real hardware.
4. **Sketch → Export Compiled Binary.** Arduino compiles the sketch and drops the 4 files you need into a `build/<board-name>/` subfolder right next to the `.ino` file itself — that's where you'll find `KnobRGBControl.ino.bootloader.bin`, `KnobRGBControl.ino.partitions.bin`, `boot_app0.bin`, and `KnobRGBControl.ino.bin` afterward. Copy all 4 into `Tools/display-updater/build/` (a *different* `build/` folder — this script's own, not Arduino's) so Step 2's command can find them by name.

These settings affect what gets baked into the four files, so they matter; the flashing steps above don't need to know or care what they were.

**For your very first real `write-flash` test:** don't change any code — just rebuild and re-export the *current*, already-working `KnobRGBControl.ino` as-is. Flashing back the same firmware that's already on the display proves the STM32 bridge/`esptool` pipe actually works, without also debugging new display code at the same time. Only try an actual firmware change once that's confirmed.

---

## For developers / internal testing

### How this actually works

The STM32's USB-CDC CLI port doubles as a bridge to the display's own UART while it's held in its ROM bootloader — `flash_display.py` handles the choreography, `esptool` does the actual flashing protocol unmodified. Three plain-text CLI commands drive it, same as any other command (`help`/`status` etc.):

- **`display_update`** — acks `OK` immediately, then asynchronously power-cycles the display's 5V rail while holding its GPIO0 (boot-strap) pin low, forcing it into its ROM bootloader. Once done, prints `[bridge] ESP32 in ROM bootloader -- relay active` and starts relaying every subsequent byte on this port directly to/from the display's UART, unmodified — from this point on, this is no longer a text CLI, it's a raw pipe. Returns `ERR: busy` if a sequence or bridge session is already in progress.
- **Exit bridge mode** — sent as a serial **BREAK** condition (not text): `ser.send_break(duration=0.5)` in `pyserial`, after opening the port. The STM32 detects it, stops relaying, and resumes the normal text CLI, printing `[bridge] resuming normal CLI`. `flash_display.py` sends this and confirms that message actually comes back before moving on, retrying (default 3 attempts) if it doesn't.  A 60-second idle timeout (no bytes relayed in either direction) is a further fallback exit in case all retries are missed or the driving script crashes — it prints an idle-timeout warning and leaves GPIO0 low (display still in its bootloader) rather than guessing at a next step.
- **`display_update_finished`** — sets GPIO0 back high and power-cycles the display again so it boots the newly-flashed firmware normally. Unlike `display_update`, the `OK` here is deliberately deferred until the power-cycle has actually completed, not sent immediately on receipt. Also forces a full display re-sync at that point (laser power, wavelength, on/off, etc.) — the STM32 itself never reboots during any of this, so its own "have I already told the display this?" diff state would otherwise still think everything's already been sent, even though the freshly-flashed ESP32 has no memory of any of it. Without this, the screen would sit blank/default after an update until some real parameter change happened to occur on its own.
- **`display_boot0 low`** / **`display_boot0 high`** — drives GPIO0 directly, with no power-cycle and no bridge mode. For bench-testing that this one pin actually moves the ESP32 side, in isolation, before trusting the full sequence above.
- **`status`** includes a `Display bridge: idle / power-cycling / ACTIVE (flashing)` line.

**Why a BREAK condition, not the more obvious DTR/RTS toggle:** two reasons, one theoretical and one confirmed. Theoretical: `pyserial` and most OS serial drivers touch DTR/RTS as a side effect of simply opening a port, which would make them an unreliable "are we done" signal. Confirmed directly against `esptool`'s own source: on Linux, `esptool` opens its own connection to the port using `pyserial`'s untouched defaults (DTR/RTS both high) — so a design that treats "both high" as the exit signal would trigger the instant `esptool` connects, not when we want it to. Flipping the polarity doesn't help either: on Windows, `esptool` explicitly forces both lines *low* on open, for its own reasons (avoiding an unwanted board reset), which would trigger a "both low" design just as prematurely. There's no DTR/RTS polarity that's safe on both platforms, given what `esptool` itself does. A BREAK, by contrast, is never sent as a side effect of anything `esptool` or `pyserial` do on their own — confirmed directly against both projects' source, not assumed.

**About the `0x0 / 0x8000 / 0xe000 / 0x10000` offsets** in Step 2: this is the standard Arduino-ESP32-S3 flash layout — confirmed directly against the Arduino ESP32 core installed on this machine (`esp32/hardware/esp32/3.3.8/platform.txt`, `tools.esptool_py.upload.pattern_args`), not assumed. It's the same regardless of which board settings the sketch was built with — those are baked into the four files at compile time, not passed to `esptool` again at flash time. Double-check against Arduino IDE's own verbose upload log (Preferences → "Show verbose output during: upload") if you want independent confirmation.

`Tools/display-updater/flash_display.py` needs no build step — it's a plain Python script, git-tracked directly (unlike `Tools/firmware-updater/`, which compiles a Windows `.exe`).

### Bench-test order — recommended, though everything below has now passed on real hardware once

1. `display_boot0 low` / `display_boot0 high`, confirmed with a multimeter on the ESP32 side, before trusting the full sequence — given this exact set of strap pins has caused a real boot-failure bug before from a bad schematic read (see `display_pin_bindings.md`). Not run as an isolated step during actual testing, but GPIO0 moving correctly in both directions has since been confirmed indirectly (entry got the ESP32 into its bootloader; exit correctly booted it back into its app) — still worth doing the isolated version once for full confidence.
2. `flash_display.py --port <...> flash-id` — a harmless, read-only `esptool` command — as a first smoke test.
3. Only then attempt a real `write-flash`.
4. Separately, confirm the 60-second idle-timeout path by deliberately killing `flash_display.py` mid-transfer and checking the STM32 returns to normal CLI on its own, GPIO0 left low, with the documented warning message.

### History — what it took to get here

The first two real-hardware attempts both flashed and verified all 4 files successfully, but the exit signal failed both times — `display_update_finished` got typed while the STM32 was still (invisibly) relaying bytes to the ESP32 instead of processing it as a command, leaving GPIO0 low and the screen black until run again by hand.

Diagnosis added a temporary counter to the firmware tracking every `CDC_SEND_BREAK` request it received, regardless of value. It stayed at zero across repeated `ser.send_break()` calls — the break was never reaching the device *at all*, not a timing race. Root cause, found by checking the STM32's own USB descriptor: the CDC ACM functional descriptor's `bmCapabilities` byte only advertised support for line-coding/control-line-state (bit D1), not `Send_Break` (bit D2) — `Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c`, unmodified from ST's stock template. Linux's `cdc_acm` driver checks that capability bit before issuing a real `CDC_SEND_BREAK` USB request; with it clear, the driver silently declined to ever send one, no error surfaced anywhere. Fixed by setting that bit (`0x02` → `0x06`) in all three of the file's descriptor tables (HS/FS/OtherSpeed, though this board only uses FS).

With that fixed, a break request finally reached the firmware — but Linux's break sequence turned out to send *more than one* `SEND_BREAK` request per `ser.send_break()` call, and the last one observed had `wValue=0x0000` (a "stop break" value the firmware was, at the time, specifically filtering out, only accepting non-zero values as "break started"). Fixed by removing that filter entirely — the firmware now treats any `SEND_BREAK` arrival as the signal, regardless of value, since only "did a break happen" matters here, not start/stop semantics.

With both fixes in place, a full flash — entry, all 4 files written and verified, and clean exit back to the display's own firmware — completed successfully.

(A brief detour along the way: DTR/RTS was reconsidered as a simpler alternative to BREAK, then ruled back out after checking `esptool`'s source directly — see "Why a BREAK condition, not the more obvious DTR/RTS toggle" above for why that's a dead end regardless of platform.)

**Still genuinely unknown:**
- **Windows** — all testing so far has been on Linux (`/dev/ttyACM0`). The specific bug found was Linux's `cdc_acm` driver checking a capability bit before sending `SEND_BREAK`; now that the STM32 correctly advertises that capability, Windows' `usbser.sys` should have no reason to withhold it either, but that's untested, not confirmed.
- **M6_EN power-cycle timing** (300ms off / 500ms on) — worked every time so far, but was chosen generously rather than measured; still not confirmed against the display's actual measured discharge/settle behavior.
