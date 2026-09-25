// Copyright (c) 0cyn All Rights Reserved.

#include "usb/PwnedDFUDevice.h"

#include <cstring>
#include <utility>

#include "control/Log.h"
#include "sys/unistd.h"
#include "usb/libusb.h"

namespace usb {
	namespace {
		constexpr uint16_t ControlRequestValue = 0xffff;
		constexpr uint64_t ControlMessageMagic = 0x2d2a4e5741502a2dull;
		constexpr uint16_t DfuResetLength = 16;
		constexpr uint16_t ExecRegistersSize = PwnedDFUDevice::ExecRegisterCount * sizeof(uint64_t);

		void PutU32(uint8_t* out, uint32_t value)
		{
			for (size_t i = 0; i < sizeof(value); ++i)
			{
				out[i] = static_cast<uint8_t>((value >> (i * 8u)) & 0xffu);
			}
		}

		void PutU64(uint8_t* out, uint64_t value)
		{
			for (size_t i = 0; i < sizeof(value); ++i)
			{
				out[i] = static_cast<uint8_t>((value >> (i * 8u)) & 0xffu);
			}
		}

		uint64_t GetU64(const uint8_t* input)
		{
			uint64_t value = 0;
			for (size_t i = 0; i < sizeof(value); ++i)
			{
				value |= static_cast<uint64_t>(input[i]) << (i * 8u);
			}
			return value;
		}

		uint16_t MinChunk(size_t remaining)
		{
			return remaining < PwnedDFUDevice::ControlMessageBodySize ?
				static_cast<uint16_t>(remaining) :
				PwnedDFUDevice::ControlMessageBodySize;
		}

		bool AddOffset(uint64_t address, size_t offset, uint64_t* result)
		{
			if (result == nullptr || offset > UINT64_MAX - address)
			{
				return false;
			}

			*result = address + static_cast<uint64_t>(offset);
			return true;
		}

