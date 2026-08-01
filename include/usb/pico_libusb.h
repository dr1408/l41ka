#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "libusb.h"
#include "pio_usb_configuration.h"

#ifdef __cplusplus
extern "C" {
#endif

int pico_libusb_set_configuration(const pio_usb_configuration_t *configuration);
bool pico_libusb_device_connected(libusb_context *ctx);
int pico_libusb_open_device(
    libusb_context *ctx,
    libusb_device_handle **dev_handle,
    uint16_t *vendor_id,
    uint16_t *product_id
);
int pico_libusb_get_connected_device_id(libusb_context *ctx, uint16_t *vendor_id, uint16_t *product_id);
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
