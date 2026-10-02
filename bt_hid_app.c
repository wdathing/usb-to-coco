// Copyright (c) 2026 William Athing
// SPDX-License-Identifier: MIT

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "pico/stdlib.h"

#include "bt_hid_app.h"
#include "keymap.h"
#include "matrix.h"

// GPIO7 is the only fully-free pin on the current board rev, wired to a
// pairing button (other leg to GND) -- see project schematic notes.
#define PAIRING_BUTTON_PIN 7
#define PAIRING_WINDOW_MS (60 * 1000)
// Bumped from 30ms after real-hardware testing showed a single physical
// press occasionally producing multiple debounced transitions -- likely a
// noisy/marginal GPIO7 connection (worth checking SW1's solder joints), but
// a longer debounce window is a cheap mitigation regardless.
#define BUTTON_DEBOUNCE_MS 75

// Most classic BT HID keyboards (this one included) wait to be found and
// connected TO during pairing -- they don't initiate the connection
// themselves. So the pairing window has to actively scan (GAP inquiry) and
// connect to what it finds, not just sit there discoverable waiting.
#define INQUIRY_DURATION_UNITS 15 // 15 * 1.28s ~= 19s per scan cycle

// Class of Device: bits 8-12 are the 5-bit Major Device Class. 0x05 =
// "Peripheral" (keyboards, mice, joysticks, etc).
#define COD_MAJOR_DEVICE_CLASS(cod) (((cod) >> 8) & 0x1Fu)
#define COD_MAJOR_PERIPHERAL 0x05u

#define MAX_SDP_ATTRIBUTE_VALUE_SIZE 300
#define MAX_ACTIVE_KEYS 12

// USB HID Usage Tables, Consumer page (0x0C): "AC Back". No named constant
// for this in btstack's headers, just the raw usage ID.
#define CONSUMER_USAGE_AC_BACK 0x0224

// TinyUSB's HID_KEY_ESCAPE (class/hid/hid.h) can't be included here --
// its hid.h conflicts with btstack_hid.h (both define hid_report_type_t
// and friends). 0x29 is the same standard USB HID Keyboard-page Escape
// usage keymap.c's KEYMAP table matches against.
#define KEYBOARD_USAGE_ESCAPE 0x29

static uint8_t hid_descriptor_storage[MAX_SDP_ATTRIBUTE_VALUE_SIZE];
static btstack_packet_callback_registration_t hci_event_callback_registration;

static uint16_t hid_host_cid;
static bool hid_host_descriptor_available;
static const hid_protocol_mode_t hid_host_report_mode = HID_PROTOCOL_MODE_REPORT;

// LE HID-over-GATT (HOGP) Host state -- most current-generation BT
// keyboards (this board was bring-up tested against an 8BitDo Retro
// Mechanical Keyboard) only pair over BLE, not classic BR/EDR, so both
// stacks run side by side and share the same pairing-window/connecting
// state machine below.
static uint8_t ble_hid_descriptor_storage[MAX_SDP_ATTRIBUTE_VALUE_SIZE];
static btstack_packet_callback_registration_t sm_event_callback_registration;
static btstack_timer_source_t ble_connect_timer;
#define BLE_CONNECT_TIMEOUT_MS 10000

// Alternates the pairing window between a classic inquiry phase and an LE
// scan phase -- see classic_scan_phase()/le_scan_phase() below.
static btstack_timer_source_t scan_phase_timer;

static hci_con_handle_t ble_connection_handle = HCI_CON_HANDLE_INVALID;
static uint16_t ble_hids_cid;
static bd_addr_t ble_remote_addr;
static bd_addr_type_t ble_remote_addr_type;

// True whenever a keyboard (classic or LE) is fully connected and ready --
// single source of truth for the auto-reconnect logic below, rather than
// inferring from hid_host_cid/ble_hids_cid directly.
static bool keyboard_connected;

// Persists which keyboard we last successfully connected to (independent of
// BTstack's own link-key/LE-bonding storage, which is already flash-backed
// automatically via cyw43_arch_init() -- this is just "which address to
// try", not authentication material) so we can auto-reconnect on boot
// without needing the pairing button. See try_auto_reconnect() below.
static const btstack_tlv_t *tlv_impl;
static void *tlv_context;

