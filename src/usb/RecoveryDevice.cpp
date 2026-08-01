// Copyright (c) 0cyn All Rights Reserved.

#include "usb/RecoveryDevice.h"

#include <stddef.h>

#include "usb/libusb.h"

namespace usb {
	namespace {
		constexpr uint8_t RecoveryVendorOut = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) | static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE);
		constexpr uint8_t ExecuteCommandRequest = 1;
		constexpr char RebootCommand[] = "reboot";
		constexpr uint16_t RebootCommandSize = sizeof(RebootCommand);
		constexpr uint8_t DeviceRequestTypeIn = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_STANDARD) | static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE);
		constexpr uint16_t DeviceDescriptorLength = 18;
		constexpr uint16_t MaxStringDescriptorLength = 255;
		constexpr uint8_t DeviceDescriptorSerialIndexOffset = 16;
		constexpr uint16_t EnglishLanguageId = 0x0409;
	}  // namespace

	RecoveryDevice::RecoveryDevice(libusb_context* context, libusb_device_handle* handle) : device_(context, handle) {}

	int RecoveryDevice::Reboot(unsigned int timeout_ms)
	{
		const int transferred = libusb_control_transfer(device_.handle_, RecoveryVendorOut, ExecuteCommandRequest, 0, 0,
			const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(RebootCommand)), RebootCommandSize,
			timeout_ms);
		return transferred < 0 || transferred == RebootCommandSize ? transferred : LIBUSB_ERROR_IO;
	}

	int RecoveryDevice::Serial(char* serial, uint16_t length, unsigned int timeout_ms)
	{
		if (serial == nullptr || length == 0u)
			return LIBUSB_ERROR_INVALID_PARAM;
		uint8_t descriptor[DeviceDescriptorLength] {};
		const int transferred = libusb_control_transfer(device_.handle_, DeviceRequestTypeIn,
			LIBUSB_REQUEST_GET_DESCRIPTOR, static_cast<uint16_t>(LIBUSB_DT_DEVICE << 8u), 0, descriptor,
			DeviceDescriptorLength, timeout_ms);
		if (transferred < 0)
			return transferred;
		if (transferred != DeviceDescriptorLength)
			return LIBUSB_ERROR_IO;
		const uint8_t serial_index = descriptor[DeviceDescriptorSerialIndexOffset];
		return serial_index == 0u ? LIBUSB_ERROR_NOT_FOUND : StringDescriptor(serial_index, serial, length, timeout_ms);
	}

	int RecoveryDevice::StringDescriptor(uint8_t index, char* value, uint16_t length, unsigned int timeout_ms)
	{
		if (index == 0u || value == nullptr || length == 0u)
			return LIBUSB_ERROR_INVALID_PARAM;
		value[0] = 0;
		uint8_t descriptor[MaxStringDescriptorLength] {};
		const int transferred = libusb_control_transfer(device_.handle_, DeviceRequestTypeIn,
			LIBUSB_REQUEST_GET_DESCRIPTOR, static_cast<uint16_t>((LIBUSB_DT_STRING << 8u) | index),
			EnglishLanguageId, descriptor, MaxStringDescriptorLength, timeout_ms);
		if (transferred < 0)
			return transferred;
		if (transferred < 2 || descriptor[0] < 2u || descriptor[0] > transferred
			|| descriptor[1] != LIBUSB_DT_STRING || (descriptor[0] & 1u) != 0u)
			return LIBUSB_ERROR_IO;
		const uint16_t character_count = static_cast<uint16_t>((descriptor[0] - 2u) / 2u);
		if (character_count >= length)
			return LIBUSB_ERROR_OVERFLOW;
		for (uint16_t i = 0; i < character_count; ++i)
		{
			const uint8_t low = descriptor[2u + i * 2u];
			const uint8_t high = descriptor[3u + i * 2u];
			value[i] = high == 0u && low >= 0x20u && low <= 0x7eu ? static_cast<char>(low) : '?';
		}
		value[character_count] = 0;
		return character_count;
	}
}  // namespace usb
