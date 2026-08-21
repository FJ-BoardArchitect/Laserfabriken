# NTC Steinhart-Hart — Beräkning av koefficienter

**Dokument:** NTC-CALC-001  
**Datum:** 2026-06-04  
**Författare:** Board Architect Design AB

---

## NTC-specifikation

| Parameter | Värde |
|---|---|
| Resistans vid 25°C | 10 kΩ |
| Tolerans | ±1% |
| B-konstant (25/50°C) | 3380 K (±1%) |
| B-konstant (25/85°C) | 3434 K |
| Drifttemperaturområde | −40°C till +125°C |

Serieresistans i spänningsdelare: **10 kΩ** (= R₂₅ för maximal känslighet kring 25°C)  
ADS1220 referens: **extern 2.9 V** (REFP0/REFN0) — används endast för ADC-råkod → resistans-omvandlingen (`raw_to_resistance()` i `bsp_temp.c`), påverkar inte Steinhart-Hart-koefficienterna nedan

---

## Steinhart-Hart-ekvationen

$$\frac{1}{T} = A + B \cdot \ln(R) + C \cdot (\ln(R))^3$$

Tre okända (A, B, C) kräver tre kända (T, R)-par.

---

## Steg 1 — Beräkna tre temperaturpunkter

Punkt 1 hämtas direkt från datablad. Punkterna 2 och 3 beräknas från B-konstanterna via:

$$R(T) = R_{25} \cdot e^{B \cdot (1/T - 1/T_{25})}$$

**Punkt 1 — 25°C (direkt från datablad):**
```
T₁ = 298.15 K
R₁ = 10 000 Ω
```

**Punkt 2 — 50°C (B₂₅/₅₀ = 3380 K):**
```
exp = 3380 × (1/323.15 − 1/298.15)
    = 3380 × (0.0030946 − 0.0033541)
    = 3380 × (−0.0002595)
    = −0.8771

R₂ = 10 000 × e^(−0.8771) = 4 160 Ω
```

**Punkt 3 — 85°C (B₂₅/₈₅ = 3434 K):**
```
exp = 3434 × (1/358.15 − 1/298.15)
    = 3434 × (0.0027921 − 0.0033541)
    = 3434 × (−0.0005620)
    = −1.9300

R₃ = 10 000 × e^(−1.9300) = 1 452 Ω
```

---

## Steg 2 — Beräkna ln(R) och ln(R)³

| Punkt | T (K) | R (Ω) | ln(R) | ln(R)³ | 1/T |
|---|---|---|---|---|---|
| 1 | 298.15 | 10 000 | 9.2103 | 781.32 | 3.3541×10⁻³ |
| 2 | 323.15 | 4 160 | 8.3334 | 578.71 | 3.0946×10⁻³ |
| 3 | 358.15 | 1 452 | 7.2798 | 385.78 | 2.7921×10⁻³ |

---

## Steg 3 — Linjärt ekvationssystem

$$A + 9.2103 \cdot B + 781.32 \cdot C = 3.3541 \times 10^{-3} \quad (1)$$
$$A + 8.3334 \cdot B + 578.71 \cdot C = 3.0946 \times 10^{-3} \quad (2)$$
$$A + 7.2798 \cdot B + 385.78 \cdot C = 2.7921 \times 10^{-3} \quad (3)$$

**Eliminera A:**

$(1)-(2)$:
```
0.8769·B + 202.61·C = 2.595×10⁻⁴    (4)
```

$(2)-(3)$:
```
1.0536·B + 192.93·C = 3.025×10⁻⁴    (5)
```

**Lös för C** — multiplicera (4) med 1.0536/0.8769 = 1.2014 och subtrahera från (5):

```
50.45·C = 9.27×10⁻⁶
C = 1.838×10⁻⁷
```

**Lös för B** via (4):

```
B = (2.595×10⁻⁴ − 202.61 × 1.838×10⁻⁷) / 0.8769
  = 2.223×10⁻⁴ / 0.8769
  = 2.535×10⁻⁴
```

**Lös för A** via (1):

```
A = 3.3541×10⁻³ − 9.2103 × 2.535×10⁻⁴ − 781.32 × 1.838×10⁻⁷
  = 3.3541×10⁻³ − 2.335×10⁻³ − 1.436×10⁻⁴
  = 8.757×10⁻⁴
```

---

## Resultat

| Koefficient | Värde |
|---|---|
| A | 8.757×10⁻⁴ |
| B | 2.535×10⁻⁴ |
| C | 1.838×10⁻⁷ |

```c
#define BSP_NTC_SH_A    8.757e-4f
#define BSP_NTC_SH_B    2.535e-4f
#define BSP_NTC_SH_C    1.838e-7f
```

---

## Verifiering

Punkt 1 (R = 10 000 Ω, förväntat 25°C):
```
1/T = 8.757×10⁻⁴ + 2.535×10⁻⁴ × 9.2103 + 1.838×10⁻⁷ × 781.32
    = 8.757×10⁻⁴ + 2.335×10⁻³ + 1.436×10⁻⁴
    = 3.354×10⁻³
T   = 298.15 K = 25.0°C ✓
```

---

## Notering — kalibrering vid idrifttagning

Koefficienterna ovan är beräknade från B-konstanten, som är en approximation av den verkliga NTC-kurvan. För maximal noggrannhet bör koefficienterna uppdateras efter kalibrering mot ett referenstermometer med minst tre mätpunkter vid faktisk drifttemperatur.

Går numera att göra direkt via CLI, utan omkompilering: `set temp.sh_a <val>`, `set temp.sh_b <val>`, `set temp.sh_c <val>` (samt `set temp.vref`/`set temp.rseries` för spänningsdelaren). `status` visar aktuella värden. Sparas till Flash som allt annat (`save`, eller automatiskt 20s efter ändring).
