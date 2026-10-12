# Laserfabriken UGN — STM32-firmware

STM32G431-baserad temperaturstyrning (TEC) för Laserfabrikens laserkontroller. Det här dokumentet är en startpunkt för att bygga, förstå och vidareutveckla projektet — särskilt tänkt för någon som tar över utan att ha varit med i utvecklingen hittills.

För djupare detaljer om specifika delsystem, se `Docs/`:
- `mjukvaruspecifikation.md` — ursprunglig kravspecifikation
- `ugn_pinout_and_bringup_status.md` — pinout-historik, datablad-verifiering, bänktest-checklista
- `parameter_storage.md` — hur parametrar sparas i Flash, och slitage-budget
- `ntc_steinhart_hart.md` — hur NTC-koefficienterna är beräknade
- `firmware_update_guide.md` — flashningsguide för slutkund (Windows)
- `utvecklingshistorik.md` — historik

---

## 1. Vad det här är

Kortet styr temperaturen på en laserkristall (TEC = termoelektrisk kylare/värmare, via en Peltier-modul) och, på ett framtida kort, även en laserdiod. Reglering sker med PID mot en NTC-termistor. En rund pekskärm (ESP32-S3, separat repo `Laserfabriken_Display`) sitter fysiskt ihopkopplad via en FPC-kabel och fungerar som både display och inmatning (ratt + touch).

**Huvudkomponenter på kortet:**
- **STM32G431RBTx** — huvud-MCU
- **ADS1220** — extern 24-bitars ADC, läser NTC-termistorns spänning över SPI3
- **TPS563252** (programmerbar buck) + diskret polaritetsomkopplare (`M4_Polarity switch TEC` — 4 dubbla N-kanal-MOSFET-paket, Q1/Q2 FDC6420C + Q3/Q4 DMN3061SVTQ, kopplade som en H-brygga) — driver kristallens TEC, med DAC-styrd amplitud och GPIO-styrd riktning (värme/kyla)
- **TPS62172 / TPS62173** — interna 3.3V- respektive 5V-spänningsregulatorer
- **USB-C PD** — strömförsörjning in
- **M8 (AFC42-S20FMA-1H)** — FPC-kontakt till displaykortet: bär UART (kommunikation), 5V (strömförsörjer displayen), samt GPIO0/GPIO45/GPIO46 (ESP32-S3:s boot-strap-pinnar, se `Docs/ugn_pinout_and_bringup_status.md` om varför de måste ha rätt startnivå)

---

## 2. Arkitektur

```
┌─────────────────────────────────────────────────────────────┐
│  Applikationslager                                          │
│  tec_control.c   laser_control.c   params.c   cli.c         │
├─────────────────────────────────────────────────────────────┤
│  BSP-lager (Core/Inc/bsp, Core/Src/bsp)                     │
│  bsp_tec.c   bsp_temp.c   bsp_laser.c   bsp_config.h        │
├─────────────────────────────────────────────────────────────┤
│  STM32 HAL / CubeMX-genererad kod (gpio.c, spi.c, usart.c…) │
└─────────────────────────────────────────────────────────────┘
```

- **`tec_control.c`** — huvudloopen (`TEC_Control_Tick()`, körs var 100:e ms från `main.c`): läser temperatur, kör PID för kristall- och laser-TEC, hanterar buck-konverterns power-good-övervakning och lasertemperaturvakten (autonom säkerhetsavstängning).
- **`laser_control.c`** — laserdiodens ström/effekt-styrning (via `bsp_laser.c`, DAC8562S — se avsnitt 3).
- **`params.c`** — sparar PID-parametrar, våglängdsmodell och laser-inställningar i Flash. Se `Docs/parameter_storage.md` för fullständig förklaring av wear-leveling-schemat.
- **`cli.c`** — kommandoradsgränssnitt, tillgängligt både över USB CDC och över UART till displayen. Håller även display-tillståndet synkat (`cli_sync_display()`, se avsnitt 2.1).
- **`bsp_*.c`** — hårdvaruabstraktion. `bsp_config.h` är **det enda filen som ska ändras när man byter kort** (se avsnitt 3).

### 2.1 Kommunikation med displayen

