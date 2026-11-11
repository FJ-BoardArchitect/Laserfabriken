# AGENTS.md — arbeta med Laserfabriken-korten

Det här dokumentet är till för en AI-agent, eller en människa, som ska bygga,
flasha, testa och vidareutveckla Laserfabrikens firmware. Det förutsätter ingen
viss dator, användare eller Linux-distribution. Två delar:

- **Del 1, setup:** vad som behövs på datorn och på bänken, per operativsystem,
  och hur man verifierar det.
- **Del 2, arbetssätt:** hur man bygger, flashar, testar, dokumenterar och
  committar, och vilka regler som gäller.

`README.md` beskriver vad firmwaren gör och hur den är uppbyggd. `Docs/`
beskriver enskilda delsystem. Det här dokumentet pekar dit i stället för att
upprepa dem.

**Status för plattformarna:** Arbetsflödet är utvecklat och verifierat på Linux.
Allt som rör Windows och macOS och är märkt *(ej verifierat)* bygger på
verktygens dokumentation. Det är inte provat mot korten än. Säg det i stället för
att låtsas att det fungerar.

---

# Del 1 — Setup

## 1.1 Repon

Två repon ska ligga i samma katalog, eftersom verktyg och dokument refererar till
`../Laserfabriken_Display`.

```
<arbetskatalog>/
├── Laserfabriken/            STM32G431-firmware (det här repot)
└── Laserfabriken_Display/    ESP32-S3-displayen (rund pekskärm + ratt)
```

```bash
git clone git@github.com:FJ-BoardArchitect/Laserfabriken.git
git clone git@github.com:FJ-BoardArchitect/Laserfabriken_Display.git
git -C Laserfabriken checkout usb-c-Test
git -C Laserfabriken_Display checkout usb-c-Test
```

| Gren | Innehåll |
|---|---|
| `usb-c-Test` | Aktiv utveckling. SPUD-kortet och displayen som USB-host på SPUD:s USB-C. |
| `main` | UGN-kortet och displayen över UART/FPC. `flash_display.py --variant uart` bygger härifrån med `git archive`. |
| övriga (`dev_fj` …) | Andra utvecklares grenar. Rör dem inte utan att fråga. |

## 1.2 Verktyg

| Verktyg | Behövs till | Testad version |
|---|---|---|
| `arm-none-eabi-gcc` + newlib | bygga STM32-firmware | 16.2 (≥ 12 bör fungera) |
| `cmake` ≥ 3.22, `ninja` | bygga (presets använder Ninja) | 4.4 / 1.13 |
| `python3` ≥ 3.8 | alla script i `Tools/`, inga pip-paket | 3.15 |
| `openocd` ≥ 0.12 | flasha och konsol över ST-Link (SWD) | 0.12.0 |
| `dfu-util` ≥ 0.10 | flasha STM32 över USB-C (DFU) | 0.11 |
| STM32CubeProgrammer (valfritt) | USB-C-flashning med option bytes, alla OS | — |
| Arduino IDE 2 eller `arduino-cli` | bygga och flasha displayen | ESP32-kärna **3.3.12+** |
| `esptool` (valfritt) | flasha displayen utan Arduino | — |
| `pdftotext` (poppler) | läsa datablad och specar | — |

Displayens Arduino-bibliotek, med detaljer i `Laserfabriken_Display/README.md` §2.1:
`lvgl` 8.4.0, `ESP32_Display_Panel` 1.0.4, `esp-lib-utils` 0.2.3 och
`ESP32_IO_Expander` 1.1.1. Lägg `lv_conf.h` i Arduino-bibliotekens rotmapp.
Board-inställningarna (FQBN) sätter `flash_display.py` själv.

### Linux

```bash
# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build \
                 openocd dfu-util poppler-utils python3
# Arch
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib cmake ninja openocd dfu-util poppler python
# Fedora
sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib cmake ninja-build openocd dfu-util poppler-utils
```

Behörigheter:

