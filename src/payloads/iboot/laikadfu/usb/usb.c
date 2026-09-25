// Copyright (c) 0cyn All Rights Reserved

#include "usb.h"

#include "ch9.h"
#include "../laikadfu_offsets.h"
#include "../diag.h"
#include "synopsys.h"

#pragma clang section text="__TEXT,__laikadfu"
#pragma clang section rodata="__DATA,__laikadfu_usb"

#define MAX_PONGO_SIZE    0x200000u
#define CACHE_CLEAN_STRIDE 64u

extern volatile const struct laikadfu_offsets gLaikaDFUOffsets;

extern __attribute__((noreturn)) void laikadfu_spin();
extern void laikadfu_cache_clean(const void *addr);
extern void laikadfu_sync_instructions(void);
extern __attribute__((noreturn)) void laikadfu_fail(uint32_t code);
extern __attribute__((noreturn)) void laikadfu_handoff(
	uint64_t next_stage, uint64_t boot_args);

struct laikadfu_usb_configuration {
	struct usb_configuration_descriptor configuration;
	struct usb_interface_descriptor interface;
	struct usb_endpoint_descriptor endpoint;
} PACKED;

static const struct usb_device_descriptor laikadfu_usb_device_descriptor = {
	.bLength = sizeof(struct usb_device_descriptor),
	.bDescriptorType = USB_DEVICE_DESCRIPTOR,
	.bcdUSB = 0x0200,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x05ac,
	.idProduct = 0x4c41,
	.bcdDevice = 0x0100,
	.bNumConfigurations = 1,
};

static const struct laikadfu_usb_configuration laikadfu_usb_configuration_descriptor = {
	.configuration = {
		.bLength = sizeof(struct usb_configuration_descriptor),
		.bDescriptorType = USB_CONFIGURATION_DESCRIPTOR,
		.wTotalLength = sizeof(struct laikadfu_usb_configuration),
		.bNumInterfaces = 1,
		.bConfigurationValue = 1,
		.bmAttributes = USB_CONFIGURATION_ATTRIBUTE_RES1,
		.bMaxPower = 50,
	},
	.interface = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = USB_INTERFACE_DESCRIPTOR,
		.bNumEndpoints = 1,
		.bInterfaceClass = 0xff,
	},
	.endpoint = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
		.bEndpointAddress = USB_ENDPOINT_ADDR_OUT(2),
		.bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
		.wMaxPacketSize = 512,
	},
};

static const struct laikadfu_usb_configuration laikadfu_usb_other_speed_configuration = {
	.configuration = {
		.bLength = sizeof(struct usb_configuration_descriptor),
		.bDescriptorType = USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR,
		.wTotalLength = sizeof(struct laikadfu_usb_configuration),
		.bNumInterfaces = 1,
		.bConfigurationValue = 1,
		.bmAttributes = USB_CONFIGURATION_ATTRIBUTE_RES1,
		.bMaxPower = 50,
	},
	.interface = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = USB_INTERFACE_DESCRIPTOR,
		.bNumEndpoints = 1,
		.bInterfaceClass = 0xff,
	},
	.endpoint = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
		.bEndpointAddress = USB_ENDPOINT_ADDR_OUT(2),
		.bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
		.wMaxPacketSize = 64,
	},
};

static const struct usb_device_qualifier_descriptor laikadfu_usb_device_qualifier = {
	.bLength = sizeof(struct usb_device_qualifier_descriptor),
	.bDescriptorType = USB_DEVICE_QUALIFIER_DESCRIPTOR,
	.bcdUSB = 0x0200,
	.bMaxPacketSize0 = 64,
	.bNumConfigurations = 1,
};

static const uint8_t laikadfu_usb_zero_response[2];
static const uint8_t laikadfu_usb_configuration_value = 1;

_Static_assert(sizeof(struct usb_device_descriptor) == 18, "descriptor size mismatch");
_Static_assert(sizeof(struct laikadfu_usb_configuration) == 25, "descriptor size mismatch");
_Static_assert(sizeof(struct usb_device_qualifier_descriptor) == 10,
	"descriptor size mismatch");

