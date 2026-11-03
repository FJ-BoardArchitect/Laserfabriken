#include "bringup.h"
#include "bsp_config.h"
#include "main.h"
#include "uart_rx.h"
#include "bsp_temp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct { const char *name; GPIO_TypeDef *port; uint16_t pin; uint8_t writable; } pin_t;
typedef struct { const char *name; UART_HandleTypeDef *h; const char *where; } uart_t;

static const pin_t s_pins[] = {
#define X(n, lbl, w) { n, lbl##_GPIO_Port, lbl##_Pin, w },
    BSP_BRINGUP_PINS(X)
#undef X
    { 0, 0, 0, 0 }
};

static const uart_t s_uarts[] = {
#define X(n, h, where) { n, &h, where },
    BSP_BRINGUP_UARTS(X)
#undef X
    { 0, 0, 0 }
};

#define OUT(...) do { char _b[160]; snprintf(_b, sizeof _b, __VA_ARGS__); out(ctx, _b); } while (0)

static int pin_is_output(const pin_t *p)
{
    uint32_t pos = 0;
    while (((p->pin >> pos) & 1u) == 0u) pos++;
    return ((p->port->MODER >> (pos * 2u)) & 3u) == 1u;   /* 01 = general purpose output */
}

static const pin_t *find_pin(const char *name)
{
    for (const pin_t *p = s_pins; p->name; p++)
        if (strcmp(p->name, name) == 0) return p;
    return 0;
}

static const uart_t *find_uart(const char *name)
{
    for (const uart_t *u = s_uarts; u->name; u++)
        if (strcmp(u->name, name) == 0) return u;
    return 0;
}

static void list_pins(bringup_out_fn out, void *ctx)
{
    if (!s_pins[0].name) { out(ctx, "no bring-up pins on this board\r\n"); return; }
    out(ctx, "name          dir  level  (rw = settable with 'pin <name> 0|1')\r\n");
    for (const pin_t *p = s_pins; p->name; p++) {
        OUT("%-13s %-4s %d      %s\r\n", p->name, pin_is_output(p) ? "out" : "in",
            HAL_GPIO_ReadPin(p->port, p->pin) == GPIO_PIN_SET, p->writable ? "rw" : "ro");
    }
}

static void list_uarts(bringup_out_fn out, void *ctx)
{
    if (!s_uarts[0].name) { out(ctx, "no bring-up UARTs on this board\r\n"); return; }
    for (const uart_t *u = s_uarts; u->name; u++) {
        OUT("%-6s %lu baud  %s\r\n", u->name, (unsigned long)u->h->Init.BaudRate, u->where);
#ifdef BSP_UART_RX_PORTS
        const uart_rx_stats_t *st = UartRx_Stats(UartRx_Find(u->h));
        if (st)
            OUT("       dma rx %s  frames=%lu bytes=%lu dropped=%lu errors=%lu\r\n",
                st->running ? "armed" : "STOPPED", (unsigned long)st->frames, (unsigned long)st->bytes,
                (unsigned long)st->dropped, (unsigned long)st->errors);
#endif
    }
}

/* Pull up to `max` received bytes, waiting at most `ms`. With the DMA layer
 * this returns one whole frame (one burst up to an idle gap); without it,
 * whatever bytes trickle in during the wait. */
static size_t rx_take(const uart_t *u, uint8_t *buf, size_t max, uint32_t ms)
{
#ifdef BSP_UART_RX_PORTS
    int port = UartRx_Find(u->h);
    uint32_t t0 = HAL_GetTick();
    do {
        size_t n = UartRx_Pop(port, buf, max);
        if (n) return n;
        UartRx_Process();   /* error restart must not wait for this command to finish */
    } while (HAL_GetTick() - t0 < ms);
    return 0;
#else
    size_t n = 0;
    uint32_t t0 = HAL_GetTick();
    while (n < max && HAL_GetTick() - t0 < ms)
        if (HAL_UART_Receive(u->h, &buf[n], 1, 5) == HAL_OK) n++;
    return n;
#endif
}