#define TLV_TAG_LAST_KEYBOARD ((((uint32_t)'L') << 24) | (((uint32_t)'K') << 16) | (((uint32_t)'B') << 8) | 'D')

typedef struct {
    bd_addr_t addr;
    uint8_t is_le; // 0 = classic BR/EDR, 1 = LE
    bd_addr_type_t le_addr_type; // only meaningful if is_le
} last_keyboard_t;

static void remember_last_keyboard(const bd_addr_t addr, bool is_le, bd_addr_type_t le_addr_type) {
    if (!tlv_impl) return;
    last_keyboard_t rec;
    memcpy(rec.addr, addr, sizeof(bd_addr_t));
    rec.is_le = is_le ? 1 : 0;
    rec.le_addr_type = le_addr_type;
    tlv_impl->store_tag(tlv_context, TLV_TAG_LAST_KEYBOARD, (const uint8_t *)&rec, sizeof(rec));
}

static bool load_last_keyboard(last_keyboard_t *out) {
    if (!tlv_impl) return false;
    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_LAST_KEYBOARD, (uint8_t *)out, sizeof(*out));
    return len == (int)sizeof(*out);
}

// Tracks, for each currently-held BT HID usage, which CoCo matrix cell was
// actually pressed for it -- same reasoning as hid_app.c's active_keys[]:
// a shift-override key's target can change while the key stays down.
typedef struct {
    uint16_t usage; // 0 = empty slot
    uint8_t row, col;
} active_key_t;

static active_key_t active_keys[MAX_ACTIVE_KEYS];
static bool coco_shift_forwarded;

// Pairing window state, driven by the physical button.
static bool pairing_active;
static uint32_t pairing_deadline_ms;
// True from the moment we call hid_host_connect() until we know whether it
// succeeded -- avoids trying to connect to a second candidate found while
// the first attempt is still in flight.
static bool connecting;
// Set once BTSTACK_EVENT_STATE reports HCI_STATE_WORKING. gap_inquiry_start()
// before this point silently does nothing useful.
static bool bt_ready;

static void reset_key_state(void) {
    memset(active_keys, 0, sizeof(active_keys));
    coco_shift_forwarded = false;
    coco_matrix_clear_all();
}

// How long to wait before retrying a gap_inquiry_start() that the controller
// rejected with COMMAND_DISALLOWED (busy finishing up something else --
// typically the tail end of the previous inquiry/connection handling).
#define INQUIRY_RETRY_MS 300

static btstack_timer_source_t inquiry_retry_timer;

static void start_inquiry(void) {
    uint8_t status = gap_inquiry_start(INQUIRY_DURATION_UNITS);
    if (status != ERROR_CODE_SUCCESS) {
        printf("BT: gap_inquiry_start failed, status 0x%02x -- retrying in %d ms\n", status, INQUIRY_RETRY_MS);
        btstack_run_loop_set_timer(&inquiry_retry_timer, INQUIRY_RETRY_MS);
        btstack_run_loop_add_timer(&inquiry_retry_timer);
    }
}

// Runs in BTstack's own context (timers fire from there), same as
// start_inquiry() itself -- safe to call gap_inquiry_start() directly.
static void inquiry_retry_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (pairing_active && !connecting) {
        start_inquiry();
    }
}

// Passive LE scan, 100% duty cycle (interval == window) -- we only care
// about catching advertisements quickly during the pairing window, power
// use doesn't matter here.
static void start_ble_scan(void) {
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void stop_all_scanning(void) {
    gap_inquiry_stop();
    gap_stop_scan();
    btstack_run_loop_remove_timer(&scan_phase_timer);
}

// Classic inquiry and LE scanning are deliberately never run at the same
// time -- real hardware testing found that running both concurrently on the
// CYW43439 silently killed classic GAP_EVENT_INQUIRY_RESULT delivery
// entirely (inquiry cycles kept completing on schedule, but zero results
// ever arrived, not even for previously-reliably-found devices). Instead
// the pairing window alternates: one classic inquiry cycle (~19s, ends
// naturally via GAP_EVENT_INQUIRY_COMPLETE below), then one LE scan phase
// of about the same length (ended by scan_phase_timer), repeating for as
// long as the window is open.
#define LE_SCAN_PHASE_MS 19000

static void classic_scan_phase(void) {
    start_inquiry();
}

static void le_scan_phase_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    gap_stop_scan();
    if (pairing_active && !connecting) {
        classic_scan_phase();
    }
}

