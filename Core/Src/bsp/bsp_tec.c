#include "bsp_tec.h"
#include "bsp_config.h"
#include "dac.h"
#include "stm32g4xx_hal.h"
#include <stdint.h>
#include <assert.h>

/*
 * TEC control — UGN board.
 *
 * Amplitude: STM32 internal DAC1_OUT2 (PA5 = Crystal TEC).
 *            DAC code 0 = no drive, 4095 = full drive.
 *            Buck converter (M5 TPS563252) inverts: VDAC high → VOUT low.
 *            So we invert the code: dac_code = 4095 - magnitude_code.
 *
 * Direction: POLARITY GPIO (PC6).
 *            GPIO_PIN_SET   = heat, GPIO_PIN_RESET = cool (verify on hardware).
 *
 * Enable:    TEC_EN GPIO (PC5) — drives M5.EN on the buck converter.
 */

// /* ── Internal DAC helper ────────────────────────────────────────────────────── */

// static void internal_dac_set(const TecChannel_t ch, uint16_t code_12bit)
// {
//     uint32_t dac_ch = (ch == TEC_CRYSTAL) ? BSP_DAC_CH_CRYSTAL : BSP_DAC_CH_LASER;
//     /* Buck converter output is inversely proportional to VDAC.
//      * Invert so that higher |output| → more TEC current. */
//     uint16_t inverted = 4095u - code_12bit;
//     HAL_DAC_SetValue(BSP_DAC, dac_ch, DAC_ALIGN_12B_R, inverted);
//     HAL_DAC_Start(BSP_DAC, dac_ch);
// }


/* ── Static variables ────────────────────────────────────────────────────────── */

static uint8_t _polarity_state_ch[TEC_CHANNELS] = { POLARITY_INIT, POLARITY_INIT };
static float _dac_val[TEC_CHANNELS] = { 0, 0 };
static uint8_t _tec_enabled[TEC_CHANNELS] = { 0, 0 };

/* ── Public API ──────────────────────────────────────────────────────────────── */

void BSP_TEC_Init(const TecChannel_t ch)
{
    assert(ch < TEC_CHANNELS);

    BSP_TEC_Reset(ch);
    BSP_TEC_SetDac(ch, 0.0f);
    BSP_TEC_Enable(ch, 0);
}

void BSP_TEC_Enable(const TecChannel_t ch, uint8_t enable)
{
    assert(ch < TEC_CHANNELS);

    _tec_enabled[ch] = enable;
    if (!enable) {
        /* Disable buck converter and zero the output. */
        HAL_GPIO_WritePin(TEC_EN_GPIO_Port, TEC_EN_Pin, GPIO_PIN_RESET);
    }
}

void BSP_TEC_Reset(const TecChannel_t ch)
{
    _polarity_state_ch[ch] = POLARITY_INIT;
}

void BSP_TEC_SetDac(TecChannel_t ch, float dac_val)
{
    _dac_val[ch] = dac_val;
}

void BSP_TEC_SetOutput(TecChannel_t ch)
// Executed 10 times per second
{
    if (ch >= TEC_CHANNELS)
        return;

    float value = _dac_val[ch];
    uint8_t polarity_state = _polarity_state_ch[ch];
    uint16_t dac_code;

    // Set polarity after initialization
    if (polarity_state == POLARITY_RUN) {
        if (value >= 0.0f) {
            polarity_state = POLARITY_HEAT;
            BSP_TEC_SetRawPolarity(true);
        } else {
            polarity_state = POLARITY_COOL;
            BSP_TEC_SetRawPolarity(false);
        }

        /* Write enable pin */
        if (_tec_enabled[ch]) {
            HAL_GPIO_WritePin(TEC_EN_GPIO_Port, TEC_EN_Pin, GPIO_PIN_SET);
        }

    } else if (((value >= 0.0f) && (polarity_state == POLARITY_COOL)) ||
        ((value < 0.0f) && (polarity_state == POLARITY_HEAT))) {
        // Check if the polarity has changed and reset to INIT if it has    

        polarity_state = POLARITY_INIT;
    }

    // Set DAC output depending on state
    if (polarity_state == POLARITY_INIT) {
        BSP_TEC_SetRawDAC(4095u);

        /* Disable buck converter */
        HAL_GPIO_WritePin(TEC_EN_GPIO_Port, TEC_EN_Pin, GPIO_PIN_RESET);
        polarity_state = POLARITY_RUN;
    } else {
        if (value < 0.0f) {
            value = -value; // Make value positive for DAC calculation
        }
        if (value > 0.5f) value = 0.5f;

        dac_code = 4095u - (uint16_t)(value * 4095.0f);
        BSP_TEC_SetRawDAC(dac_code);
    }
    _polarity_state_ch[ch] = polarity_state;
}

