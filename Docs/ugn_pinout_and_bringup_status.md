# UGN Hårdvaru-status — Pinout-ombyggnad & Datasheet-verifiering

**Dokument:** UGN-STATUS-001
**Datum:** 2026-07-05
**Författare:** Board Architect Design AB

---

## 1. Bakgrund

UGN-kortets schema gick igenom en pinout-revision (SPI flyttades från SPI1 till SPI3, flera signaler flyttades till andra pinnar). Det här dokumentet håller reda på:
- vad som fixats i mjukvaran för att matcha det nya schemat
- vilka buggar som hittades och rättades mot ADS1220- och TPS563252-datablad
- och — viktigast — vad som **fortfarande måste verifieras mot riktig hårdvara** innan det kan anses klart

Ingenting i det här dokumentet är testat på fysisk hårdvara än. Allt nedan är antingen (a) fixat och kontrollerat mot ett datablad/schema på papper, eller (b) flaggat som att det behöver mätas på bänken.

---

## 2. Fixat — pinout-ombyggnad (matchar nuvarande schema)

Omskrivet för att matcha den nya SPI3-baserade pinouten: `main.h`, `gpio.c`, `spi.c`/`spi.h`, `usart.c`, `bsp_config.h`, `main.c`, kommentarer i `bsp_tec.c`.

| Signal | Gammal pinne | Ny pinne | Kommentar |
|---|---|---|---|
| ADC_CS | PA2 | PA4 | ADS1220 chip select |
| DAC_CS | PA3 | PA3 | oförändrad — reserverad för DAC8562S (spudkortet) |
| Crystal TEC DAC | PA4 (DAC1_CH1) | PA5 (DAC1_CH2) | intern STM32-DAC |
| TEC_PG | PB2 | PA2 | TPS563252 power-good, open-drain, intern pull-up |
| TEC_EN | PB1 | PC5 | TPS563252 enable |
| POLARITY | PC15 | PC6 | H-brygga, val av riktning |
| M6_EN | PB10 | PB0 | 5V-utgång, på/av |
| SPI-buss | SPI1 (PB3/PA6/PA7) | SPI3 (PC10/PC11/PC12), AF6 | ADS1220 + DAC8562S (spudkortet) |
| DISPLAY_TX/RX | PB6/PB7 (USART1) | PA9/PA10 (USART1) | display-UART |
| DISPLAY_DO1/2/3 | PB11/PB12/PB13 | PB13/PB14/PB15 | oanvänd GPIO till displaykontakten |

`.ioc`-filen är synkad med dessa manuella C-ändringar (`PinState`, `GPIO_PuPd`, `functionlistsort`), så att en CubeMX "Generate Code"-körning ska återge samma resultat istället för att skriva över det.

---

## 3. Fixat — ADS1220-registerbuggar (verifierat mot datablad SBAS501D)

Hittades vid genomgång av `bsp_temp.c` mot ADS1220:s registertabeller i databladet (Tabell 8-10, 8-13):

| Konstant | Var | Nu | Bugg |
|---|---|---|---|
| `ADS1220_MUX_AIN0_AVSS` | `0x60` (MUX=0110b) | `0x80` (MUX=1000b) | Mätte AIN1−AIN0 (ett flytande differentialpar — AIN1 är inte inkopplad) istället för AIN0 mot AVSS (single-ended) |
| `ADS1220_VREF_EXTERNAL_REFP0` | `0x08` | `0x40` | VREF[1:0] sitter på bit 7:6, inte 4:3. Det gamla värdet satte av misstag PSW-biten (bit 3) istället, vilket lämnade ADC:n på sin **interna 2.048V-referens** medan `raw_to_resistance()` antog den externa 2.9V-skenan — varje temperaturavläsning skulle ha blivit fel |
| `BSP_ADS1220_DR_90SPS` | `0x20` (=45 SPS) | `0x40` (=90 SPS) | Felmärkt konstant, används inte just nu (`BSP_ADS1220_DATA_RATE` använder 20SPS-konstanten, som redan var korrekt) |