static void le_scan_phase(void) {
    start_ble_scan();
    btstack_run_loop_set_timer_handler(&scan_phase_timer, &le_scan_phase_timeout);
    btstack_run_loop_set_timer(&scan_phase_timer, LE_SCAN_PHASE_MS);
    btstack_run_loop_add_timer(&scan_phase_timer);
}

// Called after any dead end during pairing (a candidate that failed to
// connect/pair/bring up its HID service) to resume looking for a different
// candidate for the rest of the window -- always restarts from the classic
// phase, same as opening a fresh window.
static void resume_scanning(void) {
    if (!pairing_active) return;
    classic_scan_phase();
}

static void ble_connect_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    printf("BT: LE connection attempt timed out\n");
    gap_connect_cancel();
    connecting = false;
    resume_scanning();
}

static void ble_connect_to(const bd_addr_t addr, bd_addr_type_t addr_type) {
    btstack_run_loop_set_timer_handler(&ble_connect_timer, &ble_connect_timeout);
    btstack_run_loop_set_timer(&ble_connect_timer, BLE_CONNECT_TIMEOUT_MS);
    btstack_run_loop_add_timer(&ble_connect_timer);
    gap_connect(addr, addr_type);
}

// Retries in the background, indefinitely, whenever we're not already
// connected/mid-attempt/mid-manual-pairing -- so turning the keyboard on
// (or bringing it back in range) after the Pico boots still ends up
// connected with no button press. A direct connect to a known address
// (hid_host_connect()/gap_connect()) needs no scanning first, so this can't
// collide with classic-vs-LE-scan-concurrency the way pairing-window
// scanning can.
#define AUTO_RECONNECT_RETRY_MS 5000
static btstack_timer_source_t auto_reconnect_timer;

static void schedule_auto_reconnect_retry(void) {
    btstack_run_loop_set_timer(&auto_reconnect_timer, AUTO_RECONNECT_RETRY_MS);
    btstack_run_loop_add_timer(&auto_reconnect_timer);
}

static void try_auto_reconnect(void) {
    if (!bt_ready || keyboard_connected || connecting || pairing_active) {
        schedule_auto_reconnect_retry();
        return;
    }
    last_keyboard_t rec;
    if (!load_last_keyboard(&rec)) {
        schedule_auto_reconnect_retry();
        return;
    }
    connecting = true;
    if (rec.is_le) {
        printf("BT: auto-reconnecting to remembered keyboard %s (LE)...\n", bd_addr_to_str(rec.addr));
        ble_connect_to(rec.addr, rec.le_addr_type);
    } else {
        printf("BT: auto-reconnecting to remembered keyboard %s...\n", bd_addr_to_str(rec.addr));
        uint8_t status = hid_host_connect(rec.addr, hid_host_report_mode, &hid_host_cid);
        if (status != ERROR_CODE_SUCCESS) {
            printf("BT: auto-reconnect failed, status 0x%02x\n", status);
            connecting = false;
        }
    }
    schedule_auto_reconnect_retry();
}

static void auto_reconnect_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    try_auto_reconnect();
}

