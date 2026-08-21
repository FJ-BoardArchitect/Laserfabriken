#include "cli.h"
#include "bsp_config.h"
#include "bsp_temp.h"
#include "tec_control.h"
#include "laser_control.h"
#include "params.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

#define CLI_BUF_LEN  80

static char     s_line_buf[CLI_BUF_LEN];
static uint16_t s_line_pos;

static char     s_disp_buf[CLI_BUF_LEN];
static uint16_t s_disp_pos;

/* Diagnostic only: counts every byte that arrives on the display UART
 * (valid or garbage) and remembers the last one, so 'status' can answer
 * "is anything physically arriving at all?" independent of whether it
 * parses as a command -- splits a wiring/connector fault from a firmware
 * parsing fault instead of guessing. */
static uint32_t s_disp_rx_count;
static uint8_t  s_disp_rx_last;

/* ── Interrupt-driven display UART RX ────────────────────────────────────
 * USART1's RX buffer is only 1 byte deep (FIFO disabled, usart.c). Polling
 * it from CLI_Process() (HAL_UART_Receive, Timeout=0) silently drops any
 * byte that arrives while the main loop is briefly elsewhere -- confirmed
 * on the bench: multi-line bursts from the display consistently lost
 * bytes right at the seam between lines, matching gaps caused by
 * TEC_Control_Tick()'s SPI transactions. HAL_UART_RxCpltCallback() grabs
 * each byte into this ring buffer the instant it arrives via interrupt,
 * independent of what the main loop is doing, which removes that window
 * structurally instead of just narrowing it. Requires USART1's NVIC
 * interrupt enabled in CubeMX (Pinout & Configuration -> USART1 -> NVIC
 * Settings -> USART1 global interrupt). */
#define DISP_RX_RING_SIZE 128
static volatile uint8_t  s_disp_rx_ring[DISP_RX_RING_SIZE];
static volatile uint16_t s_disp_rx_head;
static volatile uint16_t s_disp_rx_tail;
static uint8_t            s_disp_rx_isr_byte;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        uint16_t next = (uint16_t)((s_disp_rx_head + 1u) % DISP_RX_RING_SIZE);
        if (next != s_disp_rx_tail) {
            s_disp_rx_ring[s_disp_rx_head] = s_disp_rx_isr_byte;
            s_disp_rx_head = next;
        } /* ring full (CLI_Process() not keeping up at all) -- drop rather
             than overwrite unread data */
        HAL_UART_Receive_IT(&huart1, &s_disp_rx_isr_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        __HAL_UART_CLEAR_OREFLAG(&huart1);
        __HAL_UART_CLEAR_NEFLAG(&huart1);
        __HAL_UART_CLEAR_FEFLAG(&huart1);
        __HAL_UART_CLEAR_PEFLAG(&huart1);
        /* HAL aborts the pending receive on error -- re-arm it or RX stops
         * permanently, same failure mode the old polling code hit. */
        HAL_UART_Receive_IT(&huart1, &s_disp_rx_isr_byte, 1);
    }
}

static bool disp_rx_pop(uint8_t *out)
{
    if (s_disp_rx_head == s_disp_rx_tail)
        return false;
    *out = s_disp_rx_ring[s_disp_rx_tail];
    s_disp_rx_tail = (uint16_t)((s_disp_rx_tail + 1u) % DISP_RX_RING_SIZE);
    return true;
}

/* ── Output helpers ──────────────────────────────────────────────────────── */

static void cli_send(const char *str)
{
    uint16_t len = (uint16_t)strlen(str);
    /* CDC_Transmit_FS drops the data and returns USBD_BUSY instead of
     * queuing/blocking if the previous USB packet is still in flight.
     * status prints several lines back-to-back, so without this retry
     * later lines get silently dropped or interleaved with the still-
     * transmitting buffer, corrupting the output.
     * Bounded by a timeout (not a bare spin) because CLI_Process() runs
     * in the same main-loop iteration as TEC_Control_Tick() — an
     * unbounded wait here (e.g. USB unplugged mid-transmit) would stall
     * the PG-fault/laser-guard safety checks too. */
    uint32_t start = HAL_GetTick();
    while (BSP_CLI_Send((void *)str, len) == USBD_BUSY) {
        if ((HAL_GetTick() - start) > 20u)
            break;
    }
}

static void cli_sendf(const char *fmt, ...)
{
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cli_send(buf);
}

static void disp_forward(const char *line)
{
    BSP_DISPLAY_Send(line, strlen(line));
    const char crlf[] = "\r\n";
    BSP_DISPLAY_Send(crlf, 2);
}

/* Binary-safe counterpart to cli_send()/cli_sendf() -- both of those call
 * strlen() internally, which would silently truncate at the first 0x00
 * byte. Used by the display-flashing bridge relay to forward raw
 * ESP32 ROM/stub-bootloader bytes back to the PC, where an embedded NUL
 * is expected, ordinary data. Mirrors cli_send()'s existing 20ms-bounded
 * retry-on-USBD_BUSY pattern, just with an explicit length instead of a
 * C string. */
static void cli_send_bin(const uint8_t *data, uint16_t len)
{
    uint32_t start = HAL_GetTick();
    while (BSP_CLI_Send((void *)data, len) == USBD_BUSY) {
        if ((HAL_GetTick() - start) > 20u)
            break;
    }
}

/* Defined near the display-sync state it resets, further down this file
 * (see "Display state sync") -- forward-declared here so bridge_seq_tick()
 * below can call it. */
static void cli_force_display_resync(void);

