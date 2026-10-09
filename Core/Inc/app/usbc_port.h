#ifndef USBC_PORT_H
#define USBC_PORT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * USB-C port power role: decides whether this board puts its 5V on the
 * USB-C connector's VBUS.
 *
 *   display plugged in (it presents Rd = "I want power")  -> 5V ON
 *   PC plugged in      (it, or its USB-A cable, presents Rp) -> 5V OFF
 *   nothing plugged in                                      -> 5V OFF
 *
 * Done with the STM32's own UCPD peripheral as a plain Type-C dual-role
 * port (no Power Delivery): while nothing is attached it alternates
 * between presenting Rp (source) and Rd (sink) on CC1/CC2 and watches the
 * CC voltage comparators. Data roles are unaffected: the STM32 is the USB
 * device in both cases.
 *
 * Only active when BSP_USBC_VBUS_SWITCH is defined (bsp_config.h): on UGN
 * the 5V regulator (M6_EN, PB0) feeds both the USB-C VBUS and the FPC
 * connector, so with an FPC-attached display this must stay off and the
 * 5V stays permanently on as before.
 *
 * See Docs/spud_usb_displaylank.md §3.
 */

typedef enum {
    USBC_UNATTACHED = 0,
    USBC_ATTACHED_DISPLAY,   /* we are source: 5V on */
    USBC_ATTACHED_PC,        /* we are sink:   5V off */
} UsbcState_t;

void        USBC_Init(void);            /* after MX_UCPD1_Init(), before the main loop */
void        USBC_Process(void);         /* every main-loop iteration (self-timed) */
UsbcState_t USBC_GetState(void);
const char *USBC_StateStr(void);
bool        USBC_Is5VOn(void);
bool        USBC_Enabled(void);         /* false: BSP_USBC_VBUS_SWITCH not defined */
void        USBC_DebugCC(uint8_t *cc1, uint8_t *cc2, bool *source_role); /* raw comparator states */

#endif /* USBC_PORT_H */
