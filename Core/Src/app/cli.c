#include "cli.h"
#include "bsp_config.h"
#include "bsp_temp.h"
#include "tec_control.h"
#include "laser_control.h"
#include "params.h"
#include "fw_version.h"
#include "usbc_port.h"
#include "bringup.h"
#ifdef CLI_RTT
#include "rtt.h"
#endif
#include "usb_device.h"
#include "usbd_def.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

extern USBD_HandleTypeDef hUsbDeviceFS;   /* usb_device.c -- for the configured check in usb_send() */

#define CLI_BUF_LEN  128

/* ── Channels ────────────────────────────────────────────────────────────────
 * Every byte source the CLI listens to is a channel: a byte pop function, a
 * send function, its own line buffer, and a mode.
 *
 *   machine == false  human at a terminal: echo typed characters, print the
 *                     "> " prompt, verbose "OK ..." replies.
 *   machine == true   the display protocol: no echo, no prompt, just lines.
 *
 * process_line() replies on the channel the command came from, so the
 * display gets its answers and the terminal gets its own, never crossed.
 *
 * On UGN there are two channels: USB-CDC (terminal) and USART1 (display,
 * permanently in machine mode). On SPUD the display is itself a USB host on
 * the same CDC port a PC would use, so there is only the USB channel and the
 * mode is decided at runtime: it starts human, switches to machine when the
 * display's "hello display ..." arrives, and is reset to human on every USB
 * re-enumeration (CLI_OnUsbEnumerated(), called from CDC_Init_FS). See
 * Docs/spud_usb_displaylank.md §4.1. */
typedef struct {
    bool     (*pop)(uint8_t *out);                 /* next received byte, false if none */
    void     (*send)(const void *buf, uint16_t len);
    bool     machine;
    bool     mode_fixed;                           /* true: machine never changes */
    char     line[CLI_BUF_LEN];
    uint16_t pos;
} cli_chan_t;

#define CLI_PROTO_VERSION 1u

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
#ifdef BSP_DISPLAY_LINK_UART
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
#endif /* BSP_DISPLAY_LINK_UART */

/* ── Output helpers ──────────────────────────────────────────────────────── */

static void usb_send(const void *buf, uint16_t len)
{
    /* No host configured (cable out, or between enumerations): drop
     * silently instead of spinning the 20ms retry below for every line. */
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
        return;
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
    while (BSP_CLI_Send((void *)buf, len) == USBD_BUSY) {
        if ((HAL_GetTick() - start) > 20u)
            break;
    }
}

static bool usb_pop(uint8_t *out)
{
    return BSP_CLI_Receive(out) == 0;
}

static cli_chan_t s_usb = { usb_pop, usb_send, false, false, {0}, 0 };

#ifdef BSP_DISPLAY_LINK_UART
static bool uart_pop(uint8_t *out)
{
    if (!disp_rx_pop(out))
        return false;
    s_disp_rx_count++;
    s_disp_rx_last = *out;
    return true;
}

static void uart_send(const void *buf, uint16_t len)
{
    BSP_DISPLAY_Send(buf, len);
}

static cli_chan_t s_uart = { uart_pop, uart_send, true, true, {0}, 0 };
#endif

#ifdef CLI_RTT
/* Debug console over SWD (rtt.h). Behaves exactly like the USB terminal,
 * including the hello mode switch, so the whole channel logic can be
 * exercised with only an ST-Link attached. */
static void rtt_send(const void *buf, uint16_t len) { RTT_Write(buf, len); }
static cli_chan_t s_rtt = { RTT_ReadByte, rtt_send, false, false, {0}, 0 };
#endif

/* The channel the display is on right now, or NULL if no display is
 * connected (SPUD: USB channel still in human mode). On UGN a display that
 * has said hello over USB wins over the UART, so the USB link can be bench-
 * tested on UGN hardware with the FPC still attached. */
static cli_chan_t *disp_chan(void)
{
    if (s_usb.machine)
        return &s_usb;
#ifdef BSP_DISPLAY_LINK_UART
    return &s_uart;
#else
    return NULL;
#endif
}

/* Send to the display, wherever it is -- silently dropped if there is none. */
static void disp_send(const void *buf, uint16_t len)
{
    cli_chan_t *ch = disp_chan();
    if (ch)
        ch->send(buf, len);
}

static void cli_send(cli_chan_t *ch, const char *str)
{
    ch->send(str, (uint16_t)strlen(str));
}

static void cli_sendf(cli_chan_t *ch, const char *fmt, ...)
{
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cli_send(ch, buf);
}

/* bringup.c output adapter */
static void cli_bringup_out(void *ctx, const char *text) { cli_send((cli_chan_t *)ctx, text); }

/* Unsolicited bring-up output (received UART frames): every terminal, never
 * the display's machine-mode channel. */
static void cli_terminal_out(void *ctx, const char *text)
{
    (void)ctx;
#ifdef CLI_RTT
    cli_send(&s_rtt, text);
#endif
    if (!s_usb.machine) cli_send(&s_usb, text);
}


/* Binary-safe counterpart to cli_send(ch, )/cli_sendf(ch, ) -- both of those call
 * strlen() internally, which would silently truncate at the first 0x00
 * byte. Used by the display-flashing bridge relay to forward raw
 * ESP32 ROM/stub-bootloader bytes back to the PC, where an embedded NUL
 * is expected, ordinary data. Mirrors cli_send(ch, )'s existing 20ms-bounded
 * retry-on-USBD_BUSY pattern, just with an explicit length instead of a
 * C string. */
static void cli_send_bin(cli_chan_t *ch, const uint8_t *data, uint16_t len)
{
    ch->send(data, len);
}

/* Defined near the display-sync state it resets, further down this file
 * (see "Display state sync") -- forward-declared here so bridge_seq_tick()
 * below can call it. */