Aktiverade också **samtidig 50Hz+60Hz brusfiltrering** (`ADS1220_FILTER_50_60HZ = 0x10`, var `0x00` = ingen filtrering) eftersom produkten säljs på marknader med båda nätfrekvenserna — kostar ingenting vid 20 SPS.

**Verifierat korrekt, ingen ändring behövs:** SPI mode 1 (CPOL=0/CPHA=1), kommandobyte-kodning (RESET/START/RDATA/WREG), återhämtningsfördröjning efter RESET, timingmarginal för kontinuerlig konvertering mot 100ms-huvudloopen.

---

## 4. Fixat — efterlevnad av mjukvaruspecifikationen

- **M6_EN (5V-skena) startläge**: var LOW vid uppstart, spec §3.5 kräver "aktiv vid uppstart" — ändrat till `GPIO_PIN_SET` i `gpio.c`.
- **`5v on` / `5v off` CLI-kommandon** tillagda (`cli.c`), växlar M6_EN. Fungerar över både USB-CLI och display-UART eftersom båda går genom samma `process_line()`.
- **`BSP_TEC_PowerGood()`** tillagd (`bsp_tec.c`/`.h`) — läser TEC_PG-pinnen, och är nu inkopplad i `TEC_Control_Tick()`: om PG läser lågt stängs kristall-TEC:n av (`BSP_TEC_Disable`) och hålls avstängd varje varv tills PG läser bra igen. Ett engångsmeddelande skrivs ut till CLI:n (`!! TEC DISABLED — buck power-good fault (PG low) !!`), och `status` visar en `TEC buck: FAULT`-rad medan felet är aktivt.

---

## 5. Medvetet lämnat orört (inte buggar)

- `bsp_laser.c`, `laser_control.c`, alla `laser.*` CLI-kommandon, `DAC_CS`/`DAC1_CH1` (PA4)-makron — hålls vilande för det framtida **spudkortet**, som lägger till en laserdiod. På UGN-hårdvaran gör dessa ingenting (bara en TEC-kanal, `TEC_LASER`-anrop returnerar direkt i `bsp_tec.c`).

### 5.1 Krävd PCB-ändring för spudkortet — ADC_CS måste flyttas från PA4

STM32G431 har bara **två** externa GPIO-pinnar för den interna DAC1: PA4 (DAC1_CH1) och PA5 (DAC1_CH2). På UGN driver PA5 kristall-TEC:ns amplitud och PA4 används som `ADC_CS` (ADS1220 chip select) — det finns ingen laser-TEC-hårdvara på UGN, så det är okej.

På **spudkortet** behöver laser-TEC:n sin egen DAC-amplitudkanal, och den enda lediga är PA4 (DAC1_CH1). Det betyder:

- **ADC_CS måste flyttas till en annan ledig GPIO** på spudkortets schema — den kan inte vara kvar på PA4
- Resulterande pinnplan för spudkortet: PA4 = DAC1_CH1 (laser-TEC amplitud), PA5 = DAC1_CH2 (kristall-TEC amplitud, oförändrad), PA3 = DAC_CS (DAC8562S, laserdiodström — oförändrad), ADC_CS = *(ny pinne, ej bestämd)*

Det här är en **hårdvaru-/schemaändring**, inte något som kan fixas i mjukvaran — flagga det till den som ritar spudkortets schema innan det kortet beställs.

---

## 6. Öppna punkter — kräver riktig hårdvara för att verifiera

Det här är sådant som måste mätas/testas innan motsvarande mjukvarubeteende kan anses pålitligt.

### 6.1 TEC DAC → VOUT-skalning (säkerhetsrelevant — läs detta noga)

