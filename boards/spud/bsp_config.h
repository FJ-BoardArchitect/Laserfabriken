#ifndef BSP_CONFIG_H
#define BSP_CONFIG_H

#include "adc.h"        /* hadc2: TEC Vmeas/Imeas */
#include "usart.h"      /* huart2 (laser connector), hlpuart1 (3-pin UART connector) */
#include "spi.h"
#include "dac.h"
#include "adc.h"        /* hadc2: TEC Vmeas/Imeas */
#include "main.h"
#include "usbd_cdc_if.h"

/*
 * bsp_config.h — SPUD board (boards/spud/Laserfabriken_SPUD.ioc).
 *
 * Pin names below are the CubeMX labels in main.h. Where the HiAx design
 * (beta.hiax.se, project "Laserfabriken SPUD") says something different from
 * the .ioc it is noted; the .ioc is what the generated code follows.
 *
 * Connectors (HiAx allocation, 2026-11-02):
 *   M8  USB-C      display or PC. VBUS fed by M16 (TPS62173 5V 0.5A, EN=PB12)
 *                  through Schottky M17 + bead M11; PC VBUS can feed the board
 *                  back through Schottky M13 when no barrel jack is present.
 *   M3  DC jack    main input "V12" (net54) via bead M12.
 *   M14 PH-4A      crystal oven: GND, NTC (ADS1220 AIN0), TEC+, TEC- (via M1
 *                  polarity switch, driven by M2 constant-current buck).
 *   M20 Molex 8p   USART2 (PA2 TX / PA3 RX, .ioc labels STAGE_TX/RX), four
 *                  GPIO PC7/PC8/PC9/PA8 (.ioc labels STAGE_P1..P4) and 5V from
 *                  M4 (EN=PB15 STAGE_EN, PG=PC6 STAGE_PG).
 *   M9  ZH1.5 3p   LPUART1 (PC1 TX / PC0 RX, .ioc labels LASER_TX/RX) + GND.
 *   M6  terminal   5V from M15 (EN=PA0 LASER_EN, PG=PA1 LASER_PG).
 *   M26 terminal   5V from M18 (EN=PB13 AUX_EN, PG=PB14 AUX_PG).
 *   M19 REDfit     SWD.
 */

/* Sent to the display in the hello reply / state dump. */
#define BSP_DEVICE_MODE_STR  "SPUD"

/* ── CLI terminal — USB CDC (PA11/PA12) ───────────────────────────────────── */
#define BSP_CLI_Send(buf, len)   CDC_Transmit_FS((uint8_t*)(buf), (uint16_t)(len))
#define BSP_CLI_Receive(pbyte)   CDC_CLI_Receive(pbyte)

/* No BSP_DISPLAY_LINK_UART: the display is a USB host on the same USB-C
 * port a PC uses; cli.c switches that channel to machine mode on 'hello'. */

/* ── USB-C VBUS switching (usbc_port.c) ──────────────────────────────────────
 * PB12 EN_5V_USB enables M16. Starts LOW (gpio.c / .ioc PinState RESET):
 * 5V reaches the connector only after UCPD has seen a display (Rd on CC). */
#define BSP_USBC_VBUS_SWITCH  1
#define BSP_USBC_VBUS_EN_PORT EN_5V_USB_GPIO_Port
#define BSP_USBC_VBUS_EN_PIN  EN_5V_USB_Pin

/* ── Shared SPI bus: SPI3 PC10/PC11/PC12, ADS1220 CS on PA4 ─────────────── */
#define BSP_SPI_BUS         (&hspi3)
#define BSP_ADC_CS_PORT     ADC_CS_GPIO_Port
#define BSP_ADC_CS_PIN      ADC_CS_Pin

/* ── Laser ───────────────────────────────────────────────────────────────────
 * No DAC8562S on SPUD (BSP_LASER_DAC8562S not defined -> bsp_laser.c is a
 * stub). The laser is a serial module on LPUART1 (ZH1.5 M9) plus LASER_EN/LASER_PG;
 * protocol not decided yet, see Docs/spud_usb_nasta_steg.md. */
#define BSP_LASER_UART          (&hlpuart1)
#define BSP_LASER_EN_PORT       LASER_EN_GPIO_Port
#define BSP_LASER_EN_PIN        LASER_EN_Pin
#define BSP_LASER_PG_PORT       LASER_PG_GPIO_Port
#define BSP_LASER_PG_PIN        LASER_PG_Pin
#define BSP_LASER_MAX_CURRENT_A 2.0f   /* kept for laser_control.c; unused by the stub */

/* ── Internal DAC: crystal TEC set-point ─────────────────────────────────────
 * DAC1_OUT2 (PA5) -> M2.DACin. No laser TEC channel on SPUD; the placeholder
 * keeps tec_control.c's second channel compiling (bsp_tec.c ignores it). */
#define BSP_DAC                 (&hdac1)
#define BSP_DAC_CH_CRYSTAL      DAC_CHANNEL_2
#define BSP_DAC_CH_LASER        DAC_CHANNEL_2  /* placeholder, no hardware */

/* ── Crystal TEC: M2 TPS563252 constant-current buck + M1 polarity switch ──
 * TEC_EN (PC14), TEC_PG (PC15, open-drain, pulled up), TEC_POLARITY (PA9). */