float BSP_TEC_EstimateVoltage(float normalized_output)
{
    /* Reproduce BSP_TEC_SetOutput()'s own dac_code derivation exactly
     * (lines above: clamp to [0,0.5], invert), so this always reflects
     * what's actually being commanded, not a separate approximation of it. */
    float value = normalized_output;
    if (value < 0.0f) {
        value = -value;
    }
    if (value > 0.5f) value = 0.5f;
    uint16_t dac_code = 4095u - (uint16_t)(value * 4095.0f);

    /* DAC code -> actual STM32 DAC pin voltage (BSP_DAC_VREF_V, bsp_config.h). */
    float vdac = ((float)dac_code / 4095.0f) * BSP_DAC_VREF_V;

    /* VDAC -> buck VOUT, via the FB-network formula (BSP_TEC_FB_*, bsp_config.h):
     * VOUT = VREF + R14*(VREF/R16 + (VREF-VDAC)/R15). */
    float vout = BSP_TEC_FB_VREF_V
                 + BSP_TEC_FB_R14_OHM * (BSP_TEC_FB_VREF_V / BSP_TEC_FB_R16_OHM
                                          + (BSP_TEC_FB_VREF_V - vdac) / BSP_TEC_FB_R15_OHM);

    /* VDAC above the ~2.4V design point implies a negative VOUT target,
     * which isn't physically real -- the buck goes into OVP lockout and
     * stops switching instead (matches BSP_TEC_SetRawDAC()'s own comment
     * on raw=4095 being the empirically-confirmed inert state). Clamp
     * rather than report a negative "voltage". */
    if (vout < 0.0f) vout = 0.0f;

    return vout;
}

// void BSP_TEC_Disable(TecChannel_t ch)
// {
//     if (ch != TEC_CRYSTAL)
//         return;

//     /* Zero the TEC drive only -- do NOT touch EN here. The TPS563252 has
//      * its own non-latching UVLO/thermal protection and hiccup-mode UVP
//      * recovery; it retries on its own schedule regardless of what we do.
//      * Toggling EN off on a PG fault with no corresponding re-enable
//      * elsewhere previously left EN latched off permanently after the
//      * first fault, since nothing could turn it back on once the buck
//      * could no longer run. Leaving EN alone avoids that deadlock -- our
//      * job is only to make sure the TEC gets zero drive while PG isn't
//      * confirmed good, not to manage the buck's own fault recovery. */
//     internal_dac_set(TEC_CRYSTAL, 0);
// }

bool BSP_TEC_PowerGood(void)
{
    /* TPS563252 PG is open-drain: pulled high externally when output is good,
     * driven low by the converter on fault (UVLO, OVP, thermal, etc.). */
    return HAL_GPIO_ReadPin(BSP_TEC_PG_PORT, BSP_TEC_PG_PIN) == GPIO_PIN_SET;
}

void BSP_TEC_SetRawDAC(uint16_t raw_code)
{
    /* No inversion, no clamping beyond the DAC's own 12-bit range -- this is
     * a direct hardware poke for bench sweeps, not a control-loop path.
     * Start any sweep at 4095 and step DOWN: raw=4095 (max VDAC) is the
     * empirically confirmed inert state on this board (FB pinned above the
     * OVP threshold, buck refuses to switch, PG low, no current flows).
     * Moving toward 0 is what makes the buck start actually regulating --
     * step in small increments and check FB/PG after each one instead of
     * jumping toward 0 directly. */
    HAL_DAC_SetValue(BSP_DAC, BSP_DAC_CH_CRYSTAL, DAC_ALIGN_12B_R, raw_code);
    HAL_DAC_Start(BSP_DAC, BSP_DAC_CH_CRYSTAL);
}

void BSP_TEC_SetRawPolarity(bool heat)
{
    HAL_GPIO_WritePin(BSP_TEC_POLARITY_PORT, BSP_TEC_POLARITY_PIN,
                       heat ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
