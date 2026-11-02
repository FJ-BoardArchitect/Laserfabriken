#include "bsp_laser.h"
#include "bsp_config.h"

#ifdef BSP_LASER_DAC8562S
#include "stm32g4xx_hal.h"
#include <stdint.h>

/*
 * DAC8562SDGS control — SPI Mode 1 (CPOL=0, CPHA=1), falling edge capture.
 *
 * 24-bit frame: [23:19] CMD  [18:16] DAC select  [15:0] 16-bit data
 *   CMD = 011 (0x18) → write and update selected DAC register
 *   DAC select 000 = DAC A → laser diode current set-point
 *
 * DAC code 0x0000 → 0 A, 0xFFFF → BSP_LASER_MAX_CURRENT_A.
 * The laser driver (external circuit) converts DAC voltage to current.
 */

/* ── DAC8562S SPI write ───────────────────────────────────────────────────── */

static void dac8562_write(uint8_t ch_select, uint16_t code)
{
    uint8_t tx[3] = {
        (uint8_t)(0x18u | ch_select), /* write+update: DAC A=0x18, DAC B=0x19 */
        (uint8_t)(code >> 8),
        (uint8_t)(code & 0xFFu)
    };
    HAL_GPIO_WritePin(BSP_LASER_CS_PORT, BSP_LASER_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_Transmit(BSP_SPI_BUS, tx, 3, 10);
    HAL_GPIO_WritePin(BSP_LASER_CS_PORT, BSP_LASER_CS_PIN, GPIO_PIN_SET);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void BSP_Laser_Init(void)
{
    /* Power-on reset: CMD=0x28, data=0x0001 resets DAC8562S internal registers */
    uint8_t reset_tx[3] = { 0x28u, 0x00u, 0x01u };
    HAL_GPIO_WritePin(BSP_LASER_CS_PORT, BSP_LASER_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_Transmit(BSP_SPI_BUS, reset_tx, 3, 10);
    HAL_GPIO_WritePin(BSP_LASER_CS_PORT, BSP_LASER_CS_PIN, GPIO_PIN_SET);

    dac8562_write(0u, 0u); /* DAC A = 0 (laser current = 0) */
}

void BSP_Laser_SetCurrent(float amps)
{
    // När jag säger en ström i DAC:en så styr den egentligen bara en referensspänning till buckkonvertern och buckkonvertern kan bara bestämma hur mycket ström den spottar ut.
    // Vi vet inte skalan mellan DAC-koden och den faktiska strömmen i lasern, så vi antar att det är linjärt och att maxströmmen motsvarar max DAC-kod. Men börja med en liten ström så att vi inte riskerar att bränna lasern.
    if (amps < 0.0f) amps = 0.0f;
    if (amps > BSP_LASER_MAX_CURRENT_A) amps = BSP_LASER_MAX_CURRENT_A;
    uint16_t code = (uint16_t)(amps / BSP_LASER_MAX_CURRENT_A * 65535.0f);
    dac8562_write(0u, code);
}

void BSP_Laser_Disable(void)
{
    dac8562_write(0u, 0u);
}

#else  /* no DAC8562S (SPUD): the laser is a serial module on BSP_LASER_UART.
        * Protocol not decided yet -- these are stubs so laser_control.c links.
        * Bring-up of LASER_EN/LASER_PG and the UART is done from the CLI
        * ('pin', 'uart', see bringup.c). */
void BSP_Laser_Init(void)              {}
void BSP_Laser_SetCurrent(float amps)  { (void)amps; }
void BSP_Laser_Disable(void)           {}
#endif
