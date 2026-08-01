// Copyright (c) 0cyn All Rights Reserved
#include "libusb.h"
#include "pico_libusb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pico/time.h"
#include "pico/stdlib.h"

#include "hardware/sync.h"

#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"

// this file is, not great
// usb is hard! for now just bother me if you're running into issues.
// i do intend to clean this up, but something like this wasn't explicitly required for the chain so i
//	havent really found the time or love for this file it needs yet.

#ifndef PICO_LIBUSB_CONNECT_TIMEOUT_MS
	#define PICO_LIBUSB_CONNECT_TIMEOUT_MS 0u
#endif

#ifndef PICO_LIBUSB_RESET_HOLD_MS
	#define PICO_LIBUSB_RESET_HOLD_MS 20u
#endif

#ifndef PICO_LIBUSB_RESET_SETTLE_MS
	#define PICO_LIBUSB_RESET_SETTLE_MS 50u
#endif

#define PICO_LIBUSB_ROOT_INDEX                        0u
#define PICO_LIBUSB_DEVICE_ADDRESS                    2u
#define USB_DEVICE_DESCRIPTOR_SIZE                    18u
#define USB_CONFIGURATION_DESCRIPTOR_SIZE             9u
#define USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE 7u
#define PICO_LIBUSB_MAX_CONFIGURATION_DESCRIPTOR_SIZE 512u
#define PICO_LIBUSB_DEFAULT_TIMEOUT_MS                1000u

struct libusb_context
{
	bool active;
	usb_device_t* device;
	root_port_t* root;
	uint8_t max_packet0;
};

struct libusb_device_handle
{
	libusb_context* context;
	bool open;
	bool configured;
	uint8_t configuration_value;
	uint16_t vendor_id;
	uint16_t product_id;
};

typedef struct
{
	bool finite;
	absolute_time_t deadline;
} pico_libusb_timeout_t;

static libusb_context g_context;
static libusb_device_handle g_handle;
static pio_usb_configuration_t g_configuration = PIO_USB_DEFAULT_CONFIG;
static bool g_host_started;

