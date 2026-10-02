// Copyright (c) 2026 William Athing
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// CoCo 3 keyboard matrix: 7 rows (PIA port A, sensed by the CoCo) x 8 columns
// (PIA port B, driven by the CoCo). Verified against Coco_pico_keyb PCB J1 net
// table and Tandy CoCo 3 Service Manual Figure 5-9 ("Color Computer 3
// Keyboard Array") -- see memory/project_coco_keyboard.md.
#define COCO_ROWS 7
#define COCO_COLS 8

// Configures the row/column GPIOs, the D1/D2 status LEDs, and installs the
// column-change interrupt that keeps the row outputs in sync with whatever
// the CoCo is scanning.
void coco_matrix_init(void);

// Call regularly from the main loop; lets the D1 (column scan) LED time out
// when the CoCo stops scanning instead of only reflecting an instant sample.
void coco_matrix_task(void);

// Marks the key at (row, col) as pressed or released and immediately
// updates the physical row outputs. Safe to call from any context.
void coco_matrix_set_key(uint8_t row, uint8_t col, bool pressed);

// Releases every key -- used when the USB keyboard disconnects so nothing
// is left stuck down on the CoCo side.
void coco_matrix_clear_all(void);
