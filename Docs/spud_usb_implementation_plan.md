# SPUD USB-länk — implementationsplan

**Status:** steg 0 och 1 klara och verifierade på UGN via SWD-konsol (2026-10-08). PC-terminal-test går inte på UGN: 5V är hårdkopplad till USB-C-kontakten, så en PC får inte sättas i. Första riktiga USB-testet blir steg 2 med displayen som host.
**Datum:** 2026-10-08
**Design:** `spud_usb_displaylank.md` (läs den först)
**Repon:** `Laserfabriken` (STM32, gren `usb-c-Test` från `origin/dev_fj`) och `Laserfabriken_Display` (ESP32-S3)

**Utgångsläge i `cli.c` (dev_fj):** skickar redan `set device.mode` periodiskt, strömmar `crystal.setpoint/temp/output/tec_voltage` till displayen, hanterar `crystal.tec on|off` och `display mode auto|spud|ugn`, och har en bryggmod (`display_update`, `display_boot0`) som flashar displayen via STM32:s UART. Planen bygger vidare på det, inte på `main`.

Planen följer ordningen: handskakning först, sedan värden STM32 → display, sedan display → STM32, sedan verifiering mot PC. Varje steg har ett testkriterium som ska vara uppfyllt innan nästa steg börjar. Steg 0–1 och 5 körs på UGN-kortet mot en PC-terminal, utan ny hårdvara.

---

## Steg 0 — STM32: identitet (descriptorer + versionsnummer) ✅

*Gjort 2026-10-08. Verifierat: descriptorn i chipets RAM läser VID 0x1209, PID 0x0001, bcdDevice 0x0110 (= 1.1.0). `lsusb`-kontrollen återstår tills kortets USB-C sitter i PC:n.*

**Varför först:** displayen ska öppna STM32 på VID/PID, så det måste finnas något att öppna.

| Fil | Ändring |
|---|---|
| `Core/Inc/app/fw_version.h` (ny) | `FW_VERSION_MAJOR/MINOR/PATCH` + `FW_VERSION_STR`. Enda källan; används av hello-svaret och `bcdDevice`. |
| `laserfabriken_v2.ioc` | `USB_DEVICE.VID`, `USB_DEVICE.PID_CDC_FS`, `USB_DEVICE.MANUFACTURER_STRING`, `USB_DEVICE.PRODUCT_STRING_CDC_FS`. Måste in i `.ioc`, annars skriver nästa "Generate Code" över dem. Öppna och spara i CubeMX en gång för att bekräfta att nycklarna tolkas. |
| `USB_Device/App/usbd_desc.c` | Samma värden (så bygget stämmer innan CubeMX körts). `bcdDevice` från `fw_version.h` i `USER CODE`-block. |

**Värden tills vidare:** VID `0x1209`, PID `0x0001` (pid.codes testpar, inte för produktion — öppen fråga i designdokumentet §9). Tillverkare `Laserfabriken`, produkt `Laserfabriken UGN` / `Laserfabriken SPUD` via `BSP_BOARD_NAME` i `bsp_config.h`.

**Test:** `lsusb -v` på en Linux-PC visar nytt VID/PID, produktsträng och `bcdDevice` = firmwareversion. CLI fungerar som förut.

---

## Steg 1 — STM32: kanalabstraktion + handskakning ✅

**Testverktyg:** `Tools/rtt-cli/rtt_cli.py` — CLI över SWD (SEGGER RTT via OpenOCD + ST-Link), bara Debug-byggen (`CLI_RTT`). Kör `python3 Tools/rtt-cli/rtt_cli.py status`. RTT-kanalen är en tredje `cli_chan_t` i `cli.c` med samma lägeslogik som USB-kanalen, så hela handskakningen kan köras utan USB: verifierat 2026-10-08 att eko + prompt finns i människoläge, att `hello display fw=1.0.0 proto=1` ger `hello UGN fw=1.1.0 proto=1` och därefter varken eko eller prompt, och att `set`/`status` fungerar i båda lägena.