**Kort sagt:** Mjukvaran vet inte säkert vilken utspänning (VOUT) till TEC-elementet en given DAC-kod faktiskt ger. Räknat på schemats komponentvärden stämmer inte de angivna designpunkterna. Sannolik orsak: **R14 är troligen felavläst/felmonterad som 150k, när den enligt matematiken borde vara 15k** (se härledning nedan). Det här måste bekräftas på bänken innan man kör TEC:n med någon större effekt.

**Bakgrund för hårdvarukollegan:** `bsp_tec.c: internal_dac_set()` inverterar den 12-bitars DAC-koden (`4095 - code`) och antar att TPS563252-kretsens FB-nätverk (feedback-nätverket, dvs de motstånd som talar om för buck-omvandlaren vilken spänning den ska ge) följer schemats angivna punkter: VDAC=0V→VOUT=10.8V, VDAC=2.4V→VOUT=0V.

**Komponenter i FB-nätverket:** R14 (mellan VOUT och FB-noden), R15 (mellan DAC-utgången och FB-noden), R16 (mellan FB-noden och GND). Bekräftade/antagna värden just nu: R14=150k, R15=3.3k, R16=1.2k.

#### Härledning — varför förhållandet R14/R15 måste vara 4.5

**Steg 1 — nodekvationen (Kirchhoffs strömlag).** Tre vägar möts i FB-noden: R14 till VOUT, R15 till VDAC, R16 till GND (0V). FB-pinnen på chippet drar i praktiken ingen ström (går in i en högimpedant komparator), så summan av strömmar in i noden är noll:

```
(VOUT − VFB)/R14 + (VDAC − VFB)/R15 + (0 − VFB)/R16 = 0
```

**Steg 2 — lös ut VFB.**

```
VOUT/R14 + VDAC/R15 = VFB · (1/R14 + 1/R15 + 1/R16)
```

Kalla `D = 1/R14 + 1/R15 + 1/R16` (det är här R16 kommer in):

```
VFB = (VOUT/R14 + VDAC/R15) / D
```

**Steg 3 — regulatorns villkor.** Chippet håller alltid VFB låst till sin fasta interna referensspänning `Vref` när det regulerar normalt:

```
Vref · D = VOUT/R14 + VDAC/R15        (*)
```

**Steg 4 — sätt in schemats två designpunkter.**

Designpunkt A: VDAC=2.4V, VOUT=0V:
```
Vref · D = 0/R14 + 2.4/R15 = 2.4/R15        (Ekv A)
```

Designpunkt B: VDAC=0V, VOUT=10.8V:
```
Vref · D = 10.8/R14 + 0/R15 = 10.8/R14      (Ekv B)
```

**Steg 5 — här faller R16 bort.** `D` och `Vref` är fasta egenskaper hos kretsen — de ändras inte mellan de två mätpunkterna. Vänsterledet `Vref·D` är alltså samma tal i båda ekvationerna, så högerleden måste vara lika med varandra:

```
2.4/R15 = 10.8/R14
  →  R14/R15 = 10.8/2.4 = 4.5
```

R16 finns inte kvar i sista raden — den satt inne i `D`, och `D` var en gemensam faktor som föll bort när Ekv A och Ekv B sattes lika. Så oavsett vilket värde R16 har (1.2k eller vad som helst) kan den inte ändra på det här specifika kravet: **R14/R15 måste vara 4.5 för att båda designpunkterna ska stämma.**

#### Numerisk kontroll — faktorn 10 dyker upp

Med dagens antagna värden (R14=150k, R15=3.3k, R16=1.2k):

```
D = 1/150000 + 1/3300 + 1/1200 = 0.0011430

Från Ekv A:  Vref = (2.4/3300)   / D = 0.636 V
Från Ekv B:  Vref = (10.8/150000) / D = 0.063 V
```

Det ska vara **samma** Vref i båda (det är en och samma fysiska referensspänning i chippet) — men de hamnar en faktor **~10** ifrån varandra. Det är precis den typen av avvikelse man får när en komponent är felavläst med en decimal.

Testar man istället med **R14=15k** (R15=3.3k, R16=1.2k oförändrade):

