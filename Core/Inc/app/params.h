#ifndef PARAMS_H
#define PARAMS_H

#include <stdint.h>

/*
 * Flash parameter storage — STM32G431RB only.
 * Last 2 KB page (page 63, 0x0801F800) reserved in linker script.
 * Wear leveling: 19 entries per page × 10 000 erases = 190 000 saves
 * (entry count set by PARAMS_MAX in params.c — see the comment there for
 * why it must be recomputed whenever FlashParams_t changes size).
 */

typedef struct {
    uint32_t magic;                 /* 0x4C415352 = "LASR" */
    uint32_t seq;                   /* monotonically increasing — highest valid seq wins */
    float    crystal_kp;
    float    crystal_ki;
    float    crystal_kd;
    float    crystal_setpoint;      /* °C */
    float    crystal_wl_k;          /* nm/°C */
    float    crystal_wl_m;          /* nm */
    float    crystal_wavelength_nm; /* informational: k * setpoint + m */
    float    laser_kp;
    float    laser_ki;
    float    laser_kd;
    float    laser_setpoint;        /* °C */
    float    laser_max_current_A;
    float    laser_threshold_A;
    uint8_t  laser_power_pct;       /* 0–100 */
    uint8_t  _pad[3];
    float    temp_max_dev_C;
    float    temp_abs_max_C;
    float    temp_timer_s;
    float    ntc_vref_v;            /* NTC divider reference (V) */
    float    ntc_rseries_ohm;       /* NTC series resistor (ohm) */
    float    ntc_sh_a;              /* Steinhart-Hart A */
    float    ntc_sh_b;              /* Steinhart-Hart B */
    float    ntc_sh_c;              /* Steinhart-Hart C */
    uint32_t _pad2;                 /* keeps struct doubleword-aligned */
    uint32_t crc;                   /* CRC32 over all preceding bytes */
} FlashParams_t;                    /* 104 bytes = 13 × 8 (doubleword-aligned) */

/* Call after TEC_Control_Init() and Laser_Control_Init(). */
void Params_Load(void);

/* Write current live state to flash. Returns 0 on success, -1 on error. */
int  Params_Save(void);

/* Mark parameters as dirty; Params_Process() will save after 20 s debounce. */
void Params_MarkDirty(void);

/* Call every main-loop iteration to handle deferred auto-save. */
void Params_Process(void);

#endif /* PARAMS_H */