static void rx_flush(const uart_t *u)
{
#ifdef BSP_UART_RX_PORTS
    UartRx_Flush(UartRx_Find(u->h));
#else
    uint8_t b;
    __HAL_UART_CLEAR_OREFLAG(u->h);
    while (HAL_UART_Receive(u->h, &b, 1, 2) == HAL_OK) {}
#endif
}

/* One line per received frame:   stage rx 3: 01 02 03   */
static void print_rx(const uart_t *u, const uint8_t *d, size_t n, bringup_out_fn out, void *ctx)
{
    char line[96];
    size_t p = (size_t)snprintf(line, sizeof line, "%s rx %u:", u->name, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        if (p > sizeof line - 4) { out(ctx, line); p = 0; line[0] = '\0'; }
        p += (size_t)snprintf(&line[p], sizeof line - p, " %02X", d[i]);
    }
    out(ctx, line);
    out(ctx, "\r\n");
}

static bool s_monitor = true;

/* Called from the main loop (CLI_Process): print every frame that has
 * arrived on any table UART since last time, as "<port> rx <n>: <hex>". */
size_t Bringup_Monitor(bringup_out_fn out, void *ctx)
{
    size_t frames = 0;
#ifdef BSP_UART_RX_PORTS
    if (!s_monitor) return 0;
    uint8_t buf[256];                 /* one whole DMA frame */
    for (const uart_t *u = s_uarts; u->name; u++) {
        int port = UartRx_Find(u->h);
        size_t n;
        while ((n = UartRx_Pop(port, buf, sizeof buf)) > 0) {
            print_rx(u, buf, n, out, ctx);
            frames++;
        }
    }
#else
    (void)out; (void)ctx;
#endif
    return frames;
}

static void uart_send(const uart_t *u, const uint8_t *d, size_t n, bringup_out_fn out, void *ctx)
{
    if (HAL_UART_Transmit(u->h, (uint8_t *)d, (uint16_t)n, 200) != HAL_OK)
        OUT("%s tx: HAL error\r\n", u->name);
}

/* Transmit on every table UART at the same instant (interrupt-driven TX on
 * all of them), then show what each port received: a full-duplex check. */
static void cmd_duplex(const char *text, bringup_out_fn out, void *ctx)
{
    static uint8_t msg[8][64];          /* must outlive the IT transfers */
    size_t n_ports = 0;
    for (const uart_t *u = s_uarts; u->name && n_ports < 8; u++) n_ports++;
    if (n_ports == 0) { out(ctx, "no bring-up UARTs on this board\r\n"); return; }
    for (size_t i = 0; i < n_ports; i++) rx_flush(&s_uarts[i]);
    size_t len[8];
    for (size_t i = 0; i < n_ports; i++) {
        len[i] = (size_t)snprintf((char *)msg[i], sizeof msg[i], "%s-from-%s\r\n", text, s_uarts[i].name);
    }
    out(ctx, "duplex: all ports transmit \"<text>-from-<port>\" at the same time\r\n");
    for (size_t i = 0; i < n_ports; i++) {
        if (HAL_UART_Transmit_IT(s_uarts[i].h, msg[i], (uint16_t)len[i]) != HAL_OK)
            OUT("  %s tx: HAL busy/error\r\n", s_uarts[i].name);
    }
    uint32_t t0 = HAL_GetTick();
    for (;;) {                           /* wait for every transmitter to finish */
        bool busy = false;
        for (size_t i = 0; i < n_ports; i++)
            if (s_uarts[i].h->gState != HAL_UART_STATE_READY) busy = true;
        if (!busy || HAL_GetTick() - t0 > 2000) break;
#ifdef BSP_UART_RX_PORTS
        UartRx_Process();
#endif
    }
}

