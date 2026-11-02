#include "bringup.h"
#include "bsp_config.h"
#include "main.h"
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
    for (const uart_t *u = s_uarts; u->name; u++)
        OUT("%-6s %lu baud  %s\r\n", u->name, (unsigned long)u->h->Init.BaudRate, u->where);
}

/* Receive for up to `ms`, printing bytes as hex and as printable text. */
static void dump_rx(const uart_t *u, uint32_t ms, bringup_out_fn out, void *ctx)
{
    uint8_t buf[16];
    uint32_t t0 = HAL_GetTick();
    size_t total = 0;
    __HAL_UART_CLEAR_OREFLAG(u->h);
    while (HAL_GetTick() - t0 < ms) {
        size_t n = 0;
        while (n < sizeof buf && HAL_UART_Receive(u->h, &buf[n], 1, 5) == HAL_OK) n++;
        if (n == 0) continue;
        total += n;
        char hex[3 * 16 + 1], asc[16 + 1];
        size_t hp = 0;
        for (size_t i = 0; i < n; i++) {
            hp += (size_t)snprintf(&hex[hp], sizeof hex - hp, "%02X ", buf[i]);
            asc[i] = isprint(buf[i]) ? (char)buf[i] : '.';
        }
        asc[n] = '\0';
        OUT("  rx %2u: %s |%s|\r\n", (unsigned)n, hex, asc);
    }
    if (total == 0) out(ctx, "  rx: nothing\r\n");
}

static void uart_send(const uart_t *u, const uint8_t *d, size_t n, bringup_out_fn out, void *ctx)
{
    if (HAL_UART_Transmit(u->h, (uint8_t *)d, (uint16_t)n, 200) != HAL_OK) {
        out(ctx, "  tx: HAL error\r\n");
        return;
    }
    OUT("  tx %2u bytes\r\n", (unsigned)n);
}

static bool cmd_uart(char *args, bringup_out_fn out, void *ctx)
{
    char *name = strtok(args, " ");
    if (!name) { list_uarts(out, ctx); return true; }
    const uart_t *u = find_uart(name);
    if (!u) { OUT("ERR: no uart '%s' (try 'uart')\r\n", name); return true; }
    char *sub = strtok(0, " ");
    if (!sub) { OUT("%s: %lu baud, %s\r\n", u->name, (unsigned long)u->h->Init.BaudRate, u->where); return true; }

    if (strcmp(sub, "baud") == 0) {
        char *v = strtok(0, " ");
        long baud = v ? strtol(v, 0, 10) : 0;
        if (baud < 300 || baud > 2000000) { out(ctx, "ERR: baud 300..2000000\r\n"); return true; }
        HAL_UART_DeInit(u->h);
        u->h->Init.BaudRate = (uint32_t)baud;
        if (HAL_UART_Init(u->h) != HAL_OK) out(ctx, "ERR: HAL_UART_Init failed\r\n");
        else OUT("OK %s = %ld baud\r\n", u->name, baud);
        return true;
    }
    if (strcmp(sub, "send") == 0) {
        char *text = strtok(0, "");          /* rest of line, may contain spaces */
        char msg[96];
        snprintf(msg, sizeof msg, "%s\r\n", text ? text : "");
        uart_send(u, (uint8_t *)msg, strlen(msg), out, ctx);
        dump_rx(u, 300, out, ctx);
        return true;
    }
    if (strcmp(sub, "hex") == 0) {
        uint8_t d[64]; size_t n = 0; char *tok;
        while ((tok = strtok(0, " ")) && n < sizeof d) d[n++] = (uint8_t)strtol(tok, 0, 16);
        if (n == 0) { out(ctx, "ERR: give bytes, e.g. 'uart laser hex 01 02 FF'\r\n"); return true; }
        uart_send(u, d, n, out, ctx);
        dump_rx(u, 300, out, ctx);
        return true;
    }
    if (strcmp(sub, "listen") == 0) {
        char *v = strtok(0, " ");
        long s = v ? strtol(v, 0, 10) : 2;
        if (s < 1) s = 1;
        if (s > 10) s = 10;
        OUT("listening on %s for %ld s ...\r\n", u->name, s);
        dump_rx(u, (uint32_t)s * 1000u, out, ctx);
        return true;
    }
    if (strcmp(sub, "loop") == 0) {
        static const uint8_t pat[] = { 0x55, 0xAA, 'L', 'F', 0x00, 0xFF, '\r', '\n' };
        uint8_t back[sizeof pat]; size_t n = 0;
        __HAL_UART_CLEAR_OREFLAG(u->h);
        while (HAL_UART_Receive(u->h, back, 1, 2) == HAL_OK) {}   /* flush stale bytes */
        HAL_UART_Transmit(u->h, (uint8_t *)pat, sizeof pat, 100);
        while (n < sizeof pat && HAL_UART_Receive(u->h, &back[n], 1, 50) == HAL_OK) n++;
        if (n == sizeof pat && memcmp(pat, back, n) == 0) OUT("OK loopback on %s: %u/%u bytes back\r\n", u->name, (unsigned)n, (unsigned)sizeof pat);
        else OUT("FAIL loopback on %s: %u/%u bytes back (TX-RX jumper in place?)\r\n", u->name, (unsigned)n, (unsigned)sizeof pat);
        return true;
    }
    OUT("ERR: uart %s: unknown subcommand '%s' (baud|send|hex|listen|loop)\r\n", u->name, sub);
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

bool Bringup_Process(const char *line, bringup_out_fn out, void *ctx)
{
    char buf[128];
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
    return "  pin [<name> 0|1]      — bring-up: list / drive board pins\r\n"
           "  uart [<name> baud <n> | send <text> | hex <bytes> | listen <s> | loop]\r\n"
           "                        — bring-up: raw access to the board's extra UARTs\r\n";
}
