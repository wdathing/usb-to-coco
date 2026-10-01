#pragma once

// Brings up BTstack, the CYW43439 HCI transport, and both the classic
// Bluetooth HID Host profile (older keyboards) and LE HID-over-GATT Host
// (most current-generation BT keyboards). Call once from main() after
// cyw43_arch_init().
void bt_hid_init(void);

// Debounces the pairing button (GPIO7) and times out the pairing window.
// Call regularly from the main loop.
void bt_hid_task(void);
