#include "bsp_laser.h"
#include "bsp_config.h"
#include "stm32g4xx_hal.h"
#include <string.h>

#if defined(BSP_LASER_DAC8562S)
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

static const laser_module_t s_none;
void   BSP_Laser_Process(void) {}
void   BSP_Laser_Power(bool on) { (void)on; }
const laser_module_t *BSP_Laser_Module(void) { return &s_none; }
bool   BSP_Laser_OwnsPort(const void *huart) { (void)huart; return false; }
size_t BSP_Laser_PopMonitor(uint8_t *dst, size_t max) { (void)dst; (void)max; return 0; }

#elif defined(BSP_LASER_SERIAL_DK)
/*
 * DK Photonics "Light Source Communication Protocol" (EDFA-style module)
 * on BSP_LASER_UART, received through uart_rx.c (DMA + idle interrupt).
 *
 *   to module:    EF EF LEN FF CMD [DATA] SUM
 *   from module:  ED FA LEN FF CMD [DATA] SUM      (CMD = 0xFF on error)
 *   LEN  = bytes from ADDR through SUM, SUM = low byte of the sum of
 *          everything before it. Baud 9600 8N1.
 *
 *   0x00  query all: sw, alarms, mode, pin, pout, iop1, tchip1, ... (51 B)
 *   0x51  read setting (22 23 = set current, 03 04 = max current), /10 mA
 *   0x52  set current in ACC mode: 22 HSB 23 LSB 20 0D 15, /10 mA
 *   0x0A  software switch 0/1 (reply carries CMD 0x0B)
 *
 * The module is powered from LASER_EN (5 V regulator with LASER_PG) and is
 * left powered once it has been switched on, so temperature and alarms can
 * be read while the laser is off. BSP_Laser_Power(false) cuts it.
 *
 * Power-up sequence, driven from BSP_Laser_Process():
 *   OFF -> PG (LASER_EN high, wait for LASER_PG) -> BOOT (module start-up
 *   delay) -> READ_MAX (0x51 03 04, the module's own maximum, shown by
 *   'status' for comparison; the ceiling is always BSP_LASER_MAX_CURRENT_A
 *   from bsp_config.h, SW-SPEC-001 §3.2) -> READY.
 * From READY: SET_I (0x52) when a current is pending, SWITCH (0x0A) when
 * the emit request differs from the module's switch, QUERY (0x00) once a
 * second otherwise. Every transaction waits for its reply with a timeout
 * and a few retries; a failure backs off two seconds and is counted.
 */
#include "uart_rx.h"
#include "main.h"

#define DK_REPLY_TIMEOUT_MS   300u
#define DK_RETRIES            3u
#define DK_PG_TIMEOUT_MS      500u
#define DK_BOOT_MS            800u
#define DK_QUERY_PERIOD_MS    1000u
#define DK_BACKOFF_MS         2000u
#define DK_STREAM_STALE_MS    500u

enum { ST_OFF, ST_PG, ST_BOOT, ST_READ_MAX, ST_READY, ST_SET_I, ST_SWITCH, ST_QUERY };
static const char *const s_state_names[] = {
    "off", "wait-pg", "boot", "read-max", "ready", "set-current", "switch", "query"
};

static laser_module_t s_m = { .state = "off" };
static uint8_t  s_state = ST_OFF;
static bool     s_want_power, s_want_emit;
static float    s_pending_A;        /* current to send next                      */
static bool     s_pending_dirty;    /* s_pending_A not yet acknowledged by module */
static uint32_t s_t0;               /* state entry tick                          */
static uint32_t s_retry_after;      /* READY does nothing before this tick       */
static uint32_t s_last_query;

/* ── outgoing transaction ───────────────────────────────────────────────── */
static struct {
    bool     busy;
    uint8_t  frame[16];
    uint8_t  len;
    uint8_t  expect;     /* CMD byte expected in the reply */
    uint8_t  tries_left;
    uint32_t sent;
} s_tx;

