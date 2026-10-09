# SPUD — USB-länk mellan STM32 och display

**Status:** proof of concept verifierad på UGN 2026-10-09 (se §11). Länken, protokollet och 5V-styrningen via CC fungerar på riktig hårdvara; SPUD-specifik hårdvara (§3.2) återstår.
**Datum:** 2026-10-08, uppdaterad 2026-10-09 (ersätter utkastet från 2026-10-06)
**Kodbas:** gren `usb-c-Test`, skapad från `origin/dev_fj` 2026-10-08 (den firmware som sitter på UGN-kortet). `main` saknar sex commits från `dev_fj`.
**Gäller:** SPUD-kortet (nästa kort efter UGN) och `Laserfabriken_Display`

---

## 1. Beslut

- SPUD använder **samma MCU som UGN, STM32G431**. Den har bara en USB-*device*-periferi och kan aldrig vara USB-host (se §2). Det tidigare utkastet, med STM32 som host, går därför inte att bygga.
- **STM32 är alltid USB-device.** Den som sitter i andra änden av kabeln är host.
- Två hosts förekommer, **aldrig samtidigt**:
  - **Displayen (ESP32-S3)** — host för data, men matar inte 5V. Den får sin 5V från SPUD-kortet via kabeln.
  - **En dator** — host för data, matar 5V på VBUS (som SPUD-kortet inte behöver).
- SPUD-kortet har **en USB-C-kontakt** för detta. Antingen sitter displayen i den, eller datorn. Ingen mux behövs eftersom bara en kabel någonsin är inkopplad.
- SPUD-kortet har **egen strömingång som inte är USB-C**. USB-C-kontakten bär alltså inte längre 12V PD som på UGN. Det frigör UCPD1 till CC-logiken i §3.
- UART-länken (USART1 PA9/PA10) **behålls på UGN**. Firmwaren ska kunna byggas för båda korten från samma kodbas, med valet i `bsp_config.h` (README §3: "the ONLY file that changes when switching between boards").
- Displayen är en **"setter"**: den skickar `set`-kommandon och visar värden som STM32 skickar tillbaka. STM32 äger sanningen.

---

## 2. Varför STM32 inte kan vara host

Två skilda saker som lätt blandas ihop:

| | USB-C-kontakten och CC-logiken | USB-datalänken (D+/D−) |
|---|---|---|
| Periferi på G431 | **UCPD1** (PB4/PB6) | **USB FS** (PA11/PA12) |
| Vad den gör | Avgör vem som matar ström (Source/Sink), förhandlar PD | Enumereras av en host, skickar och tar emot paket |
| G431:s förmåga | Full: Sink, Source eller DRP, med eller utan PD | **Endast device.** Ingen host-controller finns i kretsen. |

G431 kan alltså förse displayen med 5V över kontakten, men den kan aldrig enumerera displayen. Därför är **ESP32-S3 host**. Den har USB OTG med host-stöd.

**Konsekvens för rollerna.** I Type-C följer dataroll normalt strömroll (Source = host, Sink = device). Här bryts det: SPUD är strömkälla men data-device, displayen är strömsänka men data-host. Det är tillåtet — ESP32:s host-stack kontrollerar aldrig CC, och STM32:s device-stack bryr sig inte om vem som matar VBUS. Det enda som måste vara rätt är att 5V-brytaren bara slås på mot displayen, aldrig mot en dator (§3).

---

## 3. Ström: 5V ut till displayen, aldrig in i datorn

Datorn matar alltid sin egen 5V på VBUS. Om SPUD också driver 5V på samma ledare hamnar två källor mot varandra. SPUD måste därför **veta vem som sitter i kontakten innan 5V-brytaren slås på**. Det är exakt vad CC-pinnarna i USB-C finns för.

### 3.1 Så avgörs det