static uint16_t get_le16(const uint8_t* data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void put_le16(uint8_t* data, uint16_t value)
{
	data[0] = (uint8_t)(value & 0xffu);
	data[1] = (uint8_t)(value >> 8u);
}

static void timeout_init(pico_libusb_timeout_t* timeout, unsigned int timeout_ms)
{
	timeout->finite = timeout_ms != 0u;
	if (timeout->finite)
	{
		timeout->deadline = make_timeout_time_ms(timeout_ms);
	}
}

static bool timeout_reached(const pico_libusb_timeout_t* timeout)
{
	return timeout->finite && time_reached(timeout->deadline);
}

static libusb_context* default_context(void)
{
	return &g_context;
}

static int ensure_context(libusb_context* ctx)
{
	if (ctx == NULL)
	{
		ctx = default_context();
	}

	if (ctx != default_context())
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (!g_host_started)
	{
		memset(&g_context, 0, sizeof(g_context));
		g_context.device = pio_usb_host_init(&g_configuration);
		g_context.root = PIO_USB_ROOT_PORT(PICO_LIBUSB_ROOT_INDEX);
		g_context.max_packet0 = 8;
		g_host_started = true;
	}
	else
	{
		g_context.device = &pio_usb_device[0];
		g_context.root = PIO_USB_ROOT_PORT(PICO_LIBUSB_ROOT_INDEX);
	}

	g_context.active = true;
	return LIBUSB_SUCCESS;
}

static endpoint_t* find_endpoint(const libusb_context* ctx, uint8_t ep_address)
{
	const uint8_t device_address = ctx->device->address;

	for (int i = 0; i < PIO_USB_EP_POOL_CNT; ++i)
	{
		endpoint_t* ep = PIO_USB_ENDPOINT(i);
		const bool control_match = ((ep_address & 0x7fu) == 0u) && ((ep->ep_num & 0x7fu) == 0u);

		if (ep->root_idx == PICO_LIBUSB_ROOT_INDEX && ep->dev_addr == device_address && ep->size != 0u
			&& (ep->ep_num == ep_address || control_match))
		{
			return ep;
		}
	}

	return NULL;
}

static void reset_control_pipe(usb_device_t* device)
{
	control_pipe_t* pipe = &device->control_pipe;
	pipe->data_in_num = 0;
	pipe->buffer_idx = 0;
	pipe->rx_buffer = NULL;
	pipe->setup_packet.tx_address = NULL;
	pipe->setup_packet.tx_length = 0;
	pipe->out_data_packet.tx_address = NULL;
	pipe->out_data_packet.tx_length = 0;
	pipe->request_length = 0;
	pipe->operation = CONTROL_NONE;
	pipe->stage = STAGE_COMPLETE;
}

static int open_ep0(libusb_context* ctx, uint8_t max_packet_size)
{
	endpoint_descriptor_t ep0 = {
		.length = 7,
		.type = DESC_TYPE_ENDPOINT,
		.epaddr = 0x00,
		.attr = EP_ATTR_CONTROL,
		.max_size = {max_packet_size, 0x00},
		.interval = 0,
	};

	if (!pio_usb_host_endpoint_open(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, (const uint8_t*)&ep0, false))
	{
		return LIBUSB_ERROR_IO;
	}

	ctx->max_packet0 = max_packet_size;
	return LIBUSB_SUCCESS;
}

static int wait_for_connection(libusb_context* ctx)
{
	pico_libusb_timeout_t timeout;
	timeout_init(&timeout, PICO_LIBUSB_CONNECT_TIMEOUT_MS);

	while (!ctx->root->connected)
	{
		if (timeout_reached(&timeout))
		{
			return LIBUSB_ERROR_TIMEOUT;
		}

		tight_loop_contents();
	}

	return LIBUSB_SUCCESS;
}

static int reset_bus_and_open_ep0(libusb_context* ctx, uint8_t max_packet_size)
{
	pio_usb_host_close_device(PICO_LIBUSB_ROOT_INDEX, ctx->device->address);

	pio_usb_host_port_reset_start(PICO_LIBUSB_ROOT_INDEX);
	sleep_ms(PICO_LIBUSB_RESET_HOLD_MS);
	pio_usb_host_port_reset_end(PICO_LIBUSB_ROOT_INDEX);
	sleep_ms(PICO_LIBUSB_RESET_SETTLE_MS);

	ctx->device->connected = true;
	ctx->device->is_fullspeed = ctx->root->is_fullspeed;
	ctx->device->is_root = true;
	ctx->device->root = ctx->root;
	ctx->device->address = 0;

	reset_control_pipe(ctx->device);
	return open_ep0(ctx, max_packet_size);
}

static int wait_for_endpoint(
	libusb_context* ctx, endpoint_t* ep, uint8_t ep_address, const pico_libusb_timeout_t* timeout)
{
	while (ep->has_transfer)
	{
		if (!ctx->root->connected)
		{
			pio_usb_host_endpoint_abort_transfer(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, ep_address);
			return LIBUSB_ERROR_NO_DEVICE;
		}

		if (timeout_reached(timeout))
		{
			pio_usb_host_endpoint_abort_transfer(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, ep_address);
			return LIBUSB_ERROR_TIMEOUT;
		}

		tight_loop_contents();
	}

	if (ctx->device->control_pipe.operation == CONTROL_ERROR)
	{
		return LIBUSB_ERROR_PIPE;
	}

	return LIBUSB_SUCCESS;
}

static int send_setup_and_wait(libusb_context* ctx, const uint8_t setup[8], const pico_libusb_timeout_t* timeout)
{
	endpoint_t* ep = find_endpoint(ctx, 0x00);
	if (ep == NULL)
	{
		return LIBUSB_ERROR_NOT_FOUND;
	}

	reset_control_pipe(ctx->device);

	if (!pio_usb_host_send_setup(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, setup))
	{
		return LIBUSB_ERROR_IO;
	}

	return wait_for_endpoint(ctx, ep, 0x00, timeout);
}

static int endpoint_transfer_and_wait(libusb_context* ctx, uint8_t ep_address, uint8_t* data, uint16_t length,
	const pico_libusb_timeout_t* timeout, uint16_t* transferred)
{
	endpoint_t* ep = find_endpoint(ctx, ep_address);
	if (ep == NULL)
	{
		return LIBUSB_ERROR_NOT_FOUND;
	}

	reset_control_pipe(ctx->device);

	if (!pio_usb_host_endpoint_transfer(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, ep_address, data, length))
	{
		return LIBUSB_ERROR_BUSY;
	}

	const int rc = wait_for_endpoint(ctx, ep, ep_address, timeout);
	if (rc < 0)
	{
		return rc;
	}

	if (transferred != NULL)
	{
		*transferred = ep->actual_len;
	}

	return LIBUSB_SUCCESS;
}

static int control_transfer(libusb_context* ctx, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
	uint16_t wIndex, uint8_t* data, uint16_t wLength, unsigned int timeout_ms)
{
	if (wLength != 0u && data == NULL)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	uint8_t setup[8] = {0};
	setup[0] = bmRequestType;
	setup[1] = bRequest;
	put_le16(&setup[2], wValue);
	put_le16(&setup[4], wIndex);
	put_le16(&setup[6], wLength);

	pico_libusb_timeout_t timeout;
	timeout_init(&timeout, timeout_ms);

	int rc = send_setup_and_wait(ctx, setup, &timeout);
	if (rc < 0)
	{
		return rc;
	}

	const bool data_in = (bmRequestType & LIBUSB_ENDPOINT_IN) != 0u;
	uint16_t transferred = 0;

	if (wLength != 0u)
	{
		const uint8_t data_ep = data_in ? 0x80u : 0x00u;
		rc = endpoint_transfer_and_wait(ctx, data_ep, data, wLength, &timeout, &transferred);
		if (rc < 0)
		{
			return rc;
		}
	}

	const uint8_t status_ep = data_in ? 0x00u : 0x80u;
	rc = endpoint_transfer_and_wait(ctx, status_ep, NULL, 0, &timeout, NULL);
	if (rc < 0)
	{
		return rc;
	}

	return (int)transferred;
}

static int read_device_descriptor(libusb_context* ctx, uint8_t descriptor[USB_DEVICE_DESCRIPTOR_SIZE])
{
	return control_transfer(ctx, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
		LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_DEVICE << 8u), 0, descriptor, USB_DEVICE_DESCRIPTOR_SIZE,
		1000);
}