- **udev-regler** behövs för ST-Link (`0483:3748/374b/374e/374f/3753`), STM32 DFU
  (`0483:df11`) och ESP32-S3 (`303a:*`). OpenOCD-paketet brukar installera
  `60-openocd.rules`. Om reglerna saknas, kopiera `contrib/60-openocd.rules` från
  OpenOCD till `/etc/udev/rules.d/`.
- Kör sedan `sudo udevadm control --reload && sudo udevadm trigger`.
- **Seriella portar:** lägg användaren i gruppen `dialout` (Debian/Fedora) eller
  `uucp` (Arch), och logga in igen.

Om distributionens toolchain är för gammal, använd ARM:s egen från
developer.arm.com och lägg dess `bin/` först i `PATH`.

### macOS *(ej verifierat mot korten)*

```bash
brew install --cask gcc-arm-embedded      # ARM:s toolchain
brew install cmake ninja open-ocd dfu-util poppler python
brew install --cask arduino-ide           # eller: brew install arduino-cli
```

macOS behöver inga drivrutiner eller udev-regler. Seriella portar heter
`/dev/cu.usbmodem*`.

### Windows *(ej verifierat mot korten, utom kundflödet i `Docs/firmware_update_guide.md`)*

- Toolchain: *Arm GNU Toolchain* (installer från developer.arm.com, välj
  "add to PATH"), CMake och Ninja (`winget install Kitware.CMake Ninja-build.Ninja`)
  och Python (`winget install Python.Python.3.12`).
- OpenOCD: xPack-bygget (`xpack-dev-tools/openocd-xpack`), packa upp och lägg
  `bin\` i PATH.
- ST-Link-drivrutin: ST:s *STSW-LINK009*, eller WinUSB via **Zadig** på
  ST-Link-interfacet `MI_00`.
- `dfu-util`: finns som färdig fil i `Tools/firmware-updater/dist/`. Den kräver att
  **WinUSB** är bunden till `STM32 BOOTLOADER` (`0483:df11`) med Zadig, en gång per
  dator (se `Docs/firmware_update_guide.md` steg 1).
- STM32CubeProgrammer installerar en egen DFU-drivrutin. Den krockar med WinUSB:
  välj den ena eller den andra för DFU-enheten.
- ESP32-S3 i bootloadern syns som en COM-port utan extra drivrutin på Windows 10/11.
- Kör kommandona nedan i PowerShell. Byt `/` mot `\` i sökvägar vid behov och skriv
  `python` i stället för `python3`.

### Verifiera

```bash
arm-none-eabi-gcc --version; cmake --version; ninja --version
openocd --version; dfu-util --version; python3 --version
cd Laserfabriken
cmake --preset SPUD-Debug && cmake --build --preset SPUD-Debug   # slutar med en minnesrapport
cd ../Laserfabriken_Display
python3 Tools/flash_display.py --variant usbc --build-only
```

## 1.3 Hårdvaran på bänken

```
   PC ── USB ── ST-Link V2/V3 ── SWD ── M19 ┐
   │                                          │  SPUD (STM32G431RB)
   │                                          │  12 V i barrel-jacken (TEC + laser)
   └── USB-C ─── (vid DFU-flashning) ─────────┤  USB-C (UCPD, DRP)
                                              │  ZH M9 : LPUART1 "laser" → DK Photonics-modul
   Display (ESP32-S3) ══ USB-C (normal drift) ┘  M20   : USART2 "stage" → Thorlabs ELL18