static void tx_send(void)
{
    HAL_UART_Transmit(BSP_LASER_UART, s_tx.frame, s_tx.len, 50);
    s_tx.sent = HAL_GetTick();
    s_m.tx_frames++;
}

static void tx_start(uint8_t cmd, const uint8_t *data, uint8_t n, uint8_t expect)
{
    uint8_t *f = s_tx.frame;
    uint8_t  i = 0;
    f[i++] = 0xEF; f[i++] = 0xEF;
    f[i++] = (uint8_t)(n + 3u);          /* ADDR CMD DATA.. SUM */
    f[i++] = 0xFF;
    f[i++] = cmd;
    for (uint8_t k = 0; k < n; k++) f[i++] = data[k];
    uint32_t sum = 0;
    for (uint8_t k = 0; k < i; k++) sum += f[k];
    f[i++] = (uint8_t)sum;
    s_tx.len = i;
    s_tx.expect = expect;
    s_tx.tries_left = DK_RETRIES;
    s_tx.busy = true;
    tx_send();
}

static void send_query(void)      { tx_start(0x00, NULL, 0, 0x00); }
static void send_read_max(void)   { static const uint8_t d[] = { 0x03, 0x04 }; tx_start(0x51, d, 2, 0x51); }
static void send_switch(bool on)  { uint8_t d = on ? 1u : 0u; tx_start(0x0A, &d, 1, 0x0B); }
static void send_set_current(float amps)
{
    uint32_t tenths = (uint32_t)(amps * 10000.0f + 0.5f);   /* A -> 0.1 mA */
    if (tenths > 0xFFFFu) tenths = 0xFFFFu;
    uint8_t d[7] = { 0x22, (uint8_t)(tenths >> 8), 0x23, (uint8_t)tenths, 0x20, 0x0D, 0x15 };
    tx_start(0x52, d, 7, 0x52);
}

/* ── incoming byte stream -> validated reply frames ─────────────────────── */
static uint8_t  s_in[128];
static size_t   s_in_len;
static uint32_t s_in_tick;

/* reply frames kept for 'uart monitor' (bringup.c prints them as "laser rx n: ..") */
#define MON_SLOTS 4
#define MON_SLOT_BYTES 64
static uint8_t s_mon[MON_SLOTS][MON_SLOT_BYTES];
static uint8_t s_mon_len[MON_SLOTS];
static uint8_t s_mon_head, s_mon_count;

static void mon_push(const uint8_t *d, size_t n)
{
    if (n > MON_SLOT_BYTES) n = MON_SLOT_BYTES;
    uint8_t slot = (uint8_t)((s_mon_head + s_mon_count) % MON_SLOTS);
    if (s_mon_count == MON_SLOTS) { s_mon_head = (uint8_t)((s_mon_head + 1) % MON_SLOTS); s_mon_count--; }
    memcpy(s_mon[slot], d, n);
    s_mon_len[slot] = (uint8_t)n;
    s_mon_count++;
}

size_t BSP_Laser_PopMonitor(uint8_t *dst, size_t max)
{
    if (s_mon_count == 0) return 0;
    size_t n = s_mon_len[s_mon_head];
    if (n > max) n = max;
    memcpy(dst, s_mon[s_mon_head], n);
    s_mon_head = (uint8_t)((s_mon_head + 1) % MON_SLOTS);
    s_mon_count--;
    return n;
}

static void parse_query(const uint8_t *f)
{
    /* f[0..4] = ED FA 36 FF 00, data from f[5]; positions per the protocol table */
    s_m.sw       = f[5];
    s_m.cur_alm  = f[8];
    s_m.pin_alm  = f[9];
    s_m.pout_alm = f[10];
    s_m.mode     = f[13];
    s_m.pin_raw  = (uint16_t)((f[15] << 8) | f[16]);
    s_m.pout_raw = (uint16_t)((f[17] << 8) | f[18]);
    s_m.iop_A    = (float)((f[22] << 8) | f[23]) / 10000.0f;
    s_m.tchip_C  = (float)(int16_t)((f[26] << 8) | f[27]) / 10.0f;
    s_m.emitting = (s_m.sw != 0);
    s_m.telemetry_tick = HAL_GetTick();
}

