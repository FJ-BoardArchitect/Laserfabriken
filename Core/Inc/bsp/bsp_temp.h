#ifndef BSP_TEMP_H
#define BSP_TEMP_H

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

#endif /* BSP_TEMP_H */
