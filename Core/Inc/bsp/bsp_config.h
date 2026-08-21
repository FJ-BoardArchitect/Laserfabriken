#ifndef BSP_CONFIG_H
#define BSP_CONFIG_H

#include "usart.h"
#include "spi.h"
#include "dac.h"
#include "main.h"
#include "usbd_cdc_if.h"

/*
 * bsp_config.h — the ONLY file that changes when switching between boards.
 *
 * Current board: Laserfabriken UGN (single TEC channel, no laser diode)
 * Next board:    Spudkortet (adds laser diode via DAC8562S + second DRV8873S channel)
 *
 * Migration checklist for Spudkortet:
 *  1. Re-generate .ioc: add DRV1/DRV2 pins, DAC8562S CS, ADC_DRDY
 *  2. Restore BSP_DRV* macros below
 *  3. Restore BSP_LASER_CS_PORT/PIN
 *  4. Restore BSP_ADC_DRDY_PIN for EXTI-based DRDY in bsp_temp.c
 *  5. Change BSP_DEVICE_MODE_STR below to "SPUD"
 */

/* Sent to the display once per boot/resync (see cli_sync_display()) so one
 * shared display firmware binary can show the right UI for whichever board
 * it's actually plugged into, without the display having to guess. */
#define BSP_DEVICE_MODE_STR  "UGN"

/* ── CLI terminal — USB CDC ───────────────────────────────────────────────────
 * CLI over USB CDC (PA11/PA12). Display is USART1 (PA9/PA10); PC10/PC11 is SPI3.
 */
#define BSP_CLI_Send(buf, len)   CDC_Transmit_FS((uint8_t*)(buf), (uint16_t)(len))
#define BSP_CLI_Receive(pbyte)   CDC_CLI_Receive(pbyte)

/* ── Display UART ────────────────────────────────────────────────────────────
 * USART1 (PA9/PA10) — bidirectional bridge to ESP32 display.
 * RX is interrupt-driven (see cli.c) -- no BSP_DISPLAY_Receive() macro;
 * a 1-byte polling read silently drops bytes when the main loop is briefly
 * elsewhere (confirmed on the bench), which the ISR-fed ring buffer avoids.
 */
#define BSP_DISPLAY_Send(buf, len)   HAL_UART_Transmit(&huart1, (uint8_t*)(buf), (uint16_t)(len), 100)

/* ── Shared SPI bus ──────────────────────────────────────────────────────────
 * SPI3: PC10 (SCK), PC11 (MISO), PC12 (MOSI) — Mode 1 (CPOL=0, CPHA=1).
 * ADS1220 CS: PA4.  DAC8562S CS: PA3 (reserved for Spudkortet).
 */
#define BSP_SPI_BUS         (&hspi3)
#define BSP_ADC_CS_PORT     ADC_CS_GPIO_Port
#define BSP_ADC_CS_PIN      ADC_CS_Pin
#define BSP_LASER_CS_PORT   DAC_CS_GPIO_Port   /* reserved — DAC8562S on Spudkortet */
#define BSP_LASER_CS_PIN    DAC_CS_Pin

/* ── Internal DAC for TEC amplitude ─────────────────────────────────────────
 * DAC1_OUT2 (PA5) = Crystal/oven TEC amplitude
 * Laser TEC (Spudkortet): PA4 freed up on spud by moving ADC_CS → DAC1_OUT1
 */
#define BSP_DAC                 (&hdac1)
#define BSP_DAC_CH_CRYSTAL      DAC_CHANNEL_2
#define BSP_DAC_CH_LASER        DAC_CHANNEL_2  /* placeholder — spudkortet uses DAC1_CH1 (PA4) */

/* ── TEC enable and polarity (UGN — TPS563252 programmable buck) ─────────────
 * TEC_EN (PC5): enable the TEC buck converter.
 * POLARITY (PC6): H = heat, L = cool (verify empirically on hardware).
 * TEC_PG (PA2): power-good input from buck (open-drain, pulled up in gpio.c).
 */
