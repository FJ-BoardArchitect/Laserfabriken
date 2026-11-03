#include "uart_rx.h"
#ifdef BSP_UART_RX_PORTS
#include "main.h"
#include <string.h>

#define DMA_BUF_SIZE    256u    /* circular DMA target; > longest burst between idles */
#define FRAME_RING_SIZE 512u    /* bytes of queued frames per port                   */
#define FRAME_QUEUE_LEN 16u     /* frames per port                                   */

typedef struct {
    UART_HandleTypeDef *h;
    uint8_t  dma[DMA_BUF_SIZE];
    uint16_t dma_pos;                   /* where the previous event left off */

    volatile uint8_t  ring[FRAME_RING_SIZE];
    volatile uint16_t ring_head, ring_tail;
    volatile uint16_t len[FRAME_QUEUE_LEN];
    volatile uint8_t  len_head, len_tail;

    volatile bool need_restart;
    uart_rx_stats_t st;
} port_t;

static port_t s_port[UART_RX_PORT_COUNT] = {
#define X(name, handle) { .h = &handle },
    BSP_UART_RX_PORTS(X)
#undef X
};

static port_t *by_handle(const UART_HandleTypeDef *h)
{
    for (int i = 0; i < UART_RX_PORT_COUNT; i++)
        if (s_port[i].h == h) return &s_port[i];
    return 0;
}

static uint16_t ring_free(const port_t *p)
{
    return (uint16_t)((p->ring_tail + FRAME_RING_SIZE - p->ring_head - 1u) % FRAME_RING_SIZE);
}

/* ISR context: append `n` bytes from the DMA buffer as one frame. */
static void push_frame(port_t *p, uint16_t from, uint16_t n)
{
    uint8_t next_len = (uint8_t)((p->len_head + 1u) % FRAME_QUEUE_LEN);
    if (next_len == p->len_tail || ring_free(p) < n) {
        p->st.dropped += n;
        return;
    }
    for (uint16_t i = 0; i < n; i++) {
        p->ring[p->ring_head] = p->dma[(from + i) % DMA_BUF_SIZE];
        p->ring_head = (uint16_t)((p->ring_head + 1u) % FRAME_RING_SIZE);
    }
    p->len[p->len_head] = n;
    p->len_head = next_len;
    p->st.frames++;
    p->st.bytes += n;
}

static void arm(port_t *p)
{
    p->dma_pos = 0;
    p->need_restart = false;
    __HAL_UART_CLEAR_OREFLAG(p->h);
    __HAL_UART_CLEAR_NEFLAG(p->h);
    __HAL_UART_CLEAR_FEFLAG(p->h);
    __HAL_UART_CLEAR_PEFLAG(p->h);
    if (HAL_UARTEx_ReceiveToIdle_DMA(p->h, p->dma, DMA_BUF_SIZE) == HAL_OK) {
        /* Only the idle line and buffer wrap should interrupt us, not every
         * half buffer -- a half-transfer event would split frames in two. */
        __HAL_DMA_DISABLE_IT(p->h->hdmarx, DMA_IT_HT);
        p->st.running = true;
    } else {
        p->st.running = false;
    }
}

void UartRx_Init(void)
{
    for (int i = 0; i < UART_RX_PORT_COUNT; i++) arm(&s_port[i]);
}