static int ensure_device_address(libusb_context* ctx)
{
	if (ctx->device->address == PICO_LIBUSB_DEVICE_ADDRESS)
	{
		return LIBUSB_SUCCESS;
	}

	if (ctx->device->address != 0u)
	{
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}

	int rc = control_transfer(ctx, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
		LIBUSB_REQUEST_SET_ADDRESS, PICO_LIBUSB_DEVICE_ADDRESS, 0, NULL, 0, PICO_LIBUSB_DEFAULT_TIMEOUT_MS);
	if (rc < 0)
	{
		return rc;
	}

	if (rc != 0)
	{
		return LIBUSB_ERROR_IO;
	}

	pio_usb_host_endpoint_close(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, 0x00);
	ctx->device->address = PICO_LIBUSB_DEVICE_ADDRESS;
	reset_control_pipe(ctx->device);
	sleep_ms(2);

	return open_ep0(ctx, ctx->max_packet0);
}

static int enumerate_device(libusb_context* ctx, uint8_t descriptor[USB_DEVICE_DESCRIPTOR_SIZE])
{
	int rc = wait_for_connection(ctx);
	if (rc < 0)
	{
		return rc;
	}

	rc = reset_bus_and_open_ep0(ctx, 8);
	if (rc < 0)
	{
		return rc;
	}

	rc = ensure_device_address(ctx);
	if (rc < 0)
	{
		return rc;
	}

	memset(descriptor, 0, USB_DEVICE_DESCRIPTOR_SIZE);
	const int transferred = read_device_descriptor(ctx, descriptor);
	if (transferred < 0)
	{
		return transferred;
	}

	if (transferred != USB_DEVICE_DESCRIPTOR_SIZE)
	{
		return LIBUSB_ERROR_IO;
	}

	const uint8_t max_packet0 = descriptor[7] == 0u ? 8u : descriptor[7];
	if (max_packet0 != ctx->max_packet0)
	{
		pio_usb_host_endpoint_close(PICO_LIBUSB_ROOT_INDEX, ctx->device->address, 0x00);
		rc = open_ep0(ctx, max_packet0);
		if (rc < 0)
		{
			return rc;
		}
	}

	return LIBUSB_SUCCESS;
}

static int read_typed_configuration_descriptor(
	libusb_context* ctx, uint8_t descriptor_type, uint8_t* descriptor, uint16_t length)
{
	return control_transfer(ctx, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
		LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(descriptor_type << 8u), 0, descriptor, length,
		PICO_LIBUSB_DEFAULT_TIMEOUT_MS);
}