```
D = 1/15000 + 1/3300 + 1/1200 = 0.0012030

Från Ekv A:  Vref = (2.4/3300)  / D = 0.605 V
Från Ekv B:  Vref = (10.8/15000) / D = 0.599 V
```

Nu hamnar båda på ≈0.60V — konsekvent inom avrundning. Det är den numeriska grunden för misstanken att **R14 i verkligheten är 15k, inte 150k**, och att hela kretsen annars stämmer precis som schemat avsåg.

**Varför blir det inte ett orimligt högt VOUT i verkligheten (med dagens 150k-antagande)?** TPS563252 är en *buck*-omvandlare (spänningen kan bara gå ner, aldrig upp, jämfört med ingångsspänningen VIN=12V). Så även om uträkningen med R14=150k säger att FB-nätverket "vill" ha en mycket hög VOUT, kan kretsen fysiskt inte leverera mer än ungefär VIN minus lite förlust — runt **10-11V**. Regulatorn skulle då köra permanent mättad (fullt duty cycle) istället för att regulera normalt.

**Vad som behöver göras på bänken (i prioritetsordning):**
1. **Mät R14 fysiskt med multimeter** (helst med komponenten urkopplad ur kretsen) — är den 150k eller 15k? Det är den enskilt mest sannolika förklaringen och billigast att kolla först.
2. Bekräfta att R16 verkligen sitter mellan FB-noden och GND (inte någon annan nod).
3. Om R14 verkligen är 150k (dvs. mätningen inte stödjer 15k-teorin): sätt DAC-kod till 0 → mät verklig VOUT. Sätt DAC-kod till 4095 → mät verklig VOUT. Svep några mellanliggande koder för att hitta den verkliga brytpunkten där VOUT=0.
4. Rapportera värdena så att `internal_dac_set()` kan räknas om för att använda hela 12-bitars kodintervallet över den faktiska, uppmätta VDAC→VOUT-relationen (annars riskerar stora delar av DAC:ens upplösning att slösas bort på koder som inte gör något, se härledningen ovan för varför).

### 6.2 POLARITY GPIO-riktning (värme vs. kyla)

`bsp_tec.c` antar att `GPIO_PIN_SET` = värme, `GPIO_PIN_RESET` = kyla — ett platshållarval, inte härlett från schemat. M4 H-bryggans exakta switch-logik plus vilken fysisk TEC-ledare som är kopplad till kontaktens pinne A respektive B skulle behöva spåras för att avgöra detta enbart från schemat, och det gick inte att fastställa säkert från tillgänglig ritning.

**Vad som behöver göras på bänken:** koppla in en känd TEC, se vilken sida som blir varm när POLARITY drivs högt, justera tecken-konventionen i `BSP_TEC_SetOutput()` om det är fel.

### 6.4 SPI-timingmarginaler

Inga explicita fördröjningar har lagts till mellan `HAL_GPIO_WritePin` (CS aktiveras) och första `HAL_SPI_Transmit`-anropet för att uppfylla ADS1220:s `td(CSSC)` (minst 50ns). Förlitar sig på att HAL-anropets egen overhead räcker vid en SPI-klocka på 4MHz. Håll utkik efter korrupta/skräp-ADC-avläsningar; lägg till explicita fördröjningar om det förekommer.

### 6.5 ~~`BSP_TEC_PowerGood()` — inte integrerad än~~ (klart)

Fixat: `TEC_Control_Tick()` stänger nu av kristall-TEC:n och låser en felflagga (`g_tec_pg_fault`) närhelst PG läser lågt, och rensar den automatiskt när PG läser bra igen. CLI:n skriver ut ett engångsmeddelande och `status` visar felet medan det är aktivt. **Behöver fortfarande bänktestas**: tvinga PG lågt (eller simulera ett buck-fel) och bekräfta att TEC:n faktiskt stängs av och återhämtar sig som förväntat.

