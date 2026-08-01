#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

#define LIBUSB_API_VERSION 0x0100010a

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;

typedef enum libusb_error {
    LIBUSB_SUCCESS = 0,
    LIBUSB_ERROR_IO = -1,
    LIBUSB_ERROR_INVALID_PARAM = -2,
    LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4,
    LIBUSB_ERROR_NOT_FOUND = -5,
    LIBUSB_ERROR_BUSY = -6,
    LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OVERFLOW = -8,
    LIBUSB_ERROR_PIPE = -9,
    LIBUSB_ERROR_INTERRUPTED = -10,
    LIBUSB_ERROR_NO_MEM = -11,
    LIBUSB_ERROR_NOT_SUPPORTED = -12,
    LIBUSB_ERROR_OTHER = -99,
} libusb_error;

typedef enum libusb_endpoint_direction {
    LIBUSB_ENDPOINT_OUT = 0x00,
    LIBUSB_ENDPOINT_IN = 0x80,
} libusb_endpoint_direction;

typedef enum libusb_request_type {
    LIBUSB_REQUEST_TYPE_STANDARD = 0x00,
    LIBUSB_REQUEST_TYPE_CLASS = 0x20,
    LIBUSB_REQUEST_TYPE_VENDOR = 0x40,
    LIBUSB_REQUEST_TYPE_RESERVED = 0x60,
} libusb_request_type;

typedef enum libusb_request_recipient {
    LIBUSB_RECIPIENT_DEVICE = 0x00,
    LIBUSB_RECIPIENT_INTERFACE = 0x01,
    LIBUSB_RECIPIENT_ENDPOINT = 0x02,
    LIBUSB_RECIPIENT_OTHER = 0x03,
} libusb_request_recipient;

typedef enum libusb_standard_request {
    LIBUSB_REQUEST_GET_STATUS = 0x00,
    LIBUSB_REQUEST_CLEAR_FEATURE = 0x01,
    LIBUSB_REQUEST_SET_FEATURE = 0x03,
    LIBUSB_REQUEST_SET_ADDRESS = 0x05,
    LIBUSB_REQUEST_GET_DESCRIPTOR = 0x06,
    LIBUSB_REQUEST_SET_DESCRIPTOR = 0x07,
    LIBUSB_REQUEST_GET_CONFIGURATION = 0x08,
    LIBUSB_REQUEST_SET_CONFIGURATION = 0x09,
    LIBUSB_REQUEST_GET_INTERFACE = 0x0a,
    LIBUSB_REQUEST_SET_INTERFACE = 0x0b,
    LIBUSB_REQUEST_SYNCH_FRAME = 0x0c,
} libusb_standard_request;

typedef enum libusb_descriptor_type {
    LIBUSB_DT_DEVICE = 0x01,
    LIBUSB_DT_CONFIG = 0x02,
    LIBUSB_DT_STRING = 0x03,
    LIBUSB_DT_INTERFACE = 0x04,
    LIBUSB_DT_ENDPOINT = 0x05,
    LIBUSB_DT_BOS = 0x0f,
    LIBUSB_DT_HID = 0x21,
    LIBUSB_DT_REPORT = 0x22,
    LIBUSB_DT_PHYSICAL = 0x23,
    LIBUSB_DT_HUB = 0x29,
    LIBUSB_DT_SUPERSPEED_HUB = 0x2a,
    LIBUSB_DT_SS_ENDPOINT_COMPANION = 0x30,
} libusb_descriptor_type;

int LIBUSB_CALL libusb_init(libusb_context **ctx);
void LIBUSB_CALL libusb_exit(libusb_context *ctx);

libusb_device_handle *LIBUSB_CALL libusb_open_device_with_vid_pid(
    libusb_context *ctx,
    uint16_t vendor_id,
    uint16_t product_id
);

void LIBUSB_CALL libusb_close(libusb_device_handle *dev_handle);

int LIBUSB_CALL libusb_set_configuration(
    libusb_device_handle *dev_handle,
    int configuration
);

int LIBUSB_CALL libusb_control_transfer(
    libusb_device_handle *dev_handle,
    uint8_t bmRequestType,
    uint8_t bRequest,
    uint16_t wValue,
    uint16_t wIndex,
    unsigned char *data,
    uint16_t wLength,
    unsigned int timeout
);

int LIBUSB_CALL libusb_bulk_transfer(
    libusb_device_handle *dev_handle,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    int *transferred,
    unsigned int timeout
);

const char *LIBUSB_CALL libusb_error_name(int errcode);
const char *LIBUSB_CALL libusb_strerror(enum libusb_error errcode);

#ifdef __cplusplus
}
#endif
