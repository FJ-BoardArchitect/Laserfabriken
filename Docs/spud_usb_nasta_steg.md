# SPUD USB-länk — läget 2026-10-08 och nästa steg

Arbetsanteckning för att fortsätta där vi slutade. Bakgrund i `spud_usb_displaylank.md` (design) och `spud_usb_implementation_plan.md` (plan med status per steg).

---

## 1. Läget just nu

**Gren:** `usb-c-Test` i `Laserfabriken`, skapad från `origin/dev_fj` (den firmware som satt på UGN-kortet). Ingen tracking mot `dev_fj`. **Inget är committat** — allt nedan ligger som ändringar i arbetskatalogen:

| Repo | Fil | Vad |
|---|---|---|
| Laserfabriken | `Core/Inc/app/fw_version.h` (ny) | Enda källan för firmwareversion, nu 1.1.0 |
| | `USB_Device/App/usbd_desc.c`, `usb_device.c`, `laserfabriken_v2.ioc` | VID 0x1209, PID 0x0001, produktsträng, bcdDevice = version |
| | `Core/Src/app/cli.c`, `Core/Inc/app/cli.h` | Kanalabstraktion, hello-handskakning, lägesbyte, `#`-rader ignoreras, `[disp]`-spegling |
| | `Core/Src/app/rtt.c`, `Core/Inc/app/rtt.h` (nya), `CMakeLists.txt` | SWD-konsol (SEGGER RTT), bara Debug-byggen |
| | `USB_Device/App/usbd_cdc_if.c` | Anropar `CLI_OnUsbEnumerated()` vid varje enumerering |
| | `Core/Inc/bsp/bsp_config.h` | `BSP_DISPLAY_LINK_UART` |
| | `Tools/rtt-cli/rtt_cli.py` (ny) | Testverktyg: CLI över ST-Link |
| | `Docs/spud_usb_*.md` (nya) | Design, plan, den här filen |
| Laserfabriken_Display | `KnobRGBControl/stm_link.h/.cpp` (nya) | USB-host-transport + hello |
| | `KnobRGBControl/KnobRGBControl.ino` | `SerialSTM` borta, allt går via `stm_link_*` |
| | `README.md` | Nya board-inställningar |

**På UGN-kortet sitter** ett Debug-bygge av arbetskatalogen (steg 0–2 på STM32-sidan), flashat via ST-Link 2026-10-08 17:08. Verifierat via SWD-konsolen. Laser-effekten sattes till 50 under test och **återställdes till 0 och sparades**.

**Backup av den ursprungliga firmwaren** (dev_fj byggd på annan dator): `SPUD/backups/ugn_flash_before_usb_work_2026-10-08.bin`. Återställ med
`st-flash --reset write SPUD/backups/ugn_flash_before_usb_work_2026-10-08.bin 0x08000000`.

**Displayen** kör fortfarande gammal UART-firmware. Den nya koden är skriven men **inte kompilerad** (ingen `arduino-cli` på datorn).

**Fysiskt:** ST-Link V3 på ProgPAD. Displayen på FPC:n (5V + UART) **och** USB-C-kabel display → UGN. **UGN:s USB-C får inte kopplas till PC:n** (5V hårdkopplad till kontakten, skulle mata datorn baklänges). PC-terminal-testet görs därför aldrig på UGN; SWD-konsolen ersätter det.

---

## 2. Nästa steg: få displayen att enumerera STM32 (plan steg 2)

### 2.1 Kompilera displayen (Arduino IDE)

1. Öppna `Laserfabriken_Display/KnobRGBControl/KnobRGBControl.ino`.
2. Tools-menyn, **två ändringar** mot README:s tabell:
   - **USB Mode:** `USB-OTG (TinyUSB)`
   - **USB CDC On Boot:** `Disabled`
3. Kontrollera att ESP32-kärnan är **3.3.12** (Boards Manager → esp32 by Espressif). `USBHost`/`USBHostSerial` finns inte i äldre.
4. Verify/Compile. Kompileringsfel: kopiera felet ordagrant till Claude. Troligast: något i `stm_link.cpp` runt `tuh_mounted`/`tuh_vid_pid_get`/`CFG_TUH_DEVICE_MAX` (inkluderas via `USBHost.h`).

### 2.2 Flasha displayen

- Flytta displayens USB-C-kabel från UGN till PC:n.
- Håll **BOOT** nere när displayen får ström (ROM-bootloader) — auto-reset via 1200 bps fungerar inte när sketchen kör host-läge, så det här behövs **varje gång** från och med nu.
- Upload. Därefter: kabeln **tillbaka till UGN**. Behåll FPC:n.

### 2.3 Verifiera (Claude gör detta, säg bara till)

```
python3 Tools/rtt-cli/rtt_cli.py --listen 10
```
Förväntat i SWD-konsolen (displayens logg kommer via UART0 → STM32 → `[disp]`):
```
[disp] # link: host started, waiting for vid=1209 pid=0001
[disp] # link: up, vid=1209 pid=0001, hello sent
```
och STM32:s USB-kanal i maskinläge (`s_usb.machine == 1`, läses via SWD). Hello-svaret `hello UGN fw=1.1.0 proto=1` går till displayen över USB; displayens UI bör låsa upp sig (mode känd) men visar inga värden än — det är steg 3.

**Om det inte fungerar:**
- `USBHost.begin() failed` → board-inställningarna i 2.1 är fel (CDC On Boot).
- `host started` men aldrig `up` → antingen enumerering misslyckas (ESP-IDF 5.5.5-frågan från researchen, prova kärna **3.3.10**), eller VID/PID-matchning (loggen säger `ignoring device vid=... pid=...` i så fall).
- Inga `[disp]`-rader alls → displayens UART0 når inte STM32 (FPC), eller `Serial` är inte UART0 (CDC On Boot fortfarande Enabled).

### 2.4 Därefter

Plan steg 3 (STM32 skickar hela tillståndet efter hello + `get all`) och steg 4 (displayen visar det). Båda är små nu när transporten finns.

---

## 3. Obesvarat från i dag

- Reagerade displayen under steg 1-testet (laser-effekt 50 → 0 över UART)? Det är UART-regressionstestet.
- Statusutskriften visar kristalltemperatur ≈ −131 °C och TEC-buck PG-fel. Ser ut som att analog-/TEC-skenorna är strömlösa i bänkuppställningen. Väntat?
- Commit: du sa "senare". Förslag: en commit för steg 0–1 (STM32), en för steg 2 när displayen enumererar.

---

## 4. Snabbreferens

```bash
# STM32 (i Laserfabriken/)
cmake --build --preset Debug
st-flash --reset write build/Debug/laserfabriken_v2.bin 0x08000000
python3 Tools/rtt-cli/rtt_cli.py status                 # CLI över ST-Link
python3 Tools/rtt-cli/rtt_cli.py "hello display fw=1.0.0 proto=1" "set laser.power 0"
python3 Tools/rtt-cli/rtt_cli.py --listen 10            # bara lyssna (displayens logg)
python3 Tools/rtt-cli/rtt_cli.py --interactive
st-info --probe                                          # ser ST-Link chipet?
```
Bara en sak i taget får äga ST-Link: `rtt_cli.py` startar sin egen OpenOCD och stänger den när den är klar.

**Så här återupptar du med Claude:** öppna Claude Code i `Laserfabriken/`, säg t.ex. *"Fortsätt enligt Docs/spud_usb_nasta_steg.md, displayen är flashad"* (eller *"kompileringen gav det här felet: ..."*).