/* HAL calls this from the UART/DMA ISR with the DMA write position.
 *
 * Size is where the DMA will write next, except that a transfer-complete
 * (buffer wrap) reports Size == DMA_BUF_SIZE. The HAL also reports an IDLE
 * event with Size == DMA_BUF_SIZE when the line goes idle while the DMA
 * counter is untouched; the hardware raises one such IDLE right after the
 * receiver is enabled, so this happens at every arm(). Comparing positions
 * modulo the buffer size makes that case "nothing new". */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    port_t *p = by_handle(huart);
    if (!p) return;
    HAL_UART_RxEventTypeTypeDef type = HAL_UARTEx_GetRxEventType(huart);
    p->st.last_size = Size;
    p->st.last_type = (uint8_t)type;
    p->st.events++;
    p->st.last_tick = HAL_GetTick();
    p->st.ev_xfersize = huart->RxXferSize;
    p->st.ev_xfercount = huart->RxXferCount;
    p->st.ev_cndtr = (uint16_t)__HAL_DMA_GET_COUNTER(huart->hdmarx);
    p->st.ev_cr3 = huart->Instance->CR3;

    uint16_t pos = (uint16_t)(Size % DMA_BUF_SIZE);
    uint16_t n;
    if (pos == p->dma_pos) {
        /* Only a transfer-complete from position 0 means a whole buffer of
         * new data; an IDLE at the old position means nothing arrived. */
        if (type == HAL_UART_RXEVENT_TC && p->dma_pos == 0u) n = DMA_BUF_SIZE;
        else return;
    } else if (pos > p->dma_pos) {
        n = (uint16_t)(pos - p->dma_pos);
    } else {
        n = (uint16_t)(DMA_BUF_SIZE - p->dma_pos + pos);
    }
    push_frame(p, p->dma_pos, n);
    p->dma_pos = pos;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    port_t *p = by_handle(huart);
    if (!p) return;
    p->st.errors++;
    p->st.running = false;
    p->need_restart = true;   /* HAL has aborted the DMA receive; re-arm from main loop */
}

void UartRx_Process(void)
{
    for (int i = 0; i < UART_RX_PORT_COUNT; i++) {
        port_t *p = &s_port[i];
        if (p->need_restart && p->h->RxState == HAL_UART_STATE_READY) arm(p);
        if (p->st.echo) {
            uint8_t buf[DMA_BUF_SIZE];
            size_t n;
            while ((n = UartRx_Pop(i, buf, sizeof buf)) > 0) {
                /* 9600 baud ~ 1 ms per byte; allow for that plus margin. */
                HAL_UART_Transmit(p->h, buf, (uint16_t)n, (uint32_t)n * 2u + 20u);
                p->st.echoed++;
            }
        }
    }
}

int UartRx_Find(const void *huart)
{
    for (int i = 0; i < UART_RX_PORT_COUNT; i++)
        if (s_port[i].h == huart) return i;
    return -1;
}

size_t UartRx_Pending(int port)
{
    if (port < 0 || port >= UART_RX_PORT_COUNT) return 0;
    const port_t *p = &s_port[port];
    return (size_t)((p->len_head + FRAME_QUEUE_LEN - p->len_tail) % FRAME_QUEUE_LEN);
}

size_t UartRx_Pop(int port, uint8_t *dst, size_t max)
{
    if (port < 0 || port >= UART_RX_PORT_COUNT) return 0;
    port_t *p = &s_port[port];
    if (p->len_head == p->len_tail) return 0;
    uint16_t n = p->len[p->len_tail];
    size_t out = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b = p->ring[p->ring_tail];
        p->ring_tail = (uint16_t)((p->ring_tail + 1u) % FRAME_RING_SIZE);
        if (out < max) dst[out++] = b;
    }
    p->len_tail = (uint8_t)((p->len_tail + 1u) % FRAME_QUEUE_LEN);
    return out;
}

void UartRx_Flush(int port)
{
    uint8_t scratch[8];
    while (UartRx_Pop(port, scratch, sizeof scratch) > 0) {}
}

void UartRx_SetEcho(int port, bool on)
{
    if (port >= 0 && port < UART_RX_PORT_COUNT) s_port[port].st.echo = on;
}

const uart_rx_stats_t *UartRx_Stats(int port)
{
    if (port < 0 || port >= UART_RX_PORT_COUNT) return 0;
    s_port[port].st.dma_cndtr = (uint16_t)__HAL_DMA_GET_COUNTER(s_port[port].h->hdmarx);
    return &s_port[port].st;
}

void UartRx_Suspend(int port)
{
    if (port < 0 || port >= UART_RX_PORT_COUNT) return;
    port_t *p = &s_port[port];
    HAL_UART_DMAStop(p->h);
    p->st.running = false;
}

void UartRx_Resume(int port)
{
    if (port < 0 || port >= UART_RX_PORT_COUNT) return;
    arm(&s_port[port]);
}

#endif /* BSP_UART_RX_PORTS */