/* ── Bridge mode (ESP32 display firmware update) ────────────────────────────
 * Relays raw bytes between the PC (USB-CDC) and the ESP32's UART while it's
 * held in its ROM bootloader, so `esptool` on the PC can flash it through
 * the STM32 -- see Docs/esp32_display_firmware_update.md. The ESP32's EN
 * pin isn't wired to the STM32 at all; entering/leaving its ROM bootloader
 * is done instead by power-cycling its 5V rail (M6_EN) while holding GPIO0
 * (DISPLAY_DO3/PB15) low (bootloader) or high (normal boot). The STM32
 * itself runs off a separate 3.3V rail and stays up throughout.
 *
 * File-static, not `g_`: TEC_Control_Tick() and the rest of the app must
 * stay completely unaware of/unaffected by bridge mode. */

#define BRIDGE_POWER_OFF_MS    300u
#define BRIDGE_POWER_ON_MS     500u
#define BRIDGE_IDLE_TIMEOUT_MS 60000u

typedef enum {
    BRIDGE_SEQ_NONE = 0,
    BRIDGE_SEQ_POWER_OFF,     /* 5V cut, waiting out BRIDGE_POWER_OFF_MS */
    BRIDGE_SEQ_POWER_ON_WAIT, /* 5V restored, waiting out BRIDGE_POWER_ON_MS */
} BridgeSeqState_t;

typedef enum {
    BRIDGE_ACTION_ENTER_BRIDGE = 0, /* sequence result: start relaying */
    BRIDGE_ACTION_RESUME_NORMAL,    /* sequence result: back to normal CLI/app boot */
} BridgeSeqAction_t;

static uint8_t           s_bridge_active   = 0;
static BridgeSeqState_t  s_bridge_seq_state = BRIDGE_SEQ_NONE;
static BridgeSeqAction_t s_bridge_seq_action;
static uint32_t          s_bridge_seq_start;
static uint32_t          s_bridge_last_activity;

/* Not a value diff like cli_sync_display()'s other s_sync_* state further
 * down -- device.mode is resent periodically rather than once (see that
 * function's comment); declared here, ahead of process_line(), because the
 * 'display mode ...' commands below need to force an immediate resend. */
static uint32_t s_sync_mode_last_ms = (uint32_t)(0u - 1000u);  /* forces an attempt on the very first call */

/* Bench-test override: 'display mode spud'/'display mode ugn' (process_line(),
 * below) lets the CLI temporarily show the other board's UI on a real unit
 * without touching BSP_DEVICE_MODE_STR or rebuilding -- read by
 * cli_sync_display() further down. Auto-expires so a forgotten override
 * can't leave a real board stuck showing the wrong UI indefinitely. */
#define MODE_OVERRIDE_DURATION_MS 60000u
static uint8_t  s_mode_override_active   = 0;
static uint8_t  s_mode_override_is_spud  = 0;
static uint32_t s_mode_override_start_ms = 0;

/* GPIO0 write happens before cutting power, so it's already correct and
 * stable for the entire down/up cycle -- strapping pins only need to be
 * settled by the time power actually comes back, not before it goes away. */
static void bridge_start_sequence(BridgeSeqAction_t action)
{
    HAL_GPIO_WritePin(DISPLAY_DO3_GPIO_Port, DISPLAY_DO3_Pin,
                       (action == BRIDGE_ACTION_ENTER_BRIDGE) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(M6_EN_GPIO_Port, M6_EN_Pin, GPIO_PIN_RESET); /* 5V off */
    s_bridge_seq_action = action;
    s_bridge_seq_state  = BRIDGE_SEQ_POWER_OFF;
    s_bridge_seq_start  = HAL_GetTick();
}

/* Tick-driven, not HAL_Delay(): a blocking ~800ms delay here would stall
 * main.c's whole loop, including several missed TEC_Control_Tick() calls
 * -- which also gates the PG-fault and laser-temp-guard safety checks.
 * Call once per CLI_Process() iteration. */
static void bridge_seq_tick(void)
{
    if (s_bridge_seq_state == BRIDGE_SEQ_NONE) return;

    uint32_t elapsed = HAL_GetTick() - s_bridge_seq_start;

    if (s_bridge_seq_state == BRIDGE_SEQ_POWER_OFF) {
        if (elapsed >= BRIDGE_POWER_OFF_MS) {
            HAL_GPIO_WritePin(M6_EN_GPIO_Port, M6_EN_Pin, GPIO_PIN_SET); /* 5V on */
            s_bridge_seq_start = HAL_GetTick();
            s_bridge_seq_state = BRIDGE_SEQ_POWER_ON_WAIT;
        }
    } else if (s_bridge_seq_state == BRIDGE_SEQ_POWER_ON_WAIT) {
        if (elapsed >= BRIDGE_POWER_ON_MS) {
            s_bridge_seq_state = BRIDGE_SEQ_NONE;
            if (s_bridge_seq_action == BRIDGE_ACTION_ENTER_BRIDGE) {
                s_bridge_active         = 1;
                s_bridge_last_activity  = HAL_GetTick();
                cli_send("[bridge] ESP32 in ROM bootloader -- relay active\r\n");
            } else {
                /* The freshly-flashed ESP32 has no memory of anything the
                 * STM32 already "told" the previous firmware instance
                 * before it was overwritten -- force a full re-sync rather
                 * than silently relying on the diff-against-last-known-
                 * value logic in cli_sync_display(), which has no idea
                 * the display it's diffing against just lost its state. */
                cli_force_display_resync();
                cli_send("OK\r\n> ");
            }
        }
    }
}

/* Recognizes and fully handles (including its own prompt) the bridge-
 * control commands, before process_line()/disp_forward() ever see them.
 * process_line() is shared between the USB-CDC path and the display-UART
 * path (disp_rx_pop() below) -- these commands must never be reachable
 * from the display side, since a garbled/malformed byte stream arriving
 * *from* the display that happened to parse as this exact text could
 * otherwise trigger a real power-cycle + bootloader-entry sequence from
 * the wrong direction. Only called from CLI_Process()'s USB-CDC
 * line-completion branch. Returns 1 if `line` was one of these commands
 * (fully handled); 0 otherwise, so the caller falls through as normal. */
static int bridge_handle_command(const char *line)
{
    if (strcmp(line, "display_update") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send("ERR: busy\r\n> ");
            return 1;
        }
        cli_send("OK\r\n> ");
        bridge_start_sequence(BRIDGE_ACTION_ENTER_BRIDGE);
        return 1;
    }
    if (strcmp(line, "display_update_finished") == 0) {
        if (s_bridge_active) {
            cli_send("ERR: still bridging -- exit bridge mode first\r\n> ");
            return 1;
        }
        if (s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send("ERR: busy\r\n> ");
            return 1;
        }
        bridge_start_sequence(BRIDGE_ACTION_RESUME_NORMAL);
        /* OK + prompt are sent by bridge_seq_tick() once the power-cycle
         * back to normal boot has actually completed, not here. */
        return 1;
    }
    if (strcmp(line, "display_boot0 low") == 0 || strcmp(line, "display_boot0 high") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send("ERR: busy\r\n> ");
            return 1;
        }
        uint8_t low = (strcmp(line, "display_boot0 low") == 0);
        HAL_GPIO_WritePin(DISPLAY_DO3_GPIO_Port, DISPLAY_DO3_Pin,
                           low ? GPIO_PIN_RESET : GPIO_PIN_SET);
        cli_send(low ? "OK GPIO0 = LOW\r\n> " : "OK GPIO0 = HIGH\r\n> ");
        return 1;
    }
    return 0;
}

