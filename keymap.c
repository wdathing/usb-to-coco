// Copyright (c) 2026 William Athing
// SPDX-License-Identifier: MIT

#include "keymap.h"

#include <stddef.h>

#include "class/hid/hid.h"

// CoCo 3 keyboard array (Tandy Service Manual Figure 5-9), row = PA0-PA6,
// col = PB0-PB7:
//
//   Row0: @ A B C D E F G          Row4: 0 1 2 3 4 5 6 7
//   Row1: H I J K L M N O          Row5: 8 9 : ; , - . /
//   Row2: P Q R S T U V W          Row6: ENTER CLEAR BREAK ALT CTRL F1 F2 SHIFT
//   Row3: X Y Z UP DOWN LEFT RIGHT SPACE
//
// Mapping is by physical key identity, not by shifted glyph -- the CoCo's
// own hardware combines a key with its own SHIFT line, so e.g. HID '1' maps
// to CoCo row4/col1 whether or not shift is held; SHIFT itself is a
// separate CoCo key (row6/col7) driven from the HID modifier byte.
//
// Keys with no direct modern equivalent:
//   BREAK -> HID_KEY_PAUSE and HID_KEY_ESCAPE (either one)
//   @     -> HID_KEY_GRAVE (` / ~, top-left key on a US keyboard)
//   CLEAR -> HID_KEY_HOME
//
// Shift-dependent overrides (see SHIFT_OVERRIDES below): a few USB keys need
// a CoCo target -- and sometimes a CoCo SHIFT requirement -- that doesn't
// just mirror the USB shift state, so the symbol that ends up on the CoCo
// screen matches the label printed on the USB keycap:
//
//   - CoCo has separate physical ';' and ':' keys (each with its own
//     shifted symbol: ';' shifts to '+', ':' shifts to '*'), where a modern
//     keyboard has one ;/: key. Plain ';' -> CoCo ';'; shifted (':') ->
//     CoCo ':' (neither needs CoCo SHIFT held).
//   - CoCo has a dedicated '@' key with no modern equivalent, and shift+2
//     is '@' on a US keycap, so shift+2 targets CoCo's '@' key directly
//     (no CoCo SHIFT needed) instead of CoCo's '2' key, which would
//     otherwise land on '"' under plain shift.
//   - CoCo has no dedicated quote keys at all -- ' and " both live on the
//     shifted layer of digit keys (shift+7 = ', shift+2 = "). The USB
//     apostrophe key's own unshifted/shifted glyphs are exactly '/" already,
//     so it maps straight onto that: plain ' -> CoCo SHIFT+7, shifted " ->
//     CoCo SHIFT+2. This is the frequent case (LOAD"..." etc.) and it now
//     reads exactly like typing a quote on a normal keyboard.
typedef struct {
    uint8_t hid_keycode;
    uint8_t row;
    uint8_t col;
} keymap_entry_t;

