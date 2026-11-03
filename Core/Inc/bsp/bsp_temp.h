#ifndef BSP_TEMP_H
#define BSP_TEMP_H

#include <stdint.h>

typedef enum {
    TEMP_CH_CRYSTAL = 0,
    TEMP_CH_LASER   = 1,
} TempChannel_t;

/* NTC calibration -- runtime-adjustable via CLI ("set temp.* <val>", cli.c)
 * and persisted through Params_Save()/Params_Load(), same as the crystal
 * PID gains and wavelength k/m. Seeded at startup from bsp_config.h's
 * BSP_NTC_* defaults (see bsp_temp.c). */
extern float g_ntc_vref_v;
extern float g_ntc_rseries_ohm;
extern float g_ntc_sh_a;
extern float g_ntc_sh_b;
extern float g_ntc_sh_c;

/* Configure ADS1220 for continuous conversion at lowest noise setting. */
void  BSP_Temp_Init(void);

/* Read temperature in °C for the given channel.
 * Returns 0.0f until ADS1220 SPI is implemented (see bsp_temp.c TODOs). */
float BSP_Temp_Read(TempChannel_t ch);

/* Bring-up: one ADS1220 sample with every step of the signal chain. */
typedef struct {
    int32_t raw;        /* 24-bit two's complement code            */
    float   volts;      /* AIN0 vs AVSS, from raw and the reference */
    float   ohms;       /* NTC resistance from the divider          */
    float   celsius;    /* Steinhart-Hart                           */
} temp_sample_t;
temp_sample_t BSP_Temp_Sample(void);
void BSP_Temp_ReadRegs(uint8_t regs[4]);   /* ADS1220 config registers 0..3 */

#endif /* BSP_TEMP_H */