static bool cmd_uart(char *args, bringup_out_fn out, void *ctx)
{
    char *name = strtok(args, " ");
    if (!name) { list_uarts(out, ctx); return true; }
    if (strcmp(name, "monitor") == 0) {
        char *v = strtok(0, " ");
        if (v && (strcmp(v, "on") == 0 || strcmp(v, "off") == 0)) s_monitor = (v[1] == 'n');
        OUT("monitor %s: received frames print as \"<port> rx <n>: <hex>\"\r\n", s_monitor ? "on" : "off");
        return true;
    }
    if (strcmp(name, "duplex") == 0) {
        char *text = strtok(0, "");
        cmd_duplex(text && *text ? text : "hello", out, ctx);
        return true;
    }
    const uart_t *u = find_uart(name);
    if (!u) { OUT("ERR: no uart '%s' (try 'uart')\r\n", name); return true; }
    char *sub = strtok(0, " ");
    if (!sub) { OUT("%s: %lu baud, %s\r\n", u->name, (unsigned long)u->h->Init.BaudRate, u->where); return true; }

    if (strcmp(sub, "baud") == 0) {
        char *v = strtok(0, " ");
        long baud = v ? strtol(v, 0, 10) : 0;
        if (baud < 300 || baud > 2000000) { out(ctx, "ERR: baud 300..2000000\r\n"); return true; }
#ifdef BSP_UART_RX_PORTS
        UartRx_Suspend(UartRx_Find(u->h));
#endif
        HAL_UART_DeInit(u->h);
        u->h->Init.BaudRate = (uint32_t)baud;
        if (HAL_UART_Init(u->h) != HAL_OK) out(ctx, "ERR: HAL_UART_Init failed\r\n");
        else OUT("OK %s = %ld baud\r\n", u->name, baud);
#ifdef BSP_UART_RX_PORTS
        UartRx_Resume(UartRx_Find(u->h));
#endif
        return true;
    }
    if (strcmp(sub, "send") == 0) {
        char *text = strtok(0, "");          /* rest of line, may contain spaces */
        char msg[96];
        snprintf(msg, sizeof msg, "%s\r\n", text ? text : "");
        uart_send(u, (uint8_t *)msg, strlen(msg), out, ctx);
        return true;
    }
    if (strcmp(sub, "hex") == 0) {
        uint8_t d[64]; size_t n = 0; char *tok;
        while ((tok = strtok(0, " ")) && n < sizeof d) d[n++] = (uint8_t)strtol(tok, 0, 16);
        if (n == 0) { out(ctx, "ERR: give bytes, e.g. 'uart laser hex 01 02 FF'\r\n"); return true; }
        uart_send(u, d, n, out, ctx);
        return true;
    }
    if (strcmp(sub, "loop") == 0) {
        static const uint8_t pat[] = { 0x55, 0xAA, 'L', 'F', 0x00, 0xFF, '\r', '\n' };
        uint8_t back[sizeof pat]; size_t n;
        rx_flush(u);
        HAL_UART_Transmit(u->h, (uint8_t *)pat, sizeof pat, 100);
        n = rx_take(u, back, sizeof back, 100);
        if (n == sizeof pat && memcmp(pat, back, n) == 0) OUT("OK loopback on %s: %u/%u bytes back\r\n", u->name, (unsigned)n, (unsigned)sizeof pat);
        else OUT("FAIL loopback on %s: %u/%u bytes back (TX-RX jumper in place?)\r\n", u->name, (unsigned)n, (unsigned)sizeof pat);
        return true;
    }
#ifdef BSP_UART_RX_PORTS
    if (strcmp(sub, "stats") == 0) {
        const uart_rx_stats_t *st = UartRx_Stats(UartRx_Find(u->h));
        OUT("%s: dma rx %s  frames=%lu bytes=%lu dropped=%lu errors=%lu pending=%u\r\n",
            u->name, st->running ? "armed" : "STOPPED", (unsigned long)st->frames, (unsigned long)st->bytes,
            (unsigned long)st->dropped, (unsigned long)st->errors, (unsigned)UartRx_Pending(UartRx_Find(u->h)));
        OUT("       last event: size=%u type=%s  dma cndtr=%u  uart isr=0x%08lx\r\n", st->last_size,
            st->last_type == 0 ? "TC" : st->last_type == 1 ? "HT" : "IDLE", st->dma_cndtr,
            (unsigned long)u->h->Instance->ISR);
        OUT("       events=%lu last_tick=%lu  in-event: xfersize=%u xfercount=%u cndtr=%u cr3=0x%08lx\r\n",
            (unsigned long)st->events, (unsigned long)st->last_tick, st->ev_xfersize, st->ev_xfercount,
            st->ev_cndtr, (unsigned long)st->ev_cr3);
        return true;
    }
#endif
    OUT("ERR: uart %s: unknown subcommand '%s' (baud|send|hex|loop|stats)\r\n", u->name, sub);
    return true;
}