static void cli_force_display_resync(void);
static void send_state(cli_chan_t *ch);
static void confirm(cli_chan_t *ch, const char *key);

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

/* Board type as the display should see it: the bench-test override while
 * it is active, otherwise the real board (BSP_DEVICE_MODE_STR). */
static const char *effective_mode_str(void)
{
    if (s_mode_override_active &&
        (HAL_GetTick() - s_mode_override_start_ms >= MODE_OVERRIDE_DURATION_MS)) {
        s_mode_override_active = 0;  /* expired -- fall back to the real mode */
    }
    return s_mode_override_active
        ? (s_mode_override_is_spud ? "SPUD" : "UGN")
        : BSP_DEVICE_MODE_STR;
}

#ifdef BSP_DISPLAY_LINK_UART
/* GPIO0 write happens before cutting power, so it's already correct and
 * stable for the entire down/up cycle -- strapping pins only need to be
 * settled by the time power actually comes back, not before it goes away. */
static void bridge_start_sequence(BridgeSeqAction_t action)
{
    HAL_GPIO_WritePin(DISPLAY_DO3_GPIO_Port, DISPLAY_DO3_Pin,
                       (action == BRIDGE_ACTION_ENTER_BRIDGE) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN, GPIO_PIN_RESET); /* 5V off */
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
    cli_chan_t *ch = &s_usb;
    if (s_bridge_seq_state == BRIDGE_SEQ_NONE) return;

    uint32_t elapsed = HAL_GetTick() - s_bridge_seq_start;

    if (s_bridge_seq_state == BRIDGE_SEQ_POWER_OFF) {
        if (elapsed >= BRIDGE_POWER_OFF_MS) {
            HAL_GPIO_WritePin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN, GPIO_PIN_SET); /* 5V on */
            s_bridge_seq_start = HAL_GetTick();
            s_bridge_seq_state = BRIDGE_SEQ_POWER_ON_WAIT;
        }
    } else if (s_bridge_seq_state == BRIDGE_SEQ_POWER_ON_WAIT) {
        if (elapsed >= BRIDGE_POWER_ON_MS) {
            s_bridge_seq_state = BRIDGE_SEQ_NONE;
            if (s_bridge_seq_action == BRIDGE_ACTION_ENTER_BRIDGE) {
                s_bridge_active         = 1;
                s_bridge_last_activity  = HAL_GetTick();
                cli_send(ch, "[bridge] ESP32 in ROM bootloader -- relay active\r\n");
            } else {
                /* The freshly-flashed ESP32 has no memory of anything the
                 * STM32 already "told" the previous firmware instance
                 * before it was overwritten -- force a full re-sync rather
                 * than silently relying on the diff-against-last-known-
                 * value logic in cli_sync_display(), which has no idea
                 * the display it's diffing against just lost its state. */
                cli_force_display_resync();
                cli_send(ch, "OK\r\n> ");
            }
        }
    }
}

/* Recognizes and fully handles (including its own prompt) the bridge-
 * control commands, before process_line() ever sees them.
 * process_line() is shared between the USB-CDC path and the display-UART
 * path (disp_rx_pop() below) -- these commands must never be reachable
 * from the display side, since a garbled/malformed byte stream arriving
 * *from* the display that happened to parse as this exact text could
 * otherwise trigger a real power-cycle + bootloader-entry sequence from
 * the wrong direction. Only called from CLI_Process()'s USB-CDC
 * line-completion branch. Returns 1 if `line` was one of these commands
 * (fully handled); 0 otherwise, so the caller falls through as normal. */
static int bridge_handle_command(cli_chan_t *ch, const char *line)
{
    if (strcmp(line, "display_update") == 0 && USBC_Enabled()) {
        cli_send(ch, "ERR: bridge needs the FPC display -- build without BSP_USBC_VBUS_SWITCH\r\n> ");
        return 1;
    }
    if (strcmp(line, "display_update") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send(ch, "ERR: busy\r\n> ");
            return 1;
        }
        cli_send(ch, "OK\r\n> ");
        bridge_start_sequence(BRIDGE_ACTION_ENTER_BRIDGE);
        return 1;
    }
    if (strcmp(line, "display_update_finished") == 0) {
        if (s_bridge_active) {
            cli_send(ch, "ERR: still bridging -- exit bridge mode first\r\n> ");
            return 1;
        }
        if (s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send(ch, "ERR: busy\r\n> ");
            return 1;
        }
        bridge_start_sequence(BRIDGE_ACTION_RESUME_NORMAL);
        /* OK + prompt are sent by bridge_seq_tick() once the power-cycle
         * back to normal boot has actually completed, not here. */
        return 1;
    }
    if (strcmp(line, "display_boot0 low") == 0 || strcmp(line, "display_boot0 high") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            cli_send(ch, "ERR: busy\r\n> ");
            return 1;
        }
        uint8_t low = (strcmp(line, "display_boot0 low") == 0);
        HAL_GPIO_WritePin(DISPLAY_DO3_GPIO_Port, DISPLAY_DO3_Pin,
                           low ? GPIO_PIN_RESET : GPIO_PIN_SET);
        cli_send(ch, low ? "OK GPIO0 = LOW\r\n> " : "OK GPIO0 = HIGH\r\n> ");
        return 1;
    }
    return 0;
}
#else  /* no display UART / strap lines on this board (SPUD): the bridge
        * cannot exist. The commands still answer so a script gets an error
        * instead of silence. */
static void bridge_seq_tick(void) {}
static int bridge_handle_command(cli_chan_t *ch, const char *line)
{
    if (strcmp(line, "display_update") == 0 || strcmp(line, "display_update_finished") == 0 ||
        strcmp(line, "display_boot0 low") == 0 || strcmp(line, "display_boot0 high") == 0) {
        cli_send(ch, "ERR: no display UART bridge on this board (display flashes over its own USB-C)\r\n> ");
        return 1;
    }
    return 0;
}
#endif /* BSP_DISPLAY_LINK_UART */

