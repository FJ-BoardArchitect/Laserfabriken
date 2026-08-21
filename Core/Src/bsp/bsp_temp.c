#include "bsp_temp.h"
#include "bsp_config.h"
#include "stm32g4xx_hal.h"
#include <math.h>
#include <stdint.h>

/*
 * ADS1220 SPI interface — Mode 1 (CPOL=0, CPHA=1).
 *
 * UGN board: single NTC on AIN0 (single-ended vs AVSS).
 * External reference 2.9 V on REFP0/REFN0.
 * Dedicated DRDY pin not connected — continuous mode at 20 SPS (50 ms).
 * Main loop ticks every 100 ms so data is always ready; no DRDY check needed.
 */

/* ── ADS1220 command bytes ───────────────────────────────────────────────── */

#define ADS1220_CMD_RESET               0x06u
#define ADS1220_CMD_START               0x08u
#define ADS1220_CMD_RDATA               0x10u
#define ADS1220_CMD_WREG(reg, n)        ((uint8_t)(0x40u | ((reg) << 2) | ((n) - 1u)))

/* Register 0: MUX + gain */
/* AIN0 single-ended vs AVSS: MUX[3:0] = 1000b → bits [7:4] = 0x80 */
#define ADS1220_MUX_AIN0_AVSS           0x80u
#define ADS1220_GAIN_1                  0x00u
#define ADS1220_PGA_BYPASS              0x01u

/* Register 1: data rate, continuous conversion */
#define ADS1220_REG1_CONTINUOUS         0x04u

/* Register 2: external reference on REFP0/REFN0 — VREF[1:0] = 01b → bits [7:6] = 0x40 */
#define ADS1220_VREF_EXTERNAL_REFP0     0x40u
/* 50/60[1:0] = 01b → bits [5:4] = 0x10: simultaneous 50Hz+60Hz rejection.
 * Product ships internationally (both mains frequencies), so reject both
 * unconditionally rather than picking one region at compile time. */
#define ADS1220_FILTER_50_60HZ          0x10u

/* ── NTC calibration (runtime, CLI-adjustable) ──────────────────────────────
 * Seeded from bsp_config.h's BSP_NTC_* compile-time defaults; overwritten by
 * Params_Load() if a saved value exists (params.c). */
float g_ntc_vref_v      = BSP_NTC_VREF_V;
float g_ntc_rseries_ohm = BSP_NTC_RSERIES_OHM;
float g_ntc_sh_a        = BSP_NTC_SH_A;
float g_ntc_sh_b        = BSP_NTC_SH_B;
float g_ntc_sh_c        = BSP_NTC_SH_C;

/* ── CS helpers ──────────────────────────────────────────────────────────── */

static void ads_cs_low(void)
{
    HAL_GPIO_WritePin(BSP_ADC_CS_PORT, BSP_ADC_CS_PIN, GPIO_PIN_RESET);
}

static void ads_cs_high(void)
{
    HAL_GPIO_WritePin(BSP_ADC_CS_PORT, BSP_ADC_CS_PIN, GPIO_PIN_SET);
}

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

static void ads_write(uint8_t *buf, uint16_t len)
{
    HAL_SPI_Transmit(BSP_SPI_BUS, buf, len, 10);
}

static void ads_read(uint8_t *buf, uint16_t len)
{
    HAL_SPI_Receive(BSP_SPI_BUS, buf, len, 10);
}

/* ── ADS1220 helpers ─────────────────────────────────────────────────────── */

static void ads_send_cmd(uint8_t cmd)
{
    ads_cs_low();
    ads_write(&cmd, 1);
    ads_cs_high();
}

static void ads_write_regs(uint8_t start_reg, uint8_t *data, uint8_t n)
{
    uint8_t cmd = ADS1220_CMD_WREG(start_reg, n);
    ads_cs_low();
    ads_write(&cmd, 1);
    ads_write(data, n);
    ads_cs_high();
}

/* ── Raw ADC read ────────────────────────────────────────────────────────── */

static int32_t ads_read_raw(void)
{
    uint8_t raw[3] = {0, 0, 0};
    uint8_t cmd    = ADS1220_CMD_RDATA;

    ads_cs_low();
    ads_write(&cmd, 1);
    ads_read(raw, 3);
    ads_cs_high();

    int32_t val = ((int32_t)raw[0] << 16) | ((int32_t)raw[1] << 8) | raw[2];
    if (val & 0x800000) val |= (int32_t)0xFF000000;

    return val;
}

/* ── Signal chain: raw → resistance → °C ────────────────────────────────── */

static float raw_to_resistance(int32_t raw)
{
    /* Full scale ±(2^23 - 1) = ±VREF with gain=1 and external ref */
    float voltage = g_ntc_vref_v * ((float)raw / (float)0x7FFFFF);
    if (voltage <= 0.0f || voltage >= g_ntc_vref_v) return 1e9f;

    return g_ntc_rseries_ohm * voltage / (g_ntc_vref_v - voltage);
}

static float resistance_to_celsius(float r_ohm)
{
    float ln_r  = logf(r_ohm);
    float inv_T = g_ntc_sh_a
                + g_ntc_sh_b * ln_r
                + g_ntc_sh_c * ln_r * ln_r * ln_r;
    return (1.0f / inv_T) - 273.15f;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void BSP_Temp_Init(void)
{
    ads_send_cmd(ADS1220_CMD_RESET);
    HAL_Delay(1);

    uint8_t cfg[3] = {
        ADS1220_MUX_AIN0_AVSS | ADS1220_GAIN_1 | ADS1220_PGA_BYPASS,
        BSP_ADS1220_DATA_RATE  | ADS1220_REG1_CONTINUOUS,
        ADS1220_VREF_EXTERNAL_REFP0 | ADS1220_FILTER_50_60HZ,
    };
    ads_write_regs(0, cfg, 3);

    ads_send_cmd(ADS1220_CMD_START);
}

float BSP_Temp_Read(TempChannel_t ch)
{
    /* UGN has a single NTC. Both TEMP_CH_CRYSTAL and TEMP_CH_LASER return
     * the same measurement — caller decides how to use it. */
    (void)ch;

    int32_t raw = ads_read_raw();
    if (raw == 0)
        return -273.15f; /* sentinel: conversion not yet valid */

    float r = raw_to_resistance(raw);
    return resistance_to_celsius(r);
}