static bool cmd_pin(char *args, bringup_out_fn out, void *ctx)
{
    char *name = strtok(args, " ");
    if (!name) { list_pins(out, ctx); return true; }
    const pin_t *p = find_pin(name);
    if (!p) { OUT("ERR: no pin '%s' (try 'pin')\r\n", name); return true; }
    char *v = strtok(0, " ");
    if (v) {
        if (!p->writable) { OUT("ERR: %s is read-only here\r\n", p->name); return true; }
        if (!pin_is_output(p)) { OUT("ERR: %s is not configured as an output\r\n", p->name); return true; }
        HAL_GPIO_WritePin(p->port, p->pin, (v[0] == '1') ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_Delay(2);
    }
    OUT("%s = %d (%s)\r\n", p->name, HAL_GPIO_ReadPin(p->port, p->pin) == GPIO_PIN_SET, pin_is_output(p) ? "out" : "in");
    return true;
}

static bool cmd_adc(char *args, bringup_out_fn out, void *ctx)
{
    long n = args && *args ? strtol(args, 0, 10) : 1;
    if (n < 1) n = 1;
    if (n > 20) n = 20;
    uint8_t r[4];
    BSP_Temp_ReadRegs(r);
    OUT("ADS1220 regs: 0=0x%02X 1=0x%02X 2=0x%02X 3=0x%02X  (want 0=0x81 AIN0-AVSS g1 bypass, 2=0x50 REFP0 50/60Hz)\r\n",
        r[0], r[1], r[2], r[3]);
    for (long i = 0; i < n; i++) {
        temp_sample_t s = BSP_Temp_Sample();
        OUT("adc raw=%8ld (0x%06lX)  %.4f V  %.1f ohm  %.2f C\r\n",
            (long)s.raw, (unsigned long)(s.raw & 0xFFFFFF), s.volts, s.ohms, s.celsius);
        if (n > 1) HAL_Delay(100);
    }
    return true;
}

bool Bringup_Process(const char *line, bringup_out_fn out, void *ctx)
{
    char buf[128];
    if (strncmp(line, "adc", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        strncpy(buf, line + 3, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
        char *a = buf; while (*a == ' ') a++;
        return cmd_adc(a, out, ctx);
    }
    if (strncmp(line, "pin", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        strncpy(buf, line + 3, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
        return cmd_pin(buf, out, ctx);
    }
    if (strncmp(line, "uart", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        strncpy(buf, line + 4, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
        return cmd_uart(buf, out, ctx);
    }
    return false;
}

const char *Bringup_HelpText(void)
{
    return "  adc [<n>]             — bring-up: ADS1220 raw code, V, ohm, C (n samples)\r\n"
           "  pin [<name> 0|1]      — bring-up: list / drive board pins\r\n"
           "  uart [<name> baud <n> | send <text> | hex <bytes> | loop | stats]\r\n"
           "  uart duplex [<text>]  — all ports TX at once;  uart monitor on|off\r\n"
           "                        — received frames print as \"<port> rx <n>: <hex>\"\r\n"
           "                        — bring-up: raw access to the board's extra UARTs\r\n";
}