```

| Del | Detaljer |
|---|---|
| ST-Link | V2 eller V3 på **M19**: SWDIO, SWCLK, GND och 3V3-referens. Det normala sättet att flasha, och den enda vägen till konsolen (RTT). Mät pinouten mot M19 innan du använder adapterkablar. En felkopplad adapter kan få kortet att försvinna från USB, och då loggar Linux `error -71`. |
| 12 V | Driver effektdelen (TEC, laser). Över bara USB-C startar MCU:n, men inte effektdelen. |
| USB-C | Normalt sitter **displayen** här, och den är USB-host. När kortet ska flashas över DFU kopplas PC:n hit i stället. |
| UGN-kortet | Det äldre kortet. ST-Link sitter på `ProgPAD`, och displayen sitter på FPC (M8). Se `README.md` §3. |

---

# Del 2 — Arbetssätt

## 2.1 Checklista vid sessionsstart

```bash
git -C Laserfabriken status -sb; git -C Laserfabriken_Display status -sb   # rent, i synk?
ninja --version                                     # presets kräver Ninja
python3 Laserfabriken/Tools/flash-board/flash_board.py --probe-only    # vilket kort på ST-Linken?
```

**Se också vilka USB-enheter som syns:**

| OS | Kommando |
|---|---|
| Linux | `lsusb` |
| macOS | `system_profiler SPUSBDataType` |
| Windows | Enhetshanteraren, eller `Get-PnpDevice -PresentOnly \| ? InstanceId -match '0483\|303A'` |

Leta efter två tillverkar-ID:n:

- `0483` är ST. `df11` betyder att STM32 är i DFU-läge, och `37xx` är ST-Link.
- `303a` är Espressif, det vill säga displayen i bootloadern.

**Kolla om en OpenOCD-server redan kör:**

| OS | Kommando |
|---|---|
| Linux | `ss -ltn \| grep -E ':4444\|:9090'` |
| macOS | `lsof -iTCP -sTCP:LISTEN \| grep -E '4444\|9090'` |
| Windows | `netstat -an \| findstr 4444` |

## 2.2 Bygga

```bash
cmake --preset SPUD-Debug   && cmake --build --preset SPUD-Debug    # utveckling, RTT-konsol
cmake --preset SPUD-Release && cmake --build --preset SPUD-Release  # leverans
cmake --preset UGN-Debug    && cmake --build --preset UGN-Debug
```

- Utdata hamnar i `build/<preset>/laserfabriken_<kort>.{elf,bin}`.
- Koden under `Core/` delas av korten. **Bygg båda korten** när du ändrar där.
- Om Ninja saknas: ändra inte presets. Bygg tillfälligt med
  `-G "Unix Makefiles"` (eller `"MinGW Makefiles"` på Windows) till en egen
  build-katalog och ge `--bin` till flash-scriptet. Se `README.md` §4.

## 2.3 Flasha STM32 över ST-Link (alla OS, rekommenderat)

```bash
python3 Tools/flash-board/flash_board.py --probe-only                 # bara identifiera
python3 Tools/flash-board/flash_board.py --build Debug --board SPUD --yes
```

Scriptet gör det här:

- Identifierar kortet på BOOT0-pinnen och vägrar fel binär.
- Flashar och verifierar.
- På SPUD sätter det option bytes (`nSWBOOT0=0, nBOOT0=1`, OPTR `0xFBEFF8AA`) och
  kontrollerar att MCU:n startar från flash.

Om en OpenOCD-server kör går flashningen genom den.

## 2.4 Flasha över USB-C, per OS

### STM32 (SPUD) via DFU

**När fungerar det?** STM32:s ROM-bootloader har USB-DFU (`0483:df11`, "STM32
BOOTLOADER", "DFU in FS Mode"). På SPUD är BOOT0-pinnen oansluten:

- Ett **nytt, blankt** kort hamnar i DFU av sig självt, och PC:n kan kopplas direkt
  till SPUD:s USB-C.
- När firmware och option bytes väl är satta startar chippet **alltid** från flash.
  Firmwaren har inget kommando för att hoppa till bootloadern. Ett flashat SPUD
  kan alltså bara uppdateras över ST-Link, eller genom att option bytes först
  återställs över ST-Link. Ett `dfu`-kommando i CLI:t är en öppen punkt.

**Option bytes måste sättas efter DFU-flashning.** Annars styr den svävande
BOOT0-pinnen var kortet startar efter nästa reset. STM32CubeProgrammer gör både
flashningen och option bytes i ett steg. Med `dfu-util` får man sätta option bytes
över ST-Link efteråt (`flash_board.py` gör det).

**Med STM32CubeProgrammer, alla OS:**

```bash
STM32_Programmer_CLI -c port=USB1 -w build/SPUD-Release/laserfabriken_spud.bin 0x08000000 -v
STM32_Programmer_CLI -c port=USB1 -ob nSWBOOT0=0 nBOOT0=1
```

Det går också att göra i GUI:t: anslut via USB, öppna `.bin`/`.elf`, tryck
*Download*. Under *OB → User Configuration* sätter man `nSWBOOT0` av och `nBOOT0` på.

**Med dfu-util:**

| OS | Förberedelse | Kommando |
|---|---|---|
| Linux | udev-regel för `0483:df11` (1.2) | `dfu-util -a 0 -s 0x08000000:leave -D build/SPUD-Release/laserfabriken_spud.bin` |
| macOS *(ej verifierat)* | ingen | samma som Linux |
| Windows | WinUSB via Zadig på "STM32 BOOTLOADER", en gång | `Tools\firmware-updater\dist\dfu-util.exe -a 0 -s 0x08000000:leave -D laserfabriken_spud.bin`, eller kundverktyget `firmware-updater.exe` (`Docs/firmware_update_guide.md`) |

Kontrollera först med `dfu-util -l`. Alt 0 är intern flash, alt 1 är option bytes
(`0x1FFF7800`) och alt 2 är OTP. Skriv aldrig till alt 1 eller 2 med en hemmagjord
fil. Ett fel där kan låsa chippet (RDP).

### Displayen (ESP32-S3) via sin egen USB-C

Displayen är USB-host i normal drift och syns då inte på PC:n. Så här flashar man
den:

1. Koppla loss displayen från SPUD.
2. Håll **BOOT** nedtryckt.
3. Anslut displayens USB-C till PC:n och släpp BOOT efter en sekund.

Den visas då som Espressif USB JTAG/serial (`303a:1001`).

| OS | Porten heter | Kommando |
|---|---|---|
| Linux | `/dev/ttyACM*` (hittas automatiskt) | `python3 Tools/flash_display.py --variant usbc` |
| macOS *(ej verifierat)* | `/dev/cu.usbmodem*` | `python3 Tools/flash_display.py --variant usbc --port /dev/cu.usbmodemXXXX` |
| Windows *(ej verifierat)* | `COMx` (Enhetshanteraren) | `python Tools\flash_display.py --variant usbc --port COM5` |

Två saker att veta om scriptet:

- Det letar bara automatiskt efter porten på Linux. På macOS och Windows måste du
  ange `--port`. Det väntar då inte på att du trycker BOOT, så sätt displayen i
  bootloadern innan du kör.
- Om scriptet inte hittar `arduino-cli`, ange `--arduino-cli <sökväg>`.

Det finns två alternativ till scriptet:

- **Arduino IDE.** Välj inställningarna i `Laserfabriken_Display/README.md` §2.2.
  Upload Mode ska vara `UART0 / Hardware CDC`. Välj porten och tryck Upload.
- **esptool direkt**, efter `--build-only`:
  `esptool --chip esp32s3 -p <port> write_flash 0x0 Tools/build/usbc/KnobRGBControl.ino.merged.bin`.
  Det förutsätter att filen `.merged.bin` finns i build-katalogen.

Varianten `uart` är för UGN och används på samma sätt. `demo` simulerar ett
STM32-kort inne i displayen.

## 2.5 Konsol och tester på hårdvaran

Konsolen är SEGGER RTT över ST-Link. Den finns bara i Debug-bygget. En OpenOCD
kan hålla ST-Linken åt gången, så kör en server och anslut allt annat till den:

```bash
python3 Tools/rtt-cli/rtt_cli.py --server       # terminal 1: håller ST-Link, telnet 4444, RTT 9090
python3 Tools/rtt-cli/rtt_cli.py --watch        # terminal 2: allt kortet skriver
python3 Tools/rtt-cli/rtt_cli.py "status"       # engångskommandon ansluter till servern
python3 Tools/rtt-cli/rtt_cli.py "laser on" "laser off"
```

**Regler för konsolen:**

- **Starta aldrig en egen OpenOCD och döda aldrig någon annans** när port 4444
  redan lyssnar. Alla script i `Tools/` ansluter till servern automatiskt.
- **Registeråtkomst** går via telnet 4444: `halt`, sedan `mdw`/`mww`, och avsluta
  med `reset run`.
- **Efter flashning genom servern** måste RTT synkas om: `rtt stop`,
  `rtt setup ...`, `rtt start`. `flash_board.py` gör det själv.

**CLI:t** (`Core/Src/app/cli.c`) har två lägen:

- *Människa*: eko, prompt och `OK ...`-svar.
- *Maskin*: displayprotokollet, bara rader.

Hela kommandolistan får du med `help` och finns i `README.md` §3. Några vanliga:
`status`, `adc`, `tec`, `crystal.tec on|off`, `laser on|off`,
`laser module on|off`, `set laser.maxcurrent <A>`, `uart monitor on|off` och
`uart <port> stats`.

**Testprincip:**

- Prova på riktig hårdvara innan du säger att något fungerar.
- Säg sedan exakt vad som är verifierat och vad som inte är det. Exempel:
  "tillståndsmaskinen når ready, men ingen lasermodul var inkopplad".
- Om något inte svarar, fråga vad som är inkopplat innan du drar slutsatser om
  trasig hårdvara.

## 2.6 Kodregler

- **Kortskillnader** hör hemma i `boards/<kort>/bsp_config.h`, inte i
  `#ifdef BOARD_SPUD` i delad kod. Även varianter väljs där, till exempel
  `BSP_LASER_SERIAL_DK` (SPUD) och `BSP_LASER_DAC8562S` (UGN).