// The two functions below call BTstack APIs, so they must only ever run
// from within BTstack's own execution context (the cyw43 threadsafe
// background async_context) -- never called directly; always scheduled via
// btstack_run_loop_execute_on_main_thread() from bt_hid_task() below. Calling
// BTstack functions directly from the plain main-loop thread is a data race
// with BTstack's own background processing: commands can appear to succeed
// (no error code, scan "completes") while event delivery silently breaks --
// exactly the symptom that led here.
static void start_pairing_window(void *context) {
    (void)context;
    if (!bt_ready) {
        printf("BT: pairing button pressed but the stack isn't up yet (no \"BT: ready\" seen) -- ignoring.\n");
        return;
    }
    pairing_active = true;
    connecting = false;
    pairing_deadline_ms = to_ms_since_boot(get_absolute_time()) + PAIRING_WINDOW_MS;
    gap_discoverable_control(1);
    // Real bug, found on real hardware (2026-08-14): a re-press while a
    // window is already open used to jump straight into a fresh classic
    // phase without stopping whatever phase was already running. If that
    // happened to be the LE phase, classic inquiry would start running
    // concurrently with the still-active LE scan -- the exact conflict
    // fixed for the normal flow, reachable again through this unguarded
    // re-entry. stop_all_scanning() first makes this safe regardless of
    // prior state (a harmless no-op on a genuinely fresh first press).
    stop_all_scanning();
    classic_scan_phase();
    printf("BT: pairing window open (%d s), scanning for nearby devices (classic + LE, alternating)...\n", PAIRING_WINDOW_MS / 1000);
}

static void stop_pairing_window(void *context) {
    (void)context;
    pairing_active = false;
    gap_discoverable_control(0);
    stop_all_scanning();
    printf("BT: pairing window closed\n");
}

static btstack_context_callback_registration_t start_pairing_callback = {.callback = &start_pairing_window};
static btstack_context_callback_registration_t stop_pairing_callback = {.callback = &stop_pairing_window};

// Call regularly from the main loop: debounces the pairing button and
// times out the pairing window. Runs on the plain main thread -- schedules
// the actual BTstack calls onto BTstack's own context rather than making
// them directly (see comment above start_pairing_window()).
void bt_hid_task(void) {
    static bool stable_pressed = false;
    static bool last_raw_pressed = false;
    static uint32_t last_change_ms = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw_pressed = !gpio_get(PAIRING_BUTTON_PIN); // active-low

    if (raw_pressed != last_raw_pressed) {
        last_raw_pressed = raw_pressed;
        last_change_ms = now;
    } else if (raw_pressed != stable_pressed && (now - last_change_ms) >= BUTTON_DEBOUNCE_MS) {
        stable_pressed = raw_pressed;
        if (stable_pressed) {
            btstack_run_loop_execute_on_main_thread(&start_pairing_callback);
        }
    }

    if (pairing_active && (int32_t)(now - pairing_deadline_ms) >= 0) {
        btstack_run_loop_execute_on_main_thread(&stop_pairing_callback);
    }
}

// Ctrl/Alt/regular keys resolve and press directly. SHIFT is deliberately
// excluded from this table-driven path -- see process_report() below.
static void update_key(uint16_t usage, uint8_t row, uint8_t col) {
    for (int i = 0; i < MAX_ACTIVE_KEYS; i++) {
        if (active_keys[i].usage == usage) {
            if (active_keys[i].row != row || active_keys[i].col != col) {
                coco_matrix_set_key(active_keys[i].row, active_keys[i].col, false);
                coco_matrix_set_key(row, col, true);
                active_keys[i].row = row;
                active_keys[i].col = col;
            }
            return;
        }
    }
    for (int i = 0; i < MAX_ACTIVE_KEYS; i++) {
        if (active_keys[i].usage == 0) {
            active_keys[i].usage = usage;
            active_keys[i].row = row;
            active_keys[i].col = col;
            coco_matrix_set_key(row, col, true);
            return;
        }
    }
    // No free slot -- extremely unlikely (would need >12 simultaneous keys).
}

// Shared by both the classic and LE paths -- everything from parsing the
// HID report onward is identical, they only differ in how the raw report
// bytes and matching HID descriptor are obtained (see process_report() and
// process_ble_report() below).
static void process_hid_input_report(const uint8_t *descriptor_data, uint16_t descriptor_len,
                                      const uint8_t *report, uint16_t report_len) {
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, descriptor_data, descriptor_len, HID_REPORT_TYPE_INPUT, report, report_len);

    // First pass: collect currently-active keyboard usages and shift state.
    // SHIFT itself is excluded from the list -- it's not a table-driven key
    // here, it's an input to keymap_resolve() for every OTHER active key,
    // and to the deferred CoCo-SHIFT computation below. Feeding it through
    // the same direct-press path as everything else would defeat the
    // shift-override keys (e.g. shift+2 -> '@' must NOT also hold CoCo
    // SHIFT down).
    uint16_t seen[MAX_ACTIVE_KEYS];
    int seen_count = 0;
    bool shift_held = false;

    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page, usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);