USART1 (PA9/PA10) är en bidirektionell länk till ESP32-S3-displayen:
- **STM32 → display**: allt som skrivs i USB-CLI:t speglas ut (`disp_forward()`), plus en tillståndssynk (`cli_sync_display()` i `cli.c`) som — en gång per huvudloop-varv — jämför laser-effekt, våglängd, laser på/av och 5V på/av mot senast kända värden och skickar bara det som faktiskt ändrats. Det gör att displayen hålls i synk även när något ändras autonomt (t.ex. att lasertemperaturvakten stänger av lasern), inte bara vid explicita kommandon.
- **Display → STM32**: ratt-/knapptryckningar på displayen skickas som samma CLI-textkommandon (`set laser.power N`, `set crystal.wavelength N.NN`, `laser on/off`, `5v on/off`).
- Mottagningen är **avbrottsdriven** (`HAL_UART_RxCpltCallback` i `cli.c`), inte pollning — USART1:s mottagningsbuffert är bara 1 byte djup (FIFO avstängd), och pollning tappade tecken när huvudloopen var upptagen med annat (t.ex. SPI mot ADS1220). Rör inte detta tillbaka till pollning utan att förstå varför, se kommentarerna i `cli.c`.

---

## 3. UGN vs. Spudkortet

`bsp_config.h`s egen kommentar sammanfattar det kort: **"the ONLY file that changes when switching between boards."** Allt hårdvarunära (pinnar, vilken DAC-kanal, vilken SPI chip-select) är samlat där — resten av koden pratar bara med `BSP_*`-makron.

| | UGN (nuvarande kort) | Spudkortet (nästa kort) |
|---|---|---|
| Kristall-TEC | Ja — TPS563252 + polaritetsomkopplare, fullt implementerad | Samma, oförändrad |
| Laser-TEC | **Nej** — mjukvaran (PID, `BSP_TEC_Init/SetOutput`) finns och körs, men `bsp_tec.c` returnerar direkt (`if (ch != TEC_CRYSTAL) return;`) för allt som rör laserkanalen. Ingen fysisk drivkrets finns. | Planerad, men drivkretsen är **inte bestämd ännu** — se avsnitt 3.1 för det enda som faktiskt är beslutat |
| Laserdiodens ström | Kod finns (`bsp_laser.c`, DAC8562S över SPI3), men ingen fysisk DAC8562S är monterad på UGN | Ja — DAC8562S monteras |
| Laserns temperaturmätning | **Fejkad** — `bsp_temp.c` returnerar samma fysiska NTC-mätning för både `TEMP_CH_CRYSTAL` och `TEMP_CH_LASER` (UGN har bara en NTC) | Egen NTC för laserkanalen |

Det är alltså helt avsiktligt att laser-TEC:n inte gör något på UGN — det är inte en bugg som behöver fixas, det är en kortvariant som saknar hårdvaran.

Observera: `bsp_config.h`s kommentarer nämner fortfarande DRV8873S som en tänkt drivkrets för laser-TEC:n på Spudkortet — det är inte längre planen. Den enda konkreta, beslutade detaljen om Spudkortets pinout är DAC1/ADC_CS-konflikten nedan.

### 3.1 Pinkonflikten: DAC1 och ADC_CS

Det här är den konkreta anledningen till att pinouten måste ändras vid migrering till Spudkortet, inte bara en generell varning:

STM32G431:s interna DAC (DAC1) har två kanaler, med fasta fysiska pinnar:
- **DAC1_OUT1 = PA4**
- **DAC1_OUT2 = PA5**

Kristall-TEC:ns amplitud styrs redan via DAC1_OUT2 (PA5) — det är permanent upptaget och rör sig inte. Planen för laser-TEC:n (se `BSP_DAC_CH_LASER`-kommentaren i `bsp_config.h`) är att använda DAC1_OUT1 (PA4) för samma syfte på laserkanalen.

**Problemet:** PA4 är redan upptaget på UGN — det är `ADC_CS` (ADS1220:s chip select på den delade SPI3-bussen). Så länge ADC_CS ligger på PA4 kan DAC1_OUT1 inte användas för något annat.

**Lösningen vid migrering:** flytta `ADC_CS` till en annan, ledig pinne i CubeMX, vilket frigör PA4 för DAC1_OUT1. `DAC_CS` (till DAC8562S) ligger redan på en egen pinne (PA3) och krockar inte.

Gör pin-ändringen i CubeMX-GUI:t, inte genom att handpatcha genererade filer — annars riskerar en framtida "Generate Code"-körning skriva över ändringen. Se git-historiken (`git log --oneline -- laserfabriken_v2.ioc`) för exempel på när det gick fel tidigare i projektet.

---

## 4. Bygga projektet på en ny Linux-dator

### 4.1 Förutsättningar