*Implementerat 2026-10-08. Verifierat genom att skriva hello-rader direkt in i USB-mottagningsringen via SWD och läsa kanalens lägesflagga: bar `hello` ändrar inget, fel `proto` avvisas, rätt hello ger maskinläge, enumereringsflaggan återställer till människoläge. Testerna 1–4 nedan (riktig terminal, riktig display) återstår.*

**Avvikelse från planen:** `OK ...`/`ERR ...`-raderna skickas även i maskinläge (displayen ignorerar dem). Det som stängs av i maskinläge är eko, prompt och backspace-hantering. Protokollraderna som svar på `set` kommer i steg 6.

**Beteendeändring att känna till:** svar på kommandon från displayen går nu till displayen, inte till USB-terminalen. Tidigare såg en människa i terminalen `OK laser.power = 50%` när ratten vreds; nu ser den inget. Vill man ha en spårning av displaytrafik i terminalen får det bli ett `trace on`-kommando senare.

Det här är det användaren kallar "känna igen USB-C-ingången och tilldela roller". På STM32 är USB-rollen alltid device; det som tilldelas är **kanalens läge**: människa (terminal) eller maskin (display). Avgörs av hello, nollställs vid varje ny enumerering.

### 1.1 `cli.c`: en kanaltyp i stället för två kopior

```c
typedef struct {
    void   (*send)(const void *buf, uint16_t len);
    bool     machine;            /* false: eko + prompt + "OK ..."-text; true: bara protokollrader */
    char     line[CLI_BUF_LEN];
    uint16_t pos;
    volatile uint8_t  ring[CLI_RING_SIZE];
    volatile uint16_t head, tail;
} cli_chan_t;
```

- `s_usb` (USB CDC, `machine = false` vid boot).
- `s_uart` (USART1, `machine = true` alltid) — **bara när `BSP_DISPLAY_LINK_UART` är definierad** (UGN). På SPUD finns bara `s_usb`.
- `HAL_UART_RxCpltCallback` och `CDC_Receive_FS` fyller respektive kanals ring. Dagens `CDC_CLI_Receive()`/`BSP_CLI_Receive` ersätts av en `CLI_UsbRxBytes(buf, len)` som CDC-callbacken anropar.
- `process_line(cli_chan_t *ch, char *line)`: alla svar går via `ch->send`. `cli_send/cli_sendf` tar kanal som första argument.
- `disp_forward()` (spegling av terminalrader till displayen) tas bort.

### 1.2 Handskakningen

```
display -> STM32:  hello display fw=<ver> proto=1
STM32  -> display: hello <UGN|SPUD> fw=<FW_VERSION_STR> proto=1
```

- `hello display ...` på USB-kanalen → `machine = true`, svara hello. (Tillståndsdumpen efter hello läggs till i steg 3, inte här.)
- `hello display ...` på UART-kanalen (UGN) → svara hello, läget är redan maskin.
- Bryggmoden (`display_update`) och dess råa byte-relä ligger kvar på USB-kanalen oförändrade i detta steg; den är UGN-specifik och faller bort med UART-länken på SPUD (displayen flashas då via sin egen USB-C, se designdokumentet §7).
- `hello` från en terminal utan argument → svara hello i människoläge, läget ändras inte. Bekvämt för test.
- Tolkning: `sscanf(line, "hello display fw=%15s proto=%u", ...)`. Fel `proto` → svara `ERR proto` och stanna i människoläge.

### 1.3 Nollställning vid ny host

- `CDC_Init_FS()` i `usbd_cdc_if.c` körs vid varje `SET_CONFIGURATION`. Den anropar `CLI_OnUsbEnumerated()` som sätter `s_usb.machine = false`, tömmer ring och radbuffert. Körs i avbrottskontext: bara flaggor, ingen sändning.
- Vid nästa `CLI_Process()` i människoläge skrivs bannern + prompt igen (som `CLI_Init()` gör i dag).

