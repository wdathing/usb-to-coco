#pragma once

// BTstack config for Bluetooth HID Host on Pico 2 W (CYW43439): classic
// BR/EDR HID (older keyboards/mice) plus LE HID-over-GATT (HOGP -- most
// current-generation BT keyboards, incl. the 8BitDo Retro Mechanical
// Keyboard, use this instead of classic). No mesh/audio needed.

#define HAVE_EMBEDDED_TIME_MS

// ENABLE_CLASSIC comes from the linked pico_btstack_classic CMake target,
// ENABLE_BLE from pico_btstack_ble -- not defined here too (would just warn
// about redefinition).
// We only ever initiate LE connections (scan + connect to a peripheral
// keyboard), never advertise/accept one, so central-only is enough.
#define ENABLE_LE_CENTRAL
// Most modern peripherals support LE Secure Connections; sm.c falls back to
// LE Legacy pairing automatically if the peer doesn't. Pulls in P256 ECDH,
// which needs an ECC backend picked explicitly -- micro-ecc (uECC.c) is
// already unconditionally part of pico_btstack_base, so use that rather
// than pulling in mbedTLS's implementation too.
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_MICRO_ECC_P256
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
// Required by platform/embedded/hci_dump_embedded_stdout.c, which
// pico_btstack_base always compiles in regardless of whether it's used.
#define ENABLE_PRINTF_HEXDUMP

// HID reports are tiny; a modest ACL payload is plenty and keeps RAM down.
// (Also comfortably above the 69-byte minimum sm.c requires for LE Secure
// Connections.)
#define HCI_ACL_PAYLOAD_SIZE 200

// Required by the cyw43 HCI transport (btstack_hci_transport_cyw43.c):
// room for its own packet header, and chunk-size alignment for the SPI bus.
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// One keyboard connected at a time -- 2 rather than 1 so a still-closing
// old connection can't transiently block a fresh one from the same button
// press.
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_L2CAP_CHANNELS 4

// Real bug, found on real hardware (2026-08-14): this was never defined,
// which BTstack silently treats as size 0 (not an #error) -- meaning
// hid_host_connect()'s internal hid_host_connection_t pool had zero
// capacity this entire project, so it could NEVER succeed; every attempt
// returned BTSTACK_MEMORY_ALLOC_FAILED (0x56) regardless of anything else
// being right. One keyboard at a time, same as MAX_NR_HIDS_HOSTS below.
#define MAX_NR_HID_HOST_CONNECTIONS 1

// LE GATT client + HOGP host pools -- one keyboard at a time, same as the
// classic side above.
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HIDS_HOSTS 1

// We never act as a GATT server (nothing on this board ever needs to be
// discoverable/readable from the far side), but att_db_util.c -- part of
// pico_btstack_ble's source list -- is compiled in regardless and needs
// this defined even at 0.
#define MAX_ATT_DB_SIZE 0

// Remember a handful of previously-paired keyboards so reconnecting doesn't
// require re-pairing every time. NVM_NUM_DEVICE_DB_ENTRIES covers LE bonds
// (le_device_db_tlv.c), NVM_NUM_LINK_KEYS covers classic link keys.
#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define NVM_NUM_LINK_KEYS 4
