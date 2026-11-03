#ifndef UART_RX_H
#define UART_RX_H
/*
 * Frame-oriented UART reception over DMA.
 *
 * Each port in BSP_UART_RX_PORTS(X) gets a circular DMA buffer that the
 * UART fills on its own. The CPU is only interrupted when the line goes
 * idle for one character time after the last byte (HAL "ReceiveToIdle"),
 * so one interrupt per message instead of one per byte. The ISR copies
 * the new bytes into a frame queue; the main loop pops whole frames with
 * UartRx_Pop(). An "echo" mode transmits every received frame straight
 * back out on the same port (useful for bring-up with a terminal).
 *
 * Boards without BSP_UART_RX_PORTS get empty inline stubs, so main.c and
 * bringup.c can call this unconditionally.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bsp_config.h"

typedef struct {
    uint32_t frames;    /* idle events that delivered at least one byte      */
    uint32_t bytes;     /* bytes delivered to the frame queue                */
    uint32_t dropped;   /* bytes lost because the frame queue was full       */
    uint32_t errors;    /* ORE/FE/NE/PE seen -> reception was restarted      */
    uint32_t echoed;    /* frames sent back in echo mode                     */
    uint16_t last_size; /* Size argument of the last RxEvent              */
    uint8_t  last_type; /* HAL_UART_RXEVENT_TC=0 HT=1 IDLE=2 of last event    */
    uint16_t dma_cndtr; /* DMA remaining-count register when stats were read  */
    uint32_t events;    /* RxEvent callbacks seen (incl. empty ones)          */
    uint32_t last_tick; /* HAL_GetTick() at the last event                     */
    uint16_t ev_xfersize, ev_xfercount, ev_cndtr; /* HAL state inside the event */
    uint32_t ev_cr3;
    bool     echo;
    bool     running;   /* DMA reception armed                               */
} uart_rx_stats_t;

#ifdef BSP_UART_RX_PORTS

enum {
#define X(name, handle) UART_RX_##name,
    BSP_UART_RX_PORTS(X)
#undef X
    UART_RX_PORT_COUNT
};

void   UartRx_Init(void);                 /* arm DMA reception on every port   */
void   UartRx_Process(void);              /* main loop: echo, restart on error */
int    UartRx_Find(const void *huart);    /* port index for a UART handle, -1  */
size_t UartRx_Pop(int port, uint8_t *dst, size_t max);  /* one frame, 0 = none;
                                             a frame longer than max is truncated */
size_t UartRx_Pending(int port);          /* frames waiting                    */
void   UartRx_Flush(int port);            /* discard queued frames             */
void   UartRx_SetEcho(int port, bool on);
const uart_rx_stats_t *UartRx_Stats(int port);
void   UartRx_Suspend(int port);          /* stop DMA before HAL_UART_DeInit   */
void   UartRx_Resume(int port);           /* re-arm after HAL_UART_Init        */

#else

static inline void UartRx_Init(void) {}
static inline void UartRx_Process(void) {}

#endif /* BSP_UART_RX_PORTS */
#endif /* UART_RX_H */
