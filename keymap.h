#pragma once

#include <stdbool.h>
#include <stdint.h>

// Resolves a HID keycode to the CoCo matrix cell that should be pressed to
// reproduce it, given the current USB shift state, plus whether the CoCo's
// own SHIFT key (row6/col7) needs to be held at the same time to get that
// result.
//
// For most keys *needs_coco_shift just mirrors shift_held (shift forwards
// straight through, unaffected by this key). A few keys (see keymap.c)
// instead pick a different CoCo cell and/or a fixed shift requirement when
// shift is held, so the symbol that ends up on the CoCo screen matches the
// label printed on the USB keycap rather than whatever the CoCo's own
// shift-layer happens to produce for the "same" physical position.
//
// Returns false if the key has no CoCo equivalent.
bool keymap_resolve(uint8_t hid_keycode, bool shift_held, uint8_t *row, uint8_t *col, bool *needs_coco_shift);