- **Huvudloopen får inte blockera.** Drivrutiner är tillståndsmaskiner som körs
  från `*_Process()` i `main.c`. Förebild: `Core/Src/bsp/bsp_laser.c`, med timeout,
  omförsök och backoff.
- **CubeMX:** `boards/<kort>/Core/` genereras av CubeMX. Skriv bara inom
  `USER CODE BEGIN/END`. Bygget använder rotens `Drivers/`, `Middlewares/` och
  `USB_Device/`, och `USB_Device/App/` innehåller handändringar.
- **Radslut är blandade.** Behåll varje fils radslut. UGN:s `main.c` och SPUD:s
  `main.h` har CRLF, medan SPUD:s `main.c` och `Core/` har LF. Kontrollera innan du
  sparar, till exempel med `file <fil>`. Lägg inte till en `.gitattributes` som
  normaliserar radsluten.
- **Specifikationen styr.** Den gäller är *SW-SPEC-001 Mjukvaruspecifikation
  Laserfabriken* (2026-05-19). Be projektägaren om den. `Docs/mjukvaruspecifikation.md`
  är en äldre version. Om hårdvaran inte klarar ett krav (spec 50 µA, lasermodulen
  0,1 mA), implementera det närmaste och dokumentera avvikelsen. Ändra aldrig
  kravet i tysthet.