static const keymap_entry_t KEYMAP[] = {
    // Row0: @ A B C D E F G
    {HID_KEY_GRAVE, 0, 0}, // secondary way to get '@'; see shift+2 override below
    {HID_KEY_A, 0, 1},
    {HID_KEY_B, 0, 2},
    {HID_KEY_C, 0, 3},
    {HID_KEY_D, 0, 4},
    {HID_KEY_E, 0, 5},
    {HID_KEY_F, 0, 6},
    {HID_KEY_G, 0, 7},

    // Row1: H I J K L M N O
    {HID_KEY_H, 1, 0},
    {HID_KEY_I, 1, 1},
    {HID_KEY_J, 1, 2},
    {HID_KEY_K, 1, 3},
    {HID_KEY_L, 1, 4},
    {HID_KEY_M, 1, 5},
    {HID_KEY_N, 1, 6},
    {HID_KEY_O, 1, 7},

    // Row2: P Q R S T U V W
    {HID_KEY_P, 2, 0},
    {HID_KEY_Q, 2, 1},
    {HID_KEY_R, 2, 2},
    {HID_KEY_S, 2, 3},
    {HID_KEY_T, 2, 4},
    {HID_KEY_U, 2, 5},
    {HID_KEY_V, 2, 6},
    {HID_KEY_W, 2, 7},

    // Row3: X Y Z UP DOWN LEFT RIGHT SPACE
    {HID_KEY_X, 3, 0},
    {HID_KEY_Y, 3, 1},
    {HID_KEY_Z, 3, 2},
    {HID_KEY_ARROW_UP, 3, 3},
    {HID_KEY_ARROW_DOWN, 3, 4},
    {HID_KEY_ARROW_LEFT, 3, 5},
    {HID_KEY_ARROW_RIGHT, 3, 6},
    {HID_KEY_BACKSPACE, 3, 5}, // LEFT (also) -- CoCo's traditional backspace-equivalent, erases left
    {HID_KEY_SPACE, 3, 7},

    // Row4: 0 1 2 3 4 5 6 7  ('2' is a shift-override, see below -- no
    // plain entry for it here)
    {HID_KEY_0, 4, 0},
    {HID_KEY_1, 4, 1},
    {HID_KEY_3, 4, 3},
    {HID_KEY_4, 4, 4},
    {HID_KEY_5, 4, 5},
    {HID_KEY_6, 4, 6},
    {HID_KEY_7, 4, 7},

    // Row5: 8 9 : ; , - . /  (';' is a shift-override, see below -- no
    // plain entry for it here)
    {HID_KEY_8, 5, 0},
    {HID_KEY_9, 5, 1},
    {HID_KEY_COMMA, 5, 4},
    {HID_KEY_MINUS, 5, 5},
    {HID_KEY_PERIOD, 5, 6},
    {HID_KEY_SLASH, 5, 7},

    // Row6: ENTER CLEAR BREAK ALT CTRL F1 F2 SHIFT
    {HID_KEY_ENTER, 6, 0},
    {HID_KEY_HOME, 6, 1}, // CLEAR
    {HID_KEY_PAUSE, 6, 2}, // BREAK
    {HID_KEY_ESCAPE, 6, 2}, // BREAK (also)
    {HID_KEY_ALT_LEFT, 6, 3},
    {HID_KEY_ALT_RIGHT, 6, 3},
    {HID_KEY_CONTROL_LEFT, 6, 4},
    {HID_KEY_CONTROL_RIGHT, 6, 4},
    {HID_KEY_F1, 6, 5},
    {HID_KEY_F2, 6, 6},
    {HID_KEY_SHIFT_LEFT, 6, 7},
    {HID_KEY_SHIFT_RIGHT, 6, 7},
};

#define KEYMAP_LEN (sizeof(KEYMAP) / sizeof(KEYMAP[0]))

typedef struct {
    uint8_t hid_keycode;
    uint8_t unshifted_row, unshifted_col;
    bool unshifted_needs_coco_shift;
    uint8_t shifted_row, shifted_col;
    bool shifted_needs_coco_shift;
} shift_override_entry_t;

static const shift_override_entry_t SHIFT_OVERRIDES[] = {
    // plain '2' -> CoCo '2'; shift+2 ("@" on a US keycap) -> CoCo '@' (no
    // CoCo SHIFT needed either way)
    {HID_KEY_2, 4, 2, false, 0, 0, false},
    // plain ';' -> CoCo ';'; shift+; (":" on a US keycap) -> CoCo ':' (no
    // CoCo SHIFT needed either way)
    {HID_KEY_SEMICOLON, 5, 3, false, 5, 2, false},
    // plain ' -> CoCo SHIFT+7 ('); shifted " -> CoCo SHIFT+2 (") -- CoCo has
    // no dedicated quote keys, both live on the digit row's shift layer
    {HID_KEY_APOSTROPHE, 4, 7, true, 4, 2, true},
};

#define SHIFT_OVERRIDES_LEN (sizeof(SHIFT_OVERRIDES) / sizeof(SHIFT_OVERRIDES[0]))

static const shift_override_entry_t *find_override(uint8_t hid_keycode) {
    for (size_t i = 0; i < SHIFT_OVERRIDES_LEN; i++) {
        if (SHIFT_OVERRIDES[i].hid_keycode == hid_keycode) {
            return &SHIFT_OVERRIDES[i];
        }
    }
    return NULL;
}

bool keymap_resolve(uint8_t hid_keycode, bool shift_held, uint8_t *row, uint8_t *col, bool *needs_coco_shift) {
    const shift_override_entry_t *ov = find_override(hid_keycode);
    if (ov) {
        if (shift_held) {
            *row = ov->shifted_row;
            *col = ov->shifted_col;
            *needs_coco_shift = ov->shifted_needs_coco_shift;
        } else {
            *row = ov->unshifted_row;
            *col = ov->unshifted_col;
            *needs_coco_shift = ov->unshifted_needs_coco_shift;
        }
        return true;
    }

    for (size_t i = 0; i < KEYMAP_LEN; i++) {
        if (KEYMAP[i].hid_keycode == hid_keycode) {
            *row = KEYMAP[i].row;
            *col = KEYMAP[i].col;
            *needs_coco_shift = shift_held; // plain keys: shift just mirrors through
            return true;
        }
    }
    return false;
}