### 1.4 `bsp_config.h`

```c
#define BSP_BOARD_NAME        "UGN"
#define BSP_DISPLAY_LINK_UART 1        /* SPUD: ta bort raden -> displayen går via USB-kanalen */
```

`BSP_DISPLAY_Send` behålls för UART-fallet. `BSP_CLI_Send/Receive`-makrona försvinner (kanalen har sin egen sändfunktion).

**Test (UGN, PC-terminal):**
1. Öppna terminal, prompt och eko som förut. `status`, `help`, `set` fungerar oförändrat.
2. Skriv `hello display fw=1.0.0 proto=1` → svar `hello UGN fw=1.1.0 proto=1`, därefter **inget eko och ingen prompt**. Skriv `set laser.power 50` → `OK laser.power = 50% ...` kommer fortfarande (ingen prompt efter).
3. Dra ur och sätt i USB-kabeln → banner + prompt är tillbaka, eko fungerar.
4. Displayen på UART (UGN) fungerar exakt som före ändringen (synken skickar `set laser.power` m.m.).
5. Bygget går igenom med `cmake --build --preset Debug` och RAM/Flash har inte vuxit orimligt.

---

## Steg 2 — ESP32: USB-host-transport + hello 🔧 (kod skriven, väntar på kompilering/flashning i Arduino IDE)

*2026-10-08: `stm_link.h/.cpp` skrivna ovanpå kärnans `USBHost` + `USBHostSerial` (ingen rå bulk-kod behövdes, kärna 3.3.12 har klassen). `SerialSTM` borttagen ur sketchen, `Serial` = UART0 = loggkanal som STM32 speglar till SWD-konsolen (`[disp] ...`). STM32-sidan ignorerar `#`-rader och väljer USB-kanalen som displaykanal så fort den är i maskinläge, även på UGN.*

"Industristandard" här: STM32 är en **USB CDC-ACM-device** (samma klass som varje USB-till-seriell-adapter). ESP32 använder ESP-IDF:s `usb_host`-bibliotek, som följer med Arduino-kärnan 3.3.12 (`usb/usb_host.h` + `libusb.a` finns bekräftat i `~/.arduino15/.../esp32s3-libs/3.3.12`). CDC-ACM-klassdrivrutinen (`cdc_acm_host`) följer **inte** med, men behövs inte: för ett textprotokoll räcker det att claima data-interfacet och köra bulk-överföringar.

### 2.1 Ny fil `KnobRGBControl/stm_link.h/.cpp` (ursprunglig plan -- utförandet blev enklare, se ovan)

API:t som resten av sketchen använder (ersätter `SerialSTM`):

```cpp
void   stm_link_begin();                 // startar host-stacken + tasks
bool   stm_link_connected();
size_t stm_link_write(const char *s);    // en rad inkl. "\r\n", blockerande tills skickad
int    stm_link_available();
int    stm_link_read();                  // en byte, -1 om tom
```

Internt:
1. `usb_host_install()` med default-konfig. En task kör `usb_host_lib_handle_events()` i evighet.
2. `usb_host_client_register()` med callback. En andra task kör `usb_host_client_handle_events()`.
3. `USB_HOST_CLIENT_EVENT_NEW_DEV` → `usb_host_device_open()` → `usb_host_get_device_descriptor()` → **jämför VID/PID** med steg 0. Fel par → stäng och ignorera.
4. Rätt par → `usb_host_interface_claim(client, dev, 1, 0)`. I ST:s CDC-descriptor är interface 0 kommunikation (interrupt-EP `0x82`) och interface 1 data (bulk IN `0x81`, bulk OUT `0x01`, 64 byte). Läs ändå endpoint-adresserna ur config-descriptorn i stället för att hårdkoda.
5. Allokera två `usb_transfer_t`: en IN (64 byte) som ligger ute permanent — callbacken lägger bytes i en ringbuffert och skickar om transfern direkt — och en OUT för `stm_link_write()`.
6. När claim lyckats: `stm_link_write("hello display fw=" FW_VERSION " proto=1\r\n")`, sätt `connected = true`.
7. `USB_HOST_CLIENT_EVENT_DEV_GONE` → avbryt transfers, släpp interface, stäng device, `connected = false`.