- **Displayen** har pull-downs (Rd) på CC — det har alla ESP32-moduler, de är byggda som devices. Rd betyder "jag vill ha ström". *Verifierat på UGN 2026-10-09.*
- **En dator** har pull-ups (Rp) på CC och matar 5V. Rp betyder "jag ger ström". Med en **USB-A-till-C-kabel** sitter Rp (56 kΩ) i kabeln och datorns USB-A-port lägger på 5V direkt, utan att fråga. *Verifierat på UGN 2026-10-09: STM32 ser kabelns Rp som "default USB power".*
- **SPUD:s port är DRP (Dual Role Port) på CC-nivå, utan PD.** Den växlar mellan att erbjuda (Rp) och fråga (Rd):
  - Ser den displayens Rd → SPUD blir Source → **5V-brytaren på**.
  - Ser den datorns Rp → SPUD blir Sink → **5V-brytaren av**.
  - Kabeln dras ur → CC öppen → **5V-brytaren av** direkt.

Datarollen påverkas inte av detta. STM32 är device i båda fallen.

### 3.2 Hårdvara på SPUD

1. **5V-lastbrytare** från kortets 5V-regulator till kontaktens VBUS, **med backströmsskydd** (reverse-current blocking), så att en dators 5V aldrig kan läcka in i kortet om brytaren är av.
2. **CC-logik för DRP utan PD: STM32:s egen UCPD1 (PB4/PB6), styrd av egen firmware (`usbc_port.c`).** Beslut 2026-10-09, ersätter alternativen "ST:s USBPD-middleware" och "extern TUSB320". Ingen extra komponent och ingen middleware: UCPD används bara som Rp/Rd-växlare med spänningskomparatorer, och vår egen tillståndsmaskin växlar roller. Verifierat på UGN (§11). Eftersom vi äger tillståndsmaskinen kan Try.SNK (§3.3) läggas in i firmware — det var den saknade Try.SNK-funktionen i ST:s middleware som tidigare pekade mot TUSB320.
3. **VBUS-sense** (spänningsdelare till en GPIO). Säkerhetskontroll innan 5V-brytaren slås på. Saknas på UGN; där räcker CC-detekteringen, men på SPUD ska 5V aldrig slås på om VBUS redan bär spänning (t.ex. en felaktig kabel utan Rp).
4. **Strömväg från USB när kortet saknar egen matning.** Utan barrel-jack ska en dator kunna mata kortet (CLI, firmwareuppdatering), med barrel-jack ska kortet inte dra ström från datorn. Lös med en ideal-diod/power-mux (t.ex. TPS2121 med barrel som prioriterad källa, eller LM66100-par) in på kortets matning. På UGN sker detta av en slump: datorns 5V når kortet baklänges genom 5V-regulatorn, vilket fungerar men inte är en konstruktion.
5. **Barrel-detekt till en GPIO.** Utan barrel-jack räcker USB-strömmen (0,5–1,5 A vid 5V) inte till TEC eller laser. Firmware ska då vägra `laser on` och TEC-enable.

### 3.3 Fallgrop: datorer som också är DRP

De flesta bärbara har DRP-portar. Om båda sidor växlar avgör slumpen vem som blir Source. Blir SPUD Source mot en bärbar blir datorn Sink — och i en bärbars värld betyder det att den försöker bli *device*. Då finns ingen host på bussen och CLI:t är dött.

Type-C-specen löser det med **Try.SNK**: SPUD föredrar Sink när motparten också är DRP, men blir Source när motparten är ren Sink (displayen). **Implementeras i `usbc_port.c`:** när porten som Source ser Rd, presentera Rd en kort stund (tDRPTry, ~100 ms) innan 5V slås på. En DRP-dator visar då Rp och SPUD blir Sink; displayen (ren Sink) visar aldrig Rp och SPUD återgår till Source. **Ej testat än:** UGN-testet gjordes med USB-A-kabel, där datorn är ren Source och problemet inte uppstår. Behöver testas med en C-till-C-kabel mot en bärbar med DRP-port.

### 3.4 Displayens sida

