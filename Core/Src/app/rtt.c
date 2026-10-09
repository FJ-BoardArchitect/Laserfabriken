#include "rtt.h"
#include <string.h>

/* Layouts as defined by SEGGER (RTT protocol, "SEGGER_RTT.h"): field order
 * and sizes are what the host tools parse, do not reorder. */
typedef struct {
    const char       *name;
    char             *buf;
    unsigned          size;
    volatile unsigned wr;     /* written by producer only */
    volatile unsigned rd;     /* written by consumer only */
    unsigned          flags;  /* 0 = skip (drop) when full */
} rtt_buf_t;

typedef struct {
    char      id[16];
    int       max_up;
    int       max_down;
    rtt_buf_t up[1];
    rtt_buf_t down[1];
} rtt_cb_t;

static char s_up_buf[RTT_UP_SIZE];
static char s_down_buf[RTT_DOWN_SIZE];

/* Named like SEGGER's own so gdb users recognise it. Kept in .bss and the
 * id string filled in at runtime: a compile-time initialiser would put a
 * second "SEGGER RTT" literal in flash and the host scan could latch onto
 * the wrong one. */
rtt_cb_t _SEGGER_RTT;

void RTT_Init(void)
{
    memset(&_SEGGER_RTT, 0, sizeof(_SEGGER_RTT));
    _SEGGER_RTT.max_up   = 1;
    _SEGGER_RTT.max_down = 1;
    _SEGGER_RTT.up[0].name   = "Terminal";
    _SEGGER_RTT.up[0].buf    = s_up_buf;
    _SEGGER_RTT.up[0].size   = RTT_UP_SIZE;
    _SEGGER_RTT.down[0].name = "Terminal";
    _SEGGER_RTT.down[0].buf  = s_down_buf;
    _SEGGER_RTT.down[0].size = RTT_DOWN_SIZE;
    /* id last, and byte by byte, so the block is complete before the host
     * can possibly recognise it. */
    static const char id[] = "SEGGER RTT";
    for (unsigned i = 0; i < sizeof(id); i++)
        _SEGGER_RTT.id[i] = id[i];
}

void RTT_Write(const void *buf, uint16_t len)
{
    rtt_buf_t *b = &_SEGGER_RTT.up[0];
    const uint8_t *p = (const uint8_t *)buf;
    unsigned wr = b->wr;
    for (uint16_t i = 0; i < len; i++) {
        unsigned next = (wr + 1u) % b->size;
        if (next == b->rd)
            break;                 /* full: drop the rest, never block */
        b->buf[wr] = (char)p[i];
        wr = next;
    }
    b->wr = wr;
}

bool RTT_ReadByte(uint8_t *out)
{
    rtt_buf_t *b = &_SEGGER_RTT.down[0];
    unsigned rd = b->rd;
    if (rd == b->wr)
        return false;
    *out = (uint8_t)b->buf[rd];
    b->rd = (rd + 1u) % b->size;
    return true;
}