/* ── Command processing ──────────────────────────────────────────────────── */

static void process_line(char *line)
{
    /* Strip trailing whitespace */
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';

    if (len == 0) return;

    /* help */
    if (strcmp(line, "help") == 0) {
        cli_send("Commands:\r\n"
                 "  set crystal.kp         <val>\r\n"
                 "  set crystal.ki         <val>\r\n"
                 "  set crystal.kd         <val>\r\n"
                 "  set crystal.setpoint   <val>  (C)\r\n"
                 "  set crystal.wavelength <val>  (nm) — sets crystal setpoint via T=(λ-m)/k\r\n"
                 "  set crystal.k          <val>  (nm/C, slope)\r\n"
                 "  set crystal.m          <val>  (nm, offset)\r\n"
                 "  set temp.vref          <val>  (V)    — NTC divider reference\r\n"
                 "  set temp.rseries       <val>  (ohm)  — NTC series resistor\r\n"
                 "  set temp.sh_a          <val>         — Steinhart-Hart A\r\n"
                 "  set temp.sh_b          <val>         — Steinhart-Hart B\r\n"
                 "  set temp.sh_c          <val>         — Steinhart-Hart C\r\n"
                 "  set laser.kp           <val>\r\n"
                 "  set laser.ki           <val>\r\n"
                 "  set laser.kd           <val>\r\n"
                 "  set laser.setpoint     <val>  (C)  — resets temp guard\r\n"
                 "  set laser.current      <val>  (A)\r\n"
                 "  set laser.maxcurrent   <val>  (A)\r\n"
                 "  set laser.threshold    <val>  (A)\r\n"
                 "  set laser.power        <val>  (0-100 %)\r\n"
                 "  set laser.max_temp_deviation <val>  (C)\r\n"
                 "  set laser.absolute_max_temp  <val>  (C)\r\n"
                 "  set laser.temp_timer         <val>  (s)\r\n"
                 "  set sim.laser_temp   <val>  (C) — inject fake laser temp\r\n"
                 "  sim off              — disable sim overrides\r\n"
                 "  laser on\r\n"
                 "  laser off\r\n"
                 "  5v on\r\n"
                 "  5v off\r\n"
                 "  crystal.tec on\r\n"
                 "  crystal.tec off\r\n"
                 "  dac test on           — pause crystal TEC PID\r\n"
                 "  dac test off          — resume crystal TEC PID\r\n"
                 "  dac set <val>         — -1.0 - 1.0, write dac value(requires 'dac test on')\r\n"
                //  "  polarity heat         — POLARITY pin HIGH (requires 'dac test on')\r\n"
                //  "  polarity cool         — POLARITY pin LOW  (requires 'dac test on')\r\n"
                 "  display_update          — start ESP32 display flashing bridge (see Docs/esp32_display_firmware_update.md)\r\n"
                 "  display_update_finished — end bridge, boot display's new firmware\r\n"
                 "  display_boot0 low\r\n"
                 "  display_boot0 high\r\n"
                 "  display mode spud      — bench-test: show SPUD UI for 60s\r\n"
                 "  display mode ugn       — bench-test: show UGN UI for 60s\r\n"
                 "  display mode auto      — cancel override, resume BSP_DEVICE_MODE_STR\r\n"
                 "  status\r\n"
                 "  save\r\n"
                 "  help\r\n");
        return;
    }

    /* status */
    if (strcmp(line, "status") == 0) {
        float temp, output;

        TEC_Control_GetState(TEC_CRYSTAL, &temp, &output);
        float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
        cli_sendf("Crystal TEC: T=%7.4f C  SP=%7.4f C  WL=%7.2f nm  k=%.4f nm/C ",
                  (double)temp,
                  (double)g_crystal_pid.setpoint,
                  (double)wl,
                  (double)g_crystal_wl_k);

        cli_sendf("m=%.2f nm  out=%.4f  Kp=%.4f  Ki=%.6f  Kd=%.4f\r\n",
                  (double)g_crystal_wl_m,
                  (double)output,
                  (double)g_crystal_pid.Kp,
                  (double)g_crystal_pid.Ki,
                  (double)g_crystal_pid.Kd);

        if (g_tec_pg_fault)
            cli_send("  TEC buck: FAULT — power-good low\r\n");

        cli_sendf("NTC: Vref=%.3f V  Rseries=%.1f ohm  A=%.6e  B=%.6e  C=%.6e\r\n",
                  (double)g_ntc_vref_v, (double)g_ntc_rseries_ohm,
                  (double)g_ntc_sh_a, (double)g_ntc_sh_b, (double)g_ntc_sh_c);

        TEC_Control_GetState(TEC_LASER, &temp, &output);
        cli_sendf("Laser  TEC: T=%7.4f C  SP=%7.4f  out=%.4f"
                  "  Kp=%.4f  Ki=%.6f  Kd=%.4f\r\n",
                  (double)temp,
                  (double)g_laser_pid.setpoint,
                  (double)output,
                  (double)g_laser_pid.Kp,
                  (double)g_laser_pid.Ki,
                  (double)g_laser_pid.Kd);
        if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_ABS_MAX)
            cli_send("  Temp guard: TRIPPED — absolute max exceeded\r\n");
        else if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_DEV)
            cli_send("  Temp guard: TRIPPED — deviation exceeded after settling\r\n");
        else if (g_laser_temp_guard_armed)
            cli_sendf("  Temp guard: ARMED  (max_dev=%.2f C  abs_max=%.2f C)\r\n",
                      (double)g_laser_temp_max_dev_C, (double)g_laser_temp_abs_max_C);
        else
            cli_sendf("  Temp guard: settling %.1f/%.1f s  (max_dev=%.2f C  abs_max=%.2f C)\r\n",
                      (double)g_laser_temp_settle_elapsed_s,
                      (double)g_laser_temp_timer_s,
                      (double)g_laser_temp_max_dev_C,
                      (double)g_laser_temp_abs_max_C);

        const LaserState_t *ls = Laser_Control_GetState();
        cli_sendf("Laser diode: Pwr=%3u%%  I=%.4f A  Ith=%.4f A  Imax=%.4f A  %s\r\n",
                  (unsigned)ls->power_pct,
                  (double)ls->current_A,
                  (double)ls->threshold_A,
                  (double)ls->max_current_A,
                  ls->enabled ? "ON" : "OFF");

        // cli_sendf("5V output: %s\r\n",
        //     HAL_GPIO_ReadPin(M6_EN_GPIO_Port, M6_EN_Pin) == GPIO_PIN_SET ? "ON" : "OFF");
        cli_sendf("TEC output: %s\r\n",
                  HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET ? "ON" : "OFF");

        cli_sendf("Display UART: %lu bytes received, last=0x%02X\r\n",
                  (unsigned long)s_disp_rx_count, s_disp_rx_last);

        cli_sendf("Display bridge: %s\r\n",
                  s_bridge_active ? "ACTIVE (flashing)"
                  : (s_bridge_seq_state != BRIDGE_SEQ_NONE) ? "power-cycling"
                  : "idle");
        return;
    }

    /* laser on / laser off */
    if (strcmp(line, "laser on") == 0) {
 
        // FJ temporary fix
        Laser_Control_Enable();
        // cli_send("OK laser enabled\r\n
        BSP_TEC_Enable(TEC_CRYSTAL, 1);
        BSP_TEC_Reset(TEC_CRYSTAL);
        cli_send("OK crystal TEC enabled\r\n");
        return;
    }
    if (strcmp(line, "laser off") == 0) {
        // FJ temporary fix
        Laser_Control_Disable();
        // cli_send("OK laser disabled\r\n");
        BSP_TEC_Enable(TEC_CRYSTAL, 0);
        cli_send("OK crystal TEC disabled\r\n");
        return;
    }

    /* crystal.tec on / crystal.tec off */
    if (strcmp(line, "crystal.tec on") == 0) {
        BSP_TEC_Enable(TEC_CRYSTAL, 1);
        BSP_TEC_Reset(TEC_CRYSTAL);
        cli_send("OK crystal TEC enabled\r\n");
        return;
    }
    if (strcmp(line, "crystal.tec off") == 0) {
        BSP_TEC_Enable(TEC_CRYSTAL, 0);
        cli_send("OK crystal TEC disabled\r\n");
        return;
    }
    if (strcmp(line, "laser off") == 0) {
        Laser_Control_Disable();
        cli_send("OK laser disabled\r\n");
        return;
    }

    /* 5v on / 5v off */
    if (strcmp(line, "5v on") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send("ERR: busy\r\n");
            return;
        }
        HAL_GPIO_WritePin(M6_EN_GPIO_Port, M6_EN_Pin, GPIO_PIN_SET);
        cli_send("OK 5V output enabled\r\n");
        return;
    }
    if (strcmp(line, "5v off") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send("ERR: busy\r\n");
            return;
        }
        HAL_GPIO_WritePin(M6_EN_GPIO_Port, M6_EN_Pin, GPIO_PIN_RESET);
        cli_send("OK 5V output disabled\r\n");
        return;
    }

    /* display mode spud / display mode ugn / display mode auto -- bench-test
     * only, see s_mode_override_active's comment above cli_sync_display(). */
    if (strcmp(line, "display mode spud") == 0 || strcmp(line, "display mode ugn") == 0) {
        s_mode_override_active   = 1;
        s_mode_override_is_spud  = (strcmp(line, "display mode spud") == 0);
        s_mode_override_start_ms = HAL_GetTick();
        s_sync_mode_last_ms      = (uint32_t)(0u - 1000u);  /* force immediate resend */
        cli_sendf("OK display mode -> %s for %lu s\r\n",
                  s_mode_override_is_spud ? "SPUD" : "UGN",
                  (unsigned long)(MODE_OVERRIDE_DURATION_MS / 1000u));
        return;
    }
    if (strcmp(line, "display mode auto") == 0) {
        s_mode_override_active = 0;
        s_sync_mode_last_ms    = (uint32_t)(0u - 1000u);  /* force immediate resend */
        cli_send("OK display mode override cleared\r\n");
        return;
    }

    /* dac test on / dac test off / dac set <code> / polarity heat / polarity cool */
    if (strcmp(line, "dac test on") == 0) {
        g_tec_manual_test = 1;
        cli_send("OK manual DAC test mode ON - crystal TEC PID paused.\r\n");
        return;
    }
    if (strcmp(line, "dac test off") == 0) {
        g_tec_manual_test = 0;
        cli_send("OK — PID control resumed\r\n");
        return;
    }
    // if (strcmp(line, "polarity heat") == 0) {
    //     if (!g_tec_manual_test) {
    //         cli_send("ERR: run 'dac test on' first\r\n");
    //         return;
    //     }
    //     BSP_TEC_SetRawPolarity(true);
    //     cli_send("OK polarity = heat (POLARITY pin HIGH)\r\n");
    //     return;
    // }
    // if (strcmp(line, "polarity cool") == 0) {
    //     if (!g_tec_manual_test) {
    //         cli_send("ERR: run 'dac test on' first\r\n");
    //         return;
    //     }
    //     BSP_TEC_SetRawPolarity(false);
    //     cli_send("OK polarity = cool (POLARITY pin LOW)\r\n");
    //     return;
    // }

    int dac_code;
    if (sscanf(line, "dac set %i", &dac_code) == 1) {
        if (!g_tec_manual_test) {
            cli_send("ERR: run 'dac test on' first\r\n");
            return;
        }
        if (dac_code < -4095 || dac_code > 4095) {
            cli_send("ERR: value must be -4095 to 4095\r\n");
            return;
        }
        BSP_TEC_SetDac(TEC_CRYSTAL, (float)dac_code / 4095.0f);
        return;
    }

    /* sim off */
    if (strcmp(line, "sim off") == 0) {
        g_sim_laser_temp_enable = 0;
        cli_send("OK sim disabled\r\n");
        return;
    }

    /* save */
    if (strcmp(line, "save") == 0) {
        int r = Params_Save();
        cli_send(r == 0 ? "OK saved\r\n" : "ERR: flash write failed\r\n");
        return;
    }

    /* set <param> <value> */
    char param[32], value_str[16];
    if (sscanf(line, "set %31s %15s", param, value_str) == 2) {
        float val = strtof(value_str, NULL);

        /* Crystal wavelength model (checked before TEC PID to avoid param_name collision) */
        if (strcmp(param, "crystal.wavelength") == 0) {
            /* TEC_Crystal_SetWavelength() already converts nm -> C and
             * resets the PID -- don't overwrite its result with the raw nm
             * value afterward. */
            TEC_Crystal_SetWavelength(val);

            cli_sendf("OK crystal.wavelength = %.2f nm  -> SP=%.4f C\r\n",
                      (double)val, (double)g_crystal_pid.setpoint);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "crystal.k") == 0) {
            float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
            g_crystal_wl_k = val;
            TEC_Crystal_SetWavelength(wl);
            cli_sendf("OK crystal.k = %.4f nm/C  -> SP=%.4f C\r\n",
                      (double)val, (double)g_crystal_pid.setpoint);
            return;
        }
        if (strcmp(param, "crystal.m") == 0) {
            float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
            g_crystal_wl_m = val;
            TEC_Crystal_SetWavelength(wl);
            cli_sendf("OK crystal.m = %.2f nm  -> SP=%.4f C\r\n",
                      (double)val, (double)g_crystal_pid.setpoint);
            return;
        }

        /* NTC calibration -- see bsp_temp.h/bsp_temp.c. Persisted like
         * everything else above (Params_MarkDirty()), so a bench calibration
         * survives a reboot instead of needing to be redone every time. */
        if (strcmp(param, "temp.vref") == 0) {
            g_ntc_vref_v = val;
            cli_sendf("OK temp.vref = %.4f V\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.rseries") == 0) {
            g_ntc_rseries_ohm = val;
            cli_sendf("OK temp.rseries = %.1f ohm\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_a") == 0) {
            g_ntc_sh_a = val;
            cli_sendf("OK temp.sh_a = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_b") == 0) {
            g_ntc_sh_b = val;
            cli_sendf("OK temp.sh_b = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_c") == 0) {
            g_ntc_sh_c = val;
            cli_sendf("OK temp.sh_c = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }

        /* Laser diode current control (checked before TEC PID to avoid ambiguity) */
        if (strcmp(param, "laser.current") == 0) {
            Laser_Control_SetCurrent(val);
            const LaserState_t *ls = Laser_Control_GetState();
            if (val > 0.0f && ls->current_A == 0.0f)
                cli_sendf("WARN: %.4f A is below threshold (%.4f A) — current set to 0\r\n",
                          (double)val, (double)ls->threshold_A);
            else
                cli_sendf("OK laser.current = %.4f A\r\n", (double)ls->current_A);
            return;
        }
        if (strcmp(param, "laser.maxcurrent") == 0) {
            Laser_Control_SetMaxCurrent(val);
            cli_sendf("OK laser.maxcurrent = %.4f A\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.threshold") == 0) {
            float prev_current = g_laser_state.current_A;
            Laser_Control_SetThreshold(val);
            const LaserState_t *ls = Laser_Control_GetState();
            if (prev_current > 0.0f && ls->current_A == 0.0f)
                cli_sendf("OK laser.threshold = %.4f A  (current was below threshold — set to 0)\r\n",
                          (double)val);
            else
                cli_sendf("OK laser.threshold = %.4f A  (power = %u%%)\r\n",
                          (double)val, (unsigned)ls->power_pct);
            return;
        }
        if (strcmp(param, "laser.power") == 0) {
            uint8_t pct = (val < 0.0f) ? 0u : (val > 100.0f) ? 100u : (uint8_t)val;
            Laser_Control_SetPower(pct);
            cli_sendf("OK laser.power = %u%%  -> I=%.4f A\r\n",
                      (unsigned)pct, (double)Laser_Control_GetState()->current_A);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "laser.max_temp_deviation") == 0) {
            g_laser_temp_max_dev_C = val;
            cli_sendf("OK laser.max_temp_deviation = %.2f C\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.absolute_max_temp") == 0) {
            g_laser_temp_abs_max_C = val;
            cli_sendf("OK laser.absolute_max_temp = %.2f C\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.temp_timer") == 0) {
            g_laser_temp_timer_s = val;
            cli_sendf("OK laser.temp_timer = %.1f s\r\n", (double)val);
            return;
        }
        if (strcmp(param, "sim.laser_temp") == 0) {
            g_sim_laser_temp_C      = val;
            g_sim_laser_temp_enable = 1;
            cli_sendf("OK sim.laser_temp = %.4f C  (sim active)\r\n", (double)val);
            return;
        }
        /* TEC PID params */
        PID_t *pid = NULL;
        static char *param_name = NULL;

        if (strncmp(param, "crystal.", 8) == 0) {
            pid        = &g_crystal_pid;
            param_name = param + 8;
        } else if (strncmp(param, "laser.", 6) == 0) {
            pid        = &g_laser_pid;
            param_name = param + 6;
        } else {
            cli_send("ERR: unknown channel (use 'crystal' or 'laser')\r\n");
            return;
        }

        if (strcmp(param_name, "kp") == 0) {
            pid->Kp = val;
            cli_sendf("OK %s = %.4f\r\n", param, (double)val);
        } else if (strcmp(param_name, "ki") == 0) {
            pid->Ki = val;
            cli_sendf("OK %s = %.6f\r\n", param, (double)val);
        } else if (strcmp(param_name, "kd") == 0) {
            pid->Kd = val;
            cli_sendf("OK %s = %.4f\r\n", param, (double)val);
        } else if (strcmp(param_name, "setpoint") == 0) {
            pid->setpoint = val;
            PID_Reset(pid); /* reset integral when setpoint changes */
            if (pid == &g_laser_pid)
                TEC_LaserTempGuard_Reset();
            cli_sendf("OK %s = %.4f C (integral reset)\r\n", param, (double)val);
            Params_MarkDirty();
        } else {
            cli_send("ERR: unknown parameter\r\n");
        }
        return;
    }

    cli_sendf("ERR: unknown command: '%s' (type 'help')\r\n", line);
}