#ifdef DEBUG_KEY_LOG
        // Temporary diagnostic: some keys (reported not working over BT on
        // one keyboard so far) may not use the usage code our keymap
        // expects, or may not even be on the Keyboard usage page at all --
        // this shows every nonzero field regardless, to find out.
        if (value) {
            printf("HID field: usage_page=0x%04x usage=0x%04x value=%ld\n", usage_page, usage, (long)value);
        }
#endif
        if (!value) continue;

        if (usage_page == HID_USAGE_PAGE_CONSUMER && usage == CONSUMER_USAGE_AC_BACK) {
            // Found on real hardware (2026-08-14): the Anker compact
            // keyboard's Escape key sends Consumer Control "AC Back"
            // instead of a standard Keyboard-page Escape usage -- translate
            // it to the usual code so it flows through the normal keymap
            // below exactly like any other Escape/BREAK press.
            usage_page = HID_USAGE_PAGE_KEYBOARD;
            usage = KEYBOARD_USAGE_ESCAPE;
        }
        if (usage_page != HID_USAGE_PAGE_KEYBOARD) continue;
        if (usage == HID_USAGE_KEY_RESERVED) continue;

        if (usage == HID_USAGE_KEY_KEYBOARD_LEFTSHIFT || usage == HID_USAGE_KEY_KEYBOARD_RIGHTSHIFT) {
            shift_held = true;
            continue;
        }
        if (seen_count < MAX_ACTIVE_KEYS) {
            seen[seen_count++] = usage;
        }
    }

    // Releases: previously-active usages no longer present in this report.
    for (int i = 0; i < MAX_ACTIVE_KEYS; i++) {
        if (active_keys[i].usage == 0) continue;
        bool still_present = false;
        for (int j = 0; j < seen_count; j++) {
            if (seen[j] == active_keys[i].usage) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            coco_matrix_set_key(active_keys[i].row, active_keys[i].col, false);
            active_keys[i].usage = 0;
        }
    }

    // Presses / re-targets.
    bool want_coco_shift_from_keys = false;
    for (int i = 0; i < seen_count; i++) {
        uint16_t usage = seen[i];
        if (usage > 0xff) continue; // standard keyboard usages fit in a byte
        uint8_t row, col;
        bool needs_coco_shift;
        if (!keymap_resolve((uint8_t)usage, shift_held, &row, &col, &needs_coco_shift)) continue;
        if (needs_coco_shift) {
            want_coco_shift_from_keys = true;
        }
        update_key(usage, row, col);
    }

    // SHIFT is forwarded last, from the OR of what each held key needs. If
    // nothing else is held, bare shift still forwards on its own.
    bool want_coco_shift = seen_count > 0 ? want_coco_shift_from_keys : shift_held;
    if (want_coco_shift != coco_shift_forwarded) {
        uint8_t row, col;
        bool unused_needs_shift;
        if (keymap_resolve(HID_USAGE_KEY_KEYBOARD_LEFTSHIFT, false, &row, &col, &unused_needs_shift)) {
            coco_matrix_set_key(row, col, want_coco_shift);
        }
        coco_shift_forwarded = want_coco_shift;
    }
}

// Classic HID Host reports arrive over the L2CAP INTR channel with a
// leading 0xa1 (Input report) protocol byte that isn't part of the report
// itself.
static void process_report(const uint8_t *report, uint16_t report_len) {
    if (report_len < 1) return;
    if (*report != 0xa1) return;
    report++;
    report_len--;
    process_hid_input_report(
        hid_descriptor_storage_get_descriptor_data(hid_host_cid),
        hid_descriptor_storage_get_descriptor_len(hid_host_cid),
        report, report_len);
}

