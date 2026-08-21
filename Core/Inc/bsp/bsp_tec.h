#ifndef BSP_TEC_H
#define BSP_TEC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TEC_CRYSTAL = 0,
    TEC_LASER   = 1,
    TEC_CHANNELS = 2,
} TecChannel_t;

typedef enum {
    POLARITY_COOL = 0,
    POLARITY_HEAT = 1,
    POLARITY_INIT = 2,
    POLARITY_RUN = 3,
} PolarityState_t;


/* Enable the TEC buck converter and zero the output. Call once per channel at startup. */
void BSP_TEC_Init(TecChannel_t ch);

/* Set TEC DAC value using signed integer. Polarity switch is handled by PSB_TEC_SetOutput */
void BSP_TEC_SetDac(TecChannel_t ch, float dac_val);

/* Reset the TEC polarity state. */
void BSP_TEC_Reset(TecChannel_t ch);

/* Set buck converter enable state*/
void BSP_TEC_Enable(TecChannel_t ch, uint8_t enable);

/* Set TEC drive level. Called 10 times per second by TEC_Control_Tick() for each channel.
 * value ∈ [-1.0, +1.0]: sign selects direction via the POLARITY GPIO,
 * magnitude maps to the internal DAC amplitude output. */
void BSP_TEC_SetOutput(TecChannel_t ch);

/* Estimate the crystal buck's COMMANDED output voltage from a normalized PID
 * output (same [-1.0, +1.0] convention as BSP_TEC_SetOutput()'s input), by
 * reproducing that function's own DAC-code derivation and then applying the
 * documented VOUT formula (BSP_TEC_FB_R14/R15/R16_OHM, bsp_config.h).
 * This is NOT a measured voltage -- there is no current/voltage sensing on
 * this board -- it's what the firmware is telling the buck to do. Callers
 * (e.g. the display sync) must label it accordingly, not as a real reading. */
float BSP_TEC_EstimateVoltage(float normalized_output);

/* Zero the output and disable the buck converter. */
void BSP_TEC_Disable(TecChannel_t ch);

/* Read the buck converter's PG (power-good) pin.
 * Returns true when the converter reports healthy output, false on fault. */
bool BSP_TEC_PowerGood(void);

/* Bench-test only: write a raw DAC code (0-4095) straight to hardware,
 * bypassing BSP_TEC_SetOutput()'s magnitude/direction mapping and its
 * inversion entirely. Used by the CLI's 'dac test'/'dac set' commands to
 * manually sweep VDAC and find the buck's real safe range on a given board
 * without going through the PID loop. Never call this from control code —
 * TEC_Control_Tick() must skip driving the crystal channel while a manual
 * sweep is active (see g_tec_manual_test in tec_control.h), otherwise the
 * next 100 ms tick overwrites whatever the sweep just set. */
void BSP_TEC_SetRawDAC(uint16_t raw_code);

/* Bench-test only: drive the POLARITY pin directly, bypassing
 * BSP_TEC_SetOutput()'s sign-based mapping. heat=true drives POLARITY high
 * (assumed heat per the board's convention -- unverified on real hardware,
 * which is exactly what this is for). Used by the CLI's 'polarity
 * heat'/'polarity cool' commands, only while a manual DAC test is active
 * (see g_tec_manual_test in tec_control.h) so it isn't fought by the PID
 * loop's own polarity writes on the next 100 ms tick. */
void BSP_TEC_SetRawPolarity(bool heat);

#endif /* BSP_TEC_H */
