#ifndef BSP_LASER_H
#define BSP_LASER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * BSP layer for the laser. Two hardware variants, chosen in bsp_config.h:
 *
 *   BSP_LASER_DAC8562S   (UGN)  a DAC8562S over SPI sets a current
 *                               reference for an external driver.
 *   BSP_LASER_SERIAL_DK  (SPUD) a DK Photonics light-source module on
 *                               BSP_LASER_UART, "Light Source Communication
 *                               Protocol": binary frames, 9600 8N1, current
 *                               in 0.1 mA steps, software switch, telemetry.
 *
 * laser_control.c only ever calls the three functions below. The serial
 * variant runs a state machine from BSP_Laser_Process() (main loop) so
 * none of them block: a request is remembered and carried out as the
 * module answers.
 */
void BSP_Laser_Init(void);
void BSP_Laser_SetCurrent(float amps);   /* also means "emit": switch on once set */
void BSP_Laser_Disable(void);            /* SPUD: LASER_EN low (module unpowered); UGN: DAC 0 */

/* ── Serial module (SPUD) ───────────────────────────────────────────────── */
typedef struct {
    const char *state;        /* state machine name, for 'status'             */
    bool     powered;         /* LASER_EN high                                  */
    bool     pg;              /* LASER_PG seen high after power-on              */
    bool     emitting;        /* module confirmed its software switch on        */
    uint8_t  max_source;      /* module's own max: 0 not read yet, 1 read, 2 no answer */
    float    module_max_A;    /* as reported by the module (0 until read). Information
                               * only: the ceiling is BSP_LASER_MAX_CURRENT_A (spec) */
    float    set_current_A;   /* last current the module acknowledged           */
    /* telemetry from "query all" (cmd 0x00), refreshed about once a second */
    float    iop_A, tchip_C;
    uint16_t pin_raw, pout_raw;   /* units not given in the protocol document */
    uint8_t  sw, mode, cur_alm, pin_alm, pout_alm;
    uint32_t telemetry_tick;      /* HAL tick of the last good query reply    */
    uint32_t tx_frames, rx_frames, bad_frames, timeouts;
} laser_module_t;

void   BSP_Laser_Process(void);               /* main loop                      */
void   BSP_Laser_Power(bool on);              /* power the module without emitting (probe) */
const laser_module_t *BSP_Laser_Module(void);
bool   BSP_Laser_OwnsPort(const void *huart); /* true: the driver consumes this UART's frames */
size_t BSP_Laser_PopMonitor(uint8_t *dst, size_t max); /* reply frames for 'uart monitor' */

#endif /* BSP_LASER_H */