/* ── Display state sync ──────────────────────────────────────────────────── */

/* Push the "relevant" parameters (laser power, crystal wavelength, laser
 * on/off, 5V on/off) to the display whenever any of them actually change,
 * using the same command syntax the display sends us -- one parser on the
 * display side handles both directions identically.
 *
 * Diffed once per main loop iteration rather than pushed at each individual
 * 'set'/'laser on'/'5v on'/etc. call site: these can also change on their
 * own (e.g. the laser temp guard auto-disabling the laser on a trip -- see
 * g_laser_temp_trip_pending), and a diff check here catches that the same
 * way it catches an explicit CLI command, without needing a push call
 * threaded through every possible mutation site, present or future.
 *
 * Sentinel "impossible" initial values force a first sync on boot, once
 * Params_Load() has restored the real values -- see main.c USER CODE 2. */
static uint16_t s_sync_power_pct  = 0xFFFFu;
static uint32_t s_sync_power_pct_last_ms = 0u;
static float    s_sync_wavelength = -1.0f;
static uint32_t s_sync_wavelength_last_ms = 0u;
static uint8_t  s_sync_laser_on   = 0xFFu;
static uint8_t  s_sync_5v_on      = 0xFFu;
static uint8_t  s_sync_crystal_tec_on = 0xFFu;
static float    s_sync_crystal_temp = -1000.0f;
static float    s_sync_crystal_output = -1000.0f;
/* s_sync_mode_last_ms and s_mode_override_* (used by cli_sync_display()'s
 * device.mode block below, and by process_line()'s 'display mode ...'
 * commands) are declared earlier in the file, near s_bridge_active. */
