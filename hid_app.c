// Copyright (c) 2026 William Athing
// SPDX-License-Identifier: MIT

#include "tusb.h"

#include "keymap.h"
#include "matrix.h"

// Tracks, for each currently-held HID keycode, which CoCo matrix cell was
// actually pressed for it. A plain slot->row/col pairing isn't enough on
// its own because a shift-override key's target can change while the
// physical key stays down (shift toggled mid-hold) -- this lets us notice
// that and re-target instead of just diffing raw keycode presence.
//
// This scaffold assumes a single USB keyboard; a hub with several keyboards
// would need one of these per (dev_addr, instance) instead of one global copy.
typedef struct {
    uint8_t hid_keycode; // 0 = empty slot
    uint8_t row, col;
} active_key_t;

static active_key_t active_keys[6];
static uint8_t prev_modifier;
static bool coco_shift_forwarded;
static bool keyboard_mounted = false;

static active_key_t *find_active(uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (active_keys[i].hid_keycode == keycode) {
            return &active_keys[i];
        }
    }
    return NULL;
}

static bool report_has_keycode(const hid_keyboard_report_t *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

// Ctrl/Alt are simple independent lines -- always forwarded straight to
// their CoCo key, unaffected by the shift-override logic below.
static void apply_modifier(uint8_t prev, uint8_t cur, uint8_t bit, uint8_t hid_keycode) {
    bool was = (prev & bit) != 0;
    bool is = (cur & bit) != 0;
    if (was == is) {
        return;
    }
    uint8_t row, col;
    bool needs_coco_shift;
    if (keymap_resolve(hid_keycode, false, &row, &col, &needs_coco_shift)) {
        coco_matrix_set_key(row, col, is);
    }
}

static void reset_state(void) {
    for (int i = 0; i < 6; i++) {
        active_keys[i].hid_keycode = 0;
    }
    prev_modifier = 0;
    coco_shift_forwarded = false;
}

static void process_kbd_report(const hid_keyboard_report_t *report) {
    bool shift_held = (report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    apply_modifier(prev_modifier, report->modifier, KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_CONTROL_LEFT);
    apply_modifier(prev_modifier, report->modifier, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_CONTROL_RIGHT);
    apply_modifier(prev_modifier, report->modifier, KEYBOARD_MODIFIER_LEFTALT, HID_KEY_ALT_LEFT);
    apply_modifier(prev_modifier, report->modifier, KEYBOARD_MODIFIER_RIGHTALT, HID_KEY_ALT_RIGHT);
    prev_modifier = report->modifier;

    // Releases: previously-active keys no longer present in this report.
    for (int i = 0; i < 6; i++) {
        if (active_keys[i].hid_keycode != 0 && !report_has_keycode(report, active_keys[i].hid_keycode)) {
            coco_matrix_set_key(active_keys[i].row, active_keys[i].col, false);
            active_keys[i].hid_keycode = 0;
        }
    }

    // Presses / re-targets: keys in this report, resolved against the
    // current shift state. A held key whose target changes (shift toggled
    // mid-hold) gets released from its old cell and pressed on its new one.
    // Each key also reports whether *it* needs CoCo SHIFT held to produce
    // the right symbol (e.g. shift+2 -> '@' does not; plain ' -> CoCo
    // SHIFT+7 does, even though USB shift isn't held for that one) -- CoCo
    // SHIFT itself is asserted afterwards from the OR of those answers.
    bool any_key_held = false;
    bool want_coco_shift_from_keys = false;
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode == 0) {
            continue;
        }
        any_key_held = true;

        uint8_t row, col;
        bool needs_coco_shift;
        if (!keymap_resolve(keycode, shift_held, &row, &col, &needs_coco_shift)) {
            continue;
        }
        if (needs_coco_shift) {
            want_coco_shift_from_keys = true;
        }

        active_key_t *slot = find_active(keycode);
        if (slot) {
            if (slot->row != row || slot->col != col) {
                coco_matrix_set_key(slot->row, slot->col, false);
                coco_matrix_set_key(row, col, true);
                slot->row = row;
                slot->col = col;
            }
        } else {
            slot = find_active(0);
            if (slot) {
                slot->hid_keycode = keycode;
                slot->row = row;
                slot->col = col;
                coco_matrix_set_key(row, col, true);
            }
        }
    }

    // SHIFT is forwarded last, from the OR of what each held key needs. If
    // nothing is held at all, bare shift still forwards on its own.
    bool want_coco_shift = any_key_held ? want_coco_shift_from_keys : shift_held;
    if (want_coco_shift != coco_shift_forwarded) {
        uint8_t row, col;
        bool unused_needs_shift;
        if (keymap_resolve(HID_KEY_SHIFT_LEFT, false, &row, &col, &unused_needs_shift)) {
            coco_matrix_set_key(row, col, want_coco_shift);
        }
        coco_shift_forwarded = want_coco_shift;
    }
}

// Invoked when a HID interface is mounted. TinyUSB activates boot protocol
// automatically on keyboard/mouse interfaces, so a plain hid_keyboard_report_t
// is all we need to parse -- no report-descriptor parsing required.
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_mounted = true;
        reset_state();
        tuh_hid_receive_report(dev_addr, instance);
    }
}

// Invoked when a HID interface is unmounted -- release everything so
// nothing is left stuck down on the CoCo side.
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    (void)instance;

    if (keyboard_mounted) {
        keyboard_mounted = false;
        coco_matrix_clear_all();
        reset_state();
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)len;

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        process_kbd_report((hid_keyboard_report_t const *)report);
    }

    // Keep the pipe primed for the next report.
    tuh_hid_receive_report(dev_addr, instance);
}