### 6.6 M6_EN (5V-skena) — verklig last obekräftad

Enligt spec §3.5 är det bara "5V-utgången". Styrningen (`5v on`/`5v off`) är implementerad, men vad som faktiskt är inkopplat på den, och vad dess på-som-standard-beteende innebär vid uppstart, har inte bekräftats mot det riktiga kortet.

---

## 7. PID-tuning-procedur

PID-parametrarna som skickas med i `tec_control.c` (`Kp=0.5, Ki=0.02, Kd=0.1`) är spec-bestämda *standard-/återställningsvärden* (mjukvaruspecifikation §3.1), inte färdigtunade värden — de finns för att systemet ska ha en säker, definierad startpunkt efter en ny flashning, inte för att de förväntas klara <5mK stabilitetskravet direkt. Verklig tuning måste ske mot den riktiga kristallugnen + TEC + buck-termiska massan, som inte finns förrän kortet är byggt.

**Fasta implementationsdetaljer som är relevanta** (behöver inte härledas om under tuningen):
- Kontrolloopen kör var 100ms (`PID_PERIOD_S = 0.1f` i `tec_control.c`) — termiska system som det här domineras normalt av tidskonstanter på flera sekunder, så 100ms är inte den begränsande faktorn
- Integraltermen använder dubbel precision (undviker drift vid lång körning)
- D-termen är utjämnad med ett EMA-lågpassfilter (`D_ALPHA = 0.1`, kompileringstidskonstant, inte CLI-exponerad) för att dämpa mätbrus som annars matas in i derivatan
- Anti-windup via bakåträkning är redan implementerat — integraltermen "rusar" inte iväg vid mättnad, men håll ändå koll på långsam återhämtning om gainen är för aggressiv
- Utsignalen klamras till [−1.0, +1.0] innan den skickas till `BSP_TEC_SetOutput()`

### 7.1 Rekommenderad metod — Ziegler–Nichols (kritisk gain)

Vald eftersom den inte kräver någon systemmodell, fungerar bra för långsamma termiska system, och bara kräver CLI-kommandon som redan finns.

**Steg 1 — hitta den kritiska gainen:**
```
set crystal.ki 0
set crystal.kd 0
set crystal.kp 0.1        (börja lågt, öka gradvis)
status                    (upprepa, följ T vs SP över tid)
```
Öka `crystal.kp` i små steg tills temperaturen går in i en **konstant, oförändrad amplitud**-svängning runt börvärdet (varken växande eller avtagande). Notera:
- `Kc` = Kp-värdet där detta händer
- `Pc` = svängningsperioden i sekunder (tid mellan två toppar)

**Steg 2 — Ziegler–Nichols startpunkt:**
```
Kp = 0.6  × Kc
Ki = 1.2  × Kc / Pc
Kd = 0.075 × Kc × Pc
```
Sätt dessa via `set crystal.kp/ki/kd <värde>` — det här är en startpunkt, inte det slutgiltiga svaret.

**Steg 3 — finjustera:**

Håll koll på tre specifika symptom i `status`-utskriften, och justera en gain i taget:

1. **Kvarstående stationärt fel** — temperaturen lägger sig nära börvärdet men når det aldrig riktigt (t.ex. stannar på 24.95°C när börvärdet är 25.0°C). P-termen ensam kan aldrig helt eliminera en konstant störning (t.ex. värme som läcker ut mot omgivningen) — det är integraltermens jobb, och den är för svag. → **öka `Ki` något**.

2. **Överslag / ringning** — efter ett börvärdessteg (`set crystal.setpoint <värde>`, som automatiskt återställer integralen) skjuter temperaturen förbi målet innan den lägger sig (överslag), eller studsar fram och tillbaka flera gånger (ringning) istället för att närma sig mjukt. Loopen reagerar för aggressivt. → **minska `Kp` (och/eller `Kd`)**.