```bash
# ARM-toolchain (arm-none-eabi-gcc m.fl.)
sudo apt install cmake ninja-build

# Toolchain: antingen via apt...
sudo apt install gcc-arm-none-eabi

# ...eller manuellt nedladdad från ARM (developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
# Projektets .vscode/settings.json förutsätter att den ligger på
# /usr/local/arm-none-eabi/bin — justera PATH nedan om din installation
# hamnar någon annanstans.
```

Verifiera:
```bash
arm-none-eabi-gcc --version
cmake --version
ninja --version
```

### 4.2 Konfigurera och bygga

Projektet har färdiga CMake-presets (`CMakePresets.json`):

```bash
export PATH="/usr/local/arm-none-eabi/bin:$PATH"   # justera vid behov

cmake --preset Debug
cmake --build --preset Debug
```

Resultatet hamnar i `build/Debug/laserfabriken_v2.elf` och `.bin`. Ett lyckat bygge avslutas med en minnesrapport (RAM/FLASH-användning) — inget felmeddelande betyder att allt gick bra.

Om du använder VS Code: `.vscode/settings.json` sätter redan rätt `PATH` för både `cmake.configureEnvironment` och `cmake.buildEnvironment` (båda krävs — den ena räcker inte, se förklaring i filen/kommentarerna i den här historiken om du undrar varför).

### 4.3 Flasha

**Under utvecklingen har flashning i praktiken skett med STM32CubeProgrammer** (ST:s eget GUI-verktyg, finns för Linux via ST:s hemsida). Öppna `build/Debug/laserfabriken_v2.bin` (eller `.elf`), välj rätt anslutning (ST-Link/SWD om ett ST-Link-probe är inkopplat, annars USB-DFU) och tryck Flash.

**Windows, utan ST:s proprietära mjukvara:** se `Docs/firmware_update_guide.md` — beskriver flashning med enbart fri/öppen källkod (`dfu-util`, Zadig/WinUSB), inklusive avsnittet "For developers" om ST-Link/SWD mot ett riktigt UGN-kort via `ProgPAD`.

Alternativ, kommandoradsbaserad väg (samma ST-Link/SWD-anslutning som ovan, mot UGN-kortets `ProgPAD`-header):
```bash
st-flash write build/Debug/laserfabriken_v2.bin 0x08000000
```
eller `Tools/firmware-updater --target=stlink`.

**Bekräftat:** flashning av ett riktigt UGN-kort har skett via **SWD med en extern ST-Link-probe**, inkopplad på kortets `ProgPAD`-header (SWDIO/SWDCLK/3V3/GND) — inte via USB-DFU. Se `Docs/firmware_update_guide.md`s "For developers"-avsnitt för detaljer om `ProgPAD`.

⚠️ **Fortfarande obekräftat:** det exakta sättet att sätta ett riktigt UGN-kort i USB-DFU-läge (BOOT0-header J5? finns den ens monterad?) — se `Tools/firmware-updater/FIRMWARE_UPDATER.md` §4 för status. SWD/ST-Link-flashningen ovan går inte via BOOT0 alls, så den löser inte den här frågan — DFU-läget är fortfarande bara relevant för den USB-baserade kundflödet, inte för utveckling.

---

## 5. Nästa steg för UGN

- **PID-tuning** — kristall- och laser-TEC:ns PID-parametrar är fortfarande konservativa standardvärden, inte tunade mot riktig hårdvara. Följ proceduren i `Docs/ugn_pinout_and_bringup_status.md` **kapitel 7** (Ziegler–Nichols-metod, inklusive säkerhetsanmärkningar i 7.2) — upprepa den inte här, den ändras för lätt att glömma om den finns på två ställen.
- Övriga öppna punkter som kräver verifiering på riktig hårdvara (POLARITY-riktning,).
- Bänktest-checklistan för ett nytt/nyflashat kort finns i **kapitel 9** i samma dokument.

---

## 6. Var man hittar vad

| Fråga | Var |
|---|---|
| "Varför gör TEC-koden X?" | `Core/Src/bsp/bsp_tec.c`, kommentarer förklarar historik (t.ex. varför EN inte längre rörs vid PG-fel) |
| "Vilka CLI-kommandon finns?" | `Core/Src/app/cli.c`, eller kör `help` i CLI:t |
| "Hur sparas parametrar, och hur länge håller Flashen?" | `Docs/parameter_storage.md` |
| "Vad är verifierat mot riktig hårdvara vs. bara mot databladet?" | `Docs/ugn_pinout_and_bringup_status.md` |
| "Hur pratar STM32 med displayen?" | Avsnitt 2.1 ovan, samt `Core/Src/app/cli.c` (`cli_sync_display`, `disp_rx_pop`) |