Inget behöver ändras i displayens hårdvara. Dess USB-C-kontakt har redan Rd, tar emot 5V och ger ESP32:n D+/D−. Displayen (JC3636K718) har ingen batterimatning i vår uppställning: den lever helt på 5V från kortet (verifierat: den släcks när kortets 5V slås av). Utan barrel-jack på SPUD fungerar alltså inte displayen. Det som försvinner är FPC-kabeln och boot-strap-styrningen (`DISPLAY_DO1/2/3`): displayen bootar fristående på sina egna strap-pull-ups, som alla ESP32-moduler.

---

## 4. Identifiering i två lager

Rollerna har vänt jämfört med det gamla utkastet: det är nu **STM32 som identifierar sig via descriptorer**, och displayen som kontrollerar dem.

**USB-descriptorer** (STM32, `USB_Device/App/usbd_desc.c`) svarar på "vad sitter i porten":
- Eget VID/PID i stället för ST:s generiska `0x0483/0x5740` ("STM32 Virtual ComPort"), som är omöjligt att skilja från vilket ST-utvecklingskort som helst.
- Produktsträng `Laserfabriken SPUD`/`Laserfabriken UGN`, firmwareversion i `bcdDevice`.
- Displayens host-stack öppnar **bara** rätt VID/PID och ignorerar allt annat.
- Descriptorerna måste sättas i CubeMX (`.ioc`), inte bara handpatchas i `usbd_desc.c`, annars skrivs de över vid nästa "Generate Code" (README §3.1 om varför).

**Hello-handskakning** svarar på "vilken protokollversion och vilket läge":
```
display -> STM32:  hello display fw=1.0.0 proto=1
STM32  -> display: hello SPUD fw=x.y.z proto=1
STM32  -> display: <hela aktuella tillståndet, samma rader som 'get all'>
```
- Ersätter dagens `set device.mode <UGN|SPUD>`, som firmwaren (gren `dev_fj`) skickar om var 1 s utan att veta om displayen lyssnar: displayen får kortets typ i hello-svaret i stället.
- Tar bort boot-race-problemet: displayen vet exakt när STM32 finns, för det är displayen som enumererar den. Vid frånkoppling får displayen en händelse från host-stacken och kan visa "ingen kontroller" i stället för gamla värden.

### 4.1 Samma port, två sorters host — hur STM32 vet vem som pratar

På SPUD kommer både datorn och displayen in genom **samma CDC-endpoint**. STM32 kan inte se skillnad på dem i USB-lagret. Därför:

- CLI-kanalen startar i **människoläge**: eko av tecken, prompt `> `, beskrivande svar (`OK laser.power = 50%  -> I=0.8000 A`). Precis som i dag.
- När första raden som kommer in är `hello display ...` byter kanalen till **maskinläge**: inget eko, ingen prompt, bara protokollrader (§5).
- Vid **varje ny enumerering** (ny host, kabel i/ur) återställs kanalen till människoläge. Kroken är CDC-klassens init-callback (`CDC_Init_FS` i `usbd_cdc_if.c`), som körs vid varje `SET_CONFIGURATION`.
- Tillståndssynken (`cli_sync_display()`) skickar **bara** i maskinläge. Annars får en terminal på datorn oombedda `set`-rader.

På UGN finns två fysiska kanaler (USB för människa, UART för display) och UART-kanalen är alltid i maskinläge. Samma kod, bara en kanal till.

---

## 5. Protokoll

USB har CRC och omsändning i hårdvara, så textradsprotokollet i `process_line()` fungerar oförändrat över CDC. Radformatet **behålls exakt som displayen redan tolkar** (`process_stm_line()` i `KnobRGBControl.ino`), så att en parser på displaysidan hanterar båda riktningarna. Det gamla utkastets förslag om ett nytt kortformat (`x y` utan `set`) stryks — det skulle bara kosta en parserändring utan vinst.

### 5.1 Display → STM32 (oförändrat)

```
hello display fw=<ver> proto=1
set laser.power <0-100>
set crystal.wavelength <nm>
set crystal.setpoint <C>
laser on | laser off
5v on | 5v off
crystal.tec on | crystal.tec off
get all
stream <ms> | stream off
```

### 5.2 STM32 → display (maskinläge)

