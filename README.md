# usb-to-coco

Firmware that lets a modern **USB or Bluetooth keyboard** type into a real
**TRS-80 Color Computer 3**. A Raspberry Pi Pico 2 W sits on the CoCo's keyboard
connector and emulates the original 7×8 key matrix, so the CoCo sees an ordinary
keyboard and needs no software changes.

It runs on the [CoCo Pico Keyboard Interface](https://github.com/wdathing/coco_keeb)
PCB. That page covers the hardware and the board's other firmware load (QMK, which
works in the opposite direction).

## Features

- **USB keyboards** via TinyUSB host on the Pico's native USB port.
- **Bluetooth keyboards**, both Classic (BR/EDR HID) and BLE (HID-over-GATT), via
  BTstack on the Pico 2 W's CYW43439 radio.
- **Pairing button** (SW1, GPIO7): opens a 60-second window that scans for and
  connects to a nearby keyboard. The last keyboard is remembered in flash and
  reconnects automatically on boot.
- **Keycap-accurate symbols**: a few keys are remapped so what's printed on the
  modern keycap is what appears on the CoCo screen. For example, `Shift+2` gives `@`,
  `Shift+;` gives `:`, and `'`/`"` type quotes.
- **Status LEDs**: D1 (GPIO19) lights while the CoCo is scanning the keyboard, and
  D2 (GPIO18) lights while any key is held.

## Key mapping

| Modern key | CoCo key |
|---|---|
| `Esc` or `Pause` | BREAK |
| `Home` | CLEAR |
| `Backspace` | ← (left arrow) |
| `` ` `` or `Shift+2` | @ |
| `Alt`, `Ctrl`, `F1`, `F2` | ALT, CTRL, F1, F2 (CoCo 3) |

Letters, digits, arrows, `Space`, `Enter`, `, - . /` map directly.

## How it works

The CoCo drives the matrix **columns** (PIA port B) low one at a time and reads
the **rows** (PIA port A). The firmware treats the columns as inputs with an edge
interrupt. On every column change it re-computes the rows, pulling a row low only
while a held key's column is selected and otherwise leaving it high-impedance. This
is how the real diode-less keyboard behaves electrically.

| Signal | Pico GPIO |
|---|---|
| Rows 0–6 (PA0–PA6) | GP0 – GP6 |
| Columns 0–7 (PB0–PB7) | GP8 – GP15 |

## Building

Uses Pico SDK 2.3.0. Open the folder with the Raspberry Pi Pico VS Code
extension, or build from the command line:

```bash
cmake -B build -G Ninja             # default board: pico2_w
ninja -C build                      # -> build/usb-to-coco.uf2
```

Hold **BOOTSEL** while plugging in the Pico and copy `usb-to-coco.uf2` onto the
drive that appears.

### Debug build

GP0/GP1 are both CoCo matrix rows and the Pico's UART0 TX/RX, so serial logging and
driving the CoCo can't happen at the same time. For bring-up without a CoCo
attached, build with:

```bash
cmake -B build-debug -G Ninja -DDEBUG_KEY_LOG=ON
```

This build leaves the matrix pins alone and logs each decoded key (row/col) plus
Bluetooth status over UART at 115200 baud.

## License

Copyright (c) 2026 William Athing. Released under the [MIT License](LICENSE): use,
modify, and redistribute freely, including commercially, as long as the copyright
notice is kept.

`pico_sdk_import.cmake` is copied from the Raspberry Pi Pico SDK and stays under
that SDK's BSD-3-Clause license.

**Third-party components in the compiled firmware.** The `.uf2` links against the
Pico SDK (BSD-3-Clause), TinyUSB (MIT), BTstack, and the CYW43 wireless driver.
BTstack and the CYW43 driver are not MIT: Raspberry Pi licenses them for use on
Raspberry Pi RP-series microcontrollers, which includes the Pico 2 W this firmware
targets. Check their license files in the Pico SDK before reusing them on other
hardware.