3. **Ryckig/darrig utsignal** — `output`-värdet i `status` flimrar snabbt även när temperaturen ligger still nära börvärdet. D-termen deriverar temperatursignalen, vilket förstärker vilket mätbrus som än finns (även litet ADC-brus blir stort när det deriveras) — och matar ett brusigt kommando till TEC:n. → **minska `Kd`** (det fasta `D_ALPHA`-EMA-filtret dämpar bara så mycket).

**Steg 4 — verifiera mot spec:**
- Låt loopen lägga sig vid ett fast börvärde och logga `status`-utskriften (T-kolumnen) i flera minuter
- Bekräfta att topp-till-topp (eller standardavvikelse) för den stationära variationen är **< 5 mK** enligt spec §3.1
- Upprepa oberoende för laser-TEC:n (`set laser.kp/ki/kd`) när spudkortet finns — på UGN-hårdvaran gör `TEC_LASER` ingenting, så det kan bara testas via `set sim.laser_temp` i simulering tills vidare

### 7.2 Säkerhetsanmärkningar specifika för den här tuning-omgången

- **Tuna inte aggressivt (stort `Kp`) förrän §6.1 (DAC→VOUT-skalning) är löst.** Innan det verkliga sambandet VDAC→VOUT är uppmätt kan en "måttlig" kommenderad utsignal motsvara en oväntat stor verklig TEC-spänning.
- Börja tuningen med små börvärdesavvikelser från omgivningstemperatur för att hålla TEC-strömmen låg medan gainen fortfarande är okänt-osäker.
- Spara bra värden med `save` när ni är nöjda — annars går de förlorade vid nästa reset (metoden för att spara parametrar permanent är själv fortfarande ett öppet beslut, spec §7.1).

---

## 8. Sammanfattning

Allt i §2–§4 är fixat och kontrollerat mot tillgängliga datablad/scheman. Ingenting i §6 kan anses klart förrän det är uppmätt på det riktiga kortet — särskilt §6.1 (DAC-skalning) och §6.2 (polaritetsriktning) är säkerhetsrelevanta och måste verifieras innan TEC:n körs med någon större drivstyrka.

---

## 9. Bänktest-checklista — ordning för första hårdvarutestet

Den här ordningen löser det olösta (DAC-skalning, §6.1) innan TEC:n någonsin körs med verklig kraft. Testa **inte** polaritet (§6.2) före DAC-skalningen — det skulle innebära att driva TEC:n med en okänd, potentiellt för hög spänning bara för att hitta rätt tecken.

**0. Innan ström sätts på alls**
- Visuell koll av kortet (inga uppenbara lödfel, kortslutningar, löst sittande komponenter)
- **Mät R14 med multimeter** (helst urlött/urkopplad ur kretsen för en ren mätning) — är den 150k eller 15k? Kräver ingen ström och avgör direkt om §6.1-teorin stämmer, innan ni ens börjar gissa med spänning på TEC:n.