Ingen `SET_LINE_CODING`/`SET_CONTROL_LINE_STATE` behövs: ST:s CDC-device sänder oavsett DTR.

### 2.2 Board-inställningar (Arduino IDE)

| Inställning | Värde | Varför |
|---|---|---|
| USB Mode | `USB-OTG (TinyUSB)` | Host-stacken behöver OTG-PHY:n |
| USB CDC On Boot | `Disabled` | Annars startar TinyUSB *device* på samma PHY vid boot och host-install misslyckas |
| Upload Mode | `UART0 / Hardware CDC` | Oförändrat |

Konsekvens: `Serial` över USB finns inte längre. Debug: `HardwareSerial` på UART0 (GPIO43/44) mot en USB-UART-dongel, eller ingen. Dokumentera i displayens README.

**Verifiera** vid första bygget att `usb_host_install()` returnerar `ESP_OK` i OTG-läget. Gör den inte det är fallback-vägen ESP-IDF + Arduino som komponent + `usb_host_cdc_acm`, med samma `stm_link`-API utåt.

### 2.3 Testuppställning (att lösa innan steg 2 kan testas)

Displayen ska sitta i STM32-kortets USB-C med en C-till-C-kabel. På **UGN** är den kontakten samtidigt 12V-PD-ingången, så med displayen inkopplad får UGN ingen ström. Förslag, obekräftat:
- UGN:s MCU matas med 3V3 från ST-Link via `ProgPAD` (bara MCU-rail; TEC/5V-rails är döda, vilket räcker för handskakningen).
- Displayen matas med 5V på FPC-kontaktens pinne 1/20 från bänkaggregat (eller batteri om modulen har det).
- Alternativ: ett SPUD-prototypkort eller valfritt STM32-kort med USB-device som kör samma firmware.

**Test:**
1. Displayen enumererar UGN: `stm_link_connected()` blir sann, hello-svaret `hello UGN fw=... proto=1` syns i debug-UART.
2. Dra ur kabeln → `connected` falsk inom en sekund; sätt i → ny hello utan omstart av displayen.
3. Koppla in ett annat USB-device (t.ex. en ST-Link) → ignoreras, inget hänger.

---

## Steg 3 — STM32: skicka värden till displayen

- **Tillståndsdump efter hello** och på `get all` (båda lägena, i maskinläge utan `OK`-text):
  ```
  set laser.power <n>
  set crystal.wavelength <nm>
  crystal.setpoint <C>
  crystal.temp <C>
  crystal.output <x>
  laser on|off
  5v on|off
  ```
  Exakt de rader `process_stm_line()` i displayen redan tolkar.
- `cli_sync_display()` skickar till **displaykanalen**: UART på UGN, USB-kanalen om den är i maskinläge på SPUD. Aldrig till en USB-kanal i människoläge.
- Strömningen av `crystal.temp/output/tec_voltage` och den periodiska `set device.mode`-raden finns redan i `cli_sync_display()` på dev_fj. Ändringen är att de går till **displaykanalen** (UART på UGN, USB-kanalen i maskinläge på SPUD) och att `set device.mode` slutar skickas när hello har svarats. `stream <ms>`/`stream off` styr bara intervallet.
- `crystal.tec on|off` hanteras redan på dev_fj.

**Test (UGN, PC-terminal som spelar display):** `hello display fw=1.0.0 proto=1` → hello + hela dumpen. `get all` → samma dump. `stream 500` → två rader varannan halvsekund; `stream off` tystnar. I människoläge: `get all` ger samma rader, `status` oförändrat.

## Steg 4 — ESP32: visa värdena

