#ifndef _LINUX_USBDEVICE_FS_H
#define _LINUX_USBDEVICE_FS_H

/* Minimal <linux/usbdevice_fs.h> for b1nix: the usbfs (/dev/bus/usb) URB + ioctl
 * ABI. b1nix has no USB stack, so this WebUSB backend is dead code (the ioctls are
 * never issued) — it only needs to compile + link. Struct layouts and ioctl
 * numbers match the Linux usbfs UAPI. Real USB = a future kernel subsystem. */

#include <stdint.h>
#include <linux/ioctl.h>

#define USBDEVFS_MAXDRIVERNAME 255

struct usbdevfs_iso_packet_desc {
    unsigned int length;
    unsigned int actual_length;
    unsigned int status;
};

struct usbdevfs_urb {
    unsigned char type;
    unsigned char endpoint;
    int status;
    unsigned int flags;
    void *buffer;
    int buffer_length;
    int actual_length;
    int start_frame;
    union {
        int number_of_packets;
        unsigned int stream_id;
    };
    int error_count;
    unsigned int signr;
    void *usercontext;
    struct usbdevfs_iso_packet_desc iso_frame_desc[0];
};

struct usbdevfs_ctrltransfer {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint32_t timeout;
    void *data;
};

struct usbdevfs_setinterface {
    unsigned int interface;
    unsigned int altsetting;
};

struct usbdevfs_getdriver {
    unsigned int interface;
    char driver[USBDEVFS_MAXDRIVERNAME + 1];
};

struct usbdevfs_ioctl {
    int ifno;
    int ioctl_code;
    void *data;
};

#define USBDEVFS_URB_TYPE_ISO        0
#define USBDEVFS_URB_TYPE_INTERRUPT  1
#define USBDEVFS_URB_TYPE_CONTROL    2
#define USBDEVFS_URB_TYPE_BULK       3

#define USBDEVFS_CONTROL          _IOWR('U', 0, struct usbdevfs_ctrltransfer)
#define USBDEVFS_SETINTERFACE     _IOR('U', 4, struct usbdevfs_setinterface)
#define USBDEVFS_SETCONFIGURATION _IOR('U', 5, unsigned int)
#define USBDEVFS_GETDRIVER        _IOW('U', 8, struct usbdevfs_getdriver)
#define USBDEVFS_SUBMITURB        _IOR('U', 10, struct usbdevfs_urb)
#define USBDEVFS_DISCARDURB       _IO('U', 11)
#define USBDEVFS_REAPURBNDELAY    _IOW('U', 13, void *)
#define USBDEVFS_CLAIMINTERFACE   _IOR('U', 15, unsigned int)
#define USBDEVFS_RELEASEINTERFACE _IOR('U', 16, unsigned int)
#define USBDEVFS_IOCTL            _IOWR('U', 18, struct usbdevfs_ioctl)
#define USBDEVFS_RESET            _IO('U', 20)
#define USBDEVFS_CLEAR_HALT       _IOR('U', 21, unsigned int)
#define USBDEVFS_DISCONNECT       _IO('U', 22)
#define USBDEVFS_CONNECT          _IO('U', 23)

#endif /* _LINUX_USBDEVICE_FS_H */