static int read_configuration_descriptor(libusb_context* ctx, uint8_t* descriptor, uint16_t length)
{
	return read_typed_configuration_descriptor(ctx, LIBUSB_DT_CONFIG, descriptor, length);
}

static int validate_configuration_endpoints(const uint8_t* descriptor, uint16_t length)
{
	uint16_t offset = 0;
	while (offset < length)
	{
		if ((uint16_t)(length - offset) < 2u)
		{
			return LIBUSB_ERROR_IO;
		}

		const uint8_t descriptor_length = descriptor[offset];
		const uint8_t descriptor_type = descriptor[offset + 1u];
		if (descriptor_length < 2u || (uint16_t)descriptor_length > (uint16_t)(length - offset))
		{
			return LIBUSB_ERROR_IO;
		}

		if (descriptor_type == DESC_TYPE_ENDPOINT)
		{
			if (descriptor_length < sizeof(endpoint_descriptor_t))
			{
				return LIBUSB_ERROR_IO;
			}

			const endpoint_descriptor_t* endpoint = (const endpoint_descriptor_t*)&descriptor[offset];
			const uint8_t transfer_type = endpoint->attr & 0x03u;
			const uint16_t packet_size = get_le16(endpoint->max_size) & 0x07ffu;
			if ((transfer_type == EP_ATTR_BULK || transfer_type == EP_ATTR_INTERRUPT)
				&& (packet_size == 0u || packet_size > PIO_USB_EP_SIZE))
			{
				return packet_size > PIO_USB_EP_SIZE ? LIBUSB_ERROR_OVERFLOW : LIBUSB_ERROR_IO;
			}
		}

		offset = (uint16_t)(offset + descriptor_length);
	}

	return LIBUSB_SUCCESS;
}

static int open_configuration_endpoints(libusb_context* ctx, const uint8_t* descriptor, uint16_t length)
{
	if (descriptor == NULL || length < USB_CONFIGURATION_DESCRIPTOR_SIZE)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	uint16_t offset = 0;
	while (offset < length)
	{
		if ((uint16_t)(length - offset) < 2u)
		{
			return LIBUSB_ERROR_IO;
		}

		const uint8_t descriptor_length = descriptor[offset];
		const uint8_t descriptor_type = descriptor[offset + 1u];
		if (descriptor_length < 2u || (uint16_t)descriptor_length > (uint16_t)(length - offset))
		{
			return LIBUSB_ERROR_IO;
		}

		if (descriptor_type == DESC_TYPE_ENDPOINT)
		{
			if (descriptor_length < sizeof(endpoint_descriptor_t))
			{
				return LIBUSB_ERROR_IO;
			}

			endpoint_descriptor_t endpoint = *(const endpoint_descriptor_t*)&descriptor[offset];
			const uint8_t transfer_type = endpoint.attr & 0x03u;
			if (transfer_type == EP_ATTR_BULK || transfer_type == EP_ATTR_INTERRUPT)
			{
				const uint16_t packet_size = get_le16(endpoint.max_size) & 0x07ffu;
				if (packet_size == 0u)
				{
					return LIBUSB_ERROR_IO;
				}

				if (packet_size > PIO_USB_EP_SIZE)
				{
					put_le16(endpoint.max_size, PIO_USB_EP_SIZE);
				}

				if (!pio_usb_host_endpoint_open(
						PICO_LIBUSB_ROOT_INDEX, ctx->device->address, (const uint8_t*)&endpoint, false))
				{
					return LIBUSB_ERROR_NO_MEM;
				}
			}
		}

		offset = (uint16_t)(offset + descriptor_length);
	}

	return LIBUSB_SUCCESS;
}