/* ── Command processing ──────────────────────────────────────────────────── */

static void process_line(cli_chan_t *ch, char *line)
{
    /* Strip trailing whitespace */
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';

    if (len == 0) return;

#if defined(CLI_RTT) && defined(BSP_DISPLAY_LINK_UART)
    /* Debug: mirror everything the display UART delivers onto the SWD
     * console. With the display in USB-host mode its Serial log (UART0,
     * GPIO43/44) lands here, so this is the display's log viewer. */
    if (ch == &s_uart)
        cli_sendf(&s_rtt, "[disp] %s\r\n", line);
#endif
#ifdef CLI_RTT
    /* Debug: mirror every line the display sends over USB (machine mode)
     * onto the SWD console, so the display -> STM32 direction can be
     * watched on a board without the UART link. */
    if (ch == &s_usb && ch->machine)
        cli_sendf(&s_rtt, "[usb] %s\r\n", line);
#endif
    /* '#' lines are comments/log output (the display's stm_link_log()),
     * never commands -- on any channel. */
    if (line[0] == '#') return;

    /* hello -- the display's handshake (Docs/spud_usb_displaylank.md §4).
     *   display -> STM32:  hello display fw=<ver> proto=<n>
     *   STM32  -> display: hello <UGN|SPUD> fw=<ver> proto=<n>
     * "hello display ..." switches a runtime-mode channel (SPUD's USB port)
     * into machine mode. A bare "hello" from a terminal just gets the reply,
     * so the handshake can be exercised by hand without losing the prompt. */
    if (strncmp(line, "hello", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
        char who[16] = "", fw[16] = "";
        unsigned proto = 0;
        int n = sscanf(line + 5, " %15s fw=%15s proto=%u", who, fw, &proto);
        if (n >= 1 && strcmp(who, "display") == 0) {
            if (proto != CLI_PROTO_VERSION) {
                cli_sendf(ch, "ERR proto %u unsupported (want %u)\r\n", proto, (unsigned)CLI_PROTO_VERSION);
                return;
            }
            if (!ch->mode_fixed)
                ch->machine = true;
        }
        cli_sendf(ch, "hello %s fw=%s proto=%u\r\n",
                  effective_mode_str(), FW_VERSION_STR, (unsigned)CLI_PROTO_VERSION);
        if (n >= 1 && strcmp(who, "display") == 0)
            send_state(ch);   /* everything the UI needs, right away (§5.2) */
        return;
    }

    /* get all -- the complete display state, same lines the display gets
     * after hello and from the diff sync, on whichever channel asks. */
    if (strcmp(line, "get all") == 0) {
        send_state(ch);
        return;
    }

    /* help */
    if (strcmp(line, "help") == 0) {
        cli_send(ch, "Commands:\r\n"
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
                 "  usbc                  — USB-C detection state (5V on VBUS only for a display)\r\n"
                 "  status\r\n"
                 "  get all               — the display state as protocol lines\r\n"
                 "  hello                 — handshake reply (board, fw, proto)\r\n"
                 "  save\r\n"
                 "  help\r\n");
        cli_send(ch, Bringup_HelpText());
        return;
    }

    /* usbc -- USB-C port detection state and raw CC comparator readings */
    if (strcmp(line, "usbc") == 0) {
        uint8_t cc1, cc2; bool src;
        USBC_DebugCC(&cc1, &cc2, &src);
        cli_sendf(ch, "USB-C: %s\r\n  role=%s  CC1=%u  CC2=%u  (source: 0=Ra 1=Rd 2=open; sink: 0=open 1..3=Rp)\r\n",
                  USBC_StateStr(), src ? "source" : "sink", cc1, cc2);
        return;
    }

    /* status */
    if (strcmp(line, "status") == 0) {
        float temp, output;

        TEC_Control_GetState(TEC_CRYSTAL, &temp, &output);
        float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
        cli_sendf(ch, "Crystal TEC: T=%7.4f C  SP=%7.4f C  WL=%7.2f nm  k=%.4f nm/C ",
                  (double)temp,
                  (double)g_crystal_pid.setpoint,
                  (double)wl,
                  (double)g_crystal_wl_k);

        cli_sendf(ch, "m=%.2f nm  out=%.4f  Kp=%.4f  Ki=%.6f  Kd=%.4f\r\n",
                  (double)g_crystal_wl_m,
                  (double)output,
                  (double)g_crystal_pid.Kp,
                  (double)g_crystal_pid.Ki,
                  (double)g_crystal_pid.Kd);

        if (g_tec_pg_fault)
            cli_send(ch, "  TEC buck: FAULT — power-good low\r\n");
#if BSP_TEC_CURRENT_MODE
        {
            float tv, ta;
            BSP_TEC_Measure(&tv, &ta);
            cli_sendf(ch, "  TEC drive: cmd=%.3f A (Imax %.2f A)  measured V=%.2f V  I=%.3f A  EN=%d  PG=%d\r\n",
                      (double)BSP_TEC_CommandedCurrent(), (double)BSP_TEC_IMAX_A, (double)tv, (double)ta,
                      HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET,
                      BSP_TEC_PowerGood());
        }
#endif

        cli_sendf(ch, "NTC: Vref=%.3f V  Rseries=%.1f ohm  A=%.6e  B=%.6e  C=%.6e\r\n",
                  (double)g_ntc_vref_v, (double)g_ntc_rseries_ohm,
                  (double)g_ntc_sh_a, (double)g_ntc_sh_b, (double)g_ntc_sh_c);

        TEC_Control_GetState(TEC_LASER, &temp, &output);
        cli_sendf(ch, "Laser  TEC: T=%7.4f C  SP=%7.4f  out=%.4f"
                  "  Kp=%.4f  Ki=%.6f  Kd=%.4f\r\n",
                  (double)temp,
                  (double)g_laser_pid.setpoint,
                  (double)output,
                  (double)g_laser_pid.Kp,
                  (double)g_laser_pid.Ki,
                  (double)g_laser_pid.Kd);
        if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_ABS_MAX)
            cli_send(ch, "  Temp guard: TRIPPED — absolute max exceeded\r\n");
        else if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_DEV)
            cli_send(ch, "  Temp guard: TRIPPED — deviation exceeded after settling\r\n");
        else if (g_laser_temp_guard_armed)
            cli_sendf(ch, "  Temp guard: ARMED  (max_dev=%.2f C  abs_max=%.2f C)\r\n",
                      (double)g_laser_temp_max_dev_C, (double)g_laser_temp_abs_max_C);
        else
            cli_sendf(ch, "  Temp guard: settling %.1f/%.1f s  (max_dev=%.2f C  abs_max=%.2f C)\r\n",
                      (double)g_laser_temp_settle_elapsed_s,
                      (double)g_laser_temp_timer_s,
                      (double)g_laser_temp_max_dev_C,
                      (double)g_laser_temp_abs_max_C);

        const LaserState_t *ls = Laser_Control_GetState();
        cli_sendf(ch, "Laser diode: Pwr=%3u%%  I=%.4f A  Ith=%.4f A  Imax=%.4f A  %s\r\n",
                  (unsigned)ls->power_pct,
                  (double)ls->current_A,
                  (double)ls->threshold_A,
                  (double)ls->max_current_A,
                  ls->enabled ? "ON" : "OFF");

        // cli_sendf(ch, "5V output: %s\r\n",
        //     HAL_GPIO_ReadPin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN) == GPIO_PIN_SET ? "ON" : "OFF");
        cli_sendf(ch, "TEC output: %s\r\n",
                  HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET ? "ON" : "OFF");

        cli_sendf(ch, "Display UART: %lu bytes received, last=0x%02X\r\n",
                  (unsigned long)s_disp_rx_count, s_disp_rx_last);

        cli_sendf(ch, "USB-C: %s\r\n", USBC_StateStr());
        cli_sendf(ch, "USB CLI channel: %s\r\n", s_usb.machine ? "machine (display said hello)" : "human (terminal)");
        cli_sendf(ch, "Display bridge: %s\r\n",
                  s_bridge_active ? "ACTIVE (flashing)"
                  : (s_bridge_seq_state != BRIDGE_SEQ_NONE) ? "power-cycling"
                  : "idle");
        return;
    }

    /* laser on / laser off */
    if (strcmp(line, "laser on") == 0) {
 
        // FJ temporary fix
        Laser_Control_Enable();
        // cli_send(ch, "OK laser enabled\r\n
        BSP_TEC_Enable(TEC_CRYSTAL, 1);
        BSP_TEC_Reset(TEC_CRYSTAL);
        if (ch->machine) { confirm(ch, "laser"); confirm(ch, "crystal.tec"); }
        else cli_send(ch, "OK crystal TEC enabled\r\n");
        return;
    }
    if (strcmp(line, "laser off") == 0) {
        // FJ temporary fix
        Laser_Control_Disable();
        // cli_send(ch, "OK laser disabled\r\n");
        BSP_TEC_Enable(TEC_CRYSTAL, 0);
        if (ch->machine) { confirm(ch, "laser"); confirm(ch, "crystal.tec"); }
        else cli_send(ch, "OK crystal TEC disabled\r\n");
        return;
    }

    /* crystal.tec on / crystal.tec off */
    if (strcmp(line, "crystal.tec on") == 0) {
        BSP_TEC_Enable(TEC_CRYSTAL, 1);
        BSP_TEC_Reset(TEC_CRYSTAL);
        if (ch->machine) confirm(ch, "crystal.tec");
        else cli_send(ch, "OK crystal TEC enabled\r\n");
        return;
    }
    if (strcmp(line, "crystal.tec off") == 0) {
        BSP_TEC_Enable(TEC_CRYSTAL, 0);
        if (ch->machine) confirm(ch, "crystal.tec");
        else cli_send(ch, "OK crystal TEC disabled\r\n");
        return;
    }
    if (strcmp(line, "laser off") == 0) {
        Laser_Control_Disable();
        cli_send(ch, "OK laser disabled\r\n");
        return;
    }

    /* 5v on / 5v off */
    if ((strcmp(line, "5v on") == 0 || strcmp(line, "5v off") == 0) && USBC_Enabled()) {
        /* VBUS 5V follows USB-C detection (usbc_port.c); a manual 'on' could
         * drive 5V into a PC's port. The display gets the real state. */
        if (ch->machine) confirm(ch, "5v");
        else cli_sendf(ch, "ERR: 5V is automatic (USB-C: %s)\r\n", USBC_StateStr());
        return;
    }
    if (strcmp(line, "5v on") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            if (ch->machine) confirm(ch, "5v"); else cli_send(ch, "ERR: busy\r\n");
            return;
        }
        HAL_GPIO_WritePin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN, GPIO_PIN_SET);
        if (ch->machine) confirm(ch, "5v");
        else cli_send(ch, "OK 5V output enabled\r\n");
        return;
    }
    if (strcmp(line, "5v off") == 0) {
        if (s_bridge_active || s_bridge_seq_state != BRIDGE_SEQ_NONE) {
            if (ch->machine) confirm(ch, "5v"); else cli_send(ch, "ERR: busy\r\n");
            return;
        }
        HAL_GPIO_WritePin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN, GPIO_PIN_RESET);
        if (ch->machine) confirm(ch, "5v");
        else cli_send(ch, "OK 5V output disabled\r\n");
        return;
    }

    /* display mode spud / display mode ugn / display mode auto -- bench-test
     * only, see s_mode_override_active's comment above cli_sync_display(). */
    if (strcmp(line, "display mode spud") == 0 || strcmp(line, "display mode ugn") == 0) {
        s_mode_override_active   = 1;
        s_mode_override_is_spud  = (strcmp(line, "display mode spud") == 0);
        s_mode_override_start_ms = HAL_GetTick();
        s_sync_mode_last_ms      = (uint32_t)(0u - 1000u);  /* force immediate resend */
        cli_sendf(ch, "OK display mode -> %s for %lu s\r\n",
                  s_mode_override_is_spud ? "SPUD" : "UGN",
                  (unsigned long)(MODE_OVERRIDE_DURATION_MS / 1000u));
        return;
    }
    if (strcmp(line, "display mode auto") == 0) {
        s_mode_override_active = 0;
        s_sync_mode_last_ms    = (uint32_t)(0u - 1000u);  /* force immediate resend */
        cli_send(ch, "OK display mode override cleared\r\n");
        return;
    }

    /* dac test on / dac test off / dac set <code> / polarity heat / polarity cool */
    if (strcmp(line, "dac test on") == 0) {
        g_tec_manual_test = 1;
        cli_send(ch, "OK manual DAC test mode ON - crystal TEC PID paused.\r\n");
        return;
    }
    if (strcmp(line, "dac test off") == 0) {
        g_tec_manual_test = 0;
        cli_send(ch, "OK — PID control resumed\r\n");
        return;
    }
    // if (strcmp(line, "polarity heat") == 0) {
    //     if (!g_tec_manual_test) {
    //         cli_send(ch, "ERR: run 'dac test on' first\r\n");
    //         return;
    //     }
    //     BSP_TEC_SetRawPolarity(true);
    //     cli_send(ch, "OK polarity = heat (POLARITY pin HIGH)\r\n");
    //     return;
    // }
    // if (strcmp(line, "polarity cool") == 0) {
    //     if (!g_tec_manual_test) {
    //         cli_send(ch, "ERR: run 'dac test on' first\r\n");
    //         return;
    //     }
    //     BSP_TEC_SetRawPolarity(false);
    //     cli_send(ch, "OK polarity = cool (POLARITY pin LOW)\r\n");
    //     return;
    // }

    int dac_code;
    if (sscanf(line, "dac set %i", &dac_code) == 1) {
        if (!g_tec_manual_test) {
            cli_send(ch, "ERR: run 'dac test on' first\r\n");
            return;
        }
        if (dac_code < -4095 || dac_code > 4095) {
            cli_send(ch, "ERR: value must be -4095 to 4095\r\n");
            return;
        }
        BSP_TEC_SetDac(TEC_CRYSTAL, (float)dac_code / 4095.0f);
        return;
    }

    /* sim off */
    if (strcmp(line, "sim off") == 0) {
        g_sim_laser_temp_enable = 0;
        cli_send(ch, "OK sim disabled\r\n");
        return;
    }

    /* save */
    if (strcmp(line, "save") == 0) {
        int r = Params_Save();
        cli_send(ch, r == 0 ? "OK saved\r\n" : "ERR: flash write failed\r\n");
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

            if (ch->machine) confirm(ch, "crystal.setpoint");
            else cli_sendf(ch, "OK crystal.wavelength = %.2f nm  -> SP=%.4f C\r\n",
                           (double)val, (double)g_crystal_pid.setpoint);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "crystal.k") == 0) {
            float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
            g_crystal_wl_k = val;
            TEC_Crystal_SetWavelength(wl);
            cli_sendf(ch, "OK crystal.k = %.4f nm/C  -> SP=%.4f C\r\n",
                      (double)val, (double)g_crystal_pid.setpoint);
            return;
        }
        if (strcmp(param, "crystal.m") == 0) {
            float wl = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
            g_crystal_wl_m = val;
            TEC_Crystal_SetWavelength(wl);
            cli_sendf(ch, "OK crystal.m = %.2f nm  -> SP=%.4f C\r\n",
                      (double)val, (double)g_crystal_pid.setpoint);
            return;
        }

        /* NTC calibration -- see bsp_temp.h/bsp_temp.c. Persisted like
         * everything else above (Params_MarkDirty()), so a bench calibration
         * survives a reboot instead of needing to be redone every time. */
        if (strcmp(param, "temp.vref") == 0) {
            g_ntc_vref_v = val;
            cli_sendf(ch, "OK temp.vref = %.4f V\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.rseries") == 0) {
            g_ntc_rseries_ohm = val;
            cli_sendf(ch, "OK temp.rseries = %.1f ohm\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_a") == 0) {
            g_ntc_sh_a = val;
            cli_sendf(ch, "OK temp.sh_a = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_b") == 0) {
            g_ntc_sh_b = val;
            cli_sendf(ch, "OK temp.sh_b = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "temp.sh_c") == 0) {
            g_ntc_sh_c = val;
            cli_sendf(ch, "OK temp.sh_c = %.6e\r\n", (double)val);
            Params_MarkDirty();
            return;
        }

        /* Laser diode current control (checked before TEC PID to avoid ambiguity) */
        if (strcmp(param, "laser.current") == 0) {
            Laser_Control_SetCurrent(val);
            const LaserState_t *ls = Laser_Control_GetState();
            if (val > 0.0f && ls->current_A == 0.0f)
                cli_sendf(ch, "WARN: %.4f A is below threshold (%.4f A) — current set to 0\r\n",
                          (double)val, (double)ls->threshold_A);
            else
                cli_sendf(ch, "OK laser.current = %.4f A\r\n", (double)ls->current_A);
            return;
        }
        if (strcmp(param, "laser.maxcurrent") == 0) {
            Laser_Control_SetMaxCurrent(val);
            cli_sendf(ch, "OK laser.maxcurrent = %.4f A\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.threshold") == 0) {
            float prev_current = g_laser_state.current_A;
            Laser_Control_SetThreshold(val);
            const LaserState_t *ls = Laser_Control_GetState();
            if (prev_current > 0.0f && ls->current_A == 0.0f)
                cli_sendf(ch, "OK laser.threshold = %.4f A  (current was below threshold — set to 0)\r\n",
                          (double)val);
            else
                cli_sendf(ch, "OK laser.threshold = %.4f A  (power = %u%%)\r\n",
                          (double)val, (unsigned)ls->power_pct);
            return;
        }
        if (strcmp(param, "laser.power") == 0) {
            uint8_t pct = (val < 0.0f) ? 0u : (val > 100.0f) ? 100u : (uint8_t)val;
            Laser_Control_SetPower(pct);
            if (ch->machine) confirm(ch, "laser.power");
            else cli_sendf(ch, "OK laser.power = %u%%  -> I=%.4f A\r\n",
                           (unsigned)pct, (double)Laser_Control_GetState()->current_A);
            Params_MarkDirty();
            return;
        }
        if (strcmp(param, "laser.max_temp_deviation") == 0) {
            g_laser_temp_max_dev_C = val;
            cli_sendf(ch, "OK laser.max_temp_deviation = %.2f C\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.absolute_max_temp") == 0) {
            g_laser_temp_abs_max_C = val;
            cli_sendf(ch, "OK laser.absolute_max_temp = %.2f C\r\n", (double)val);
            return;
        }
        if (strcmp(param, "laser.temp_timer") == 0) {
            g_laser_temp_timer_s = val;
            cli_sendf(ch, "OK laser.temp_timer = %.1f s\r\n", (double)val);
            return;
        }
        if (strcmp(param, "sim.laser_temp") == 0) {
            g_sim_laser_temp_C      = val;
            g_sim_laser_temp_enable = 1;
            cli_sendf(ch, "OK sim.laser_temp = %.4f C  (sim active)\r\n", (double)val);
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
            cli_send(ch, "ERR: unknown channel (use 'crystal' or 'laser')\r\n");
            return;
        }

        if (strcmp(param_name, "kp") == 0) {
            pid->Kp = val;
            cli_sendf(ch, "OK %s = %.4f\r\n", param, (double)val);
        } else if (strcmp(param_name, "ki") == 0) {
            pid->Ki = val;
            cli_sendf(ch, "OK %s = %.6f\r\n", param, (double)val);
        } else if (strcmp(param_name, "kd") == 0) {
            pid->Kd = val;
            cli_sendf(ch, "OK %s = %.4f\r\n", param, (double)val);
        } else if (strcmp(param_name, "setpoint") == 0) {
            pid->setpoint = val;
            PID_Reset(pid); /* reset integral when setpoint changes */
            if (pid == &g_laser_pid)
                TEC_LaserTempGuard_Reset();
            if (ch->machine && pid == &g_crystal_pid) confirm(ch, "crystal.setpoint");
            else cli_sendf(ch, "OK %s = %.4f C (integral reset)\r\n", param, (double)val);
            Params_MarkDirty();
        } else {
            cli_send(ch, "ERR: unknown parameter\r\n");
        }
        return;
    }

    if (Bringup_Process(line, cli_bringup_out, ch)) return;

    cli_sendf(ch, "ERR: unknown command: '%s' (type 'help')\r\n", line);
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

