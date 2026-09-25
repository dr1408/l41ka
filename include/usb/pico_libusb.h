#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "libusb.h"
#include "pio_usb_configuration.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pico_libusb_diag {
    uint32_t sequence;
    uint32_t connected;
    uint32_t stage;
    int32_t rc;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t address;
    uint8_t max_packet0;
    uint8_t is_fullspeed;
    uint8_t descriptor_length;
    uint8_t descriptor_type;
} pico_libusb_diag_t;

enum {
    PICO_LIBUSB_DIAG_NONE = 0,
    PICO_LIBUSB_DIAG_WAIT_CONNECTION = 1,
    PICO_LIBUSB_DIAG_RESET = 2,
    PICO_LIBUSB_DIAG_OPEN_EP0 = 3,
    PICO_LIBUSB_DIAG_SET_ADDRESS = 4,
    PICO_LIBUSB_DIAG_READ_DESCRIPTOR = 5,
    PICO_LIBUSB_DIAG_REOPEN_EP0 = 6,
    PICO_LIBUSB_DIAG_DONE = 7,
};

int pico_libusb_set_configuration(const pio_usb_configuration_t *configuration);
bool pico_libusb_device_connected(libusb_context *ctx);
int pico_libusb_open_device(
    libusb_context *ctx,
    libusb_device_handle **dev_handle,
    uint16_t *vendor_id,
    uint16_t *product_id
);
int pico_libusb_get_connected_device_id(libusb_context *ctx, uint16_t *vendor_id, uint16_t *product_id);
int pico_libusb_get_diag(libusb_context *ctx, pico_libusb_diag_t *diag);
int pico_libusb_reset_ep0(libusb_device_handle *dev_handle, uint8_t max_packet_size);
int pico_libusb_raw_ep0_transfer(
    libusb_device_handle *dev_handle,
    uint8_t token_pid,
    const void *encoded_packet,
    uint16_t encoded_length,
    uint8_t *handshake
);

#ifdef __cplusplus
}
#endif