static void enter(uint8_t st) { s_state = st; s_t0 = HAL_GetTick(); s_m.state = s_state_names[st]; }

static void fail(void)
{
    s_m.timeouts++;
    s_tx.busy = false;
    s_retry_after = HAL_GetTick() + DK_BACKOFF_MS;
    if (s_state == ST_READ_MAX) s_m.max_source = 2;    /* no answer; ceiling stays the spec value */
    enter(ST_READY);
}

static void on_reply(const uint8_t *f, size_t n)
{
    s_m.rx_frames++;
    mon_push(f, n);
    if (!s_tx.busy) return;                       /* unsolicited: shown by monitor only */
    uint8_t cmd = f[4];
    if (cmd == 0xFF) { fail(); return; }          /* module rejected the command */
    if (cmd != s_tx.expect) return;               /* someone else's reply, keep waiting */
    s_tx.busy = false;
    switch (s_state) {
    case ST_READ_MAX:
        if (n >= 8) {
            s_m.module_max_A = (float)((f[5] << 8) | f[6]) / 10000.0f;
            s_m.max_source = 1;
        }
        enter(ST_READY);
        break;
    case ST_SET_I:
        s_m.set_current_A = s_pending_A;
        s_pending_dirty = false;
        enter(ST_READY);
        break;
    case ST_SWITCH:
        if (n >= 7) { s_m.sw = f[5]; s_m.emitting = (f[5] != 0); }
        enter(ST_READY);
        break;
    case ST_QUERY:
        if (n >= 56 && f[2] == 0x36) parse_query(f);
        enter(ST_READY);
        break;
    default:
        enter(ST_READY);
        break;
    }
}

static void drain_uart(void)
{
    int port = UartRx_Find(BSP_LASER_UART);
    if (port < 0) return;
    uint8_t buf[128];
    size_t n;
    while ((n = UartRx_Pop(port, buf, sizeof buf)) > 0) {
        if (s_in_len && HAL_GetTick() - s_in_tick > DK_STREAM_STALE_MS) s_in_len = 0;
        if (n > sizeof s_in - s_in_len) { s_in_len = 0; if (n > sizeof s_in) n = sizeof s_in; }
        memcpy(&s_in[s_in_len], buf, n);
        s_in_len += n;
        s_in_tick = HAL_GetTick();
    }
    /* pull complete frames off the front of the stream */
    for (;;) {
        if (s_in_len < 3) return;
        if (s_in[0] != 0xED || s_in[1] != 0xFA) {
            memmove(s_in, s_in + 1, --s_in_len);     /* resync on the header */
            s_m.bad_frames++;
            continue;
        }
        size_t total = 3u + s_in[2];
        if (total > sizeof s_in) { s_in_len = 0; s_m.bad_frames++; return; }
        if (s_in_len < total) return;                /* wait for the rest */
        uint32_t sum = 0;
        for (size_t i = 0; i + 1 < total; i++) sum += s_in[i];
        if ((uint8_t)sum == s_in[total - 1]) {
            on_reply(s_in, total);
        } else {
            s_m.bad_frames++;
        }
        memmove(s_in, s_in + total, s_in_len - total);
        s_in_len -= total;
    }
}

/* ── public API ─────────────────────────────────────────────────────────── */
void BSP_Laser_Init(void)
{
    HAL_GPIO_WritePin(BSP_LASER_EN_PORT, BSP_LASER_EN_PIN, GPIO_PIN_RESET);
    memset(&s_m, 0, sizeof s_m);
    enter(ST_OFF);
}

