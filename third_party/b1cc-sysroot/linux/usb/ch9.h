#ifndef _LINUX_USB_CH9_H
#define _LINUX_USB_CH9_H

/* Minimal <linux/usb/ch9.h> for b1nix: the USB 2.0 chapter-9 control-request
 * bmRequestType bitmask constants + the SETUP packet. b1nix has no USB device
 * stack, so WebUSB (services/device/usb/usb_device_handle_usbfs.cc) is dead here
 * — but it must compile + link (usb_device_linux.cc references
 * UsbDeviceHandleUsbfs). Full descriptor structs are omitted (unused). Real USB =
 * a future kernel subsystem. */

#include <stdint.h>

/* The 8-byte control-transfer SETUP packet (USB 2.0 §9.3). */
struct usb_ctrlrequest {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

#define USB_DIR_OUT          0      /* host-to-device */
#define USB_DIR_IN           0x80   /* device-to-host */

#define USB_TYPE_MASK        (0x03 << 5)
#define USB_TYPE_STANDARD    (0x00 << 5)
#define USB_TYPE_CLASS       (0x01 << 5)
#define USB_TYPE_VENDOR      (0x02 << 5)
#define USB_TYPE_RESERVED    (0x03 << 5)

#define USB_RECIP_MASK       0x1f
#define USB_RECIP_DEVICE     0x00
#define USB_RECIP_INTERFACE  0x01
#define USB_RECIP_ENDPOINT   0x02
#define USB_RECIP_OTHER      0x03

#endif /* _LINUX_USB_CH9_H */