static float    s_sync_crystal_setpoint = -1000.0f;
static uint32_t s_sync_crystal_setpoint_last_ms = 0u;
static float    s_sync_crystal_tec_v = -1000.0f;

/* Resets the sync state back to the same impossible sentinel values used
 * at STM32 boot (above), forcing the next cli_sync_display() call to push
 * every tracked value fresh regardless of whether anything actually
 * changed. Called from bridge_seq_tick() once a display firmware update's
 * exit power-cycle completes -- see the forward declaration and call site
 * near the top of this file. */
static void cli_force_display_resync(void)
{
    s_sync_power_pct      = 0xFFFFu;
    s_sync_power_pct_last_ms = (uint32_t)(0u - 1000u);
    s_sync_wavelength     = -1.0f;
    s_sync_wavelength_last_ms = (uint32_t)(0u - 1000u);
    s_sync_laser_on       = 0xFFu;
    s_sync_5v_on          = 0xFFu;
    s_sync_crystal_tec_on = 0xFFu;
    s_sync_crystal_temp   = -1000.0f;
    s_sync_crystal_output = -1000.0f;
    s_sync_mode_last_ms    = (uint32_t)(0u - 1000u);
    s_sync_crystal_setpoint = -1000.0f;
    s_sync_crystal_setpoint_last_ms = (uint32_t)(0u - 1000u);
    s_sync_crystal_tec_v    = -1000.0f;
}

