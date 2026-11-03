#ifndef RTT_H
#define RTT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Minimal SEGGER RTT (Real Time Transfer) implementation -- a console over
 * the SWD debug link, needing nothing but the ST-Link that is already
 * attached for flashing. No UART, no USB.
 *
 * The control block lives in RAM in the layout SEGGER's host tools and
 * OpenOCD expect, so the host finds it by scanning for "SEGGER RTT" and then
 * reads/writes the two ring buffers directly over SWD while the firmware
 * runs -- no halt, no interrupt, no CPU involvement on the target beyond the
 * firmware's own ring-pointer updates.
 *
 * Host side (OpenOCD 0.12):
 *   rtt setup 0x20000000 0x8000 "SEGGER RTT"
 *   rtt start
 *   rtt server start 9090 0        # TCP: send a line, read the reply
 *
 * Debug builds only (CLI_RTT, see CMakeLists.txt). Costs RTT_UP_SIZE +
 * RTT_DOWN_SIZE bytes of RAM plus the control block.
 */

#define RTT_UP_SIZE    2048u   /* target -> host */
#define RTT_DOWN_SIZE  256u   /* host -> target; a whole CLI line while the firmware is busy */

void RTT_Init(void);
void RTT_Write(const void *buf, uint16_t len);   /* drops what does not fit */
bool RTT_ReadByte(uint8_t *out);

#endif /* RTT_H */