static int set_device_configuration(libusb_device_handle* dev_handle, uint8_t configuration)
{
	int rc = ensure_device_address(dev_handle->context);
	if (rc < 0)
	{
		return rc;
	}

	uint8_t descriptor[PICO_LIBUSB_MAX_CONFIGURATION_DESCRIPTOR_SIZE] = {0};
	int transferred = read_configuration_descriptor(dev_handle->context, descriptor, USB_CONFIGURATION_DESCRIPTOR_SIZE);
	if (transferred < 0)
	{
		return transferred;
	}

	if (transferred != USB_CONFIGURATION_DESCRIPTOR_SIZE || descriptor[1] != DESC_TYPE_CONFIG)
	{
		return LIBUSB_ERROR_IO;
	}

	uint16_t total_length = get_le16(&descriptor[2]);
	if (total_length < USB_CONFIGURATION_DESCRIPTOR_SIZE)
	{
		return LIBUSB_ERROR_IO;
	}

	if (total_length > sizeof(descriptor))
	{
		return LIBUSB_ERROR_OVERFLOW;
	}

	transferred = read_configuration_descriptor(dev_handle->context, descriptor, total_length);
	if (transferred < 0)
	{
		return transferred;
	}

	if (transferred != total_length || descriptor[1] != DESC_TYPE_CONFIG)
	{
		return LIBUSB_ERROR_IO;
	}

	if (descriptor[5] != configuration)
	{
		return LIBUSB_ERROR_NOT_FOUND;
	}

	rc = validate_configuration_endpoints(descriptor, total_length);
	if (rc == LIBUSB_ERROR_OVERFLOW)
	{
		memset(descriptor, 0, sizeof(descriptor));
		transferred = read_typed_configuration_descriptor(dev_handle->context,
			USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE, descriptor, USB_CONFIGURATION_DESCRIPTOR_SIZE);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != USB_CONFIGURATION_DESCRIPTOR_SIZE
			|| descriptor[1] != USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE)
		{
			return LIBUSB_ERROR_IO;
		}

		const uint16_t other_speed_total_length = get_le16(&descriptor[2]);
		if (other_speed_total_length < USB_CONFIGURATION_DESCRIPTOR_SIZE
			|| other_speed_total_length > sizeof(descriptor))
		{
			return LIBUSB_ERROR_OVERFLOW;
		}

		transferred = read_typed_configuration_descriptor(dev_handle->context,
			USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE, descriptor, other_speed_total_length);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != other_speed_total_length
			|| descriptor[1] != USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR_TYPE
			|| descriptor[5] != configuration)
		{
			return LIBUSB_ERROR_IO;
		}

		rc = validate_configuration_endpoints(descriptor, other_speed_total_length);
		if (rc < 0)
		{
			return rc;
		}
		total_length = other_speed_total_length;
	}
	else if (rc < 0)
	{
		return rc;
	}

	rc = control_transfer(dev_handle->context,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE, LIBUSB_REQUEST_SET_CONFIGURATION,
		configuration, 0, NULL, 0, PICO_LIBUSB_DEFAULT_TIMEOUT_MS);
	if (rc < 0)
	{
		return rc;
	}

	if (rc != 0)
	{
		return LIBUSB_ERROR_IO;
	}

	rc = open_configuration_endpoints(dev_handle->context, descriptor, total_length);
	if (rc < 0)
	{
		return rc;
	}

	dev_handle->configured = true;
	dev_handle->configuration_value = configuration;
	return LIBUSB_SUCCESS;
}

int pico_libusb_set_configuration(const pio_usb_configuration_t* configuration)
{
	if (configuration == NULL)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (g_host_started || g_context.active)
	{
		return LIBUSB_ERROR_BUSY;
	}

	g_configuration = *configuration;
	return LIBUSB_SUCCESS;
}

bool pico_libusb_device_connected(libusb_context* ctx)
{
	if (ctx == NULL)
	{
		ctx = default_context();
	}

	return ensure_context(ctx) == LIBUSB_SUCCESS && ctx->root->connected;
}

int pico_libusb_get_connected_device_id(libusb_context* ctx, uint16_t* vendor_id, uint16_t* product_id)
{
	if (vendor_id == NULL || product_id == NULL)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (ctx == NULL)
	{
		ctx = default_context();
	}

	int rc = ensure_context(ctx);
	if (rc < 0)
	{
		return rc;
	}

	uint8_t descriptor[USB_DEVICE_DESCRIPTOR_SIZE] = {0};
	rc = enumerate_device(ctx, descriptor);
	if (rc < 0)
	{
		return rc;
	}

	*vendor_id = get_le16(&descriptor[8]);
	*product_id = get_le16(&descriptor[10]);
	return LIBUSB_SUCCESS;
}