- **Säkerhetsspärrar.** Försvaga dem inte utan uttryckligt besked:
  - Lasern är av vid boot.
  - Laser och TEC är helt oberoende: `laser on|off` styr bara LASER_EN, `crystal.tec on|off`
    bara TEC_EN. Koppla inte ihop dem igen.
  - Strömmen klampas alltid mot `BSP_LASER_MAX_CURRENT_A` i `bsp_config.h`.
  - En ny mjukvarugräns under nuvarande ström sänker strömmen direkt.
- **Datablad och protokoll:** läs PDF:er med `pdftotext -layout`. Kontrollera
  exemplen i dem mot texten. Exemplet för "query all" i DK Photonics-protokollet
  har till exempel fel LEN.

## 2.7 Dokumentation

Uppdatera dokumentationen i samma commit som koden:

- `README.md` §3 ska innehålla kommandon och skillnader mellan UGN och SPUD.
- `Docs/spud_usb_displaylank.md` §12 ska innehålla SPUD-status, avvikelser mot
  specen och öppna punkter.
- Docs skrivs på svenska och kodkommentarer på engelska.
- Lägg testinstruktioner till kollegor, till exempel en tabell med förväntat
  beteende, där teamet når dem. Uppdatera befintliga i stället för att skapa
  dubbletter.