Alltid det **auktoritativa** värdet, oavsett om det ändrades av displayen, av terminalen eller autonomt (t.ex. att lasertemperaturvakten stänger av lasern):

```
hello <UGN|SPUD> fw=<ver> proto=1
set laser.power <n>
set crystal.wavelength <nm>
crystal.setpoint <C>
crystal.temp <C>
crystal.output <-1..1>
laser on | laser off
5v on | 5v off
```

- **Tydlig setter-roll.** Displayen uppdaterar sitt UI enbart från dessa rader, aldrig från sitt eget inmatade värde. STM32 svarar på varje `set` med motsvarande rad ovan (i dag gäller det bara via diff-synken, och svaret `OK ...` går till USB-terminalen i stället för till avsändaren).
- **`get all`** skickar hela listan ovan. Används av displayen vid (åter)anslutning efter hello.
- **`stream <ms>`** slår på periodisk sändning av `crystal.temp`/`crystal.output` med angivet intervall, `stream off` stänger av. Ersätter att displayen frågar.
- **Inget eko, ingen prompt** i maskinläge. `ERR ...`-rader skickas fortfarande; displayen ignorerar okända rader som i dag.
- Displayen ekar aldrig något tillbaka.

---

## 6. Firmware-flöde på STM32 (SPUD)

1. Boot: USB-device-stacken startar som i dag. CC-logiken börjar DRP-växla (50 ms per roll). 5V-brytaren är **av** — även GPIO:ns startnivå i CubeMX är låg, och brytaren behöver en pull-down så att den är av medan STM32 är i reset eller flashas.
2. **Display ansluten (CC ser Rd):** kontrollera att VBUS-sense är låg (ingen främmande 5V) → slå på 5V-brytaren → displayen bootar → dess host-stack enumererar STM32 → CDC öppnas → `hello` kommer → kanalen går till maskinläge → svara `hello` + fullt tillstånd.
3. **Dator ansluten (CC ser Rp, VBUS-sense hög):** 5V-brytaren förblir av → datorn enumererar STM32 → CLI i människoläge som i dag. ROM-bootloaderns DFU fungerar oförändrat.
4. **Kabel ur:** CC öppen → 5V-brytaren av → USB-stacken ser suspend/reset → kanalen återställs till människoläge vid nästa enumerering.
5. DRP-dator (§3.3): Try.SNK i `usbc_port.c` innan 5V slås på.

Steg 1–4 är implementerade och verifierade på UGN (§11); steg 5 återstår.

---

## 7. Konkreta ändringar per repo

### STM32 (`Laserfabriken`)

Gemensamt för båda korten (kan göras och testas på UGN i dag mot en PC-terminal):
- **En kanalabstraktion i `cli.c`**: sändfunktion, mottagningsring, radbuffert och lägesflagga (människa/maskin) per kanal. `process_line()` svarar på den kanal som skickade kommandot. I dag finns två handskrivna kopior av radhanteringen och display-svaren går till fel kanal.
- `hello`, `get all`, `stream` och lägesbyte enligt §4.1.
- Ta bort speglingen av terminalinmatning till displayen (`disp_forward()`). Diff-synken täcker redan det fallet.
- Eget VID/PID, produktsträng och firmwareversion i descriptorerna (via CubeMX).
- Firmwareversion på ett ställe i koden, så att hello-svaret och `bcdDevice` inte kan glida isär.

Bara SPUD (`bsp_config.h`-val):
- Displaykanalen pekar på USB-kanalen i stället för USART1. USART1 och `DISPLAY_DO1/2/3` tas bort ur `.ioc`.
- Tre nya GPIO: 5V-brytarens enable, VBUS-sense och barrel-detekt. Bestäm dem samtidigt som ADC_CS flyttas från PA4 (README §3.1), pinbudgeten är trång.
- CC-logik: `usbc_port.c` (finns, UCPD1 utan middleware) + Try.SNK. `BSP_USBC_VBUS_EN_PORT/PIN` pekas om från M6_EN till den nya brytarens enable.

### Display (`Laserfabriken_Display`)