int pico_libusb_open_device(
	libusb_context* ctx, libusb_device_handle** dev_handle, uint16_t* vendor_id, uint16_t* product_id)
{
	if (dev_handle == NULL || vendor_id == NULL || product_id == NULL)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	*dev_handle = NULL;
	*vendor_id = 0;
	*product_id = 0;

	if (ctx == NULL)
	{
		ctx = default_context();
	}

	int rc = ensure_context(ctx);
	if (rc < 0)
	{
		return rc;
	}

	if (g_handle.open)
	{
		return LIBUSB_ERROR_BUSY;
	}

	uint8_t descriptor[USB_DEVICE_DESCRIPTOR_SIZE] = {0};
	rc = enumerate_device(ctx, descriptor);
	if (rc < 0)
	{
		return rc;
	}

	memset(&g_handle, 0, sizeof(g_handle));
	g_handle.context = ctx;
	g_handle.open = true;
	g_handle.vendor_id = get_le16(&descriptor[8]);
	g_handle.product_id = get_le16(&descriptor[10]);

	*vendor_id = g_handle.vendor_id;
	*product_id = g_handle.product_id;
	*dev_handle = &g_handle;
	return LIBUSB_SUCCESS;
}

int pico_libusb_reset_ep0(libusb_device_handle* dev_handle, uint8_t max_packet_size)
{
	if (dev_handle == NULL || dev_handle != &g_handle || !dev_handle->open || max_packet_size == 0u)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (!dev_handle->context->root->connected)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return reset_bus_and_open_ep0(dev_handle->context, max_packet_size);
}