// LE HOGP reports are delivered as plain GATT Characteristic Value
// notifications -- no equivalent header byte to strip.
static void process_ble_report(uint8_t service_index, const uint8_t *report, uint16_t report_len) {
    process_hid_input_report(
        hids_host_descriptor_storage_get_descriptor_data(ble_hids_cid, service_index),
        hids_host_descriptor_storage_get_descriptor_len(ble_hids_cid, service_index),
        report, report_len);
}

// Handles events from the LE HOGP client after hids_host_connect() --
// registered as its packet_handler argument, not through the general HCI
// event handler above.
static void ble_hid_service_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    uint8_t status;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("BT: LE HID service connect failed, status 0x%02x\n", status);
                gap_disconnect(ble_connection_handle);
                connecting = false;
                resume_scanning();
                break;
            }
            connecting = false;
            keyboard_connected = true;
            remember_last_keyboard(ble_remote_addr, true, ble_remote_addr_type);
            reset_key_state();
            // Mirrors the classic CONNECTION_OPENED success path: close the
            // window early on a successful connect rather than wait for
            // timeout.
            if (pairing_active) {
                stop_pairing_window(NULL);
            }
            printf("BT: keyboard connected (LE).\n");
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            keyboard_connected = false;
            ble_connection_handle = HCI_CON_HANDLE_INVALID;
            ble_hids_cid = 0;
            reset_key_state();
            printf("BT: keyboard disconnected (LE).\n");
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            process_ble_report(
                gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            break;
    }
}

// SM (Security Manager) events for LE pairing -- registered separately from
// the general HCI event handler via sm_add_event_handler().
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("BT: confirming pairing (%06" PRIu32 ")\n", sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_passkey_display_number_get_handle(packet));
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            printf("BT: type this passkey on the keyboard: %06" PRIu32 "\n", sm_event_passkey_display_number_get_passkey(packet));
            break;

        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                printf("BT: LE pairing complete, connecting HID service...\n");
                hids_host_connect(ble_connection_handle, ble_hid_service_packet_handler, hid_host_report_mode, &ble_hids_cid);
            } else {
                printf("BT: LE pairing failed, status 0x%02x\n", sm_event_pairing_complete_get_status(packet));
                gap_disconnect(ble_connection_handle);
                connecting = false;
                resume_scanning();
            }
            break;

        // An already-bonded device reconnecting re-encrypts using the
        // stored LTK instead of running a fresh pairing dance -- that path
        // reports here, not SM_EVENT_PAIRING_COMPLETE. Found via real-world
        // testing (2026-08-15): without this case, reconnecting to an
        // already-paired LE keyboard would silently never get past this
        // point -- the link would encrypt fine but hids_host_connect()
        // would never be called.
        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                printf("BT: LE re-encryption complete, connecting HID service...\n");
                hids_host_connect(ble_connection_handle, ble_hid_service_packet_handler, hid_host_report_mode, &ble_hids_cid);
            } else {
                printf("BT: LE re-encryption failed, status 0x%02x\n", sm_event_reencryption_complete_get_status(packet));
                gap_disconnect(ble_connection_handle);
                connecting = false;
                resume_scanning();
            }
            break;

        default:
            break;
    }
}

