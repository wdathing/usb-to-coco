#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

// CFG_TUSB_MCU is supplied by the SDK's tinyusb integration (rp2040 family
// covers the Pico 2 / RP2350 as well, same USB core) via compiler defines.
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// Host Configuration -- this board is host-only (native RP2350 USB
// controller in host mode), so root hub port is always 0.
//--------------------------------------------------------------------

#define CFG_TUH_ENABLED 1
#define BOARD_TUH_RHPORT 0
#define CFG_TUH_MAX_SPEED OPT_MODE_DEFAULT_SPEED

// Size of buffer to hold descriptors and other data used for enumeration.
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// A physical hub lets multiple USB devices (keyboard + something else)
// share the one port.
#define CFG_TUH_HUB 1
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

// Typical keyboard exposes a couple of HID interfaces (main + consumer
// control); leave headroom for a hub with a couple of devices attached.
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)
#define CFG_TUH_HID (3 * CFG_TUH_DEVICE_MAX)
#define CFG_TUH_HID_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif
