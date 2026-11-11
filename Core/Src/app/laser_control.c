#include "laser_control.h"
#include "tec_control.h"
#include "bsp_laser.h"
#include "bsp_config.h"

/* SW-SPEC-001 §3.2: the laser is always off at start-up; the software limit
 * is configurable but never exceeds BSP_LASER_MAX_CURRENT_A (hardware max
 * in bsp_config.h); commissioning starts at a 0.1 A limit and raises it
 * stepwise against a multimeter. The limit is persisted (params.c). */
LaserState_t g_laser_state = {
    .current_A     = 0.0f,
    .max_current_A = 0.1f,   /* first-start software limit, raised via 'set laser.maxcurrent' */
    .threshold_A   = 0.1f,   /* lasing threshold — no output below this */
    .power_pct     = 0,
    .enabled       = 0,
};

/* Reverse-map current → power_pct so both fields stay in sync. */
static uint8_t current_to_pct(float amps)
{
    if (amps <= 0.0f) return 0;
    float range = g_laser_state.max_current_A - g_laser_state.threshold_A;
    if (range <= 0.0f) return 0;
    int pct = (int)((amps - g_laser_state.threshold_A) / range * 100.0f + 0.5f);
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

void Laser_Control_Init(void)
{
    BSP_Laser_Init();
}

void Laser_Control_Process(void)
{
    BSP_Laser_Process();
}

void Laser_Control_SetCurrent(float amps)
{
    if (amps < 0.0f) amps = 0.0f;
    if (amps > g_laser_state.max_current_A) amps = g_laser_state.max_current_A;
    /* Below threshold but not zero: no lasing occurs — snap to 0 */
    if (amps > 0.0f && amps < g_laser_state.threshold_A) amps = 0.0f;
    g_laser_state.current_A = amps;
    g_laser_state.power_pct = current_to_pct(amps);
    if (g_laser_state.enabled)
        BSP_Laser_SetCurrent(amps);
}

void Laser_Control_SetMaxCurrent(float amps)
{
    if (amps < 0.0f) amps = 0.0f;
    if (amps > BSP_LASER_MAX_CURRENT_A) amps = BSP_LASER_MAX_CURRENT_A;   /* hardware max wins */
    g_laser_state.max_current_A = amps;
    if (g_laser_state.current_A > amps) {
        g_laser_state.current_A = amps;          /* spec: reduce immediately */
        if (g_laser_state.enabled)
            BSP_Laser_SetCurrent(amps);
    }
    if (g_laser_state.current_A > 0.0f)
        g_laser_state.power_pct = current_to_pct(g_laser_state.current_A);
}

void Laser_Control_SetThreshold(float amps)
{
    if (amps < 0.0f) amps = 0.0f;
    g_laser_state.threshold_A = amps;

    if (g_laser_state.current_A > 0.0f && g_laser_state.current_A < amps) {
        /* Current now below threshold — zero it */
        g_laser_state.current_A = 0.0f;
        g_laser_state.power_pct = 0;
        if (g_laser_state.enabled)
            BSP_Laser_SetCurrent(0.0f);   /* stays on (LASER_EN high), just no current */
    } else {
        /* Threshold changed — recompute power percentage */
        g_laser_state.power_pct = current_to_pct(g_laser_state.current_A);
    }
}

void Laser_Control_SetPower(uint8_t pct)
{
    if (pct > 100) pct = 100;
    g_laser_state.power_pct = pct;

    float amps;
    if (pct == 0) {
        amps = 0.0f;
    } else {
        amps = g_laser_state.threshold_A
             + (pct / 100.0f) * (g_laser_state.max_current_A - g_laser_state.threshold_A);
    }

    g_laser_state.current_A = amps;
    if (g_laser_state.enabled)
        BSP_Laser_SetCurrent(amps);
}

void Laser_Control_Enable(void)
{
    g_laser_state.enabled = 1;
    TEC_LaserTempGuard_Reset();
    BSP_Laser_SetCurrent(g_laser_state.current_A);
}

void Laser_Control_Disable(void)
{
    g_laser_state.enabled = 0;
    BSP_Laser_Disable();
}

const LaserState_t *Laser_Control_GetState(void)
{
    return &g_laser_state;
}