- ESP32-S3 blir **USB-host**. Börja i Arduino IDE: ESP-IDF:s `usb_host`-API följer med Arduino-kärnan, och CDC behöver inte mer än att claima data-interfacet och köra bulk-överföringar (ingen line coding behövs för ett textprotokoll). Fungerar inte det, eller visar det sig för skört: flytta till ESP-IDF med Arduino som komponent och använd komponenten `usb_host_cdc_acm` från component registry.
- Öppna bara enheter med rätt VID/PID. Skicka `hello` direkt när öppningen lyckats, stäng handtaget vid frånkopplingshändelsen och visa "ingen kontroller".
- Ersätt `SerialSTM` med ett litet transportlager (`write/read/available`) som `drain_stm_uart()` och `send_*()` använder. Parsern `process_stm_line()` är oförändrad.
- **USB-porten på displayen är upptagen av host-läget.** Konsekvenser:
  - `Serial` (USB-debug) försvinner. UART0 (GPIO43/44) är ledig på SPUD och blir debugport mot en USB-UART-dongel.
  - **Flashning:** sätt displayen i ROM-bootloader (håll BOOT vid uppstart) och koppla dess USB-C till en PC. Då är den device, inte host, och flashas som vanligt. Arduinos auto-reset via 1200 bps fungerar **inte** när sketchen kör host-läge. Displayens USB-C måste alltså vara åtkomlig i färdig produkt — det är samma kontakt som kabeln till SPUD sitter i.
- Board-inställningen "USB CDC On Boot" spelar ingen roll längre när host-stacken tar över PHY:n, men "USB Mode" ska stå på OTG. Verifiera mot aktuell Arduino-kärna.
- Displayen kan testas mot ett **UGN-kort** innan SPUD finns: UGN:s USB-C in i displayens USB-C med en C-till-C-kabel, med displayen strömsatt separat. UGN:s descriptorer och kanallogik är samma kod.

---

## 8. Vad USB ger jämfört med UART, ärligt

| | UART (UGN i dag) | USB (SPUD) |
|---|---|---|
| Felhantering | Ingen; avbrottsdriven RX fungerar bevisat | CRC + omsändning i hårdvara |
| Identifiering | Ingen | VID/PID + produktsträng |
| Anslutningsstatus | Gissas (periodisk omsändning) | Händelser i host-stacken |
| Kablage | FPC med UART, 5V, tre strap-ledare | En USB-C-kabel |
| Displayens USB-debug | Finns | Försvinner, ersätts av UART0 |
| Displaykod | `HardwareSerial` | Host-stack, ev. ESP-IDF-migrering |
| Kortkomponenter | — | 5V-brytare, CC-logik, VBUS-sense |

Protokollförbättringarna i §5 (hello, `get all`, setter-roll, inget boot-race) är oberoende av transport och görs för båda korten. Det USB köper utöver det är identifiering, anslutningshändelser och en enklare kabel, mot priset av host-stacken på displayen och tre komponenter på SPUD.

---

## 9. Öppna frågor

- [x] CC-lösning: UCPD1 med egen firmware (beslut 2026-10-09, §3.2).
- [ ] Try.SNK i `usbc_port.c`, och test mot en DRP-bärbar med C-till-C-kabel (§3.3).
- [ ] Vilka GPIO får 5V-brytarens enable, VBUS-sense och barrel-detekt? Bestäm tillsammans med ADC_CS-flytten.
- [ ] Strömväg USB/barrel (§3.2 punkt 4): TPS2121 eller diod-OR?
- [ ] Fungerar `usb_host`-API:t från Arduino-kärnan i praktiken, eller krävs ESP-IDF direkt? Avgörs i steg 2 nedan.
- [ ] VID/PID: eget internt par, eller ansökan (pid.codes för öppna projekt)?
- [ ] Protokollversion: räcker `proto=1` med "ignorera okända rader", eller behövs en lista över vilka rader varje version garanterar?

## 10. Ordning

