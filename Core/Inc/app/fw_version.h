#ifndef FW_VERSION_H
#define FW_VERSION_H

/*
 * Firmware version -- the single source for every place a version is
 * reported: the 'hello' reply to the display (cli.c), the USB device
 * descriptor's bcdDevice field (usb_device.c, USER CODE PreTreatment), and
 * the CLI banner. Bump here only.
 *
 * bcdDevice is BCD-coded 0xJJMN (major JJ, minor M, patch N), so minor and
 * patch must each stay in 0..9 for the USB field to read correctly.
 */
#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  1
#define FW_VERSION_PATCH  0

#define FW_VERSION_STR_(a, b, c)  #a "." #b "." #c
#define FW_VERSION_STR__(a, b, c) FW_VERSION_STR_(a, b, c)
#define FW_VERSION_STR  FW_VERSION_STR__(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)

#define FW_VERSION_BCD  (uint16_t)(((FW_VERSION_MAJOR / 10) << 12) | ((FW_VERSION_MAJOR % 10) << 8) \
                                 | (FW_VERSION_MINOR << 4) | FW_VERSION_PATCH)

#endif /* FW_VERSION_H */