/* The complete display state, unconditionally, as the same protocol lines
 * cli_sync_display() below pushes one by one when something changes. Sent
 * to the display right after its hello (so a freshly connected display has
 * everything at once instead of waiting for the periodic resends), and to
 * whoever asks with 'get all'. Does not touch the sync's s_sync_* memory:
 * a change after this dump is still a change. */
static void send_state(cli_chan_t *ch)
{
    const LaserState_t *ls = Laser_Control_GetState();
    float wavelength = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
    float temp, output;
    TEC_Control_GetState(TEC_CRYSTAL, &temp, &output);
    uint8_t v5_on = (HAL_GPIO_ReadPin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    uint8_t crystal_tec_on = (HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET) ? 1u : 0u;

    cli_sendf(ch, "set device.mode %s\r\n", effective_mode_str());
    cli_sendf(ch, "set laser.power %u\r\n", (unsigned)ls->power_pct);
    cli_sendf(ch, "set crystal.wavelength %.2f\r\n", (double)wavelength);
    cli_send(ch, ls->enabled ? "laser on\r\n" : "laser off\r\n");
    cli_send(ch, v5_on ? "5v on\r\n" : "5v off\r\n");
    cli_send(ch, crystal_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    cli_sendf(ch, "crystal.setpoint %.4f\r\n", (double)g_crystal_pid.setpoint);
    cli_sendf(ch, "crystal.temp %.4f\r\n", (double)temp);
    cli_sendf(ch, "crystal.output %.4f\r\n", (double)output);
    cli_sendf(ch, "crystal.tec_voltage %.2f\r\n", (double)BSP_TEC_EstimateVoltage(output));
}

/* Machine-mode reply to a command that changes display state: the
 * authoritative value as a protocol line, so the display shows what the
 * STM32 actually applied (clamped, rounded, refused) rather than what it
 * asked for. The diff sync would push a changed value anyway within 50 ms;
 * this covers the unchanged/refused case and answers at once. Human mode
 * keeps its "OK ..." text; callers pick one or the other. */
static void confirm(cli_chan_t *ch, const char *key)
{
    if (strcmp(key, "laser.power") == 0) {
        cli_sendf(ch, "set laser.power %u\r\n", (unsigned)Laser_Control_GetState()->power_pct);
    } else if (strcmp(key, "crystal.setpoint") == 0) {
        float wavelength = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
        cli_sendf(ch, "crystal.setpoint %.4f\r\n", (double)g_crystal_pid.setpoint);
        cli_sendf(ch, "set crystal.wavelength %.2f\r\n", (double)wavelength);
    } else if (strcmp(key, "laser") == 0) {
        cli_send(ch, Laser_Control_GetState()->enabled ? "laser on\r\n" : "laser off\r\n");
    } else if (strcmp(key, "5v") == 0) {
        cli_send(ch, HAL_GPIO_ReadPin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN) == GPIO_PIN_SET ? "5v on\r\n" : "5v off\r\n");
    } else if (strcmp(key, "crystal.tec") == 0) {
        cli_send(ch, HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    }
}

static void cli_sync_display(void)
{
    const LaserState_t *ls = Laser_Control_GetState();
    float wavelength = g_crystal_wl_k * g_crystal_pid.setpoint + g_crystal_wl_m;
    float temp, output;
    uint8_t v5_on = (HAL_GPIO_ReadPin(BSP_USBC_VBUS_EN_PORT, BSP_USBC_VBUS_EN_PIN) == GPIO_PIN_SET) ? 1u : 0u;
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

        snprintf(buf, sizeof(buf), "set device.mode %s\r\n", effective_mode_str());
        disp_send(buf, strlen(buf));
    }

    if (ls->power_pct != s_sync_power_pct ||
        HAL_GetTick() - s_sync_power_pct_last_ms >= 1000u) {
        s_sync_power_pct = ls->power_pct;
        s_sync_power_pct_last_ms = HAL_GetTick();
        snprintf(buf, sizeof(buf), "set laser.power %u\r\n", (unsigned)ls->power_pct);
        disp_send(buf, strlen(buf));
    }
    if (wavelength != s_sync_wavelength ||
        HAL_GetTick() - s_sync_wavelength_last_ms >= 1000u) {
        s_sync_wavelength = wavelength;
        s_sync_wavelength_last_ms = HAL_GetTick();
        snprintf(buf, sizeof(buf), "set crystal.wavelength %.2f\r\n", (double)wavelength);
        disp_send(buf, strlen(buf));
    }
    if (ls->enabled != s_sync_laser_on) {
        s_sync_laser_on = ls->enabled;
        disp_send(ls->enabled ? "laser on\r\n" : "laser off\r\n",
                          ls->enabled ? 10 : 11);
    }
    if (v5_on != s_sync_5v_on) {
        s_sync_5v_on = v5_on;
        disp_send(v5_on ? "5v on\r\n" : "5v off\r\n",
                          v5_on ? 7 : 8);
    }

    uint8_t crystal_tec_on = (HAL_GPIO_ReadPin(BSP_TEC_EN_PORT, BSP_TEC_EN_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    if (crystal_tec_on != s_sync_crystal_tec_on) {
        s_sync_crystal_tec_on = crystal_tec_on;
        disp_send(crystal_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n",
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
        disp_send(buf, strlen(buf));
    }

    TEC_Control_GetState(TEC_CRYSTAL, &temp, &output);
    if (temp != s_sync_crystal_temp) {
        s_sync_crystal_temp = temp;
        snprintf(buf, sizeof(buf), "crystal.temp %.4f\r\n", (double)temp);
        disp_send(buf, strlen(buf));
    }

    if (output != s_sync_crystal_output) {
        s_sync_crystal_output = output;
        snprintf(buf, sizeof(buf), "crystal.output %.4f\r\n", (double)output);
        disp_send(buf, strlen(buf));
    }

    /* Commanded (not measured -- no current/voltage sensing on this board)
     * buck output voltage, derived from the same `output` already fetched
     * above -- see BSP_TEC_EstimateVoltage()'s own doc-comment for the
     * derivation and its caveats. */
    float tec_v = BSP_TEC_EstimateVoltage(output);
    if (tec_v != s_sync_crystal_tec_v) {
        s_sync_crystal_tec_v = tec_v;
        snprintf(buf, sizeof(buf), "crystal.tec_voltage %.2f\r\n", (double)tec_v);
        disp_send(buf, strlen(buf));
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static void banner(cli_chan_t *ch)
{
    cli_sendf(ch, "\r\n=== Laserfabriken %s fw %s ===\r\nType 'help' for commands\r\n> ",
              BSP_DEVICE_MODE_STR, FW_VERSION_STR);
}

/* Set from USB interrupt context (CDC_Init_FS) on every enumeration, i.e.
 * every time a host -- PC or display -- plugs in. Acted on from the main
 * loop: only a flag here, no sending. */
static volatile uint8_t s_usb_enumerated_pending;

static uint8_t s_usb_dtr_prev;   /* last DTR level, see CLI_OnUsbLineState() */

void CLI_OnUsbEnumerated(void)
{
    s_usb_enumerated_pending = 1;
    s_usb_dtr_prev = 0;   /* a new host starts with the port closed: its first open must count as a rise */
}

/* Set from USB interrupt context (CDC_Control_FS, SET_CONTROL_LINE_STATE)
 * when a program on the host opens the port and raises DTR. The banner is
 * sent then, plus a short delay, instead of at enumeration: at enumeration
 * no program is listening yet, and a Linux tty still in its default
 * echo mode bounced the banner straight back into the CLI as garbage
 * input ("ERR: unknown command: '=== Laserfabri...'"). The delay covers the
 * gap between the port opening (DTR up) and the program switching the tty
 * to raw mode. A host that never raises DTR just gets no banner. */
#define CLI_BANNER_DELAY_MS 150u
static volatile uint8_t s_usb_dtr_rise_pending;

void CLI_OnUsbLineState(uint8_t dtr)
{
    if (dtr && !s_usb_dtr_prev)
        s_usb_dtr_rise_pending = 1;
    s_usb_dtr_prev = dtr;
}

/* Feed one channel's pending bytes through line assembly and dispatch.
 * Human-mode niceties (echo, prompt, backspace) are skipped in machine
 * mode; the display never wants them and would have to filter them out. */
static void chan_pump(cli_chan_t *ch)
{
    uint8_t byte;
    while (ch->pop(&byte)) {
        if (byte == '\r' || byte == '\n') {
            if (ch->pos > 0) {
                ch->line[ch->pos] = '\0';
                ch->pos = 0;
                if (!ch->machine)
                    cli_send(ch, "\r\n");
                /* Bridge control is terminal-only: never reachable from a
                 * display, whichever transport it is on (see
                 * bridge_handle_command()'s comment). */
                int handled = (ch == &s_usb && !ch->machine)
                              ? bridge_handle_command(ch, ch->line) : 0;
                if (!handled) {
                    process_line(ch, ch->line);
                    if (!ch->machine)   /* process_line() may just have switched mode */
                        cli_send(ch, "> ");
                }
            } else if (!ch->machine) {
                cli_send(ch, "> ");
            }
        } else if (byte == 0x7F || byte == 0x08) {
            if (!ch->machine && ch->pos > 0) {
                ch->pos--;
                cli_send(ch, "\b \b");
            }
        } else if (byte >= 0x20 && ch->pos < CLI_BUF_LEN - 1) {
            ch->line[ch->pos++] = (char)byte;
            if (!ch->machine)
                ch->send(&byte, 1); /* echo */
        }
    }
}

void CLI_Init(void)
{
#ifdef BSP_DISPLAY_LINK_UART
    HAL_UART_Receive_IT(&huart1, &s_disp_rx_isr_byte, 1);
#endif
#ifdef CLI_RTT
    RTT_Init();
    banner(&s_rtt);
#endif
    /* The USB banner is printed when a program opens the port on the host
     * (CLI_OnUsbLineState, DTR rising), not here: at boot nothing is
     * listening and the bytes would just be dropped by usb_send(). */
}

void CLI_Process(void)
{
    cli_chan_t *ch = &s_usb;
    uint8_t byte;

    if (s_usb_enumerated_pending) {
        s_usb_enumerated_pending = 0;
        s_usb.machine = false;
        s_usb.pos     = 0;
    }
    {
        static uint8_t  banner_armed;
        static uint32_t banner_at;
        if (s_usb_dtr_rise_pending) {
            s_usb_dtr_rise_pending = 0;
            banner_armed = 1;
            banner_at = HAL_GetTick() + CLI_BANNER_DELAY_MS;
        }
        if (banner_armed && (int32_t)(HAL_GetTick() - banner_at) >= 0) {
            banner_armed = 0;
            if (!s_usb.machine)   /* never to the display */
                banner(&s_usb);
        }
    }

    bridge_seq_tick();
    if (Bringup_Monitor(cli_terminal_out, NULL) > 0)
        cli_terminal_out(NULL, "> ");     /* fresh prompt after unsolicited lines */

    if (s_bridge_active) {
        /* Raw byte relay between the PC (USB-CDC) and the ESP32's ROM
         * bootloader (USART1) while it's held in download mode -- returns
         * before any of the normal CLI handling below runs this call.
         * Fault notifications, line buffering, process_line(),
         * cli_sync_display() are all skipped for as long as this is
         * active. */
        if (CDC_TakeBreakFlag()) {
            s_bridge_active = 0;
            cli_send(ch, "[bridge] resuming normal CLI\r\n> ");
            return;
        }
        if (HAL_GetTick() - s_bridge_last_activity >= BRIDGE_IDLE_TIMEOUT_MS) {
            s_bridge_active = 0;
            cli_send(ch, "[bridge] idle timeout -- resuming normal CLI (GPIO0 left low)\r\n> ");
            return;
        }

#ifdef BSP_DISPLAY_LINK_UART
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
                cli_send_bin(ch, relay_buf, n);
                s_bridge_last_activity = HAL_GetTick();
            }
        }

#endif
        return;
    }

    /* Print TEC power-good fault notification as soon as it is set by the control loop */
    if (g_tec_pg_fault_pending == 1) {
        g_tec_pg_fault_pending = 2; /* 2 = printed, waiting for clear */
        cli_send(ch, "\r\nTEC buck power-good fault (PG low) !!\r\n> ");
    } else if (g_tec_pg_fault_pending == 3) {
        g_tec_pg_fault_pending = 0;
        cli_send(ch, "\r\nTEC buck power-good OK\r\n> ");
    }

    /* Print trip notification as soon as it is set by the control loop */
    if (g_laser_temp_trip_pending) {
        g_laser_temp_trip_pending = 0;
        if (g_laser_temp_trip_reason == LASER_TEMP_TRIP_ABS_MAX)
            cli_send(ch, "\r\n!! LASER OFF — absolute max temperature exceeded !!\r\n> ");
        else
            cli_send(ch, "\r\n!! LASER OFF — temperature deviation exceeded after settling !!\r\n> ");
    }

    /* Terminal / SPUD display: USB-CDC channel */
    chan_pump(&s_usb);
#ifdef CLI_RTT
    chan_pump(&s_rtt);
#endif
    /* Also needed outside bridge mode: the usbd_cdc_if.c backpressure fix
     * NAKs the USB-CDC OUT endpoint for *all* traffic, not just bridge-mode
     * bytes, whenever the ring fills -- without this call here too, a burst
     * large enough to fill it during normal (non-flashing) use would hang
     * the USB CLI until reset, since nothing else outside bridge mode would
     * ever re-arm it. */
    CDC_CLI_ResumeIfRoom();

#ifdef BSP_DISPLAY_LINK_UART
    /* UGN display: USART1 channel. Bytes are captured by
     * HAL_UART_RxCpltCallback() into a ring buffer the instant they arrive
     * (interrupt-driven, see top of file) -- this just consumes what's
     * already been captured, no direct UART access here. */
    chan_pump(&s_uart);
#endif

    // Sync display state at a fixed interval (50 ms) to avoid flooding the display with updates
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();
    if (now - last_tick >= 50) {
        last_tick = now;
        cli_sync_display();
    }
}
