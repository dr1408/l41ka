// Copyright (c) 0cyn All Rights Reserved
#include <cstring>

#include "pico/unique_id.h"
#include "tusb.h"

namespace {
	constexpr uint16_t UsbVid = 0x4141;
	constexpr uint16_t UsbPid = 0x4c41;
	constexpr uint8_t MicrosoftVendorRequest = 0x20;

	constexpr char Manufacturer[] = "cynder the dragon";
	constexpr char Product[] = "l41ka for " BOARD_NAME "";
	constexpr char ControlInterface[] = "l41ka control";
	constexpr char DebugInterface[] = "l41ka debug";
	static_assert(sizeof(Manufacturer) - 1u <= 126u);
	static_assert(sizeof(Product) - 1u <= 126u);

	enum Interface : uint8_t {
		InterfaceCdc,
		InterfaceCdcData,
		InterfaceControl,
		InterfaceCount,
	};

	enum StringId : uint8_t {
		StringLanguage,
		StringManufacturer,
		StringProduct,
		StringSerial,
		StringDebugInterface,
		StringControlInterface,
	};

	constexpr uint8_t EndpointCdcNotification = 0x81;
	constexpr uint8_t EndpointCdcOut = 0x02;
	constexpr uint8_t EndpointCdcIn = 0x82;
	constexpr uint8_t EndpointControlOut = 0x03;
	constexpr uint8_t EndpointControlIn = 0x83;
	constexpr uint16_t ConfigurationLength = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN;

	const tusb_desc_device_t DeviceDescriptor = {
		.bLength = sizeof(tusb_desc_device_t),
		.bDescriptorType = TUSB_DESC_DEVICE,
		.bcdUSB = 0x0210,
		.bDeviceClass = TUSB_CLASS_MISC,
		.bDeviceSubClass = MISC_SUBCLASS_COMMON,
		.bDeviceProtocol = MISC_PROTOCOL_IAD,
		.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
		.idVendor = UsbVid,
		.idProduct = UsbPid,
		.bcdDevice = 0x0100,
		.iManufacturer = StringManufacturer,
		.iProduct = StringProduct,
		.iSerialNumber = StringSerial,
		.bNumConfigurations = 1,
	};

	const uint8_t ConfigurationDescriptor[] = {
		TUD_CONFIG_DESCRIPTOR(1, InterfaceCount, 0, ConfigurationLength, 0, 250),
		TUD_CDC_DESCRIPTOR(InterfaceCdc, StringDebugInterface, EndpointCdcNotification, 8, EndpointCdcOut,
			EndpointCdcIn, 64),
		TUD_VENDOR_DESCRIPTOR(InterfaceControl, StringControlInterface, EndpointControlOut, EndpointControlIn, 64),
	};
	static_assert(sizeof(ConfigurationDescriptor) == ConfigurationLength);

	constexpr uint16_t MicrosoftDescriptorLength = 0xb2;
	constexpr uint16_t BosLength = TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN;
	const uint8_t BosDescriptor[] = {
		TUD_BOS_DESCRIPTOR(BosLength, 1),
		TUD_BOS_MS_OS_20_DESCRIPTOR(MicrosoftDescriptorLength, MicrosoftVendorRequest),
	};

	// Bind only the vendor control interface to WinUSB.
	const uint8_t MicrosoftDescriptor[] = {
		U16_TO_U8S_LE(0x000a), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
		U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MicrosoftDescriptorLength),
		U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
		0, 0, U16_TO_U8S_LE(MicrosoftDescriptorLength - 0x0a),
		U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
		InterfaceControl, 0, U16_TO_U8S_LE(MicrosoftDescriptorLength - 0x0a - 0x08),
		U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
		'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,
		U16_TO_U8S_LE(MicrosoftDescriptorLength - 0x0a - 0x08 - 0x08 - 0x14),
		U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
		U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002a),
		'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0,
		'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
		U16_TO_U8S_LE(0x0050),
		'{', 0, 'D', 0, '8', 0, '6', 0, '7', 0, '9', 0, '2', 0, '6', 0, 'C', 0, '-', 0,
		'2', 0, '1', 0, '1', 0, '1', 0, '-', 0, '4', 0, '1', 0, 'B', 0, '9', 0, '-', 0,
		'B', 0, 'C', 0, '6', 0, '1', 0, '-', 0, '6', 0, 'D', 0, 'B', 0, '9', 0, 'D', 0,
		'1', 0, 'B', 0, 'D', 0, 'D', 0, '3', 0, 'E', 0, '1', 0, '}', 0, 0, 0, 0, 0,
	};
	static_assert(sizeof(MicrosoftDescriptor) == MicrosoftDescriptorLength);

	uint16_t StringDescriptor[127];
	char Serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2u + 1u];

	const char* GetString(uint8_t index)
	{
		switch (index)
		{
		case StringManufacturer:
			return Manufacturer;
		case StringProduct:
			return Product;
		case StringSerial:
			if (Serial[0] == 0)
				pico_get_unique_board_id_string(Serial, sizeof(Serial));
			return Serial;
		case StringDebugInterface:
			return DebugInterface;
		case StringControlInterface:
			return ControlInterface;
		default:
			return nullptr;
		}
	}
}

const uint8_t* tud_descriptor_device_cb()
{
	return reinterpret_cast<const uint8_t*>(&DeviceDescriptor);
}

const uint8_t* tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return ConfigurationDescriptor;
}

const uint8_t* tud_descriptor_bos_cb()
{
	return BosDescriptor;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
	(void)language_id;
	if (index == StringLanguage)
	{
		StringDescriptor[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8u) | 4u);
		StringDescriptor[1] = 0x0409;
		return StringDescriptor;
	}

	const char* value = GetString(index);
	if (value == nullptr)
		return nullptr;
	const size_t length = std::strlen(value);
	if (length > 126u)
		return nullptr;
	for (size_t i = 0; i < length; ++i)
		StringDescriptor[i + 1u] = static_cast<uint8_t>(value[i]);
	StringDescriptor[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8u) | (2u * length + 2u));
	return StringDescriptor;
}

bool tud_vendor_control_xfer_cb(uint8_t root_port, uint8_t stage, const tusb_control_request_t* request)
{
	if (stage != CONTROL_STAGE_SETUP)
		return true;
	if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR || request->bRequest != MicrosoftVendorRequest
		|| request->wIndex != 7)
		return false;
	return tud_control_xfer(root_port, request, const_cast<uint8_t*>(MicrosoftDescriptor), sizeof(MicrosoftDescriptor));
}