#define BSP_TEC_EN_PORT         TEC_EN_GPIO_Port
#define BSP_TEC_EN_PIN          TEC_EN_Pin
#define BSP_TEC_POLARITY_PORT   POLARITY_GPIO_Port
#define BSP_TEC_POLARITY_PIN    POLARITY_Pin
#define BSP_TEC_PG_PORT         TEC_PG_GPIO_Port
#define BSP_TEC_PG_PIN          TEC_PG_Pin

/* ── TEC buck FB-network (for BSP_TEC_EstimateVoltage(), a COMMANDED, not
 * measured, VOUT estimate -- see bsp_tec.c) ─────────────────────────────────
 * VOUT = VREF + R14*(VREF/R16 + (VREF-VDAC)/R15), per the M5_TPS563252
 * schematic sheet (R14 between VOUT and FB, R15 between DACin and FB, R16
 * between FB and GND).
 *
 * Docs/ugn_pinout_and_bringup_status.md §6.1 previously flagged R14 as
 * possibly misread (150k vs 15k), based on the schematic's two documented
 * design points (VDAC=0V->VOUT=10.8V, VDAC=2.4V->VOUT=0V) not agreeing on a
 * consistent internal reference voltage with R15=3.3k/R16=1.2k. Corrected:
 * it's actually R15 and R16 that were misread by 10x (33k/12k, not
 * 3.3k/1.2k) -- confirmed by working the same two-design-point check
 * against these values: both agree to ~1% on an implied reference of
 * ~0.60V, which independently matches the TPS563252's actual datasheet FB
 * reference voltage (~0.6V) -- strong, if not bench-measured, confirmation.
 * R14=150k (as originally read) was correct all along.
 */
#define BSP_TEC_FB_R14_OHM   150000.0f
#define BSP_TEC_FB_R15_OHM   33000.0f
#define BSP_TEC_FB_R16_OHM   12000.0f
#define BSP_TEC_FB_VREF_V    0.6f      /* TPS563252 internal FB reference --
                                         * NOT BSP_NTC_VREF_V, a different,
                                         * unrelated 2.9V rail (ADS1220 only) */

/* STM32 internal DAC's own reference -- standard STM32G4 default (no VREF+
 * override configured in dac.c), so DAC full-scale (code 4095) tracks VDDA,
 * which this board ties to the same 3.3V rail as VDD (M1_TPS62172). */
#define BSP_DAC_VREF_V        3.3f

/* ── NTC thermistor: Steinhart-Hart coefficients ─────────────────────────────
 * Replace with values from your NTC datasheet or calibration.
 * Example below is for a generic 100 kΩ NTC at 25°C (B=3950 K).
 */
#define BSP_NTC_SH_A    8.757e-4f
#define BSP_NTC_SH_B    2.535e-4f
#define BSP_NTC_SH_C    1.838e-7f

/* ── NTC voltage divider ─────────────────────────────────────────────────────
 * ADS1220 external reference = STM32 VREFBUF output (SCALE2, enabled in
 * main.c USER CODE BEGIN 2), routed to REFP0 via net "V2.9".
 * Measured on bench: 2.899V
 * R_series = resistor in series with NTC.
 */
#define BSP_NTC_VREF_V       2.9f
#define BSP_NTC_RSERIES_OHM  10000.0f

/* ── Laser diode hardware current limit ──────────────────────────────────────
 * Used by bsp_laser.c (Spudkortet). Not active on UGN.
 */
#define BSP_LASER_MAX_CURRENT_A  2.0f

/* ── ADS1220 data rate ───────────────────────────────────────────────────────
 * 0x00 → 20 SPS (50 ms per conversion).
 * Main loop ticks every 100 ms — data is always ready, no DRDY needed.
 */
#define BSP_ADS1220_DR_20SPS   0x00u
#define BSP_ADS1220_DR_90SPS   0x40u
#define BSP_ADS1220_DATA_RATE  BSP_ADS1220_DR_20SPS

#endif /* BSP_CONFIG_H */
