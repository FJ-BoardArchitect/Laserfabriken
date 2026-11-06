#ifndef LASER_CONTROL_H
#define LASER_CONTROL_H

#include <stdint.h>

typedef struct {
    float   current_A;       /* current set-point (amps) */
    float   max_current_A;   /* software current limit (amps) */
    float   threshold_A;     /* lasing threshold — laser produces no output below this (amps) */
    uint8_t power_pct;       /* power level 0–100 %; 0 = off, 1–100 = linear threshold→max */
    uint8_t enabled;         /* 0 = laser off, 1 = laser on */
} LaserState_t;

extern LaserState_t g_laser_state;

void               Laser_Control_Init(void);
/* Main loop: runs the laser BSP (serial module state machine on SPUD). */
void               Laser_Control_Process(void);
void               Laser_Control_SetCurrent(float amps);
void               Laser_Control_SetMaxCurrent(float amps);
void               Laser_Control_SetThreshold(float amps);
void               Laser_Control_SetPower(uint8_t pct);
void               Laser_Control_Enable(void);
void               Laser_Control_Disable(void);
const LaserState_t *Laser_Control_GetState(void);

#endif /* LASER_CONTROL_H */