- `drain_stm_uart()` läser från `stm_link_read()` i stället för `SerialSTM`. `process_stm_line()` oförändrad.
- Hello-svaret tolkas: `hello UGN ...` → `g_ui_mode = MODE_UGN`, `hello SPUD ...` → `MODE_SPUD`, `g_mode_known = 1`. Ersätter den periodiska `set device.mode`-raden. (Behåll den i parsern tills UART-vägen på UGN är avvecklad.)
- Frånkoppling (`stm_link_connected()` falsk) → visa "ingen kontroller"-läge i stället för gamla värden; återanslutning → vänta på ny dump (`g_values_known` nollställs).

**Test:** displayen visar samma värden som `status` i en terminal på UGN (kör terminalen via UART-debug eller kontrollera i tur och ordning). Ändra `set laser.power` på STM32 → displayen följer. Dra ur/sätt i → displayen går till "ingen kontroller" och tillbaka med rätt värden.

## Steg 5 — ESP32 → STM32: skicka värden

- `send_power()`, `send_wavelength()`, `send_crystal_setpoint()`, `send_laser_state()`, `send_crystal_tec_state()` skriver via `stm_link_write()`. Samma strängar som i dag.
- Displayen ändrar **inte** sitt visade värde när den skickar; den väntar på STM32:s rad (steg 6). Ratt/touch uppdaterar ett "önskat" värde som skickas, UI:t uppdateras från svaret. (Undantag kan behövas för rattens känsla — testa; om det känns segt, uppdatera lokalt men låt svaret vinna.)

## Steg 6 — STM32: ta emot och bekräfta

- I maskinläge svarar varje `set`, `laser on/off`, `5v on/off` med motsvarande tillståndsrad till **avsändaren** (samma rader som i steg 3). I människoläge som i dag (`OK ...`).
- Avvisade värden (t.ex. `laser.power 150`) svarar med det **faktiska** värdet, så displayen rättar sig.
- Diff-synken kvarstår för autonoma ändringar (temperaturvakten etc.).

**Test:** vrid på ratten → `set laser.power N` går till STM32 → `set laser.power N` kommer tillbaka → displayen visar N. `status` i terminal bekräftar. Framkalla vakt-trip med `set sim.laser_temp 99` → displayen visar laser av utan att ha rört något.

## Steg 7 — PC: samma sak från en dator

Inget nytt att bygga; detta är regressionstestet för att steg 1–6 inte bröt terminalen:
1. PC-terminal mot UGN: banner, prompt, eko, `help`, `status`, `set`, `save` oförändrat.
2. PC som låtsas vara display (`hello display ...`): får exakt det displayen får.
3. Byte mellan display och PC i samma port (på UGN: UART-display + PC samtidigt) stör inte varandra.
4. DFU via ROM-bootloader fungerar fortfarande med nytt VID/PID (ROM:en använder sitt eget, men kontrollera att `Tools/firmware-updater` inte filtrerar på det gamla).

## Steg 8 — SPUD-hårdvara (när kortet finns)

- `bsp_config.h`: ta bort `BSP_DISPLAY_LINK_UART`, sätt `BSP_BOARD_NAME "SPUD"`, nya makron för 5V-brytare (`BSP_VBUS_EN_*`) och VBUS-sense (`BSP_VBUS_SENSE_*`).
- `.ioc`: USART1 + `DISPLAY_DO1/2/3` bort, nya GPIO in, ADC_CS flyttad från PA4.
- CC-logik enligt designdokumentet §3 (UCPD + USBPD-middleware i Type-C-only, eller TUSB320). Try.SNK på.
- `BSP_VBUS_Enable()` bara när CC säger Source **och** VBUS-sense är låg.

---

## Vad som inte går att göra härifrån

- ESP32-koden kan inte testkompileras på den här datorn (ingen `arduino-cli`). Den kompileras i Arduino IDE av användaren efter varje steg.
- Steg 2–6 kräver testuppställningen i 2.3.