**1. Första strömpåslag — utan att röra TEC-utgången**
- Koppla in USB-CLI, kör `status`, bekräfta att kortet svarar
- **Verifiera ADC-avläsningen innan ni går vidare till DAC/TEC-testerna:**
  - `T`-värdet i `status` ska ligga i ett rimligt rumstemperaturintervall (typiskt 15-30°C). Ett värde som är extremt orimligt (t.ex. under −100°C) är **inte** en riktig temperaturmätning — det är `raw_to_resistance()` i `bsp_temp.c` som faller tillbaka på ett "utanför intervall"-värde (1e9 Ω) när ADC-spänningen är ≤0V eller ≥VREF, vilket sedan matas genom Steinhart-Hart-formeln och ger ett absurt resultat. Det betyder att den *elektriska* mätningen är trasig, inte att kretsen faktiskt är −144°C kall.
  - Om ni ser ett sånt orimligt värde, felsök i den här ordningen:
    1. **Mät NTC:n direkt med multimeter** (motstånd, inte spänning) — ska vara några kΩ till några tiotals kΩ vid rumstemperatur, inte öppen krets (oändligt motstånd) eller kortslutning (0 Ω)
    2. **Mät den externa referensspänningen** på REFP0/REFN0 (ADS1220) — ska vara ~2.9V enligt schemat. Om den saknas/är fel förklarar det direkt ett orimligt värde, eftersom hela beräkningen bygger på VREF.
    3. **Kontrollera SPI-anslutningen** (§6.4) — särskilt att ADC_CS (PA4) faktiskt togglar (synlig med oscilloskop/logikanalysator om tillgängligt), annars kan ADS1220 svara med skräp eller inte svara alls
    4. Om allt ovan ser bra ut men värdet ändå är fel: dubbelkolla att `raw` faktiskt är nollskild (`raw == 0` ger istället sentinelvärdet exakt −273.15°C i `status` — ett annat, mer uppenbart symptom som betyder "konvertering ännu inte klar", inte samma fel som ovan)
  - Ett rimligt `T`-värde bekräftar att SPI/ADS1220-kedjan fungerar end-to-end — det är en förutsättning för att lita på DAC-svepningen i steg 3, eftersom ni annars inte kan skilja på "DAC-skalningen är fel" och "temperaturmätningen är fel" när ni tolkar resultaten
- Testa `5v on`/`5v off` (M6_EN, §6.6) och bekräfta med multimeter vad som faktiskt ligger på den skenan. Observera att M6_EN är satt HIGH redan i den råa GPIO-initieringen (`gpio.c`), dvs. 5V-skenan är strömsatt direkt vid uppstart, innan CLI:n ens är ansluten — kör `5v off` direkt efter uppstart om ni vill ha den avstängd tills lasten är bekräftad.

**2. Verifiera säkerhetsmekanismen innan ni litar på den (§6.5)**
- ~~Det finns inget CLI-kommando för att stänga av TEC_EN manuellt~~ — **inaktuellt**: `crystal.tec off` (och `laser off`, som gör samma sak som bieffekt) finns nu i `cli.c` och skriver `TEC_EN` (PC5) lågt direkt via `BSP_TEC_Enable(TEC_CRYSTAL, 0)`. Men trigga **inte** det här testet via den vägen — testet nedan ska specifikt verifiera `TEC_Control_Tick()`s *automatiska* PG-felhantering (att kretsen själv reagerar på ett PG-fel), inte att ett manuellt kommando kan slå av EN, vilket är en annan kodväg.
- Trigga istället ett PG-fel direkt vid mikrokontrollern: **kortslut TEC_PG (PA2) till GND med en trådstump/jumper** (ofarligt, bara en digital ingång med intern pull-up). Bekräfta att `TEC DISABLED — buck power-good fault`-meddelandet dyker upp, att utgången faktiskt stängs av, och att den återhämtar sig när jumpern släpps.
- Vill ni även bekräfta att buck-kretsen *själv* drar ner PG vid ett riktigt fel (inte bara att firmware svarar rätt på en spoofad signal): sänk VIN under UVLO-tröskeln med en justerbar labbnätaggregat.
- Gör det här före de mer aggressiva DAC/PID-testerna nedan — ni vill veta att skyddet fungerar innan ni behöver lita på det.

**3. Försiktig DAC-svepning (nu när R14 är känd)**