#define BSP_TEC_EN_PORT         TEC_EN_GPIO_Port
#define BSP_TEC_EN_PIN          TEC_EN_Pin
#define BSP_TEC_POLARITY_PORT   TEC_POLARITY_GPIO_Port
#define BSP_TEC_POLARITY_PIN    TEC_POLARITY_Pin
#define BSP_TEC_PG_PORT         TEC_PG_GPIO_Port
#define BSP_TEC_PG_PIN          TEC_PG_Pin

/* M2 is a CURRENT source, unlike UGN's voltage-mode buck (read off the HiAx
 * module schematic 2026-11-02, verify on the bench):
 *   FB 0.6 V, R39 22k from FB to the INA181A2 output (1 V/A over the 0.02 ohm
 *   shunt R40), R41 22k from DACin to FB, R43 10k FB to GND
 *     => Iout ~= 2.52 A - VDAC (0 V -> 2.5 A, 2.5 V -> 0 A)
 *   Vmeas (PA6, ADC2_IN3) = VOUT * 16k/(47k+16k)      = 0.254 * VOUT
 *   Imeas (PA7, ADC2_IN4) = 1 V/A * 33k/(10k+33k)      = 0.767 V/A
 * BSP_TEC_EstimateVoltage() and its R14/R15/R16 model are UGN-specific; the
 * values below only keep bsp_tec.c compiling until a current-mode model and
 * the ADC2 readings replace it. */
#define BSP_TEC_CURRENT_MODE     1
#define BSP_TEC_IOUT_AT_0V_A     2.52f
#define BSP_TEC_IOUT_PER_V_A    -1.0f
#define BSP_TEC_VMEAS_GAIN       (16.0f / 63.0f)
#define BSP_TEC_IMEAS_V_PER_A    (33.0f / 43.0f)
/* Largest current the PID may command (|output| = 1.0). Set from the TEC
 * element's rating; 1.0 A is a cautious bench default. The converter itself
 * tops out at BSP_TEC_IOUT_AT_0V_A. */
#define BSP_TEC_IMAX_A           1.0f
#define BSP_TEC_MEAS_ADC         (&hadc2)
#define BSP_TEC_VMEAS_CH         ADC_CHANNEL_3   /* PA6 */
#define BSP_TEC_IMEAS_CH         ADC_CHANNEL_4   /* PA7 */
#define BSP_TEC_FB_R14_OHM   150000.0f  /* UGN model, not valid on SPUD */
#define BSP_TEC_FB_R15_OHM   33000.0f
#define BSP_TEC_FB_R16_OHM   12000.0f
#define BSP_TEC_FB_VREF_V    0.6f

/* STM32 DAC and ADC reference = VREF+ pin, driven by the internal VREFBUF at
 * 2.9 V (main.c, SCALE2) -- not VDDA. Full-scale DAC code 4095 = 2.9 V. */
#define BSP_DAC_VREF_V        2.9f
#define BSP_ADC_VREF_V        2.9f

/* ── NTC: Steinhart-Hart + divider (copied from UGN, verify for SPUD) ─────── */
#define BSP_NTC_SH_A    8.757e-4f
#define BSP_NTC_SH_B    2.535e-4f
#define BSP_NTC_SH_C    1.838e-7f
#define BSP_NTC_VREF_V       2.9f       /* VREFBUF SCALE2 -> REFP0 ("V2.9"), set in main.c */
#define BSP_NTC_RSERIES_OHM  10000.0f

/* ── ADS1220 data rate ─────────────────────────────────────────────────────── */
#define BSP_ADS1220_DR_20SPS   0x00u
#define BSP_ADS1220_DR_90SPS   0x40u
#define BSP_ADS1220_DATA_RATE  BSP_ADS1220_DR_20SPS

/* ── Bring-up tables (Core/Src/app/bringup.c: CLI 'pin' and 'uart') ─────────
 * X(name, cubemx_label, writable). Read-only entries are owned by other code
 * (TEC_EN by tec_control, EN_5V_USB by usbc_port) or are inputs.
 * PC7/PC8/PC9 are inputs in the .ioc but DIGITAL.DO in the HiAx allocation --
 * to be settled; they are read-only here until then. */
#define BSP_BRINGUP_PINS(X) \
    X("laser_en",  LASER_EN,  1) \
    X("laser_pg",  LASER_PG,  0) \
    X("stage_en",  STAGE_EN,  1) \
    X("stage_pg",  STAGE_PG,  0) \
    X("stage_p1",  STAGE_P1,  0) \
    X("stage_p2",  STAGE_P2,  0) \
    X("stage_p3",  STAGE_P3,  0) \
    X("stage_p4",  STAGE_P4,  0) \
    X("aux_en",    AUX_EN,    1) \
    X("aux_pg",    AUX_PG,    0) \
    X("tec_en",    TEC_EN,    0) \
    X("tec_pg",    TEC_PG,    0) \
    X("5v_usb",    EN_5V_USB, 0)

/* X(name, handle, connector) */
/* UARTs received through DMA + IDLE-line interrupt by Core/Src/bsp/uart_rx.c.
 * Same order as BSP_BRINGUP_UARTS below. Each needs a circular RX DMA channel
 * and the UART's global interrupt enabled in the .ioc (done for both). */
#define BSP_UART_RX_PORTS(X) \
    X(laser, hlpuart1) \
    X(stage, huart2)

#define BSP_BRINGUP_UARTS(X) \
    X("laser", hlpuart1, "LPUART1 PC1/PC0 -> ZH1.5 M9") \
    X("stage", huart2,   "USART2 PA2/PA3 -> Molex M20")

#endif /* BSP_CONFIG_H */