void laikadfu_usb_enumerate(uintptr_t scratch)
{
	synopsys_ep0_wait_setup(scratch);
	for (;;)
	{
		volatile const union usb_setup_packet *setup =
			(volatile const union usb_setup_packet *)scratch;
		uint8_t request_type = setup->raw.bmRequestType;
		uint8_t request = setup->raw.bRequest;
		uint16_t length = setup->raw.wLength;
		laikadfu_diag_checkpoint(LAIKADFU_DIAG_SETUP_SEEN);

		if ((request_type & USB_REQUEST_TYPE_DIRECTION(
			USB_REQUEST_TYPE_DIRECTION_DEVICE2HOST)) != 0)
		{
			if (request == USB_REQUEST_GET_DESCRIPTOR)
			{
				laikadfu_diag_checkpoint(LAIKADFU_DIAG_DESCRIPTOR);
				uint8_t descriptor_type = setup->get_descriptor.type;
				if (descriptor_type == USB_DEVICE_DESCRIPTOR)
					synopsys_ep0_send_data(scratch, &laikadfu_usb_device_descriptor,
						sizeof(laikadfu_usb_device_descriptor), length);
				else if (descriptor_type == USB_CONFIGURATION_DESCRIPTOR)
					synopsys_ep0_send_data(scratch,
						&laikadfu_usb_configuration_descriptor,
						sizeof(laikadfu_usb_configuration_descriptor), length);
				else if (descriptor_type == USB_DEVICE_QUALIFIER_DESCRIPTOR)
					synopsys_ep0_send_data(scratch, &laikadfu_usb_device_qualifier,
						sizeof(laikadfu_usb_device_qualifier), length);
				else if (descriptor_type == USB_OTHER_SPEED_CONFIGURATION_DESCRIPTOR)
					synopsys_ep0_send_data(scratch,
						&laikadfu_usb_other_speed_configuration,
						sizeof(laikadfu_usb_other_speed_configuration), length);
				else
					goto stall;
			}
			else if (request == USB_REQUEST_GET_STATUS)
				synopsys_ep0_send_data(
					scratch, laikadfu_usb_zero_response, 2, length);
			else if (request == USB_REQUEST_GET_CONFIGURATION)
				synopsys_ep0_send_data(
					scratch, &laikadfu_usb_configuration_value,
					sizeof(laikadfu_usb_configuration_value), length);
			else if (request == USB_REQUEST_GET_INTERFACE)
				synopsys_ep0_send_data(
					scratch, laikadfu_usb_zero_response, 1, length);
			else
				goto stall;
		}
		else if (request == USB_REQUEST_SET_ADDRESS)
		{
			laikadfu_diag_checkpoint(LAIKADFU_DIAG_SET_ADDRESS);
			synopsys_ep0_set_address(setup->set_address.address);
			synopsys_ep0_send_status(scratch);
		}
		else if (request == USB_REQUEST_SET_CONFIGURATION)
		{
			synopsys_ep0_send_status(scratch);
			return;
		}
		else if (request == USB_REQUEST_SET_INTERFACE)
			synopsys_ep0_send_status(scratch);
		else
		{
		stall:
			synopsys_ep0_stall();
		}

		synopsys_ep0_wait_setup(scratch);
	}
}

static void clean_received_range(const void *address, uint32_t length)
{
	uintptr_t current = (uintptr_t)address & ~(uintptr_t)(CACHE_CLEAN_STRIDE - 1u);
	uintptr_t end = (uintptr_t)address + length;
	while (current < end)
	{
		laikadfu_cache_clean((const void *)current);
		current += CACHE_CLEAN_STRIDE;
	}
}

__attribute__((noreturn)) void laikadfu_usb_receive(
	uint64_t next_stage, uint64_t boot_args, uintptr_t scratch)
{
	synopsys_ep2_initialize();
	synopsys_ep2_prime(scratch);
	uint32_t count = synopsys_ep2_wait(scratch);
	uint8_t *packet = (uint8_t *)(scratch + 128);
	if (count != 16)
		laikadfu_fail(0xe001u);
	if (*(volatile uint64_t *)packet != 0x474e50414b49414cull)
		laikadfu_fail(0xe002u);

	uint32_t remaining = *(volatile uint32_t *)(packet + 8);
	if (remaining == 0 || remaining > MAX_PONGO_SIZE)
		laikadfu_fail(0xe003u);
	if (*(volatile uint32_t *)(packet + 12) != 0)
		laikadfu_fail(0xe004u);
	*(volatile uint32_t *)(scratch + 256) = remaining;

	uint8_t *destination = (uint8_t *)gLaikaDFUOffsets.sram_base;
	synopsys_ep2_prime(scratch);
	while (remaining != 0)
	{
		count = synopsys_ep2_wait(scratch);
		if (count == 0)
			laikadfu_fail(0xe005u);
		if (count > remaining)
			laikadfu_fail(0xe006u);

		uint8_t *clean_address = destination;
		uint32_t copied = 0;
		for (; copied + sizeof(uint64_t) <= count; copied += sizeof(uint64_t))
			*(uint64_t *)(destination + copied) =
				*(volatile uint64_t *)(packet + copied);
		for (; copied < count; copied++)
			destination[copied] = packet[copied];
		clean_received_range(clean_address, count);

		destination += count;
		remaining -= count;
		if (remaining != 0)
			synopsys_ep2_prime(scratch);
	}

	synopsys_shutdown(); // or we break pongo
	laikadfu_sync_instructions();
	laikadfu_handoff(next_stage, boot_args);

	laikadfu_spin();
}
