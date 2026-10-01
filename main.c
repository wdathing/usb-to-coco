#include "bsp/board_api.h"
#include "pico/cyw43_arch.h"
#include "tusb.h"

#include "bt_hid_app.h"
#include "matrix.h"

int main(void) {
    board_init(); // also brings up UART stdio for printf-based debugging

    coco_matrix_init();

    tusb_rhport_init_t host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUH_RHPORT, &host_init);

    // cyw43_arch_none uses a threadsafe_background async context, so
    // BTstack services itself via a low-priority IRQ -- no manual pump
    // needed here, just the pairing-button debounce in bt_hid_task().
    if (cyw43_arch_init()) {
        printf("BT: cyw43_arch_init failed, Bluetooth unavailable\n");
    } else {
        bt_hid_init();
    }

    while (true) {
        tuh_task();
        coco_matrix_task();
        bt_hid_task();
    }
}
