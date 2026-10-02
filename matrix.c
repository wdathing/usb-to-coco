// Copyright (c) 2026 William Athing
// SPDX-License-Identifier: MIT

#include "matrix.h"

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#ifdef DEBUG_KEY_LOG
#include <stdio.h>
#endif

// GPIO assignment, verified pin-for-pin against J1 on the Coco_pico_keyb PCB.
static const uint8_t row_gpio[COCO_ROWS] = {0, 1, 2, 3, 4, 5, 6};
static const uint8_t col_gpio[COCO_COLS] = {8, 9, 10, 11, 12, 13, 14, 15};

// D1 lights while the CoCo is actively scanning the keyboard columns; D2
// lights while at least one USB key is held down. Traced from the PCB net
// table: D1 cathode -> R1 -> GPIO19, D2 cathode -> R2 -> GPIO18, both anodes
// returned to a common GND net -- so both are active-high (drive high to
// light). If yours light backwards, the two gpio_put calls below just need
// their sense inverted.
#define LED_SCAN_PIN 19
#define LED_KEYDOWN_PIN 18

// How long D1 stays lit after the last column edge, so a normal scan (many
// edges per second whenever the CoCo is running) reads as steady-on instead
// of a flicker too fast to see.
#define SCAN_LED_HOLD_MS 80

// keys_held[row] bit `col` set => that key is currently held (from USB HID).
static volatile uint8_t keys_held[COCO_ROWS];
static volatile uint32_t last_scan_activity_ms;
static volatile bool scan_led_on;

// Row pins are driven open-drain style: IN (Hi-Z) when released so the
// CoCo's own pull-ups win, OUT-low only while a held key's column is
// actively selected -- this mirrors how the real diode-less matrix behaves.
static inline void set_row_output(uint8_t row, bool pull_low) {
    if (pull_low) {
        gpio_put(row_gpio[row], 0);
        gpio_set_dir(row_gpio[row], GPIO_OUT);
    } else {
        gpio_set_dir(row_gpio[row], GPIO_IN);
    }
}

// Re-derives every row output from the current column-select state and the
// held-key bitmap. Called on every column edge, so this needs to stay cheap.
// Compiled out entirely in DEBUG_KEY_LOG builds -- see coco_matrix_init().
#ifndef DEBUG_KEY_LOG
static void recompute_rows(void) {
    uint32_t all = gpio_get_all();
    uint8_t col_active = 0; // bit c set => CoCo is currently driving column c low
    for (uint8_t c = 0; c < COCO_COLS; c++) {
        if ((all & (1u << col_gpio[c])) == 0) {
            col_active |= (uint8_t)(1u << c);
        }
    }
    for (uint8_t r = 0; r < COCO_ROWS; r++) {
        set_row_output(r, (keys_held[r] & col_active) != 0);
    }
}
#endif

#ifndef DEBUG_KEY_LOG
static void column_irq_handler(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    recompute_rows();

    last_scan_activity_ms = to_ms_since_boot(get_absolute_time());
    if (!scan_led_on) {
        scan_led_on = true;
        gpio_put(LED_SCAN_PIN, 1);
    }
}
#endif

// Turns D2 on iff at least one key is currently held anywhere in the matrix.
static void update_keydown_led(void) {
    uint8_t any = 0;
    for (uint8_t r = 0; r < COCO_ROWS; r++) {
        any |= keys_held[r];
    }
    gpio_put(LED_KEYDOWN_PIN, any != 0);
}

void coco_matrix_init(void) {
#ifdef DEBUG_KEY_LOG
    // GPIO0/1 (CoCo row1/row2) are also this board's default UART TX/RX --
    // board_init() claims them for UART first, and this function normally
    // reclaims them for the matrix right after, which is why UART printf
    // has never actually worked on real hardware. In this build we leave
    // row/col GPIOs completely untouched so UART keeps working, and log
    // decoded key events instead of driving real matrix pins -- for
    // bring-up/testing the USB+BT+keymap pipeline with no CoCo attached.
    printf("matrix: DEBUG_KEY_LOG build -- row/col GPIOs NOT driven, logging key events instead\n");
#else
    for (uint8_t r = 0; r < COCO_ROWS; r++) {
        gpio_init(row_gpio[r]);
        gpio_set_dir(row_gpio[r], GPIO_IN);
        gpio_disable_pulls(row_gpio[r]);
    }
    for (uint8_t c = 0; c < COCO_COLS; c++) {
        gpio_init(col_gpio[c]);
        gpio_set_dir(col_gpio[c], GPIO_IN);
        // Weak pull-up so an unconnected/unpowered CoCo doesn't leave the
        // input floating and firing spurious interrupts; a live CoCo's PIA
        // easily overdrives this when it selects a column.
        gpio_pull_up(col_gpio[c]);
        gpio_set_irq_enabled_with_callback(
            col_gpio[c], GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &column_irq_handler);
    }
#endif

    gpio_init(LED_SCAN_PIN);
    gpio_set_dir(LED_SCAN_PIN, GPIO_OUT);
    gpio_put(LED_SCAN_PIN, 0);

    gpio_init(LED_KEYDOWN_PIN);
    gpio_set_dir(LED_KEYDOWN_PIN, GPIO_OUT);
    gpio_put(LED_KEYDOWN_PIN, 0);
}

void coco_matrix_task(void) {
    if (scan_led_on && (to_ms_since_boot(get_absolute_time()) - last_scan_activity_ms) > SCAN_LED_HOLD_MS) {
        scan_led_on = false;
        gpio_put(LED_SCAN_PIN, 0);
    }
}

void coco_matrix_set_key(uint8_t row, uint8_t col, bool pressed) {
    if (row >= COCO_ROWS || col >= COCO_COLS) {
        return;
    }
#ifdef DEBUG_KEY_LOG
    printf("key %s: row=%u col=%u\n", pressed ? "DOWN" : "UP ", row, col);
#endif
    uint32_t irq_state = save_and_disable_interrupts();
    if (pressed) {
        keys_held[row] |= (uint8_t)(1u << col);
    } else {
        keys_held[row] &= (uint8_t)~(1u << col);
    }
#ifndef DEBUG_KEY_LOG
    recompute_rows();
#endif
    update_keydown_led();
    restore_interrupts(irq_state);
}

void coco_matrix_clear_all(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    for (uint8_t r = 0; r < COCO_ROWS; r++) {
        keys_held[r] = 0;
    }
#ifndef DEBUG_KEY_LOG
    recompute_rows();
#endif
    update_keydown_led();
    restore_interrupts(irq_state);
}