void BSP_Laser_SetCurrent(float amps)
{
    if (amps < 0.0f) amps = 0.0f;
    if (amps > BSP_LASER_MAX_CURRENT_A) amps = BSP_LASER_MAX_CURRENT_A;   /* hardware max, never exceeded */
    s_pending_A = amps;
    s_pending_dirty = true;
    s_want_power = true;
    s_want_emit = true;
}

void BSP_Laser_Disable(void)
{
    s_want_emit = false;
    s_pending_A = 0.0f;
    /* nothing to undo on a module that never got a current or the switch */
    s_pending_dirty = (s_m.set_current_A != 0.0f) || s_m.emitting;
}

void BSP_Laser_Power(bool on)
{
    if (on) { s_want_power = true; return; }
    s_want_power = false;
    s_want_emit = false;
    HAL_GPIO_WritePin(BSP_LASER_EN_PORT, BSP_LASER_EN_PIN, GPIO_PIN_RESET);
    s_tx.busy = false;
    s_in_len = 0;
    s_m.powered = false; s_m.pg = false; s_m.emitting = false; s_m.sw = 0;
    s_m.max_source = 0; s_m.module_max_A = 0.0f;
    s_m.set_current_A = 0.0f;
    enter(ST_OFF);
}

const laser_module_t *BSP_Laser_Module(void) { return &s_m; }

bool BSP_Laser_OwnsPort(const void *huart) { return huart == (const void *)BSP_LASER_UART; }

void BSP_Laser_Process(void)
{
    uint32_t now = HAL_GetTick();
    drain_uart();

    if (s_tx.busy && now - s_tx.sent > DK_REPLY_TIMEOUT_MS) {
        if (--s_tx.tries_left) tx_send();
        else fail();
    }
    if (s_tx.busy) return;

    switch (s_state) {
    case ST_OFF:
        if (!s_want_power) break;
        HAL_GPIO_WritePin(BSP_LASER_EN_PORT, BSP_LASER_EN_PIN, GPIO_PIN_SET);
        s_m.powered = true;
        enter(ST_PG);
        break;
    case ST_PG:
        if (HAL_GPIO_ReadPin(BSP_LASER_PG_PORT, BSP_LASER_PG_PIN) == GPIO_PIN_SET) { s_m.pg = true; enter(ST_BOOT); }
        else if (now - s_t0 > DK_PG_TIMEOUT_MS) enter(ST_BOOT);   /* try anyway, pg stays false */
        break;
    case ST_BOOT:
        if (now - s_t0 > DK_BOOT_MS) { UartRx_Flush(UartRx_Find(BSP_LASER_UART)); s_in_len = 0; send_read_max(); enter(ST_READ_MAX); }
        break;
    case ST_READY:
        if ((int32_t)(now - s_retry_after) < 0) break;
        if (s_pending_dirty)                      { send_set_current(s_pending_A); enter(ST_SET_I); }
        else if (s_want_emit != s_m.emitting)     { send_switch(s_want_emit); enter(ST_SWITCH); }
        else if (now - s_last_query > DK_QUERY_PERIOD_MS) { s_last_query = now; send_query(); enter(ST_QUERY); }
        break;
    default:
        break;   /* waiting for a reply: handled above */
    }
}

#else  /* no laser hardware: stubs so laser_control.c links */
static const laser_module_t s_none;
void   BSP_Laser_Init(void)              {}
void   BSP_Laser_SetCurrent(float amps)  { (void)amps; }
void   BSP_Laser_Disable(void)           {}
void   BSP_Laser_Process(void) {}
void   BSP_Laser_Power(bool on) { (void)on; }
const laser_module_t *BSP_Laser_Module(void) { return &s_none; }
bool   BSP_Laser_OwnsPort(const void *huart) { (void)huart; return false; }
size_t BSP_Laser_PopMonitor(uint8_t *dst, size_t max) { (void)dst; (void)max; return 0; }
#endif
