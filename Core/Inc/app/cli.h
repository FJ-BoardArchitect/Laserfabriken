#ifndef CLI_H
#define CLI_H

/* Print welcome banner and prompt. */
void CLI_Init(void);

/* Drain every channel (USB-CDC terminal/display, display UART on UGN),
 * process complete lines, push state changes to the display.
 * Call from main loop on every iteration (non-blocking). */
void CLI_Process(void);

/* Called from the USB stack (CDC_Init_FS, interrupt context) each time a
 * host enumerates the device. Resets the USB channel to human/terminal
 * mode so a new host -- PC or display -- always starts from a known state;
 * the display re-establishes machine mode with its 'hello'. */
void CLI_OnUsbEnumerated(void);

#endif /* CLI_H */