1. STM32: kanalabstraktion, hello/`get all`/`stream`, descriptorer. Testas på UGN mot PC-terminal, utan ny hårdvara.
2. Display: transportlager + host-stack. Testas mot UGN-kortet via C-till-C-kabel.
3. Först därefter fryses SPUD-schemat — steg 2 är det som visar om host-vägen på displayen håller.

---

## 11. Verifierat på UGN (2026-10-09)

Proof of concept på befintligt UGN-kort, utan omlödning. Uppställning: barrel-jack in, display (ESP32-S3) ansluten **bara** via USB-C-kabel till UGN:s USB-C, ST-Link på ProgPAD.

**Hårdvarufakta som mättes fram:**
- 5V på UGN:s USB-C VBUS kommer från 5V-regulatorn M6, som STM32 slår av/på med PB0 (`M6_EN`). Ingen backströmsspärr finns mellan VBUS och 5V-skenan.
- UCPD1:s CC-pinnar (PB4/PB6) är kopplade till kontakten. Som Source ser STM32 displayens Rd; som Sink ser den USB-A-kabelns Rp.
- Utan barrel-jack matar en dator (USB-A) hela kortet baklänges genom 5V-regulatorn; STM32 startar och enumereras.
- HiAx-schemat för UGN är inte uppdaterat och ska inte användas som referens för strömvägar.

**Funktion som verifierats:**

| Test | Resultat |
|---|---|
| Displayen som USB-host enumererar STM32 (VID 0x1209, PID 0x0001) | ✅ |
| Hello-handskakning, fullt tillstånd efter hello, `get all` | ✅ |
| Rattvridning på displayen → STM32:s börvärde följer i realtid | ✅ |
| Börvärde satt på STM32 → displayen följer | ✅ |
| Auktoritativt svar: `set laser.power 150` → `set laser.power 100` | ✅ |
| Dator (USB-A) → STM32, CLI i människoläge och lägesbyte via hello | ✅ |
| Automatisk 5V: display in → 5V på, ur → 5V av, dator in → 5V förblir av | ✅ (live, båda kontaktvändningarna) |
| Värde satt på displayen → kabelbyte → läses av datorn (`get all`, `status`) | ✅ |
| Värde satt från datorn → kabelbyte → visas på displayen | ✅ |
| Värde överlever omstart av STM32 (läses från flash) | ✅ |
| Välkomsttext skickas när ett program öppnar porten (DTR), första kommandot är rent | ✅ |
| Utan barrel-jack: datorn (USB-A) matar kortet, STM32 startar, sparade värden läses, nytt börvärde sätts och sparas i flash | ✅ |

### 11.1 Att använda UGN-uppställningen

| Barrel-jack | USB-C | Vad händer |
|---|---|---|
| in | display | 5V på, displayen kör. Normalläget. |
| in | dator (USB-A) | 5V av, kortet kör på barrel, datorn har CLI. |
| ur | dator (USB-A) | Kortet kör på datorns ström: CLI, läsa/spara värden. TEC och laser får inte slås på (räcker inte, och firmware spärrar inte än). |
| ur | display | Ingenting fungerar: displayen har inget batteri och kortet ingen ström. |

**Byten:** kabel- och barrel-byten fungerar i valfri ordning med nuvarande firmware (5V styrs av CC-detekteringen, PB0 startar låg). Att sätta i barrel-jacken medan datorn sitter i bör vara ofarligt: regulatorn är avstängd, barrel-12V når inte USB-C, och kortet går över till barrel-matning. Det bygger på kortets beteende, inte på ett verifierat schema. **Säkrast:** dra ur datorn, sätt i barrel, sätt i datorn igen. Flasha inte STM32 medan en dator sitter i USB-C (PB0 är odriven under reset).

**Kvirk på UGN:** med en dator i och 5V-regulatorn av matas kortets 5V-skena av datorn, även med barrel-jacken i. Ofarligt men oavsiktligt; SPUD:s backströmsspärr tar bort det.

**Inte verifierat / kvar till SPUD:** Try.SNK mot DRP-dator, backströmsspärr, 5V-läget under reset/flashning, strömväg och barrel-detekt (§3.2 punkt 4–5).
