#ifndef BRINGUP_H
#define BRINGUP_H
/*
 * Board bring-up commands for the CLI: poke enable pins, read power-good
 * and other inputs, and push raw bytes through the auxiliary UARTs, so a
 * new board can be checked against its schematic before any module driver
 * exists. Tables come from the board's bsp_config.h (BSP_BRINGUP_PINS,
 * BSP_BRINGUP_UARTS).
 *
 *   pin                         list all table pins with direction and level
 *   pin <name> 0|1              drive a writable pin, read it back
 *   uart                        list the table UARTs with their baud rate
 *   uart <name> baud <n>        re-init the UART at a new baud rate
 *   uart <name> send <text>     send text + CRLF, print what comes back (300 ms)
 *   uart <name> hex <aa bb ..>  send raw bytes, print what comes back (300 ms)
 *   uart <name> loop            TX->RX jumper test: send a pattern, expect it back
 *   uart <name> stats           DMA reception counters (DMA boards)
 *   uart monitor on|off         print received frames as they arrive (default on)
 *   uart duplex [<text>]        every table UART transmits "<text>-from-<name>" at
 *                               the same instant; shows tx and rx per port
 *
 * On boards with BSP_UART_RX_PORTS reception goes through uart_rx.c (DMA +
 * idle-line interrupt); "listen" then prints one line per received frame.
 *
 * Every frame a table UART receives is printed as soon as it arrives, on
 * the terminal channels only (never to a display in machine mode), as
 *   stage rx 3: 01 02 03   -- receiving port, byte count, bytes in hex.
 * Nothing is printed for what was sent. Bringup_Monitor() does this and is
 * called from CLI_Process().
 *
 * Replies are plain text; in the display's machine mode these commands are
 * never issued, so no protocol line format is affected.
 */
#include <stdbool.h>
#include <stddef.h>

typedef void (*bringup_out_fn)(void *ctx, const char *text);

/* Returns true when the line was a bring-up command (handled, reply sent). */
bool Bringup_Process(const char *line, bringup_out_fn out, void *ctx);

/* Print frames received since the last call (see above); returns how many. */
size_t Bringup_Monitor(bringup_out_fn out, void *ctx);

/* Help text lines, for the CLI 'help' output. */
const char *Bringup_HelpText(void);

#endif /* BRINGUP_H */