> **⚠ Incident (dokumenterad för framtida läsare):** en tidig version av det här steget ersatte `internal_dac_set()`s invertering med en rak (icke-inverterad) mappning, i tron att DAC-kod 0 (PID-utgång "av") då skulle ge VDAC≈0 och därmed VOUT≈0 — den "säkra" viloläget. Det var fel. Enligt FB-formeln i §6.1 (`VOUT = VREF + R14×(VREF/R16 + (VREF−VDAC)/R15)`) är det tvärtom: VDAC=0 maximerar `(VREF−VDAC)/R15`-termen, vilket tvingar fram det **högsta** VOUT-målet, inte det lägsta. Den "avstängda" viloläget kommenderade i praktiken bucken mot max-utgång mot en TEC-last, och bucken brändes. Den ursprungliga inverterade mappningen (kod 0 → VDAC≈max) är den bekräftat säkra: vid max VDAC blir VOUT-målet negativt (omöjligt för en buck), så kretsen går in i OVP-lås och slutar switcha helt — det är därför `internal_dac_set()` fortfarande använder `4095 - code_12bit`. **Svep aldrig direkt mot DAC-kod 0/rå-DAC=0 — svep alltid nedåt från rå-DAC=4095, i små steg, med FB och PG kontrollerade mellan varje steg.**

Använd CLI:t istället för att skriva om `internal_dac_set()` för det här testet — det håller PID-loopen borta från DAC:n medan ni sveper manuellt:

1. `dac test on` — pausar PID-drivningen av kristall-TEC:n och tvingar rå-DAC till 4095 (bekräftat säkert/inert läge: FB fastnar över OVP-tröskeln, bucken switchar aldrig, PG låg, ingen ström flyter).
2. Mät FB (och VOUT om ni har fler mätpunkter) med multimeter. Bekräfta att det matchar det inerta läget innan ni fortsätter.
3. `dac set 4095`, sedan stegvis nedåt i hopp om **100-200 steg** åt gången (t.ex. `dac set 3900`, `dac set 3700`, …) — **aldrig direkt till ett lågt värde**. Efter varje kommando: mät FB, och läs `PG=` i CLI-svaret (eller kör `status` för fullständig bild).
4. Stanna **omedelbart** vid första tecken på att FB närmar sig 0.6V eller att `PG=GOOD` dyker upp — det är brytpunkten där bucken börjar switcha och regleras på riktigt. Fortsätt **inte** ned mot 0 "för att se hela intervallet".
5. Om R14=15k bekräftades i steg 0 har ni redan en bra uppskattning av var brytpunkten borde ligga — bekräfta ändå empiriskt, men ni vet ungefär vad ni väntar er.
6. Notera de verkliga brytpunkterna (rå-DAC-värdet där PG blir GOOD, motsvarande FB och VOUT) — det är det som ska in i `internal_dac_set()`s skalning efteråt.
7. `dac test off` när ni är klara — det tvingar rå-DAC till 4095 igen och lämnar tillbaka kontrollen till PID-loopen.

Ha strömbegränsad labbnätaggregat inkopplad under hela den här proceduren om ni har tillgång till en (se listan längst ner i det här kapitlet) — det är den enskilt bästa spärren mot att en felaktig brytpunkt bränner något igen.

**4. Polaritetstest (§6.2) — nu, med känd säker spänningsnivå**
- Använd en låg, bekräftat säker DAC-kod från steg 3, driv POLARITY högt, känn/mät vilken sida som blir varm
- Justera tecken-konventionen i `BSP_TEC_SetOutput()` om det är omvänt mot antagandet

**5. PID-tuning (§7) — sist, som redan sagt ovan**
- Börja med litet `Kp`, små börvärdesavvikelser från omgivningstemperatur
- Följ Ziegler-Nichols-proceduren i §7.1

**Vad ni annars lätt missar:**
- **Strömbegränsad labbnätaggregat** om ni har tillgång till en — mycket säkrare än väggadapter för de första testerna, ni kan då fysiskt inte mata mer ström än ni ställt in oavsett vad som går fel
- **Skriv ner alla mätvärden** (R14, VOUT vid olika DAC-koder, vilken sida som blev varm) så dokumentet kan uppdateras direkt efteråt med verkliga siffror istället för antaganden
- **Lämna inte kortet strömsatt utan uppsikt** under de första testerna, särskilt innan PG-skyddet är bekräftat fungera
- Ha multimetern på VOUT **kontinuerligt** under DAC-svepningen, inte bara enstaka mätpunkter — ni vill se om något hoppar oväntat mellan koderna