static void cli_sync_display(void)
{
    const LaserState_t *ls = Laser_Control_GetState();
    float wavelength = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
    float temp, output;
    uint8_t v5_on = (HAL_GPIO_ReadPin(M6_EN_GPIO_Port, M6_EN_Pin) == GPIO_PIN_SET) ? 1u : 0u;
    char buf[40];

    /* Lets one shared display firmware binary show the right UI for
     * whichever board it's actually plugged into. Placed first since it
     * logically precedes the telemetry it gates.
     *
     * Resent every ~1s rather than once: CLI_Process() (and so this
     * function) runs in main.c's tight, unthrottled while(1) loop, so the
     * STM32 reaches this line within microseconds of boot -- long before
     * the ESP32-S3's own boot chain (ROM bootloader, PSRAM init, Arduino
     * core init) has gotten as far as calling SerialSTM.begin(). A single
     * send here reliably went out while nothing was listening, leaving the
     * display stuck on its own default (MODE_SPUD) forever, regardless of
     * which board it's actually on. Periodic resend makes this self-healing
     * -- it lands whenever the display's UART actually comes up, and also
     * recovers if only the display resets later (brownout, USB replug)
     * without the STM32 resetting too, a case cli_force_display_resync()
     * alone doesn't cover since nothing calls it outside a display firmware
     * update. The display re-applying the same mode repeatedly costs it
     * nothing (see apply_ui_mode()'s prev_ui_mode guard). */
    if (HAL_GetTick() - s_sync_mode_last_ms >= 1000u) {
        s_sync_mode_last_ms = HAL_GetTick();

        if (s_mode_override_active &&
            (HAL_GetTick() - s_mode_override_start_ms >= MODE_OVERRIDE_DURATION_MS)) {
            s_mode_override_active = 0;  /* expired -- fall back to the real mode below */
        }

        const char *mode_str = s_mode_override_active
            ? (s_mode_override_is_spud ? "SPUD" : "UGN")
            : BSP_DEVICE_MODE_STR;
        snprintf(buf, sizeof(buf), "set device.mode %s\r\n", mode_str);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }

    if (ls->power_pct != s_sync_power_pct ||
        HAL_GetTick() - s_sync_power_pct_last_ms >= 1000u) {
        s_sync_power_pct = ls->power_pct;
        s_sync_power_pct_last_ms = HAL_GetTick();
        snprintf(buf, sizeof(buf), "set laser.power %u\r\n", (unsigned)ls->power_pct);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }
    if (wavelength != s_sync_wavelength ||
        HAL_GetTick() - s_sync_wavelength_last_ms >= 1000u) {
        s_sync_wavelength = wavelength;
        s_sync_wavelength_last_ms = HAL_GetTick();
        snprintf(buf, sizeof(buf), "set crystal.wavelength %.2f\r\n", (double)wavelength);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }
    if (ls->enabled != s_sync_laser_on) {
        s_sync_laser_on = ls->enabled;
        BSP_DISPLAY_Send(ls->enabled ? "laser on\r\n" : "laser off\r\n",
                          ls->enabled ? 10 : 11);
    }
    if (v5_on != s_sync_5v_on) {
        s_sync_5v_on = v5_on;
        BSP_DISPLAY_Send(v5_on ? "5v on\r\n" : "5v off\r\n",
                          v5_on ? 7 : 8);
    }

    uint8_t crystal_tec_on = (HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    if (crystal_tec_on != s_sync_crystal_tec_on) {
        s_sync_crystal_tec_on = crystal_tec_on;
        BSP_DISPLAY_Send(crystal_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n",
                          crystal_tec_on ? 16 : 17);
    }

    /* Target temperature in degrees C, decoupled from the crystal.wavelength
     * line above -- UGN mode's display doesn't care about wavelength at all,
     * only this. Also the knob-editable value UGN mode's display sends back
     * via "set crystal.setpoint ..." when turned; the existing generic
     * PID-parameter handler in process_line() already applies that
     * correctly (cli.c ~line 626), so this push doubles as the confirmation
     * echo back to the display once it does. */
    if (g_crystal_pid.setpoint != s_sync_crystal_setpoint ||
        HAL_GetTick() - s_sync_crystal_setpoint_last_ms >= 1000u) {
        s_sync_crystal_setpoint = g_crystal_pid.setpoint;
        s_sync_crystal_setpoint_last_ms = HAL_GetTick();
        snprintf(buf, sizeof(buf), "crystal.setpoint %.4f\r\n", (double)g_crystal_pid.setpoint);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }

    TEC_Control_GetState(TEC_CRYSTAL, &temp, &output);
    if (temp != s_sync_crystal_temp) {
        s_sync_crystal_temp = temp;
        snprintf(buf, sizeof(buf), "crystal.temp %.4f\r\n", (double)temp);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }

    if (output != s_sync_crystal_output) {
        s_sync_crystal_output = output;
        snprintf(buf, sizeof(buf), "crystal.output %.4f\r\n", (double)output);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }

    /* Commanded (not measured -- no current/voltage sensing on this board)
     * buck output voltage, derived from the same `output` already fetched
     * above -- see BSP_TEC_EstimateVoltage()'s own doc-comment for the
     * derivation and its caveats. */
    float tec_v = BSP_TEC_EstimateVoltage(output);
    if (tec_v != s_sync_crystal_tec_v) {
        s_sync_crystal_tec_v = tec_v;
        snprintf(buf, sizeof(buf), "crystal.tec_voltage %.2f\r\n", (double)tec_v);
        BSP_DISPLAY_Send(buf, strlen(buf));
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void CLI_Init(void)
{
    HAL_UART_Receive_IT(&huart1, &s_disp_rx_isr_byte, 1);
    cli_send("\r\n=== Laserfabriken ===\r\n");
    cli_send("Type 'help' for commands\r\n> ");
}

void CLI_Process(void)
{
    uint8_t byte;

    bridge_seq_tick();

    if (s_bridge_active) {
        /* Raw byte relay between the PC (USB-CDC) and the ESP32's ROM
         * bootloader (USART1) while it's held in download mode -- returns
         * before any of the normal CLI handling below runs this call.
         * Fault notifications, line buffering, process_line(),
         * cli_sync_display() are all skipped for as long as this is
         * active. */
        if (CDC_TakeBreakFlag()) {
            s_bridge_active = 0;
            cli_send("[bridge] resuming normal CLI\r\n> ");
            return;
        }
        if (HAL_GetTick() - s_bridge_last_activity >= BRIDGE_IDLE_TIMEOUT_MS) {
            s_bridge_active = 0;
            cli_send("[bridge] idle timeout -- resuming normal CLI (GPIO0 left low)\r\n> ");
            return;
        }

        /* USB -> UART (PC -> ESP32), batched: BSP_DISPLAY_Send() is a
         * 100ms-timeout blocking HAL_UART_Transmit() -- at 115200 baud,
         * 256 bytes takes ~22ms, comfortably under that limit. Draining
         * the whole ~2KB ring in one call would risk the transmit
         * silently truncating against its own timeout instead. */
        {
            uint8_t relay_buf[256];
            uint16_t n = 0;
            while (n < sizeof(relay_buf) && BSP_CLI_Receive(&byte) == 0) {
                relay_buf[n++] = byte;
            }
            if (n > 0) {
                BSP_DISPLAY_Send(relay_buf, n);
                s_bridge_last_activity = HAL_GetTick();
            }
            CDC_CLI_ResumeIfRoom();
        }

        /* UART -> USB (ESP32 -> PC) */
        {
            uint8_t relay_buf[256];
            uint16_t n = 0;
            while (n < sizeof(relay_buf) && disp_rx_pop(&byte)) {
                relay_buf[n++] = byte;
            }
            if (n > 0) {
                cli_send_bin(relay_buf, n);
                s_bridge_last_activity = HAL_GetTick();
            }
        }

        return;
    }

    /* Print TEC power-good fault notification as soon as it is set by the control loop */
    if (g_tec_pg_fault_pending == 1) {
        g_tec_pg_fault_pending = 2; /* 2 = printed, waiting for clear */
        cli_send("\r\nTEC buck power-good fault (PG low) !!\r\n> ");
    } else if (g_tec_pg_fault_pending == 3) {
        g_tec_pg_fault_pending = 0;
        cli_send("\r\nTEC buck power-good OK\r\n> ");
    }


    /* Print trip notification as soon as it is set by the control loop */
    if (g_laser_temp_trip_pending) {
        g_laser_temp_trip_pending = 0;
        if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_ABS_MAX)
            cli_send("\r\n!! LASER OFF — absolute max temperature exceeded !!\r\n> ");
        else
            cli_send("\r\n!! LASER OFF — temperature deviation exceeded after settling !!\r\n> ");
    }

    /* Drain all available bytes without blocking the main loop */
    while (BSP_CLI_Receive(&byte) == 0) {
        if (byte == '\r' || byte == '\n') {
            if (s_line_pos > 0) {
                s_line_buf[s_line_pos] = '\0';
                cli_send("\r\n");
                s_line_pos = 0;
                if (!bridge_handle_command(s_line_buf)) {
                    disp_forward(s_line_buf);
                    process_line(s_line_buf);
                    cli_send("> ");
                }
                /* else: bridge_handle_command() already sent its own
                 * output (incl. prompt), or -- for display_update_finished
                 * -- is deferring both until its async power-cycle
                 * sequence actually completes. */
            } else {
                cli_send("> ");
            }
        } else if (byte == 0x7F || byte == 0x08) {
            /* Backspace / DEL */
            if (s_line_pos > 0) {
                s_line_pos--;
                cli_send("\b \b");
            }
        } else if (byte >= 0x20 && s_line_pos < CLI_BUF_LEN - 1) {
            s_line_buf[s_line_pos++] = (char)byte;
            BSP_CLI_Send(&byte, 1); /* echo */
        }
    }
    /* Also needed outside bridge mode: the usbd_cdc_if.c backpressure fix
     * NAKs the USB-CDC OUT endpoint for *all* traffic, not just bridge-mode
     * bytes, whenever the ring fills -- without this call here too, a burst
     * large enough to fill it during normal (non-flashing) use would hang
     * the USB CLI until reset, since nothing else outside bridge mode would
     * ever re-arm it. */
    CDC_CLI_ResumeIfRoom();

    /* Drain display UART — process received commands without forwarding back.
     * Bytes are captured by HAL_UART_RxCpltCallback() into a ring buffer the
     * instant they arrive (interrupt-driven, see top of file) -- this just
     * consumes what's already been captured, no direct UART access here. */
    while (disp_rx_pop(&byte)) {
        s_disp_rx_count++;
        s_disp_rx_last = byte;
        if (byte == '\r' || byte == '\n') {
            if (s_disp_pos > 0) {
                s_disp_buf[s_disp_pos] = '\0';
                cli_send("\r\n");
                process_line(s_disp_buf);
                s_disp_pos = 0;
                cli_send("> ");
            }
        } else if (byte >= 0x20 && s_disp_pos < CLI_BUF_LEN - 1) {
            s_disp_buf[s_disp_pos++] = (char)byte;
        }
    }

    // Sync display state at a fixed interval (50 ms) to avoid flooding the display with updates
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();
    if (now - last_tick >= 50) {
        last_tick = now;
        cli_sync_display();
    }
}