int pico_libusb_raw_ep0_transfer(libusb_device_handle* dev_handle, uint8_t token_pid, const void* encoded_packet,
	uint16_t encoded_length, uint8_t* handshake)
{
	if (dev_handle == NULL || dev_handle != &g_handle || !dev_handle->open)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (encoded_packet == NULL || encoded_length == 0u)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (token_pid != USB_PID_SETUP && token_pid != USB_PID_OUT)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	libusb_context* ctx = dev_handle->context;
	if (!ctx->root->connected)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	endpoint_t* ep0 = find_endpoint(ctx, 0x00);
	if (ep0 == NULL)
	{
		return LIBUSB_ERROR_NOT_FOUND;
	}

	if (ep0->has_transfer)
	{
		return LIBUSB_ERROR_BUSY;
	}

	pio_port_t* pp = PIO_USB_PIO_PORT(PICO_LIBUSB_ROOT_INDEX);
	uint8_t raw_handshake = 0;

	const uint32_t irq = save_and_disable_interrupts();
	{
		pio_usb_bus_prepare_receive(pp);
		pio_usb_bus_send_token(pp, token_pid, ctx->device->address, 0);
		pio_usb_bus_usb_transfer(pp, (uint8_t*)encoded_packet, encoded_length);
		pio_usb_bus_start_receive(pp);
		raw_handshake = pio_usb_bus_wait_handshake(pp);
		pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
		pp->usb_rx_buffer[0] = 0;
		pp->usb_rx_buffer[1] = 0;
	}
	restore_interrupts(irq);

	if (handshake != NULL)
	{
		*handshake = raw_handshake;
	}

	return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_init(libusb_context** ctx)
{
	const int rc = ensure_context(default_context());
	if (rc < 0)
	{
		return rc;
	}

	if (ctx != NULL)
	{
		*ctx = default_context();
	}

	return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_exit(libusb_context* ctx)
{
	if (ctx == NULL)
	{
		ctx = default_context();
	}

	if (ctx != default_context())
	{
		return;
	}

	g_context.active = false;
}

libusb_device_handle* LIBUSB_CALL libusb_open_device_with_vid_pid(
	libusb_context* ctx, uint16_t vendor_id, uint16_t product_id)
{
	libusb_device_handle* handle = NULL;
	uint16_t actual_vendor_id = 0;
	uint16_t actual_product_id = 0;
	if (pico_libusb_open_device(ctx, &handle, &actual_vendor_id, &actual_product_id) < 0)
	{
		return NULL;
	}

	if (actual_vendor_id != vendor_id || actual_product_id != product_id)
	{
		libusb_close(handle);
		return NULL;
	}

	return handle;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* dev_handle)
{
	if (dev_handle == NULL || dev_handle != &g_handle)
	{
		return;
	}

	pio_usb_host_close_device(PICO_LIBUSB_ROOT_INDEX, dev_handle->context->device->address);
	memset(dev_handle, 0, sizeof(*dev_handle));
}

int LIBUSB_CALL libusb_set_configuration(libusb_device_handle* dev_handle, int configuration)
{
	if (dev_handle == NULL || dev_handle != &g_handle || !dev_handle->open)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (configuration <= 0 || configuration > UINT8_MAX)
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (!dev_handle->context->root->connected)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (dev_handle->configured && dev_handle->configuration_value == (uint8_t)configuration)
	{
		return LIBUSB_SUCCESS;
	}

	return set_device_configuration(dev_handle, (uint8_t)configuration);
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* dev_handle, uint8_t bmRequestType, uint8_t bRequest,
	uint16_t wValue, uint16_t wIndex, unsigned char* data, uint16_t wLength, unsigned int timeout)
{
	if (dev_handle == NULL || dev_handle != &g_handle || !dev_handle->open)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (!dev_handle->context->root->connected)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	return control_transfer(dev_handle->context, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle* dev_handle, unsigned char endpoint, unsigned char* data,
	int length, int* transferred, unsigned int timeout)
{
	if (transferred != NULL)
	{
		*transferred = 0;
	}

	if (dev_handle == NULL || dev_handle != &g_handle || !dev_handle->open)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	if (length < 0 || length > UINT16_MAX || (length != 0 && data == NULL))
	{
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	if (!dev_handle->context->root->connected)
	{
		return LIBUSB_ERROR_NO_DEVICE;
	}

	pico_libusb_timeout_t transfer_timeout;
	timeout_init(&transfer_timeout, timeout);

	uint16_t actual = 0;
	const int rc =
		endpoint_transfer_and_wait(dev_handle->context, endpoint, data, (uint16_t)length, &transfer_timeout, &actual);
	if (rc < 0)
	{
		return rc;
	}

	if (transferred != NULL)
	{
		*transferred = (int)actual;
	}

	return LIBUSB_SUCCESS;
}

const char* LIBUSB_CALL libusb_error_name(int errcode)
{
	switch (errcode)
	{
	case LIBUSB_SUCCESS:
		return "LIBUSB_SUCCESS";
	case LIBUSB_ERROR_IO:
		return "LIBUSB_ERROR_IO";
	case LIBUSB_ERROR_INVALID_PARAM:
		return "LIBUSB_ERROR_INVALID_PARAM";
	case LIBUSB_ERROR_ACCESS:
		return "LIBUSB_ERROR_ACCESS";
	case LIBUSB_ERROR_NO_DEVICE:
		return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_NOT_FOUND:
		return "LIBUSB_ERROR_NOT_FOUND";
	case LIBUSB_ERROR_BUSY:
		return "LIBUSB_ERROR_BUSY";
	case LIBUSB_ERROR_TIMEOUT:
		return "LIBUSB_ERROR_TIMEOUT";
	case LIBUSB_ERROR_OVERFLOW:
		return "LIBUSB_ERROR_OVERFLOW";
	case LIBUSB_ERROR_PIPE:
		return "LIBUSB_ERROR_PIPE";
	case LIBUSB_ERROR_INTERRUPTED:
		return "LIBUSB_ERROR_INTERRUPTED";
	case LIBUSB_ERROR_NO_MEM:
		return "LIBUSB_ERROR_NO_MEM";
	case LIBUSB_ERROR_NOT_SUPPORTED:
		return "LIBUSB_ERROR_NOT_SUPPORTED";
	default:
		return "LIBUSB_ERROR_OTHER";
	}
}

const char* LIBUSB_CALL libusb_strerror(enum libusb_error errcode)
{
	switch (errcode)
	{
	case LIBUSB_SUCCESS:
		return "Success";
	case LIBUSB_ERROR_IO:
		return "Input/output error";
	case LIBUSB_ERROR_INVALID_PARAM:
		return "Invalid parameter";
	case LIBUSB_ERROR_ACCESS:
		return "Access denied";
	case LIBUSB_ERROR_NO_DEVICE:
		return "No such device";
	case LIBUSB_ERROR_NOT_FOUND:
		return "Entity not found";
	case LIBUSB_ERROR_BUSY:
		return "Resource busy";
	case LIBUSB_ERROR_TIMEOUT:
		return "Operation timed out";
	case LIBUSB_ERROR_OVERFLOW:
		return "Overflow";
	case LIBUSB_ERROR_PIPE:
		return "Pipe error";
	case LIBUSB_ERROR_INTERRUPTED:
		return "Interrupted";
	case LIBUSB_ERROR_NO_MEM:
		return "Insufficient memory";
	case LIBUSB_ERROR_NOT_SUPPORTED:
		return "Operation not supported";
	default:
		return "Other error";
	}
}