## 2.8 Git

- **Committa och pusha bara när någon ber om det.** Arbeta på `usb-c-Test`.
- Commit-meddelandet börjar med område: `SPUD: ...`, `cli: ...`, `Docs: ...`,
  `bring-up: ...`, `flash-board: ...`. Första raden säger vad som ändrades,
  brödtexten varför.
- En logisk ändring per commit. Om en ändring rör båda repona, committa i vart och
  ett och nämn kopplingen.
- Efter push: kontrollera `git status -sb` i **båda** repona. "Pushat" betyder inte
  "flashat". Säg vilket kort och vilken display som kör vilken commit.
- Ingen force-push och inga ändringar i andras grenar utan att fråga.

## 2.9 Kommunikation

- Teamet arbetar på svenska. Svara på svenska, kort och konkret.
- När hårdvaran krånglar, säg vad du ser (USB-listan, kernel- eller OpenOCD-loggen,
  `--probe-only`), vad det betyder och **ett** konkret nästa steg.
- Be inte om "testa igen" om inget har ändrats.
- Fråga innan saker som är svåra att ångra: option bytes, radering av flash,
  force-push och publicering.
- Agenten kan inte mata in `sudo`-lösenord eller trycka på knappar på kortet. Be
  människan göra det och säg exakt vad som ska göras.

## 2.10 Felsökning

| Symptom | Trolig orsak | Åtgärd |
|---|---|---|
| `ninja: not found` | Ninja saknas eller försvann vid en systemuppdatering | Installera om. Byt inte presets, bygg tillfälligt med Make (2.2) |
| OpenOCD: `LIBUSB_ERROR_BUSY` eller "unable to open" | En annan OpenOCD, STM32CubeProgrammer eller `st-flash` håller ST-Linken | Använd den körande servern eller stäng det andra programmet |
| OpenOCD hittar ingen ST-Link (Windows) | Drivrutin saknas | STSW-LINK009, eller Zadig/WinUSB på `MI_00` |
| Kortet försvinner från USB, `error -71` (Linux) | Kortsluten eller felkopplad SWD-adapter, eller dålig kabel | Koppla ur adaptern och mät pinouten mot M19 |
| Kortet syns som `0483:df11` efter reset | Blankt kort, eller option bytes inte satta (BOOT0 svävar) | Flasha med `flash_board.py`, eller sätt option bytes (2.4) |
| `dfu-util: No DFU capable USB device` | Inte i DFU, ingen behörighet (Linux) eller ingen WinUSB (Windows) | `dfu-util -l`, udev-regel eller Zadig |
| RTT tyst efter flashning | RTT-kontrollblocket flyttade | `rtt stop`, `rtt setup ...`, `rtt start` (`flash_board.py` gör det) |
| Displayen syns inte på PC:n | Den är USB-host, och det är normalt | Håll BOOT medan den får ström |

## 2.11 Var man hittar vad

| Fråga | Var |
|---|---|
| Arkitektur, UGN vs SPUD, kommandon, bygge | `README.md` |
| SPUD-status, USB-länken till displayen, öppna punkter | `Docs/spud_usb_displaylank.md` |
| Pinnar och konstanter per kort | `boards/<kort>/bsp_config.h`, `boards/<kort>/*.ioc` |
| CLI och displayprotokoll | `Core/Src/app/cli.c` |
| Laserdrivrutin (DK Photonics på LPUART1) | `Core/Src/bsp/bsp_laser.c`, `Core/Inc/bsp/bsp_laser.h` |
| TEC-styrning | `Core/Src/bsp/bsp_tec.c`, `Core/Src/app/tec_control.c` |
| Parameterlagring i flash | `Docs/parameter_storage.md` |
| Kundflashning på Windows (DFU) | `Docs/firmware_update_guide.md`, `Tools/firmware-updater/` |
| Displayens kod, UI-lägen, board-inställningar | `../Laserfabriken_Display/README.md`, `KnobRGBControl/` |
| Historik | `Docs/utvecklingshistorik.md`, `git log` |