// Logs every LE advertisement seen during the LE scan phase, unconditionally
// -- not just ones that pass the HID service UUID filter -- mirroring how
// the classic path already logs every inquiry result before applying its
// own CoD filter. Added specifically to answer "is the keyboard advertising
// at all, just not with the 0x1812 HID service UUID we filter on?" versus
// "is LE scanning not receiving anything at all?" -- the two are
// indistinguishable from silence alone.
static void log_le_advertisement(const bd_addr_t addr, int8_t rssi, const uint8_t *ad_data, uint8_t ad_len, bool matched_hid_uuid) {
    char name[32];
    name[0] = '\0';
    ad_context_t context;
    for (ad_iterator_init(&context, ad_len, ad_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        if (data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME && data_type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) continue;
        uint8_t data_len = ad_iterator_get_data_len(&context);
        if (data_len >= sizeof(name)) data_len = sizeof(name) - 1;
        memcpy(name, ad_iterator_get_data(&context), data_len);
        name[data_len] = '\0';
        break;
    }
    printf("BT: LE advertisement from %s, rssi %d, name \"%s\"%s\n",
           bd_addr_to_str(addr), rssi, name, matched_hid_uuid ? ", matches HID UUID filter" : "");
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    bd_addr_t event_addr;
    uint8_t status;

    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bt_ready = true;
                printf("BT: ready. Press the pairing button to make this board discoverable.\n");
                // Real hardware quirk, found and confirmed via repeated
                // testing (2026-08-14): the CYW43439's very first classic
                // inquiry after the stack comes up consistently finds
                // nothing (cycle completes normally, zero results), while
                // every inquiry after that works fine for the rest of that
                // power cycle -- confirmed via a dedicated test (button
                // pressed a 3rd time in the same session connected
                // immediately, vs. needing 2 presses every time before this
                // fix). Firing one throwaway inquiry cycle here, right as
                // the stack comes up and well before the user ever reaches
                // for the button, "spends" that quirk silently in the
                // background instead of costing a real pairing attempt.
                // pairing_active is false at this point, so
                // GAP_EVENT_INQUIRY_COMPLETE won't try to chain into an LE
                // phase afterward -- this cycle just runs once and stops.
                start_inquiry();
                // Kick off the auto-reconnect cycle now that the stack is
                // up -- see try_auto_reconnect() for what this actually
                // does and why it's safe to run alongside the warm-up
                // inquiry above.
                try_auto_reconnect();
            } else {
                printf("BT: state 0x%02x\n", btstack_event_state_get_state(packet));
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            if (pairing_active) {
                gap_pin_code_response(event_addr, "0000");
            } else {
                gap_pin_code_negative(event_addr);
            }
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            if (pairing_active) {
                gap_ssp_confirmation_response(event_addr);
            } else {
                // Not in the pairing window -- refuse so a stray/unexpected
                // pairing attempt can't complete without the button.
                gap_ssp_confirmation_negative(event_addr);
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT: {
            if (!pairing_active || connecting) break;
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            gap_event_inquiry_result_get_bd_addr(packet, event_addr);
            printf("BT: found %s, class 0x%06x\n", bd_addr_to_str(event_addr), (unsigned)cod);
            if (COD_MAJOR_DEVICE_CLASS(cod) != COD_MAJOR_PERIPHERAL) break;

            printf("BT: connecting to %s...\n", bd_addr_to_str(event_addr));
            connecting = true;
            stop_all_scanning();
            status = hid_host_connect(event_addr, hid_host_report_mode, &hid_host_cid);
            if (status != ERROR_CODE_SUCCESS) {
                printf("BT: hid_host_connect failed, status 0x%02x\n", status);
                connecting = false;
                resume_scanning();
            }
            break;
        }

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (!pairing_active || connecting) break;
            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            bool matched = ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
            gap_event_advertising_report_get_address(packet, event_addr);
            log_le_advertisement(event_addr, gap_event_advertising_report_get_rssi(packet), ad_data, ad_len, matched);
            if (!matched) break;

            bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);
            printf("BT: found LE HID device %s, connecting...\n", bd_addr_to_str(event_addr));
            connecting = true;
            stop_all_scanning();
            ble_connect_to(event_addr, addr_type);
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            btstack_run_loop_remove_timer(&ble_connect_timer);
            ble_connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            gap_subevent_le_connection_complete_get_peer_address(packet, ble_remote_addr);
            ble_remote_addr_type = gap_subevent_le_connection_complete_get_peer_address_type(packet);
            // Request pairing unconditionally -- sm_request_pairing() is a
            // no-op if the link is already encrypted (a previously bonded
            // keyboard reconnecting), otherwise it kicks off SM pairing;
            // completion is handled in sm_packet_handler() below.
            sm_request_pairing(ble_connection_handle);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (hci_event_disconnection_complete_get_connection_handle(packet) == ble_connection_handle) {
                ble_connection_handle = HCI_CON_HANDLE_INVALID;
                // If this happened after the HID service was already up,
                // GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED (not this
                // event) is what reports it -- connecting is already false
                // by then, so this only fires for a dead link mid-pairing.
                if (connecting) {
                    printf("BT: LE link closed before HID service was ready\n");
                    connecting = false;
                    resume_scanning();
                }
            }
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            printf("BT: inquiry cycle complete\n");
            // Nothing matched (or nothing found) this classic cycle --
            // hand off to an LE scan phase (see classic_scan_phase() /
            // le_scan_phase() above) rather than immediately restarting
            // inquiry, so the two never run concurrently.
            if (pairing_active && !connecting) {
                le_scan_phase();
            }
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    if (hid_subevent_incoming_connection_get_status(packet) == ERROR_CODE_SUCCESS) {
                        hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet), hid_host_report_mode);
                    }
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED:
                    status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("BT: connection failed, status 0x%02x\n", status);
                        hid_host_cid = 0;
                        connecting = false;
                        resume_scanning();
                        break;
                    }
                    hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    hid_host_descriptor_available = false;
                    connecting = false;
                    keyboard_connected = true;
                    {
                        bd_addr_t remote_addr;
                        hid_subevent_connection_opened_get_bd_addr(packet, remote_addr);
                        remember_last_keyboard(remote_addr, false, 0);
                    }
                    reset_key_state();
                    // A successful pairing/connection is a good time to
                    // close the window early rather than wait for timeout.
                    // Safe to call directly (not via execute_on_main_thread)
                    // -- we're already running inside BTstack's own context
                    // here, since BTstack itself invoked packet_handler().
                    if (pairing_active) {
                        stop_pairing_window(NULL);
                    }
                    printf("BT: keyboard connected.\n");
                    break;

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
                    status = hid_subevent_descriptor_available_get_status(packet);
                    hid_host_descriptor_available = (status == ERROR_CODE_SUCCESS);
                    if (!hid_host_descriptor_available) {
                        printf("BT: HID descriptor unavailable, status 0x%02x\n", status);
                    }
                    break;

                case HID_SUBEVENT_REPORT:
                    if (hid_host_descriptor_available) {
                        process_report(hid_subevent_report_get_report(packet), hid_subevent_report_get_report_len(packet));
                    }
                    break;

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    hid_host_cid = 0;
                    hid_host_descriptor_available = false;
                    keyboard_connected = false;
                    reset_key_state();
                    printf("BT: keyboard disconnected.\n");
                    break;

                default:
                    break;
            }
            break;

        default:
            break;
    }
}

