// Copyright (c) 0cyn All Rights Reserved

#include "usb/DFUDevice.h"

#include <stddef.h>
#include <string.h>

#include "usb/libusb.h"
#include "usb/pico_libusb.h"
#include "usb_definitions.h"

extern "C" uint8_t pio_usb_ll_encode_tx_data(const uint8_t* buffer, uint8_t buffer_len, uint8_t* encoded_data);
extern "C" uint16_t calc_usb_crc16(const uint8_t* data, uint16_t len);

namespace usb {
	namespace {
		enum class Request : uint8_t
		{
			Detach = 0,
			Download = 1,
			Upload = 2,
			GetStatus = 3,
			ClearStatus = 4,
			GetState = 5,
			Abort = 6,
		};

		constexpr uint8_t DfuRequestTypeOut = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);
		constexpr uint8_t DfuRequestTypeIn = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);
		constexpr uint8_t DeviceRequestTypeIn = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_STANDARD) | static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE);

		constexpr uint16_t DeviceDescriptorLength = 18;
		constexpr uint16_t MaxStringDescriptorLength = 255;
		constexpr uint8_t DeviceDescriptorSerialIndexOffset = 16;
		constexpr uint16_t EnglishLanguageId = 0x0409;
		constexpr uint16_t DfuStatusLength = 6;
		constexpr uint16_t DfuStateLength = 1;

		uint64_t ParseCpid(const char* serial)
		{
			if (serial == nullptr)
			{
				return 0;
			}

			constexpr char Prefix[] = "CPID:";
			constexpr size_t PrefixLength = sizeof(Prefix) - 1u;

			for (const char* cursor = serial; *cursor != '\0'; ++cursor)
			{
				size_t matched = 0;
				while (matched < PrefixLength && cursor[matched] == Prefix[matched])
				{
					++matched;
				}

				if (matched != PrefixLength)
				{
					continue;
				}

				uint64_t value = 0;
				const char* digits = cursor + PrefixLength;
				for (size_t i = 0; i < 4u; ++i)
				{
					const char ch = digits[i];
					const int digit = ch >= '0' && ch <= '9' ?
						ch - '0' :
						ch >= 'a' && ch <= 'f' ?
						ch - 'a' + 10 :
						ch >= 'A' && ch <= 'F' ?
						ch - 'A' + 10 :
						-1;
					if (digit < 0)
					{
						return 0;
					}

					value = (value * 16u) + static_cast<uint64_t>(digit);
				}

				return value;
			}

			return 0;
		}

		bool Contains(const char* value, const char* needle)
		{
			if (value == nullptr || needle == nullptr || needle[0] == '\0')
			{
				return false;
			}

			for (const char* cursor = value; *cursor != '\0'; ++cursor)
			{
				size_t matched = 0;
				while (needle[matched] != '\0' && cursor[matched] == needle[matched])
				{
					++matched;
				}

				if (needle[matched] == '\0')
				{
					return true;
				}
			}

			return false;
		}
	}  // namespace

	DFUDevice::DFUDevice(libusb_context* context, libusb_device_handle* handle) : device_(context, handle)
	{
		char serial[MaxSerialLength] = {0};
		(void)Serial(serial, sizeof(serial));
	}

	uint16_t DFUDevice::InterfaceNumber() const
	{
		return interface_number_;
	}

	void DFUDevice::SetInterfaceNumber(uint16_t interface_number)
	{
		interface_number_ = interface_number;
	}

	uint64_t DFUDevice::CPID() const
	{
		return cpid_;
	}

	bool DFUDevice::IsPwned() const
	{
		return pwnd_;
	}

	bool DFUDevice::IdentityValid() const
	{
		return identity_valid_;
	}

	int DFUDevice::Serial(char* serial, uint16_t length, unsigned int timeout_ms)
	{
		if (serial == nullptr || length == 0u)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		serial[0] = '\0';

		unsigned char device_descriptor[DeviceDescriptorLength] = {0};
		int transferred = libusb_control_transfer(device_.handle_, DeviceRequestTypeIn, LIBUSB_REQUEST_GET_DESCRIPTOR,
			static_cast<uint16_t>(LIBUSB_DT_DEVICE << 8u), 0, device_descriptor, DeviceDescriptorLength, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != DeviceDescriptorLength)
		{
			return LIBUSB_ERROR_IO;
		}

		const uint8_t serial_index = device_descriptor[DeviceDescriptorSerialIndexOffset];
		if (serial_index == 0u)
		{
			return LIBUSB_ERROR_NOT_FOUND;
		}

		const int string_length = StringDescriptor(serial_index, serial, length, timeout_ms);
		if (string_length < 0)
			return string_length;

		cpid_ = ParseCpid(serial);
		identity_valid_ = true;
		if (Contains(serial, PwnedSerialMarker))
		{
			pwnd_ = true;
		}
		else
		{
			pwnd_ = false;
		}
		return string_length;
	}

	int DFUDevice::StringDescriptor(uint8_t index, char* value, uint16_t length, unsigned int timeout_ms)
	{
		if (index == 0u || value == nullptr || length == 0u)
			return LIBUSB_ERROR_INVALID_PARAM;
		value[0] = '\0';
		unsigned char descriptor[MaxStringDescriptorLength] = {0};
		const int transferred = libusb_control_transfer(device_.handle_, DeviceRequestTypeIn, LIBUSB_REQUEST_GET_DESCRIPTOR,
			static_cast<uint16_t>((LIBUSB_DT_STRING << 8u) | index), EnglishLanguageId, descriptor,
			MaxStringDescriptorLength, timeout_ms);
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
		value[character_count] = '\0';
		return character_count;
	}

	int DFUDevice::Detach(uint16_t detach_timeout_ms, unsigned int timeout_ms)
	{
		return libusb_control_transfer(device_.handle_, DfuRequestTypeOut, (uint8_t)Request::Detach, detach_timeout_ms,
			interface_number_, nullptr, 0, timeout_ms);
	}

	int DFUDevice::Download(uint16_t block_number, const void* data, uint16_t length, unsigned int timeout_ms)
	{
		return libusb_control_transfer(device_.handle_, DfuRequestTypeOut, (uint8_t)Request::Download, block_number,
			interface_number_, const_cast<unsigned char*>(static_cast<const unsigned char*>(data)), length, timeout_ms);
	}

	int DFUDevice::Upload(uint16_t block_number, void* data, uint16_t length, unsigned int timeout_ms)
	{
		return libusb_control_transfer(device_.handle_, DfuRequestTypeIn, (uint8_t)Request::Upload, block_number,
			interface_number_, static_cast<unsigned char*>(data), length, timeout_ms);
	}

	int DFUDevice::GetStatus(Status* status, unsigned int timeout_ms)
	{
		if (status == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		unsigned char data[DfuStatusLength] = {0};
		const int transferred = libusb_control_transfer(device_.handle_, DfuRequestTypeIn, (uint8_t)Request::GetStatus,
			0, interface_number_, data, DfuStatusLength, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != DfuStatusLength)
		{
			return LIBUSB_ERROR_IO;
		}

		status->code = static_cast<StatusCode>(data[0]);
		status->poll_timeout_ms = (uint32_t)data[1] | ((uint32_t)data[2] << 8u) | ((uint32_t)data[3] << 16u);
		status->state = static_cast<State>(data[4]);
		status->string_index = data[5];
		return LIBUSB_SUCCESS;
	}

	int DFUDevice::ClearStatus(unsigned int timeout_ms)
	{
		return libusb_control_transfer(device_.handle_, DfuRequestTypeOut, (uint8_t)Request::ClearStatus, 0,
			interface_number_, nullptr, 0, timeout_ms);
	}

	int DFUDevice::GetState(State* state, unsigned int timeout_ms)
	{
		if (state == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		unsigned char data[DfuStateLength] = {0};
		const int transferred = libusb_control_transfer(device_.handle_, DfuRequestTypeIn, (uint8_t)Request::GetState,
			0, interface_number_, data, DfuStateLength, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != DfuStateLength)
		{
			return LIBUSB_ERROR_IO;
		}

		*state = static_cast<State>(data[0]);
		return LIBUSB_SUCCESS;
	}

	int DFUDevice::Abort(unsigned int timeout_ms)
	{
		return libusb_control_transfer(
			device_.handle_, DfuRequestTypeOut, (uint8_t)Request::Abort, 0, interface_number_, nullptr, 0, timeout_ms);
	}

	int DFUDevice::ResetEp0(uint8_t max_packet_size)
	{
		return pico_libusb_reset_ep0(device_.handle_, max_packet_size);
	}

	int DFUDevice::RawEp0Transfer(
		RawTokenPid token_pid, const void* encoded_packet, uint16_t encoded_length, RawHandshake* handshake)
	{
		uint8_t raw_handshake = 0;
		const int rc = pico_libusb_raw_ep0_transfer(device_.handle_, static_cast<uint8_t>(token_pid), encoded_packet,
			encoded_length, handshake != nullptr ? &raw_handshake : nullptr);

		if (rc == LIBUSB_SUCCESS && handshake != nullptr)
		{
			*handshake = static_cast<RawHandshake>(raw_handshake);
		}

		return rc;
	}

	int DFUDevice::RawEp0DataTransfer(RawTokenPid token_pid, RawDataPid data_pid, const void* payload,
		uint16_t payload_length, RawHandshake* handshake)
	{
		if (payload_length > RawEp0MaxPayloadLength || (payload_length != 0u && payload == nullptr))
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		constexpr size_t RawPacketCapacity = 2u + RawEp0MaxPayloadLength + 2u;
		constexpr size_t EncodedPacketCapacity = (RawPacketCapacity * 2u * 7u / 6u) + 8u;
		uint8_t raw[RawPacketCapacity] = {0};
		uint8_t encoded[EncodedPacketCapacity] = {0};
		raw[0] = USB_SYNC;
		raw[1] = static_cast<uint8_t>(data_pid);
		if (payload_length != 0u)
		{
			memcpy(raw + 2, payload, payload_length);
		}
		const auto* payload_bytes =
			payload_length != 0u ? static_cast<const uint8_t*>(payload) : raw + 2;
		const uint16_t crc = calc_usb_crc16(payload_bytes, payload_length);
		raw[2u + payload_length] = static_cast<uint8_t>(crc);
		raw[3u + payload_length] = static_cast<uint8_t>(crc >> 8u);
		const uint8_t encoded_length =
			pio_usb_ll_encode_tx_data(raw, static_cast<uint8_t>(payload_length + 4u), encoded);
		return RawEp0Transfer(token_pid, encoded, encoded_length, handshake);
	}
}  // namespace usb