		uint32_t Fnv1a(const void* data, size_t length)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			uint32_t hash = 2166136261u;
			for (size_t i = 0; i < length; ++i)
				hash = (hash ^ bytes[i]) * 16777619u;
			return hash;
		}
	}  // namespace

	PwnedDFUDevice::PwnedDFUDevice(DFUDevice&& dfu) : DFUDevice(std::move(dfu)) {}

	int PwnedDFUDevice::Read(uint64_t address, void* data, size_t length, unsigned int timeout_ms)
	{
		return ReadMemory(address, data, length, timeout_ms);
	}

	int PwnedDFUDevice::Write(uint64_t address, const void* data, size_t length, unsigned int timeout_ms)
	{
		return WriteMemory(address, data, length, timeout_ms);
	}

	int PwnedDFUDevice::Exec(uint64_t address, const uint64_t* arguments, size_t argument_count, ExecResult* result,
		const void* suffix, uint16_t suffix_length, unsigned int timeout_ms)
	{
		return ExecMessage(
			MessageType::Exec, address, arguments, argument_count, result, suffix, suffix_length, timeout_ms);
	}

	int PwnedDFUDevice::ExecEl1(uint64_t address, const uint64_t* arguments, size_t argument_count, ExecResult* result,
		const void* suffix, uint16_t suffix_length, unsigned int timeout_ms)
	{
		return ExecMessage(
			MessageType::ExecEl1, address, arguments, argument_count, result, suffix, suffix_length, timeout_ms);
	}

	int PwnedDFUDevice::SetBootLR(uint64_t target_lr, unsigned int timeout_ms)
	{
		L41KA_LOG(logging::Level::Info, "set-boot-lr submit lr=0x%llx", static_cast<unsigned long long>(target_lr));
		uint8_t response[ControlMessageSize] = {0};
		const int rc =
			ControlMessage(MessageType::SetBootLR, target_lr, nullptr, 0, sizeof(uint64_t), response, timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			L41KA_LOG(logging::Level::Warn, "set-boot-lr transport rc=%d", rc);
			return rc;
		}

		const uint64_t ack = GetU64(response + ControlMessageHeaderSize);
		const int final_rc = ack == 1u ? LIBUSB_SUCCESS : LIBUSB_ERROR_IO;
		L41KA_LOG(logging::Level::Info, "set-boot-lr returned ack=0x%llx rc=%d",
			static_cast<unsigned long long>(ack), final_rc);
		return final_rc;
	}

	int PwnedDFUDevice::ReadMemory(uint64_t address, void* data, size_t length, unsigned int timeout_ms)
	{
		if (length != 0u && data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		uint8_t* output = static_cast<uint8_t*>(data);
		size_t offset = 0;
		while (offset < length)
		{
			const uint16_t chunk_size = MinChunk(length - offset);
			uint64_t chunk_address = 0;
			if (!AddOffset(address, offset, &chunk_address))
			{
				return LIBUSB_ERROR_INVALID_PARAM;
			}

			uint8_t response[ControlMessageSize] = {0};
			const int rc =
				ControlMessage(MessageType::Read, chunk_address, nullptr, 0, chunk_size, response, timeout_ms);
			if (rc != LIBUSB_SUCCESS)
			{
				return rc;
			}

			std::memcpy(output + offset, response + ControlMessageHeaderSize, chunk_size);
			offset += chunk_size;
		}

		return LIBUSB_SUCCESS;
	}

	int PwnedDFUDevice::WriteMemory(uint64_t address, const void* data, size_t length, unsigned int timeout_ms)
	{
		if (length != 0u && data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		const uint8_t* input = static_cast<const uint8_t*>(data);
		size_t offset = 0;
		while (offset < length)
		{
			const uint16_t chunk_size = MinChunk(length - offset);
			uint64_t chunk_address = 0;
			if (!AddOffset(address, offset, &chunk_address))
			{
				return LIBUSB_ERROR_INVALID_PARAM;
			}

			L41KA_LOG(logging::Level::Info, "pwneddfu write begin addr=0x%llx len=%u fnv=%08lx",
				static_cast<unsigned long long>(chunk_address), static_cast<unsigned int>(chunk_size),
				static_cast<unsigned long>(Fnv1a(input + offset, chunk_size)));
			uint8_t response[ControlMessageSize] = {0};
			const int rc = ControlMessage(
				MessageType::Write, chunk_address, input + offset, chunk_size, chunk_size, response, timeout_ms);
			if (rc != LIBUSB_SUCCESS)
			{
				L41KA_LOG(logging::Level::Warn, "pwneddfu write fail addr=0x%llx len=%u rc=%d",
					static_cast<unsigned long long>(chunk_address), static_cast<unsigned int>(chunk_size), rc);
				return rc;
			}

			uint8_t verify[ControlMessageBodySize] = {0};
			const int verify_rc = ReadMemory(chunk_address, verify, chunk_size, timeout_ms);
			const bool matches = verify_rc == LIBUSB_SUCCESS && std::memcmp(verify, input + offset, chunk_size) == 0;
			L41KA_LOG(matches ? logging::Level::Info : logging::Level::Warn,
				"pwneddfu write verify addr=0x%llx len=%u rc=%d match=%lu read_fnv=%08lx",
				static_cast<unsigned long long>(chunk_address), static_cast<unsigned int>(chunk_size), verify_rc,
				static_cast<unsigned long>(matches ? 1u : 0u),
				static_cast<unsigned long>(verify_rc == LIBUSB_SUCCESS ? Fnv1a(verify, chunk_size) : 0u));
			if (!matches)
				return verify_rc == LIBUSB_SUCCESS ? LIBUSB_ERROR_IO : verify_rc;

			offset += chunk_size;
		}

		return LIBUSB_SUCCESS;
	}

	int PwnedDFUDevice::ExecMessage(MessageType type, uint64_t address, const uint64_t* arguments,
		size_t argument_count, ExecResult* result, const void* suffix, uint16_t suffix_length, unsigned int timeout_ms)
	{
		if (argument_count > ExecRegisterCount || (argument_count != 0u && arguments == nullptr)
			|| (suffix_length != 0u && suffix == nullptr) || suffix_length > ControlMessageBodySize - ExecRegistersSize)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		uint8_t body[ControlMessageBodySize] = {0};
		for (size_t i = 0; i < argument_count; ++i)
		{
			PutU64(body + (i * sizeof(uint64_t)), arguments[i]);
		}

		if (suffix_length != 0u)
		{
			std::memcpy(body + ExecRegistersSize, suffix, suffix_length);
		}

		uint8_t response[ControlMessageSize] = {0};
		const int rc = ControlMessage(type, address, body, static_cast<uint16_t>(ExecRegistersSize + suffix_length),
			static_cast<uint16_t>(ExecRegistersSize + suffix_length), response, timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		if (result != nullptr)
		{
			for (size_t i = 0; i < ExecRegisterCount; ++i)
			{
				result->registers[i] = GetU64(response + ControlMessageHeaderSize + (i * sizeof(uint64_t)));
			}

			std::memcpy(result->body, response + ControlMessageHeaderSize + ExecRegistersSize, suffix_length);
			result->body_length = suffix_length;
		}

		return LIBUSB_SUCCESS;
	}

	int PwnedDFUDevice::ControlMessage(MessageType type, uint64_t arg, const void* body, uint16_t body_length,
		uint16_t message_length, uint8_t response[ControlMessageSize], unsigned int timeout_ms)
	{
		if ((body_length != 0u && body == nullptr) || body_length > ControlMessageBodySize
			|| message_length > ControlMessageBodySize || response == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		uint8_t reset[DfuResetLength] = {0};
		int transferred = Download(0, reset, sizeof(reset), timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}
		if (transferred != static_cast<int>(sizeof(reset)))
		{
			return LIBUSB_ERROR_IO;
		}

		transferred = Download(0, nullptr, 0, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}
		if (transferred != 0)
		{
			return LIBUSB_ERROR_IO;
		}

		Status status = {};
		int rc = GetStatus(&status, timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		rc = GetStatus(&status, timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		uint8_t message[ControlMessageSize] = {0};
		PutU64(message, ControlMessageMagic);
		PutU32(message + 0x08, static_cast<uint32_t>(type));
		PutU32(message + 0x0c, 0);
		PutU64(message + 0x10, arg);
		PutU64(message + 0x18, message_length);
		if (body_length != 0u)
		{
			std::memcpy(message + ControlMessageHeaderSize, body, body_length);
		}

		transferred = Download(0, message, sizeof(message), timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}
		if (transferred != static_cast<int>(sizeof(message)))
		{
			return LIBUSB_ERROR_IO;
		}

		transferred = Upload(ControlRequestValue, response, ControlMessageSize, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}
		if (transferred != static_cast<int>(ControlMessageSize) || GetU64(response) != ControlMessageMagic)
		{
			return LIBUSB_ERROR_IO;
		}

		return LIBUSB_SUCCESS;
	}
}  // namespace usb