void bt_hid_init(void) {
    gpio_init(PAIRING_BUTTON_PIN);
    gpio_set_dir(PAIRING_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PAIRING_BUTTON_PIN);

    btstack_run_loop_set_timer_handler(&inquiry_retry_timer, &inquiry_retry_timer_handler);
    btstack_run_loop_set_timer_handler(&auto_reconnect_timer, &auto_reconnect_timer_handler);

    // cyw43_arch_init() (called before this, in main()) already wires up
    // flash-backed persistent storage as the global TLV instance -- just
    // fetch it here for our own use (remembering which keyboard to
    // auto-reconnect to). Confirmed on real hardware that classic link
    // keys/LE bonds already survive a plain power cycle via this mechanism
    // with zero extra code; this reuses the same instance for a different,
    // smaller piece of data (which address to try, not the key material
    // itself -- BTstack's own link-key/LE-device DBs already handle that
    // part transparently once hid_host_connect()/gap_connect() is called).
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);

    l2cap_init();

    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    // LE side: SM (pairing/bonding) + GATT client + HOGP host. IO capability
    // is Display Only -- we have no input beyond the single pairing button,
    // so Passkey Entry pairing shows the code via printf (the only "display"
    // this board has) for the user to type on the keyboard; Just Works and
    // Numeric Comparison are auto-confirmed in sm_packet_handler() below.
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    gatt_client_init();
    hids_host_init(ble_hid_descriptor_storage, sizeof(ble_hid_descriptor_storage));

    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    // RSSI + EIR so inquiry results can include the device name for logging.
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // Always connectable, so an already-paired keyboard can reconnect any
    // time; discoverable only opens during a pairing-button-triggered
    // window (see start_pairing_window()).
    gap_connectable_control(1);
    gap_discoverable_control(0);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
}
